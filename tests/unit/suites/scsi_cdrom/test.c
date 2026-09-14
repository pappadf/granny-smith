// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// CD-ROM transfer-size tests for the SCSI target stack.
//
// Guards a bug that only a CD-ROM can reach.  READ(6) carries an 8-bit
// transfer length (0 meaning 256 blocks), and run_cmd used to reject any
// request whose byte count exceeded BUF_LIMIT (256 * 512) with CHECK
// CONDITION.  At a hard disk's 512-byte blocks that ceiling is exactly the
// most a READ(6) can ask for, so no HD access ever reaches it — but a CD-ROM's
// blocks are 2048 bytes, so the same block count is four times larger and any
// READ(6) of more than 64 blocks was refused.  Booting System 7.5.3 from a
// pressed Apple CD died on `lba=2531 tl=124`, a 248 KB read, and the System
// reported the refused read as "Not enough memory is available".
//
// Every image in gs-test-data is mastered at 512 bytes/block, so no
// integration row can reach this; hence a unit test.  It drives the real
// scsi.c/scsi_cdrom.c stack over a temp file through the external-initiator
// API — the same entry the 53C96 bus master uses.

#include "cpu.h"
#include "image.h"
#include "memory.h"
#include "scsi.h"
#include "scsi_internal.h"
#include "system.h"
#include "test_assert.h"
#include "via.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ============================================================
// Link stubs
// ============================================================
// scsi.c registers an object-model node and carries shell-facing helpers, so
// it references the wider emulator.  None of that is on the path this test
// drives (select -> CDB -> data-in), so the references are satisfied here
// rather than by linking system.c and pulling in the whole machine.

config_t *global_emulator = NULL;

void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                    void *context) {
    (void)mem, (void)addr, (void)size, (void)name, (void)iface, (void)context;
}
uint32_t cpu_get_pc(cpu_t *restrict cpu) {
    (void)cpu;
    return 0;
}
void via_input_c(via_t *via, int port, int c, bool value) {
    (void)via, (void)port, (void)c, (void)value;
}
image_t *setup_get_image_by_filename(const char *filename) {
    (void)filename;
    return NULL;
}
int system_hd_attach(const char *path, int scsi_id) {
    (void)path, (void)scsi_id;
    return -1;
}
bool add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id) {
    (void)config, (void)filename, (void)scsi_id;
}
// The bus-explicit forms, which `scsi.attach_hd` / `scsi.attach_cdrom` call
// so a machine with more than one visible SCSI bus (the Apple Network
// Servers' two fast/wide channels) can attach to the one it was asked for.
int system_hd_attach_on(struct scsi *bus, const char *path, int scsi_id) {
    (void)bus, (void)path, (void)scsi_id;
    return -1;
}
bool add_scsi_cdrom_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
    (void)config, (void)bus, (void)filename, (void)scsi_id;
}

#define CD_BLOCK  2048u
#define CD_BLOCKS 4096u // 8 MB of scratch medium
#define TARGET    3

// A scratch disc whose every block is stamped with its own LBA, so a read can
// be checked for landing at the right offset as well as the right length.
static char g_path[] = "/tmp/gs-scsi-cdrom-XXXXXX";

static void make_disc(void) {
    int fd = mkstemp(g_path);
    ASSERT_TRUE(fd >= 0);
    uint8_t *blk = calloc(1, CD_BLOCK);
    ASSERT_TRUE(blk != NULL);
    for (uint32_t lba = 0; lba < CD_BLOCKS; lba++) {
        blk[0] = (uint8_t)(lba >> 24);
        blk[1] = (uint8_t)(lba >> 16);
        blk[2] = (uint8_t)(lba >> 8);
        blk[3] = (uint8_t)lba;
        ASSERT_TRUE(write(fd, blk, CD_BLOCK) == (ssize_t)CD_BLOCK);
    }
    free(blk);
    close(fd);
}

// Issue a 6-byte CDB and return the phase the target moved to.  Used for the
// commands whose payload does not matter.
static int issue_cdb6(scsi_t *scsi, const uint8_t cdb[6]) {
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    int phase = scsi_get_bus_phase(scsi);
    if (phase == scsi_status) {
        scsi_external_status_byte(scsi);
        scsi_external_message_byte(scsi);
    }
    scsi_external_release(scsi);
    return phase;
}

static scsi_t *attach_disc(void) {
    scsi_t *scsi = scsi_init(NULL);
    ASSERT_TRUE(scsi != NULL);
    image_t *img = image_open_readonly(g_path);
    ASSERT_TRUE(img != NULL);
    scsi_add_device(scsi, TARGET, "SONY", "CD-ROM CDU-8002", "1.8g", img, scsi_dev_cdrom, CD_BLOCK, true);
    // A freshly loaded medium raises UNIT ATTENTION, so the first command off
    // any initiator comes back CHECK CONDITION.  Burn it with TEST UNIT READY
    // the way a real driver does, so the reads below start from a clean bus.
    const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
    issue_cdb6(scsi, tur);
    issue_cdb6(scsi, tur);
    return scsi;
}

// Issue READ(6) for `tl` blocks at `lba` and drain whatever the target
// delivers.  Returns the byte count; the caller checks it against the request.
// `first` receives the first four bytes so the landing offset is verifiable.
static size_t read6(scsi_t *scsi, uint32_t lba, uint8_t tl, uint8_t first[4]) {
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    ASSERT_EQ_INT(scsi_command, scsi_get_bus_phase(scsi));

    // READ(6): opcode, LBA[20:16], LBA[15:8], LBA[7:0], length, control.
    const uint8_t cdb[6] = {0x08, (uint8_t)((lba >> 16) & 0x1F), (uint8_t)(lba >> 8), (uint8_t)lba, tl, 0x00};
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);

    // A refused read lands in STATUS with CHECK CONDITION instead of DATA-IN.
    if (scsi_get_bus_phase(scsi) != scsi_data_in) {
        scsi_external_release(scsi);
        return 0;
    }

    size_t n = 0;
    uint8_t b;
    while (scsi_pop_data_in_byte(scsi, &b)) {
        if (n < 4 && first)
            first[n] = b;
        n++;
    }
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);
    return n;
}

// 64 blocks * 2048 = 131072 — exactly BUF_LIMIT, the largest read that
// succeeded even before the fix.  Pins the boundary from below.
TEST(read6_at_buf_limit) {
    scsi_t *scsi = attach_disc();
    uint8_t first[4] = {0};
    size_t n = read6(scsi, 100, 64, first);
    ASSERT_EQ_INT(64 * CD_BLOCK, (int)n);
    ASSERT_EQ_INT(100, (int)((first[0] << 24) | (first[1] << 16) | (first[2] << 8) | first[3]));
    scsi_delete(scsi);
}

// The regression: 124 blocks * 2048 = 253952, just under 2x BUF_LIMIT.  This
// is the exact request the 7.5.3 CD boot died on.
TEST(read6_over_buf_limit_cdrom) {
    scsi_t *scsi = attach_disc();
    uint8_t first[4] = {0};
    size_t n = read6(scsi, 2531, 124, first);
    ASSERT_EQ_INT(124 * CD_BLOCK, (int)n);
    ASSERT_EQ_INT(2531, (int)((first[0] << 24) | (first[1] << 16) | (first[2] << 8) | first[3]));
    scsi_delete(scsi);
}

// The ceiling: transfer length 0 means 256 blocks, 512 KB at 2048-byte
// blocks — four times BUF_LIMIT and the largest a READ(6) can express.
TEST(read6_max_blocks_cdrom) {
    scsi_t *scsi = attach_disc();
    uint8_t first[4] = {0};
    size_t n = read6(scsi, 1000, 0, first);
    ASSERT_EQ_INT(256 * CD_BLOCK, (int)n);
    ASSERT_EQ_INT(1000, (int)((first[0] << 24) | (first[1] << 16) | (first[2] << 8) | first[3]));
    scsi_delete(scsi);
}

// Out-of-range must still be refused — growing the buffer must not have turned
// the bounds check into a read past the end of the medium.
// INQUIRY: what the target has, and only pages it actually has.
//
// The allocation length is a ceiling — "the target shall terminate the DATA
// IN phase when it has transferred all available data" — so a driver that
// offers 255 bytes gets the 36 this model builds, and the additional-length
// byte has to agree with that.  AIX's `pscsidd` opens with exactly that
// command.
TEST(inquiry_standard_is_36_bytes) {
    scsi_t *scsi = attach_disc();
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    const uint8_t cdb[6] = {0x12, 0x00, 0x00, 0x00, 0xFF, 0x00};
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    ASSERT_EQ_INT(scsi_data_in, scsi_get_bus_phase(scsi));
    uint8_t buf[64] = {0};
    size_t n = 0;
    uint8_t b;
    while (scsi_pop_data_in_byte(scsi, &b)) {
        if (n < sizeof(buf))
            buf[n] = b;
        n++;
    }
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);
    ASSERT_EQ_INT(36, (int)n);
    ASSERT_EQ_INT(0x05, buf[0]); // CD-ROM
    ASSERT_EQ_INT(0x80, buf[1]); // removable
    ASSERT_EQ_INT(0x1F, buf[4]); // additional length: 31 more -> 36 total
}

// EVPD asks for a VITAL PRODUCT DATA page, and answering with the standard
// data is not a harmless approximation: the initiator parses the reply as
// the page it asked for.  A page this model does not have must be refused —
// SCSI-2 §8.2.5.1, ILLEGAL REQUEST / INVALID FIELD IN CDB.  AIX asks for
// page $C7 while configuring the device.
TEST(inquiry_evpd_unsupported_page_is_refused) {
    scsi_t *scsi = attach_disc();
    const uint8_t cdb[6] = {0x12, 0x01, 0xC7, 0x00, 0xFF, 0x00};
    ASSERT_EQ_INT(scsi_status, issue_cdb6(scsi, cdb));
}

// Page $00 is the list of supported pages, and this model supports exactly
// one: page $00.
TEST(inquiry_evpd_page_zero_lists_itself) {
    scsi_t *scsi = attach_disc();
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    const uint8_t cdb[6] = {0x12, 0x01, 0x00, 0x00, 0xFF, 0x00};
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    ASSERT_EQ_INT(scsi_data_in, scsi_get_bus_phase(scsi));
    uint8_t buf[8] = {0};
    size_t n = 0;
    uint8_t b;
    while (scsi_pop_data_in_byte(scsi, &b)) {
        if (n < sizeof(buf))
            buf[n] = b;
        n++;
    }
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);
    ASSERT_EQ_INT(5, (int)n);
    ASSERT_EQ_INT(0x05, buf[0]); // CD-ROM
    ASSERT_EQ_INT(0x00, buf[1]); // page code $00
    ASSERT_EQ_INT(0x01, buf[3]); // one page in the list
    ASSERT_EQ_INT(0x00, buf[4]); // ...page $00
}

TEST(read6_past_end_still_refused) {
    scsi_t *scsi = attach_disc();
    size_t n = read6(scsi, CD_BLOCKS - 4, 200, NULL);
    ASSERT_EQ_INT(0, (int)n);
    scsi_delete(scsi);
}

// ===========================================================================
// F-08: UNIT ATTENTION must report the cause that was staged, and removal is
// not one of the causes at all.
// ===========================================================================
//
// AUTHORITY: Sony CDU-541 SCSI manual S4.1.3 -- the drive we advertise is a
// SONY CD-ROM CDU-8002 (system.c), so its sense vocabulary is the one that
// applies, not SCSI-2's.  The unit attention condition arises on power-on, a
// reset, "the insertion of a caddy with the successful recovery of the table
// of contents", or MODE SELECT from another initiator.  Its UNIT ATTENTION
// (6h) table holds exactly three codes: 0x28 caddy inserted, 0x29 power-on or
// reset, 0x2A mode parameters changed.  Removal appears in neither list, and
// 0x3A does not appear anywhere in this drive's sense tables -- an empty bay
// is NOT READY (2h) with vendor code 0xB0, "Caddy not inserted in drive".

// Read one sense block with REQUEST SENSE.  Exempt from the UNIT ATTENTION
// gate, so it reports without being swallowed.
static void request_sense(scsi_t *scsi, uint8_t out[18]) {
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    const uint8_t cdb[6] = {0x03, 0, 0, 0, 18, 0};
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_in);
    size_t n = 0;
    uint8_t b;
    while (scsi_pop_data_in_byte(scsi, &b))
        if (n < 18)
            out[n++] = b;
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);
}

// Issue START/STOP UNIT.  flags bit 0 = Start, bit 1 = LoEj, so 0x02 is
// "stop and eject" (CDU-541 manual S5.2.33).
static int start_stop(scsi_t *scsi, uint8_t flags) {
    const uint8_t cdb[6] = {0x1B, 0, 0, 0, flags, 0};
    return issue_cdb6(scsi, cdb);
}

// A freshly inserted disc reports 0x28 "caddy inserted" -- not a hardcoded
// constant, but the cause staged at the point of insertion.
TEST(unit_attention_on_insert_reports_caddy_inserted) {
    scsi_t *scsi = scsi_init(NULL);
    ASSERT_TRUE(scsi != NULL);
    image_t *img = image_open_readonly(g_path);
    ASSERT_TRUE(img != NULL);
    scsi_add_device(scsi, TARGET, "SONY", "CD-ROM CDU-8002", "1.8g", img, scsi_dev_cdrom, CD_BLOCK, true);

    uint8_t sense[18] = {0};
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_UNIT_ATTENTION);
    ASSERT_EQ_INT(sense[12], 0x28); // not ready to ready transition
    ASSERT_EQ_INT(sense[13], 0x00);
    scsi_delete(scsi);
}

// The bug: eject staged UNIT ATTENTION / 0x3A, and the gate then overwrote it
// with 0x28 -- telling a guest that had just ejected a disc that one had
// arrived.  Removal now raises no unit attention at all, and the empty bay is
// reported as the persistent NOT READY condition the drive actually uses.
TEST(eject_reports_not_ready_not_media_arrived) {
    scsi_t *scsi = attach_disc();

    ASSERT_EQ_INT(start_stop(scsi, 0x02), scsi_status); // stop + LoEj = eject
    ASSERT_TRUE(!scsi_device_medium_present(scsi, TARGET));

    const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
    issue_cdb6(scsi, tur);

    uint8_t sense[18] = {0};
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_NOT_READY);
    ASSERT_EQ_INT(sense[12], ASC_SONY_CADDY_NOT_INSERTED); // 0xB0, not 0x3A
    scsi_delete(scsi);
}

// UNIT ATTENTION is a one-shot cleared by the first CHECK CONDITION; NOT READY
// is a state.  Modelling an empty bay as the former meant the second command
// after an eject succeeded against a drive with no disc in it.  Every command
// that needs the medium must keep failing for as long as the bay is empty.
TEST(empty_bay_keeps_failing_not_just_once) {
    scsi_t *scsi = attach_disc();
    ASSERT_EQ_INT(start_stop(scsi, 0x02), scsi_status);

    const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        issue_cdb6(scsi, tur);
        uint8_t sense[18] = {0};
        request_sense(scsi, sense);
        ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_NOT_READY);
    }
    scsi_delete(scsi);
}

// CDU-541 S4.1.3: "If an STOP UNIT command (with LoEj set) is received from an
// initiator with a pending unit attention condition the controller will
// perform the command and will not clear the unit attention condition."  A
// Sony extension -- ANSI X3.131-1986 S6.1.3 has no such carve-out.  Without it,
// ejecting a disc that was only just inserted fails, swallowed by the insert's
// own still-pending attention.
TEST(eject_is_exempt_from_pending_unit_attention) {
    scsi_t *scsi = scsi_init(NULL);
    ASSERT_TRUE(scsi != NULL);
    image_t *img = image_open_readonly(g_path);
    ASSERT_TRUE(img != NULL);
    scsi_add_device(scsi, TARGET, "SONY", "CD-ROM CDU-8002", "1.8g", img, scsi_dev_cdrom, CD_BLOCK, true);
    // UNIT ATTENTION deliberately left pending -- no command burns it first.

    ASSERT_EQ_INT(start_stop(scsi, 0x02), scsi_status);

    // The eject must have actually happened, not been swallowed by the gate.
    ASSERT_TRUE(!scsi_device_medium_present(scsi, TARGET));

    // ...and it must NOT have cleared the attention: S4.1.3 says the controller
    // "will perform the command and will not clear the unit attention
    // condition".  So the insert's 0x28 is still owed to the next command.
    const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
    issue_cdb6(scsi, tur);
    uint8_t sense[18] = {0};
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_UNIT_ATTENTION);
    ASSERT_EQ_INT(sense[12], 0x28);

    // Only once that is burned does the empty bay show through.
    issue_cdb6(scsi, tur);
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_NOT_READY);
    ASSERT_EQ_INT(sense[12], ASC_SONY_CADDY_NOT_INSERTED);
    scsi_delete(scsi);
}

// INQUIRY is exempt in both the ANSI text and Sony's, and must NOT clear the
// condition -- the attention still has to be reported to the next real command.
TEST(inquiry_does_not_clear_unit_attention) {
    scsi_t *scsi = scsi_init(NULL);
    ASSERT_TRUE(scsi != NULL);
    image_t *img = image_open_readonly(g_path);
    ASSERT_TRUE(img != NULL);
    scsi_add_device(scsi, TARGET, "SONY", "CD-ROM CDU-8002", "1.8g", img, scsi_dev_cdrom, CD_BLOCK, true);

    const uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    ASSERT_EQ_INT(issue_cdb6(scsi, inq), scsi_data_in); // executes normally

    uint8_t sense[18] = {0};
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_UNIT_ATTENTION); // still pending
    ASSERT_EQ_INT(sense[12], 0x28);
    scsi_delete(scsi);
}

// ===========================================================================
// F-09: a refused eject must say the prevent bit is set, not that the drive is
// empty.
// ===========================================================================
//
// AUTHORITY: CDU-541 SCSI manual S5.2.33 -- "If a PREVENT MEDIUM REMOVAL
// command has been issued, a request to eject the disc will be terminated with
// a CHECK CONDITION status.  The sense key will be set to ILLEGAL REQUEST, and
// the additional sense code set to PREVENT BIT SET", which its ILLEGAL REQUEST
// (5h) table numbers 0x80.  The old code reported 0x3A MEDIUM NOT PRESENT --
// "the drive is empty" -- which is the opposite of the truth and a reason for a
// driver to stop retrying.  SCSI-2's 0x53/0x02 is a different vocabulary and
// appears nowhere in this drive's tables.

// PREVENT/ALLOW MEDIUM REMOVAL: CDB byte 4 bit 0 is the prevent bit.
static int prevent_allow(scsi_t *scsi, bool prevent) {
    const uint8_t cdb[6] = {0x1E, 0, 0, 0, (uint8_t)(prevent ? 1 : 0), 0};
    return issue_cdb6(scsi, cdb);
}

TEST(eject_while_prevented_reports_prevent_bit_set) {
    scsi_t *scsi = attach_disc();

    ASSERT_EQ_INT(prevent_allow(scsi, true), scsi_status);
    ASSERT_EQ_INT(start_stop(scsi, 0x02), scsi_status); // eject: must be refused

    // The disc is still in the drive.
    ASSERT_TRUE(scsi_device_medium_present(scsi, TARGET));

    uint8_t sense[18] = {0};
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_ILLEGAL_REQUEST);
    ASSERT_EQ_INT(sense[12], ASC_SONY_PREVENT_BIT_SET); // 0x80, not 0x3A or 0x53
    ASSERT_EQ_INT(sense[13], 0x00);
    scsi_delete(scsi);
}

// ALLOW must lift the lock, so the same eject then succeeds.  Without this the
// test above would pass against a drive that simply never ejects.
TEST(allow_then_eject_succeeds) {
    scsi_t *scsi = attach_disc();

    ASSERT_EQ_INT(prevent_allow(scsi, true), scsi_status);
    ASSERT_EQ_INT(start_stop(scsi, 0x02), scsi_status);
    ASSERT_TRUE(scsi_device_medium_present(scsi, TARGET));

    ASSERT_EQ_INT(prevent_allow(scsi, false), scsi_status);
    ASSERT_EQ_INT(start_stop(scsi, 0x02), scsi_status);
    ASSERT_TRUE(!scsi_device_medium_present(scsi, TARGET));
    scsi_delete(scsi);
}

// The host's device[N].eject() -- the model of the front-panel button -- had no
// test at all, and honoured no lock: a guest that had locked the drive still
// lost its medium.  S5.2.14 inhibits removal "by use of a command through the
// interface OR BY USE OF THE EJECT BUTTON", so both routes now ask the same
// function and get the same answer.
TEST(host_eject_honours_the_guest_lock) {
    scsi_t *scsi = attach_disc();

    ASSERT_EQ_INT(prevent_allow(scsi, true), scsi_status);
    ASSERT_EQ_INT(scsi_eject_device(scsi, TARGET), -2); // refused, not -1 or 1
    ASSERT_TRUE(scsi_device_medium_present(scsi, TARGET));

    // And the refusal is the lock, not a broken host path: ALLOW frees it.
    ASSERT_EQ_INT(prevent_allow(scsi, false), scsi_status);
    ASSERT_EQ_INT(scsi_eject_device(scsi, TARGET), 1);
    ASSERT_TRUE(!scsi_device_medium_present(scsi, TARGET));
    scsi_delete(scsi);
}

// The return codes are a contract the object model reads to tell "refused" from
// "invalid id" from "nothing there"; collapsing any pair would make
// scsi.devices.N.eject() report the wrong thing.
TEST(host_eject_return_codes_are_distinct) {
    scsi_t *scsi = attach_disc();

    ASSERT_EQ_INT(scsi_eject_device(scsi, 7), -1); // 7 is the initiator
    ASSERT_EQ_INT(scsi_eject_device(scsi, -1), -1); // out of range
    ASSERT_EQ_INT(scsi_eject_device(NULL, TARGET), -1);
    ASSERT_EQ_INT(scsi_eject_device(scsi, 1), 0); // a slot with no device

    ASSERT_EQ_INT(scsi_eject_device(scsi, TARGET), 1); // the real thing
    ASSERT_EQ_INT(scsi_eject_device(scsi, TARGET), 0); // already empty
    scsi_delete(scsi);
}

// Removal does not lift the lock (S5.2.14 ends it on ALLOW, BUS DEVICE RESET or
// a reset condition -- never on removal), and the model cannot reach a locked
// empty drive by any other route either.
TEST(eject_leaves_no_locked_empty_drive) {
    scsi_t *scsi = attach_disc();

    ASSERT_EQ_INT(prevent_allow(scsi, false), scsi_status);
    ASSERT_EQ_INT(scsi_eject_device(scsi, TARGET), 1);

    // PREVENT on the now-empty drive is refused, so the only way in is shut.
    ASSERT_EQ_INT(prevent_allow(scsi, true), scsi_status);
    uint8_t sense[18] = {0};
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_NOT_READY);
    scsi_delete(scsi);
}

// CDU-541 S5.2.14: "If a PREVENT MEDIUM REMOVAL command is issued without the
// drive being in the ready condition [the] command will be terminated with a
// CHECK CONDITION status.  The sense key will be set to NOT READY."  The ready
// condition is a caddy inserted with its TOC recovered (S4.1.4).  Locking an
// empty drive used to be accepted, which then made the next inserted disc
// unejectable for no reason the guest could see.
TEST(prevent_on_empty_drive_is_refused) {
    scsi_t *scsi = attach_disc();
    ASSERT_EQ_INT(start_stop(scsi, 0x02), scsi_status); // eject first
    ASSERT_TRUE(!scsi_device_medium_present(scsi, TARGET));

    ASSERT_EQ_INT(prevent_allow(scsi, true), scsi_status);

    uint8_t sense[18] = {0};
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_NOT_READY);
    ASSERT_EQ_INT(sense[12], ASC_SONY_CADDY_NOT_INSERTED);
    scsi_delete(scsi);
}

// ALLOW is deliberately NOT refused on an empty drive: S5.2.14's sentence names
// PREVENT only, and a driver tidying up after an eject has every reason to send
// it.
TEST(allow_on_empty_drive_is_accepted) {
    scsi_t *scsi = attach_disc();
    ASSERT_EQ_INT(start_stop(scsi, 0x02), scsi_status);

    // Drain first.  Sense data persists until REQUEST SENSE reads it, so
    // without this the assertion below would be reading whatever the last
    // CHECK CONDITION left behind rather than this command's outcome.
    uint8_t sense[18] = {0};
    request_sense(scsi, sense);

    ASSERT_EQ_INT(prevent_allow(scsi, false), scsi_status);
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, 0x00); // NO SENSE: nothing was raised
    scsi_delete(scsi);
}

// ===========================================================================
// F-10: an allocation length of zero means zero.
// ===========================================================================
//
// AUTHORITY: the allocation length is a ceiling, never a request.  The CDU-541
// manual S4.2.6 states it once for every CDB that carries one, in the section
// describing "the common parts of the CDB": "An allocation length of zero
// indicates that no sense data will be transferred.  This condition will not be
// considered as an error."  ANSI X3.131-1986 says the same per command --
// INQUIRY (Table 7-8): "An allocation length of zero indicates that no INQUIRY
// data shall be transferred.  This condition shall not be considered as an
// error."
//
// Four different behaviours used to share this subsystem: five CD-ROM handlers
// sent the WHOLE response on zero, REQUEST SENSE substituted 18, INQUIRY
// substituted 36, and only the HD MODE SENSE path sent nothing.
//
// The consequence was not a wrong byte count but a stuck bus.  A handler that
// armed DATA IN with bytes the initiator never allocated for left the bus
// there: every exit from DATA IN is guarded by buf.size == 0, so a phase full
// of undrained bytes does not leave on its own.  These tests therefore assert
// the PHASE, not just the length -- a zero-allocation command must land in
// STATUS with no data phase at all.

// Issue a 6-byte CDB with byte 4 as the allocation length; return the phase the
// target ends up in without draining anything.
static int phase_after_cdb6(scsi_t *scsi, const uint8_t cdb[6]) {
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    int phase = scsi_get_bus_phase(scsi);
    scsi_external_release(scsi);
    return phase;
}

// Same for a 10-byte CDB, whose allocation length is bytes 7-8.
static int phase_after_cdb10(scsi_t *scsi, const uint8_t cdb[10]) {
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 10; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    int phase = scsi_get_bus_phase(scsi);
    scsi_external_release(scsi);
    return phase;
}

TEST(zero_allocation_length_transfers_nothing) {
    scsi_t *scsi = attach_disc();

    // opcode, then the CDB with its allocation length field zeroed.
    const uint8_t inquiry[6] = {0x12, 0, 0, 0, 0, 0};
    const uint8_t mode_sense[6] = {0x1A, 0, 0x3F, 0, 0, 0}; // page 0x3F = all
    const uint8_t request_sense[6] = {0x03, 0, 0, 0, 0, 0};
    ASSERT_EQ_INT(phase_after_cdb6(scsi, inquiry), scsi_status);
    ASSERT_EQ_INT(phase_after_cdb6(scsi, mode_sense), scsi_status);
    ASSERT_EQ_INT(phase_after_cdb6(scsi, request_sense), scsi_status);

    // READ TOC (0x43), READ SUB-CHANNEL (0x42), READ HEADER (0x44) and the
    // Sony READ TOC (0xC1) all carry theirs in bytes 7-8.
    const uint8_t read_toc[10] = {0x43, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t read_subch[10] = {0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t read_header[10] = {0x44, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t read_toc_sony[10] = {0xC1, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    ASSERT_EQ_INT(phase_after_cdb10(scsi, read_toc), scsi_status);
    ASSERT_EQ_INT(phase_after_cdb10(scsi, read_subch), scsi_status);
    ASSERT_EQ_INT(phase_after_cdb10(scsi, read_header), scsi_status);
    ASSERT_EQ_INT(phase_after_cdb10(scsi, read_toc_sony), scsi_status);
    scsi_delete(scsi);
}

// The ceiling must still work in both directions: a short allocation truncates,
// and an over-generous one gets the response's own length, not the ceiling.
// Without this, "return 0 always" would pass the test above.
TEST(allocation_length_is_a_ceiling_not_a_request) {
    scsi_t *scsi = attach_disc();

    uint8_t sense[18] = {0};
    request_sense(scsi, sense); // drain, so INQUIRY below is the live command

    // INQUIRY offering 5 bytes gets exactly 5 of the 36 available.
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    const uint8_t inq5[6] = {0x12, 0, 0, 0, 5, 0};
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, inq5[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_in);
    int n = 0;
    uint8_t b;
    while (scsi_pop_data_in_byte(scsi, &b))
        n++;
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);
    ASSERT_EQ_INT(n, 5);

    // Offering 255 gets the 36 the response actually holds, not 255.
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    const uint8_t inq255[6] = {0x12, 0, 0, 0, 255, 0};
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, inq255[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_in);
    n = 0;
    while (scsi_pop_data_in_byte(scsi, &b))
        n++;
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);
    ASSERT_EQ_INT(n, 36);
    scsi_delete(scsi);
}

// F-24: the two Apple vendor page $30 strings differ, and that is pinned here
// so it stays a decision rather than a drift.
//
// The emitter is shared; the content is not.  The hard-disk string is verified
// against HD SC Setup, which requests the page four times during a format.
// This one is verified by nothing -- no test in this tree requests it, counted
// at zero calls across se30-cdrom and iici-cdrom-boot -- so pinning the exact
// bytes is the most that can honestly be done: if someone changes it, they are
// changing something no other test covers, and this will say so.
TEST(apple_vendor_page_30_bytes_are_pinned) {
    scsi_t *scsi = attach_disc();

    // MODE SENSE(6), page $30, enough allocation for the whole page.
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    const uint8_t cdb[6] = {0x1A, 0x00, 0x30, 0x00, 0xFF, 0x00};
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_in);

    uint8_t resp[64];
    size_t n = 0;
    uint8_t b;
    while (scsi_pop_data_in_byte(scsi, &b))
        if (n < sizeof resp)
            resp[n++] = b;
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);

    // Find page $30 past the 4-byte mode header and the 8-byte block descriptor.
    size_t p = 4 + (size_t)resp[3];
    ASSERT_TRUE(p + 2 <= n);
    ASSERT_EQ_INT(resp[p], 0x30); // page code
    ASSERT_EQ_INT(resp[p + 1], 30); // page length: 30, not the HD's 20

    // 41 50 50 4C 45 20 43 4F 4D 50 55 54 45 52 2C 20 49 4E 43 20 20 20
    // -- "APPLE COMPUTER, INC" then THREE SPACES, and no trailing period.
    ASSERT_EQ_INT(memcmp(resp + p + 2, "APPLE COMPUTER, INC   ", 22), 0);
    // The string starts at p+2, so its 20th byte -- where the hard disk's
    // form carries a trailing '.' -- is p+21 here, and is a space.
    ASSERT_TRUE(resp[p + 21] == ' ');

    // 22 bytes of string inside a 30-byte page leaves 8 zeros.
    for (size_t i = 24; i < 32; i++)
        ASSERT_EQ_INT(resp[p + i], 0x00);
    scsi_delete(scsi);
}

// Drive a MODE SENSE(6) and return the response bytes.
static size_t mode_sense(scsi_t *scsi, int pc, uint8_t page, uint8_t *out, size_t max, int *phase_out) {
    const uint8_t cdb[6] = {0x1A, 0x00, (uint8_t)((pc << 6) | page), 0x00, 0xF0, 0x00};
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    *phase_out = scsi_get_bus_phase(scsi);
    size_t n = 0;
    if (*phase_out == scsi_data_in) {
        uint8_t b;
        while (scsi_pop_data_in_byte(scsi, &b))
            if (n < max)
                out[n++] = b;
        scsi_external_data_in_complete(scsi);
    }
    if (scsi_get_bus_phase(scsi) == scsi_status) {
        scsi_external_status_byte(scsi);
        scsi_external_message_byte(scsi);
    }
    scsi_external_release(scsi);
    return n;
}

// CDU-541 manual Table 5-47 lists this drive's MODE SENSE pages as 01h, 02h,
// 07h, 08h, 09h and 3Fh.  07h -- Verify Error Recovery Parameters -- was the
// one we did not build, so asking for it fell through to the unknown-page case
// and the host got GOOD status with no page at all.
TEST(mode_sense_page_07_is_returned) {
    scsi_t *scsi = attach_disc();
    uint8_t r[0xF0];
    int phase = 0;
    size_t n = mode_sense(scsi, 0, 0x07, r, sizeof(r), &phase);

    ASSERT_EQ_INT(phase, scsi_data_in);
    ASSERT_TRUE(n >= 12 + 8); // header + block descriptor + the page
    ASSERT_EQ_INT(r[3], 8); // block descriptor present
    ASSERT_EQ_INT(r[12], 0x07); // page code
    ASSERT_EQ_INT(r[13], 0x06); // page length
    scsi_delete(scsi);
}

// ...and it belongs in the "all pages" answer, in ascending order.  S5.2.3:
// "If the page code is 3Fh, all implemented pages are requested to be returned
// by the controller.  The pages are returned in ascending order."
TEST(mode_sense_all_pages_includes_07_in_order) {
    scsi_t *scsi = attach_disc();
    uint8_t r[0xF0];
    int phase = 0;
    size_t n = mode_sense(scsi, 0, 0x3F, r, sizeof(r), &phase);
    ASSERT_EQ_INT(phase, scsi_data_in);

    uint8_t seen[8];
    int count = 0;
    size_t i = 12; // past header + block descriptor
    size_t end = (size_t)r[0] + 1;
    ASSERT_TRUE(end <= n);
    while (i + 1 < end && count < 8) {
        seen[count++] = r[i];
        i += 2u + r[i + 1];
    }
    // Table 5-47's five, plus Apple's vendor page 30h, which is not Sony's and
    // so is not in that table -- but it IS implemented here, and "all
    // implemented pages" means all of them.  Ascending order puts it last.
    ASSERT_EQ_INT(count, 6);
    ASSERT_EQ_INT(seen[0], 0x01);
    ASSERT_EQ_INT(seen[1], 0x02);
    ASSERT_EQ_INT(seen[2], 0x07); // the one that was missing
    ASSERT_EQ_INT(seen[3], 0x08);
    ASSERT_EQ_INT(seen[4], 0x09);
    ASSERT_EQ_INT(seen[5], 0x30);
    scsi_delete(scsi);
}

// The whole "all pages" response must fit the buffer it is assembled in.
//
// 03-scsi F-41 proposed the stack buffer this now uses and put the response at
// "at most 96 bytes".  It was 94 when the review was written; adding page 07h
// (F-37) and counting page 30h's two-byte page header put it at 104.  A
// uint8_t resp[96] -- which is exactly what the hard disk's MODE SENSE next
// door declares, so it is the number a reader would copy -- would have
// overflowed the stack by eight bytes.
//
// So the length is pinned here.  If this fails, CD_MODE_SENSE_MAX in
// scsi_cdrom.c is the thing to change, not this number.
TEST(mode_sense_all_pages_is_exactly_the_buffer_size) {
    scsi_t *scsi = attach_disc();
    uint8_t r[0xF0];
    int phase = 0;
    size_t n = mode_sense(scsi, 0, 0x3F, r, sizeof(r), &phase);
    ASSERT_EQ_INT(phase, scsi_data_in);
    ASSERT_EQ_INT((int)n, 104);
    // Byte 0 is the mode data length, which excludes itself.
    ASSERT_EQ_INT(r[0], 103);
    scsi_delete(scsi);
}

// Two bytes of the mode parameter header are written by nobody -- medium type
// and the device-specific parameter -- and used to be zeroed only as a side
// effect of a 131072-byte memset over the transfer buffer.  A scratch buffer
// zeroes them instead; this checks they are still zero, and that the block
// descriptor length beside them is right.
TEST(mode_sense_header_reserved_bytes_are_zero) {
    scsi_t *scsi = attach_disc();
    uint8_t r[0xF0];
    int phase = 0;
    size_t n = mode_sense(scsi, 0, 0x01, r, sizeof(r), &phase);
    ASSERT_EQ_INT(phase, scsi_data_in);
    ASSERT_TRUE(n >= 12);
    ASSERT_EQ_INT(r[1], 0x00); // medium type
    ASSERT_EQ_INT(r[2], 0x00); // device-specific parameter
    ASSERT_EQ_INT(r[3], 8); // block descriptor length
    scsi_delete(scsi);
}

// A long response followed by a short one must not show any of the long one.
// The response is built in a buffer the transfer also uses, so "what is left
// over from last time" is a real question to ask of it.
TEST(mode_sense_does_not_leak_a_previous_response) {
    scsi_t *scsi = attach_disc();
    uint8_t big[0xF0], small[0xF0];
    int phase = 0;
    size_t nbig = mode_sense(scsi, 0, 0x3F, big, sizeof(big), &phase);
    ASSERT_EQ_INT((int)nbig, 104);

    memset(small, 0xCD, sizeof(small));
    size_t nsmall = mode_sense(scsi, 0, 0x08, small, sizeof(small), &phase);
    ASSERT_EQ_INT(phase, scsi_data_in);
    // Header + block descriptor + page 08h only.
    ASSERT_EQ_INT((int)nsmall, 4 + 8 + 12);
    ASSERT_EQ_INT(small[0], (int)nsmall - 1);
    ASSERT_EQ_INT(small[12], 0x08); // the page, immediately after the descriptor
    ASSERT_EQ_INT(small[13], 0x0A);
    for (size_t i = 14; i < nsmall; i++)
        ASSERT_EQ_INT(small[i], 0x00); // page 08h's body is all zeros
    scsi_delete(scsi);
}

// S5.2.3: "If the page code specified is not implemented the command will be
// terminated with a CHECK CONDITION status.  The sense key will be set to
// ILLEGAL REQUEST and the additional sense code set to ILLEGAL VALUE IN CDB."
//
// This used to answer GOOD with a header and no page, under a comment claiming
// that was "like real hardware".  A host told everything went well then parses
// whatever its own buffer held.
TEST(mode_sense_unimplemented_page_is_refused) {
    scsi_t *scsi = attach_disc();
    uint8_t r[0xF0];
    int phase = 0;
    (void)mode_sense(scsi, 0, 0x25, r, sizeof(r), &phase); // 0x25: not in Table 5-47

    ASSERT_TRUE(phase != scsi_data_in); // no data phase at all

    uint8_t sense[18] = {0};
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_ILLEGAL_REQUEST);
    ASSERT_EQ_INT(sense[12], ASC_INVALID_FIELD_IN_CDB);
    scsi_delete(scsi);
}

// S5.3.1.1, in prose because Table 5-33 is a scanned image: "The read retry
// count field specifies the number of times that the controller will attempt
// its read recovery algorithm.  The default value is zero."  We emitted 1, and
// scsi_cdrom.md said 3.
TEST(mode_sense_retry_counts_default_to_zero) {
    scsi_t *scsi = attach_disc();
    uint8_t r[0xF0];
    int phase = 0;

    size_t n = mode_sense(scsi, 0, 0x01, r, sizeof(r), &phase);
    ASSERT_TRUE(n >= 12 + 8);
    ASSERT_EQ_INT(r[12], 0x01);
    ASSERT_EQ_INT(r[15], 0x00); // read retry count

    n = mode_sense(scsi, 0, 0x07, r, sizeof(r), &phase);
    ASSERT_TRUE(n >= 12 + 8);
    ASSERT_EQ_INT(r[12], 0x07);
    ASSERT_EQ_INT(r[15], 0x00); // verify retry count -- "same as for read"
    scsi_delete(scsi);
}

// Drive a 10-byte CDB and collect the DATA IN bytes.
static size_t issue_cdb10(scsi_t *scsi, const uint8_t cdb[10], uint8_t *out, size_t max, int *phase_out) {
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 10; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    *phase_out = scsi_get_bus_phase(scsi);
    size_t n = 0;
    if (*phase_out == scsi_data_in) {
        uint8_t b;
        while (scsi_pop_data_in_byte(scsi, &b))
            if (n < max)
                out[n++] = b;
        scsi_external_data_in_complete(scsi);
    }
    if (scsi_get_bus_phase(scsi) == scsi_status) {
        scsi_external_status_byte(scsi);
        scsi_external_message_byte(scsi);
    }
    scsi_external_release(scsi);
    return n;
}

// READ TOC's MSF bit (byte 1 bit 1).  CDU-541 S5.2.24: "the format of the CD
// Address is determined by the MSF bit in the CDB"; X3.131-1994 Table 237 gives
// the field as reserved / M / S / F.  It used to be ignored, so a driver asking
// for MSF got an LBA and read it as minutes, seconds and frames.
//
// LBA 0 is 00:02:00 -- the Red Book two-second lead-in, 150 frames at 75 fps.
TEST(read_toc_honours_the_msf_bit) {
    scsi_t *scsi = attach_disc();
    uint8_t r[32];
    int phase = 0;

    const uint8_t lba_cdb[10] = {0x43, 0x00, 0, 0, 0, 0, 0x01, 0x00, 0x14, 0x00};
    size_t n = issue_cdb10(scsi, lba_cdb, r, sizeof(r), &phase);
    ASSERT_EQ_INT(phase, scsi_data_in);
    ASSERT_TRUE(n >= 20);
    ASSERT_EQ_INT(r[8], 0x00); // track 1 address, LBA 0
    ASSERT_EQ_INT(r[9], 0x00);
    ASSERT_EQ_INT(r[10], 0x00);
    ASSERT_EQ_INT(r[11], 0x00);

    const uint8_t msf_cdb[10] = {0x43, 0x02, 0, 0, 0, 0, 0x01, 0x00, 0x14, 0x00};
    n = issue_cdb10(scsi, msf_cdb, r, sizeof(r), &phase);
    ASSERT_EQ_INT(phase, scsi_data_in);
    ASSERT_TRUE(n >= 20);
    ASSERT_EQ_INT(r[8], 0x00); // reserved
    ASSERT_EQ_INT(r[9], 0x00); // M
    ASSERT_EQ_INT(r[10], 0x02); // S -- the 150-frame lead-in
    ASSERT_EQ_INT(r[11], 0x00); // F
    scsi_delete(scsi);
}

// AAh asks for the lead-out alone, so the descriptor list is one entry shorter
// -- and the TOC data length follows the REQUEST.  It does not follow the
// allocation length: X3.131-1994 Table 261 calls it the length "available to be
// transferred", which is how an initiator learns there is more to ask for.
TEST(read_toc_lead_out_only_and_the_length_is_what_is_available) {
    scsi_t *scsi = attach_disc();
    uint8_t r[32];
    int phase = 0;

    const uint8_t all_cdb[10] = {0x43, 0x00, 0, 0, 0, 0, 0x01, 0x00, 0x14, 0x00};
    size_t n = issue_cdb10(scsi, all_cdb, r, sizeof(r), &phase);
    ASSERT_TRUE(n >= 4);
    ASSERT_EQ_INT(r[1], 18); // header(2 past the length) + two 8-byte descriptors

    const uint8_t leadout_cdb[10] = {0x43, 0x00, 0, 0, 0, 0, 0xAA, 0x00, 0x14, 0x00};
    n = issue_cdb10(scsi, leadout_cdb, r, sizeof(r), &phase);
    ASSERT_EQ_INT(phase, scsi_data_in);
    ASSERT_EQ_INT((int)n, 12); // header + ONE descriptor
    ASSERT_EQ_INT(r[1], 10);
    ASSERT_EQ_INT(r[6], 0xAA); // and it is the lead-out

    // Truncating the transfer must NOT shrink the reported length.
    const uint8_t short_cdb[10] = {0x43, 0x00, 0, 0, 0, 0, 0x01, 0x00, 0x04, 0x00};
    n = issue_cdb10(scsi, short_cdb, r, sizeof(r), &phase);
    ASSERT_EQ_INT((int)n, 4);
    ASSERT_EQ_INT(r[1], 18); // still says 18 are available
    scsi_delete(scsi);
}

// A track this disc does not have is refused.  CDU-541 S5.2.24 and X3.131-1994
// S14.2.11 agree; they differ only on zero, which is taken as "from the first
// track" so that 43h and the Sony C1h handler answer alike.
TEST(read_toc_rejects_a_track_the_disc_does_not_have) {
    scsi_t *scsi = attach_disc();
    uint8_t r[32];
    int phase = 0;

    const uint8_t bad[10] = {0x43, 0x00, 0, 0, 0, 0, 0x09, 0x00, 0x14, 0x00};
    (void)issue_cdb10(scsi, bad, r, sizeof(r), &phase);
    ASSERT_TRUE(phase != scsi_data_in);
    uint8_t sense[18] = {0};
    request_sense(scsi, sense);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_ILLEGAL_REQUEST);
    ASSERT_EQ_INT(sense[12], ASC_INVALID_FIELD_IN_CDB);

    // Zero is accepted, and means track 1.
    const uint8_t zero[10] = {0x43, 0x00, 0, 0, 0, 0, 0x00, 0x00, 0x14, 0x00};
    size_t n = issue_cdb10(scsi, zero, r, sizeof(r), &phase);
    ASSERT_EQ_INT(phase, scsi_data_in);
    ASSERT_TRUE(n >= 20);
    ASSERT_EQ_INT(r[6], 0x01);
    scsi_delete(scsi);
}

// READ HEADER carries the same bit -- X3.131-1994 S14.1.5: "The READ HEADER,
// READ SUB-CHANNEL and READ TABLE OF CONTENTS commands have this feature."
TEST(read_header_honours_the_msf_bit) {
    scsi_t *scsi = attach_disc();
    uint8_t r[16];
    int phase = 0;

    // LBA 75 -> 00:03:00 once the 150-frame lead-in is added.
    const uint8_t msf_cdb[10] = {0x44, 0x02, 0, 0, 0, 75, 0, 0x00, 0x08, 0x00};
    size_t n = issue_cdb10(scsi, msf_cdb, r, sizeof(r), &phase);
    ASSERT_EQ_INT(phase, scsi_data_in);
    ASSERT_TRUE(n >= 8);
    ASSERT_EQ_INT(r[4], 0x00); // reserved
    ASSERT_EQ_INT(r[5], 0x00); // M
    ASSERT_EQ_INT(r[6], 0x03); // S
    ASSERT_EQ_INT(r[7], 0x00); // F

    const uint8_t lba_cdb[10] = {0x44, 0x00, 0, 0, 0, 75, 0, 0x00, 0x08, 0x00};
    n = issue_cdb10(scsi, lba_cdb, r, sizeof(r), &phase);
    ASSERT_TRUE(n >= 8);
    ASSERT_EQ_INT(r[7], 75); // plain LBA
    scsi_delete(scsi);
}

int main(void) {
    make_disc();
    RUN(read6_at_buf_limit);
    RUN(read6_over_buf_limit_cdrom);
    RUN(read6_max_blocks_cdrom);
    RUN(read6_past_end_still_refused);
    RUN(unit_attention_on_insert_reports_caddy_inserted);
    RUN(eject_reports_not_ready_not_media_arrived);
    RUN(empty_bay_keeps_failing_not_just_once);
    RUN(eject_is_exempt_from_pending_unit_attention);
    RUN(inquiry_does_not_clear_unit_attention);
    RUN(eject_while_prevented_reports_prevent_bit_set);
    RUN(allow_then_eject_succeeds);
    RUN(prevent_on_empty_drive_is_refused);
    RUN(allow_on_empty_drive_is_accepted);
    RUN(host_eject_honours_the_guest_lock);
    RUN(host_eject_return_codes_are_distinct);
    RUN(eject_leaves_no_locked_empty_drive);
    RUN(read_toc_honours_the_msf_bit);
    RUN(read_toc_lead_out_only_and_the_length_is_what_is_available);
    RUN(read_toc_rejects_a_track_the_disc_does_not_have);
    RUN(read_header_honours_the_msf_bit);
    RUN(mode_sense_page_07_is_returned);
    RUN(mode_sense_all_pages_includes_07_in_order);
    RUN(mode_sense_all_pages_is_exactly_the_buffer_size);
    RUN(mode_sense_header_reserved_bytes_are_zero);
    RUN(mode_sense_does_not_leak_a_previous_response);
    RUN(mode_sense_unimplemented_page_is_refused);
    RUN(mode_sense_retry_counts_default_to_zero);
    RUN(apple_vendor_page_30_bytes_are_pinned);
    RUN(zero_allocation_length_transfers_nothing);
    RUN(allocation_length_is_a_ceiling_not_a_request);
    RUN(inquiry_standard_is_36_bytes);
    RUN(inquiry_evpd_unsupported_page_is_refused);
    RUN(inquiry_evpd_page_zero_lists_itself);
    unlink(g_path);
    printf("[scsi_cdrom] all tests passed\n");
    return 0;
    return true;
}
