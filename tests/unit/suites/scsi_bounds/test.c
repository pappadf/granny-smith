// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// SCSI transfer-range bounds, for READ and WRITE on both CDB lengths.
//
// 03-scsi F-02: WRITE had no bounds check at all.  Its only guards were two
// assert()s in command_complete:
//
//     assert(scsi->cmd.tl * blk_sz == scsi->buf.size);
//     assert(((size_t)scsi->cmd.lba + scsi->cmd.tl) * blk_sz <= device_bytes);
//
// and -DNDEBUG removes assert from the release wasm profile (Makefile:131).
// In the build that ships, an out-of-range WRITE therefore fell through to
// disk_write_data, which drops the unbacked tail (image.c) -- and the SCSI
// layer then reported STATUS GOOD.  Silent data loss reported as success,
// which is the worst failure mode a disk has.  READ, meanwhile, had a real
// check and returned ILLEGAL REQUEST / LBA OUT OF RANGE.
//
// 03-scsi F-04: that READ check computed `(size_t)lba * blk_sz`, and size_t is
// 32 bits on wasm32, so a large LBA wrapped and the check passed on a range
// that is nowhere near the medium.
//
// These tests exist because neither is reachable from an integration row: the
// guest drivers in gs-test-data do not issue out-of-range commands, and the
// assert-based version "passes" every debug build by aborting.  They drive real
// CDBs through the real scsi.c against a real image, via the same
// external-initiator API the 53C96 bus master uses.

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
// scsi.c registers an object-model node and carries shell-facing helpers, so it
// references the wider emulator.  None of that is on the path these tests drive.

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

#define BLK    512u
#define BLOCKS 64u // a deliberately tiny 32 KB medium, so "past the end" is cheap
#define TARGET 1

static char g_path[] = "/tmp/gs-scsi-bounds-XXXXXX";

// A scratch disk whose every block is stamped with its own LBA, so a write can
// be checked for landing where it claimed to -- and for NOT landing when the
// command should have been refused.
static void make_disk(void) {
    int fd = mkstemp(g_path);
    ASSERT_TRUE(fd >= 0);
    uint8_t *blk = calloc(1, BLK);
    ASSERT_TRUE(blk != NULL);
    for (uint32_t lba = 0; lba < BLOCKS; lba++) {
        blk[0] = 0xA5;
        blk[1] = (uint8_t)(lba >> 8);
        blk[2] = (uint8_t)lba;
        ASSERT_TRUE(write(fd, blk, BLK) == (ssize_t)BLK);
    }
    free(blk);
    close(fd);
}

static scsi_t *attach_disk(void) {
    scsi_t *scsi = scsi_init(NULL);
    ASSERT_TRUE(scsi != NULL);
    image_t *img = image_create(g_path, NULL);
    ASSERT_TRUE(img != NULL);
    scsi_add_device(scsi, TARGET, "GS", "SCRATCH", "1.0", img, scsi_dev_hd, BLK, false);
    return scsi;
}

// Issue a CDB and, if the target went to DATA OUT, push `data_len` bytes.
// Returns the SCSI status byte.
static uint8_t issue(scsi_t *scsi, const uint8_t *cdb, int cdb_len, const uint8_t *data, size_t data_len) {
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < cdb_len; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);

    if (scsi_get_bus_phase(scsi) == scsi_data_out && data)
        for (size_t i = 0; i < data_len; i++)
            scsi_push_data_out_byte(scsi, data[i]);

    uint8_t status = 0xFF;
    if (scsi_get_bus_phase(scsi) == scsi_status) {
        status = scsi_external_status_byte(scsi);
        scsi_external_message_byte(scsi);
    }
    scsi_external_release(scsi);
    return status;
}

static uint8_t write6(scsi_t *scsi, uint32_t lba, uint8_t tl, const uint8_t *data) {
    const uint8_t cdb[6] = {0x0A, (uint8_t)((lba >> 16) & 0x1F), (uint8_t)(lba >> 8), (uint8_t)lba, tl, 0x00};
    return issue(scsi, cdb, 6, data, (size_t)(tl ? tl : 256) * BLK);
}

static uint8_t write10(scsi_t *scsi, uint32_t lba, uint16_t tl, const uint8_t *data) {
    const uint8_t cdb[10] = {0x2A,         0x00, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                             (uint8_t)lba, 0x00, (uint8_t)(tl >> 8),   (uint8_t)tl,          0x00};
    return issue(scsi, cdb, 10, data, (size_t)tl * BLK);
}

static uint8_t read10_status(scsi_t *scsi, uint32_t lba, uint16_t tl) {
    const uint8_t cdb[10] = {0x28,         0x00, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                             (uint8_t)lba, 0x00, (uint8_t)(tl >> 8),   (uint8_t)tl,          0x00};
    return issue(scsi, cdb, 10, NULL, 0);
}

static uint8_t read6_status(scsi_t *scsi, uint32_t lba, uint8_t tl) {
    const uint8_t cdb[6] = {0x08, (uint8_t)((lba >> 16) & 0x1F), (uint8_t)(lba >> 8), (uint8_t)lba, tl, 0x00};
    return issue(scsi, cdb, 6, NULL, 0);
}

// Read a block straight from the image, bypassing SCSI, to see what actually
// landed on the medium.
static void peek(scsi_t *scsi, uint32_t lba, uint8_t out[BLK]) {
    image_t *img = scsi_device_image(scsi, TARGET);
    ASSERT_TRUE(img != NULL);
    ASSERT_TRUE(disk_read_data(img, (size_t)lba * BLK, out, BLK) == BLK);
}

// An in-range WRITE must succeed and the bytes must land where it said.
TEST(test_write_in_range_lands) {
    scsi_t *scsi = attach_disk();
    uint8_t payload[BLK];
    memset(payload, 0x5C, sizeof payload);

    ASSERT_EQ_INT(write6(scsi, 3, 1, payload), STATUS_GOOD);

    uint8_t got[BLK];
    peek(scsi, 3, got);
    ASSERT_TRUE(memcmp(got, payload, BLK) == 0);
    scsi_delete(scsi);
}

// The whole point of F-02: a WRITE that runs past the end must be REFUSED, and
// the medium must be untouched.  Before the fix this returned STATUS GOOD and
// the in-range part of the transfer was written.
TEST(test_write_past_end_is_refused) {
    scsi_t *scsi = attach_disk();
    uint8_t payload[BLK * 4];
    memset(payload, 0x77, sizeof payload);

    // Last two blocks of the medium plus two that do not exist.
    ASSERT_EQ_INT(write6(scsi, BLOCKS - 2, 4, payload), STATUS_CHECK_CONDITION);

    // Nothing may have landed -- not even the part that was in range.
    uint8_t got[BLK];
    peek(scsi, BLOCKS - 2, got);
    ASSERT_EQ_INT(got[0], 0xA5); // still the stamp make_disk() wrote
    scsi_delete(scsi);
}

// A WRITE starting entirely beyond the medium is refused too.
TEST(test_write_beyond_end_is_refused) {
    scsi_t *scsi = attach_disk();
    uint8_t payload[BLK];
    memset(payload, 0x33, sizeof payload);
    ASSERT_EQ_INT(write6(scsi, BLOCKS + 10, 1, payload), STATUS_CHECK_CONDITION);
    scsi_delete(scsi);
}

// The 10-byte path is a separate decode with its own copy of everything, so it
// needs its own coverage -- it had no bounds check either.
TEST(test_write10_past_end_is_refused) {
    scsi_t *scsi = attach_disk();
    uint8_t payload[BLK * 2];
    memset(payload, 0x11, sizeof payload);

    ASSERT_EQ_INT(write10(scsi, 0, 1, payload), STATUS_GOOD); // control
    ASSERT_EQ_INT(write10(scsi, BLOCKS - 1, 2, payload), STATUS_CHECK_CONDITION);

    uint8_t got[BLK];
    peek(scsi, BLOCKS - 1, got);
    ASSERT_EQ_INT(got[0], 0xA5);
    scsi_delete(scsi);
}

// READ keeps its existing behaviour: in range succeeds, past the end is
// refused.  Pinned here so the shared range check cannot regress one direction
// while fixing the other.
TEST(test_read_range) {
    scsi_t *scsi = attach_disk();
    ASSERT_EQ_INT(read6_status(scsi, BLOCKS + 1, 1), STATUS_CHECK_CONDITION);
    ASSERT_EQ_INT(read6_status(scsi, BLOCKS - 1, 2), STATUS_CHECK_CONDITION);
    scsi_delete(scsi);
}

// F-04: the range arithmetic must not wrap.  On wasm32 `(size_t)lba * blk_sz`
// is 32-bit, and a READ(6) can reach lba 0x1FFFFF, which at 2048-byte blocks is
// 0xFFFFF800 -- one more block wraps to 0 and the check passes on a range
// nowhere near the medium.  Computed in uint64_t, every one of these is simply
// out of range.
TEST(test_large_lba_does_not_wrap) {
    scsi_t *scsi = attach_disk();
    // READ(6)'s LBA field is 21 bits, so only values that survive the CDB can
    // be tested here; 0x1FFFFF is the largest, and at this medium's 512-byte
    // blocks it is far past the end.  The wrap F-04 describes needs a CD-ROM's
    // 2048-byte blocks to reach 0xFFFFF800, which read10 covers below.
    const uint32_t lbas[] = {0x001FFFFFu, 0x00100000u, 0x0000FFFFu};
    for (unsigned i = 0; i < sizeof lbas / sizeof lbas[0]; i++) {
        uint8_t st = read6_status(scsi, lbas[i], 1);
        if (st != STATUS_CHECK_CONDITION)
            fprintf(stderr, "DIAG lba=%08X status=%u phase=%d\n", lbas[i], st, scsi_get_bus_phase(scsi));
        ASSERT_EQ_INT(st, STATUS_CHECK_CONDITION);
    }
    scsi_delete(scsi);
}

// The wrap F-04 actually describes needs READ(10)'s 32-bit LBA.  At 512-byte
// blocks, lba 0x00800000 scales to 0x1_0000_0000 -- exactly one past what a
// 32-bit size_t holds, so the old `(size_t)lba * blk_sz` truncated it to 0 and
// the bounds check passed on a range nowhere near a 32 KB medium.  On a 64-bit
// host the old code got this right by accident, which is why nothing caught it:
// the bug only existed in the build that ships.  Computed in uint64_t it is out
// of range on every host, which is what this pins.
TEST(test_read10_large_lba_does_not_wrap) {
    scsi_t *scsi = attach_disk();
    const uint32_t lbas[] = {
        0x00800000u, // * 512 == 2^32 exactly: truncates to 0 in a 32-bit size_t
        0x00800001u, // one block past
        0x01000000u, // * 512 == 2^33: truncates to 0 again
        0x80000000u, // CDB byte 2 == 0x80: `data[2] << 24` overflows a signed int
        0xFFFFFFFFu, // all ones: decodes to -1, which sign-extends if cast naively
    };
    for (unsigned i = 0; i < sizeof lbas / sizeof lbas[0]; i++)
        ASSERT_EQ_INT(read10_status(scsi, lbas[i], 1), STATUS_CHECK_CONDITION);
    scsi_delete(scsi);
}

// F-03: cmd_size() decodes the CDB length from the opcode's group code, and
// run_cmd() fires the instant the accumulated count matches.  Size a group
// wrong and the command dispatches early, leaving the unread tail of the CDB to
// be consumed by whatever phase comes next.  The old table was
// `opcode < 0x20 ? 6 : 10`, which gets group 5 (twelve-byte) wrong by two bytes
// and groups 3 and 4 wrong by four.
//
// Every opcode below is deliberately one we do NOT implement, so the outcome is
// always CHECK CONDITION / INVALID OPCODE and the phase after the final byte is
// STATUS.  That makes the phase itself the assertion: while the target is still
// short of a full CDB it must stay in COMMAND phase, and it must leave on the
// byte the group code says is the last one.  See the AUTHORITY comment on
// cmd_size() in scsi.c for where these lengths come from.
TEST(test_cdb_length_by_group_code) {
    static const struct {
        uint8_t opcode;
        int len;
    } cases[] = {
        {0x1F, 6 }, // group 0: six-byte              (ANSI X3.131-1986)
        {0x3F, 10}, // group 1: ten-byte              (ANSI X3.131-1986)
        {0x5F, 10}, // group 2: ten-byte              (SCSI-2 / 53C94 S2FE)
        {0x7F, 6 }, // group 3: reserved -> six-byte  (53C94)
        {0x9F, 6 }, // group 4: reserved -> six-byte  (53C94; 16-byte is SCSI-3)
        {0xBF, 12}, // group 5: twelve-byte           (ANSI X3.131-1986)
        {0xDF, 10}, // group 6: vendor, ten-byte      (Sony CDU-541)
        {0xFF, 10}, // group 7: vendor, ten-byte      (53C94)
    };

    scsi_t *scsi = attach_disk();
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ASSERT_TRUE(scsi_external_select(scsi, TARGET));
        scsi_push_data_out_byte(scsi, cases[i].opcode);

        // Every byte before the last must leave the target in COMMAND phase.
        for (int b = 1; b < cases[i].len; b++) {
            ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_command);
            scsi_push_data_out_byte(scsi, 0x00);
        }

        // ...and the last byte must complete the command, not a byte sooner or
        // later.  An unimplemented opcode is declined, so that means STATUS.
        ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_status);
        ASSERT_EQ_INT(scsi_external_status_byte(scsi), STATUS_CHECK_CONDITION);
        scsi_external_message_byte(scsi);
        scsi_external_release(scsi);
    }

    // The bus must be clean afterwards: if any of those CDBs had dispatched
    // early, its leftover bytes would have been swallowed by the next phase and
    // this TEST UNIT READY would not come back GOOD.  (TUR is the probe to use
    // here because it carries no data phase -- issue() only drains DATA OUT, so
    // a successful READ would park in DATA IN and never reach STATUS.)
    const uint8_t tur[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ASSERT_EQ_INT(issue(scsi, tur, 6, NULL, 0), STATUS_GOOD);
    scsi_delete(scsi);
}

// F-10: an allocation length of zero means zero, on the HD paths too.
//
// The HD MODE SENSE path in scsi.c always had this right and documented why;
// INQUIRY, a few lines above it, substituted 36.  Both now share
// scsi_data_in_alloc().  A zero-allocation command must land in STATUS with no
// data phase at all -- arming DATA IN with bytes the initiator never allocated
// for strands the bus, because every exit from DATA IN is guarded by
// buf.size == 0.
TEST(test_zero_allocation_length_transfers_nothing) {
    scsi_t *scsi = attach_disk();

    static const struct {
        const char *what;
        uint8_t cdb[6];
    } cases[] = {
        {"INQUIRY",             {0x12, 0, 0, 0, 0, 0}   },
        {"MODE SENSE page $3F", {0x1A, 0, 0x3F, 0, 0, 0}},
        {"MODE SENSE page $03", {0x1A, 0, 0x03, 0, 0, 0}},
        {"REQUEST SENSE",       {0x03, 0, 0, 0, 0, 0}   },
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ASSERT_TRUE(scsi_external_select(scsi, TARGET));
        for (int b = 0; b < 6; b++)
            scsi_push_data_out_byte(scsi, cases[i].cdb[b]);
        ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_status);
        scsi_external_status_byte(scsi);
        scsi_external_message_byte(scsi);
        scsi_external_release(scsi);
    }
    scsi_delete(scsi);
}

// F-13: a REJECTED read must not touch the staging buffer.
//
// phase_data_in calls scsi_buf_ensure, which reallocs the staging buffer to the
// full requested size -- and the buffer never shrinks.  With the phase change
// before the range check, a READ(10) of tl=0xFFFF against this 32 KB medium was
// correctly refused with CHECK CONDITION and still left a 32 MB buffer behind
// for the life of the machine (134 MB at a CD-ROM's 2048-byte blocks).  Measured
// before the fix: cap 131072 -> 33553920 from one refused CDB.
//
// So the assertion is on buf.cap, the thing that actually leaked, not just on
// the status byte -- the status was already right.
TEST(test_rejected_read_does_not_grow_buffer) {
    scsi_t *scsi = attach_disk();
    size_t cap_before = scsi->buf.cap;

    // READ(10), 65535 blocks: 32 MB, against a 32 KB medium.
    ASSERT_EQ_INT(read10_status(scsi, 0, 0xFFFF), STATUS_CHECK_CONDITION);
    ASSERT_TRUE(scsi->buf.cap == cap_before);

    // READ(6), 256 blocks (tl=0): 128 KB -- exactly BUF_LIMIT, so the medium
    // is the only thing that makes it invalid.  Pins the 6-byte path too.
    ASSERT_EQ_INT(read6_status(scsi, BLOCKS - 1, 0), STATUS_CHECK_CONDITION);
    ASSERT_TRUE(scsi->buf.cap == cap_before);

    // And the bus is in STATUS, not parked in DATA IN with stale bytes.
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_bus_free); // released by issue()
    scsi_delete(scsi);
}

// A VALID large read still grows the buffer -- the reorder must not have made
// the range check reject legitimate transfers bigger than BUF_LIMIT.  (Those
// are real: the Apple SCSI driver issues them writing the System file.)  This
// medium is only 32 KB, so the biggest valid read is the whole disk.
TEST(test_valid_read_still_transfers) {
    scsi_t *scsi = attach_disk();
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    const uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 0, 0, (uint8_t)(BLOCKS >> 8), (uint8_t)BLOCKS, 0};
    for (int i = 0; i < 10; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_in);
    size_t n = 0;
    uint8_t b;
    while (scsi_pop_data_in_byte(scsi, &b))
        n++;
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);
    ASSERT_TRUE(n == (size_t)BLOCKS * BLK);
    scsi_delete(scsi);
}

// MODE SENSE and READ CAPACITY describe the same drive, so they have to agree
// about how big its blocks are.  The HD MODE SENSE path used to divide by a
// literal 512 and report a literal 512 while READ CAPACITY asked the device, so
// a drive with any other block size would have been described two ways at once
// -- and a host that believes the wrong one addresses the wrong blocks.
//
// 512 is the only size an HD can hold today (every creation path passes it, and
// unlike the CD-ROM's, the HD's MODE SELECT discards the block descriptor), so
// this attaches one directly at 1024 to exercise what the literals hid.
TEST(mode_sense_and_read_capacity_agree_about_block_size) {
    const uint16_t odd_blk = 1024;
    scsi_t *scsi = scsi_init(NULL);
    ASSERT_TRUE(scsi != NULL);
    image_t *img = image_create(g_path, NULL);
    ASSERT_TRUE(img != NULL);
    scsi_add_device(scsi, TARGET, "GS", "SCRATCH", "1.0", img, scsi_dev_hd, odd_blk, false);

    // READ CAPACITY(10): last LBA then block length, both big-endian.
    const uint8_t cap_cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t cap[8];
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 10; i++)
        scsi_push_data_out_byte(scsi, cap_cdb[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_in);
    for (int i = 0; i < 8; i++)
        ASSERT_TRUE(scsi_pop_data_in_byte(scsi, &cap[i]));
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);

    uint32_t cap_blk = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) | ((uint32_t)cap[6] << 8) | cap[7];
    uint32_t cap_last = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) | ((uint32_t)cap[2] << 8) | cap[3];
    ASSERT_EQ_INT((int)cap_blk, (int)odd_blk);

    // MODE SENSE(6), all pages: header then an 8-byte block descriptor whose
    // last three bytes are the block length and whose first four carry the
    // count.
    const uint8_t ms_cdb[6] = {0x1A, 0x00, 0x3F, 0x00, 0x40, 0x00};
    uint8_t ms[0x40];
    size_t got = 0;
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, ms_cdb[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_in);
    while (got < sizeof(ms) && scsi_pop_data_in_byte(scsi, &ms[got]))
        got++;
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);

    ASSERT_TRUE(got >= 12);
    ASSERT_EQ_INT(ms[3], 8); // a block descriptor is present
    uint32_t ms_blk = ((uint32_t)ms[9] << 16) | ((uint32_t)ms[10] << 8) | ms[11];
    uint32_t ms_blocks = ((uint32_t)ms[5] << 16) | ((uint32_t)ms[6] << 8) | ms[7];

    // The two commands must describe the same geometry.
    ASSERT_EQ_INT((int)ms_blk, (int)odd_blk);
    ASSERT_EQ_INT((int)ms_blocks, (int)(cap_last + 1));

    // Page 3's bytes-per-physical-sector comes from the same source: these
    // images have no physical geometry distinct from their logical one.
    ASSERT_TRUE(got >= 12 + 14);
    uint32_t phys = ((uint32_t)ms[12 + 12] << 8) | ms[12 + 13];
    ASSERT_EQ_INT((int)phys, (int)odd_blk);

    scsi_delete(scsi);
}

// MODE SENSE page control.  PC=1 asks "which fields can I change?", and this
// path used to ignore the field entirely and answer with the CURRENT values --
// telling the host every field was modifiable.
//
// CDU-541 manual S5.2.3.2: "The page requested will be returned with the bits
// that are allowed to be changed set to one.  Parameters that are not
// changeable will be set to zero. ... The page descriptor ... will always be
// returned even if none of parameters are changeable within the page."
//
// Nothing in a hard disk's pages here is changeable, so the answer is the page
// headers with zero bodies -- present, not omitted.
static void mode_sense(scsi_t *scsi, int pc, uint8_t page, uint8_t *out, size_t *got) {
    const uint8_t cdb[6] = {0x1A, 0x00, (uint8_t)((pc << 6) | page), 0x00, 0x60, 0x00};
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_in);
    *got = 0;
    while (*got < 0x60 && scsi_pop_data_in_byte(scsi, &out[*got]))
        (*got)++;
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);
}

TEST(mode_sense_changeable_mask_reports_nothing_changeable) {
    scsi_t *scsi = attach_disk();
    uint8_t cur[0x60], chg[0x60];
    size_t n_cur = 0, n_chg = 0;

    mode_sense(scsi, 0, 0x3F, cur, &n_cur); // current values
    mode_sense(scsi, 1, 0x3F, chg, &n_chg); // changeable mask

    // Same shape: the pages are all still there.
    ASSERT_EQ_INT((int)n_chg, (int)n_cur);
    ASSERT_EQ_INT(chg[3], 8); // block descriptor still present

    // The current view carries real geometry...
    bool cur_has_values = false;
    for (size_t i = 12; i < n_cur; i++)
        if (cur[i] != 0)
            cur_has_values = true;
    ASSERT_TRUE(cur_has_values);

    // ...and the changeable view carries page headers and nothing else.  Each
    // page is <code><len> followed by len zero bytes.
    size_t end = (size_t)chg[0] + 1; // mode data length excludes itself
    ASSERT_TRUE(end <= n_chg);
    size_t i = 12; // past header + block descriptor
    int pages = 0;
    while (i + 1 < end) {
        uint8_t len = chg[i + 1];
        // The three pages this device emits: format, geometry, Apple ident.
        ASSERT_TRUE(chg[i] == 0x03 || chg[i] == 0x04 || chg[i] == 0x30);
        for (uint8_t k = 0; k < len; k++)
            ASSERT_EQ_INT(chg[i + 2 + k], 0); // body all zero: nothing changeable
        i += 2u + len;
        pages++;
    }
    ASSERT_EQ_INT(pages, 3);

    // The block size is not changeable on a hard disk either -- its MODE SELECT
    // discards the block descriptor -- so no bit of that field is set.
    ASSERT_EQ_INT(chg[9], 0);
    ASSERT_EQ_INT(chg[10], 0);
    ASSERT_EQ_INT(chg[11], 0);
    scsi_delete(scsi);
}

// PC=3 is answered, not refused: CDU-541 Table 5-5 maps page control "1 1" to
// Default Values.  On a drive with nothing changeable that is the same answer
// as PC=0 and PC=2.
TEST(mode_sense_saved_values_are_answered_like_defaults) {
    scsi_t *scsi = attach_disk();
    uint8_t a[0x60], b[0x60], c[0x60];
    size_t na = 0, nb = 0, nc = 0;

    mode_sense(scsi, 0, 0x3F, a, &na); // current
    mode_sense(scsi, 2, 0x3F, b, &nb); // default
    mode_sense(scsi, 3, 0x3F, c, &nc); // saved

    ASSERT_EQ_INT((int)nb, (int)na);
    ASSERT_EQ_INT((int)nc, (int)na);
    ASSERT_EQ_INT(memcmp(a, b, na), 0);
    ASSERT_EQ_INT(memcmp(a, c, na), 0);
    scsi_delete(scsi);
}

// ============================================================
// VERIFY, SEEK and FORMAT UNIT (F-39)
// ============================================================
// All three used to answer GOOD without decoding their CDB.  VERIFY is not a
// stub nobody reaches: Apple HD SC Setup 7.3.5 sweeps the whole disk with it
// after a format -- 677 of them in the Mac OS 7.6 install row -- so the bound
// has to be exactly right in both directions.

static uint8_t verify10(scsi_t *scsi, uint32_t lba, uint16_t len, bool bytchk, const uint8_t *data) {
    const uint8_t cdb[10] = {0x2F,
                             (uint8_t)(bytchk ? 0x02 : 0x00),
                             (uint8_t)(lba >> 24),
                             (uint8_t)(lba >> 16),
                             (uint8_t)(lba >> 8),
                             (uint8_t)lba,
                             0x00,
                             (uint8_t)(len >> 8),
                             (uint8_t)len,
                             0x00};
    return issue(scsi, cdb, 10, data, (size_t)len * BLK);
}

static uint8_t seek6(scsi_t *scsi, uint32_t lba) {
    const uint8_t cdb[6] = {0x0B, (uint8_t)((lba >> 16) & 0x1F), (uint8_t)(lba >> 8), (uint8_t)lba, 0x00, 0x00};
    return issue(scsi, cdb, 6, NULL, 0);
}

static uint8_t seek10(scsi_t *scsi, uint32_t lba) {
    const uint8_t cdb[10] = {
        0x2B, 0x00, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba, 0x00, 0x00,
        0x00, 0x00};
    return issue(scsi, cdb, 10, NULL, 0);
}

// REQUEST SENSE, so a rejection can be checked for the code it reports and not
// just for being a rejection.  Fills key/asc from the extended sense data.
static void read_sense(scsi_t *scsi, uint8_t *key, uint8_t *asc) {
    const uint8_t cdb[6] = {0x03, 0x00, 0x00, 0x00, 0x12, 0x00};
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_in);
    uint8_t sense[18];
    size_t got = 0;
    while (got < sizeof sense && scsi_pop_data_in_byte(scsi, &sense[got]))
        got++;
    scsi_external_data_in_complete(scsi);
    scsi_external_release(scsi);
    ASSERT_TRUE(got >= 13);
    *key = sense[2] & 0x0F;
    *asc = sense[12];
}

// The shape of HD SC Setup's sweep: the final chunk ends on the medium's LAST
// block, so `lba + len == capacity` has to be ACCEPTED.  If this is ever
// tightened by one the Mac OS 7.6 install stops formatting its disk.
TEST(verify_up_to_the_final_block_is_accepted) {
    scsi_t *scsi = attach_disk();
    ASSERT_EQ_INT(verify10(scsi, 0, 32, false, NULL), STATUS_GOOD);
    ASSERT_EQ_INT(verify10(scsi, 32, BLOCKS - 32, false, NULL), STATUS_GOOD);
    ASSERT_EQ_INT(verify10(scsi, BLOCKS - 1, 1, false, NULL), STATUS_GOOD);
    scsi_delete(scsi);
}

// The 10-byte decode shifts byte 2 left by 24, and byte 2 promotes to `int`:
// for anything >= 0x80 that overflows, which is undefined behaviour rather
// than merely implementation-defined (C11 6.5.7p4).  READ(10) has had a test
// for this since F-04 (test_read10_large_lba_does_not_wrap); VERIFY got a
// verbatim copy of the same decode in F-39 and no test with a high LBA, so
// nothing reached it -- UBSan is only as good as the path a test takes
// (03-scsi F-48).
//
// The refusal is the easy half; the point is that getting here is defined.
TEST(verify_with_a_high_lba_is_refused_not_wrapped) {
    scsi_t *scsi = attach_disk();
    // 0x80000000 blocks x 512 is 2^40 bytes, nowhere near the 32 KB medium --
    // but as a signed int the decode made it negative, and a negative LBA
    // sign-extends to something that passes any bound.
    ASSERT_EQ_INT(verify10(scsi, 0x80000000u, 1, false, NULL), STATUS_CHECK_CONDITION);
    uint8_t key = 0, asc = 0;
    read_sense(scsi, &key, &asc);
    ASSERT_EQ_INT(key, SENSE_ILLEGAL_REQUEST);
    ASSERT_EQ_INT(asc, ASC_LBA_OUT_OF_RANGE);

    // Every byte set: the CDB that used to land as -1.
    ASSERT_EQ_INT(verify10(scsi, 0xFFFFFFFFu, 0xFFFF, false, NULL), STATUS_CHECK_CONDITION);
    scsi_delete(scsi);
}

// ...and one block further is not.  X3.131-1994 S9.1.2; the CDU-541 manual
// S5.2.15 names the code, which its table 5-49 numbers 21h.
TEST(verify_past_the_end_is_refused) {
    scsi_t *scsi = attach_disk();
    ASSERT_EQ_INT(verify10(scsi, BLOCKS - 1, 2, false, NULL), STATUS_CHECK_CONDITION);
    uint8_t key = 0, asc = 0;
    read_sense(scsi, &key, &asc);
    ASSERT_EQ_INT(key, SENSE_ILLEGAL_REQUEST);
    ASSERT_EQ_INT(asc, ASC_LBA_OUT_OF_RANGE);

    ASSERT_EQ_INT(verify10(scsi, BLOCKS + 100, 1, false, NULL), STATUS_CHECK_CONDITION);
    scsi_delete(scsi);
}

// "A transfer length of zero indicates that no logical blocks shall be
// verified.  This condition shall not be considered as an error" -- and that
// holds even for an address the medium does not have, because the length is
// answered before the bound, exactly as READ(10) treats its own.
TEST(verify_of_zero_blocks_is_not_an_error) {
    scsi_t *scsi = attach_disk();
    ASSERT_EQ_INT(verify10(scsi, 0, 0, false, NULL), STATUS_GOOD);
    ASSERT_EQ_INT(verify10(scsi, BLOCKS + 100, 0, false, NULL), STATUS_GOOD);
    scsi_delete(scsi);
}

// BytChk set: the initiator sends the blocks and the target compares them
// against the medium.  Matching data is GOOD...
TEST(verify_bytchk_compares_against_the_medium) {
    scsi_t *scsi = attach_disk();
    // What make_disk() stamped into blocks 5 and 6.
    uint8_t want[BLK * 2];
    memset(want, 0, sizeof want);
    for (uint32_t i = 0; i < 2; i++) {
        want[i * BLK + 0] = 0xA5;
        want[i * BLK + 1] = (uint8_t)((5 + i) >> 8);
        want[i * BLK + 2] = (uint8_t)(5 + i);
    }
    ASSERT_EQ_INT(verify10(scsi, 5, 2, true, want), STATUS_GOOD);
    scsi_delete(scsi);
}

// ...and a single wrong byte is a MISCOMPARE, which is the one answer a verify
// can give that a read cannot.  Sense key 0Eh (X3.131-1994 table 69), ASC 1Dh
// (table 71).
TEST(verify_bytchk_reports_a_miscompare) {
    scsi_t *scsi = attach_disk();
    uint8_t wrong[BLK];
    memset(wrong, 0, sizeof wrong);
    wrong[0] = 0xA5;
    wrong[1] = 0x00;
    wrong[2] = 0x07;
    wrong[400] = 0x01; // the medium has 0x00 here

    ASSERT_EQ_INT(verify10(scsi, 7, 1, true, wrong), STATUS_CHECK_CONDITION);
    uint8_t key = 0, asc = 0;
    read_sense(scsi, &key, &asc);
    ASSERT_EQ_INT(key, SENSE_MISCOMPARE);
    ASSERT_EQ_INT(asc, ASC_MISCOMPARE_VERIFY);
    scsi_delete(scsi);
}

// A BytChk verify has to ASK for the data.  Before this, byte 1 was never
// read, so the target went straight to STATUS and the blocks the initiator was
// holding had nowhere to go.
TEST(verify_bytchk_enters_data_out_and_plain_verify_does_not) {
    scsi_t *scsi = attach_disk();

    const uint8_t with[10] = {0x2F, 0x02, 0, 0, 0, 0, 0, 0x00, 0x01, 0};
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 10; i++)
        scsi_push_data_out_byte(scsi, with[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_out);
    scsi_external_release(scsi);

    const uint8_t without[10] = {0x2F, 0x00, 0, 0, 0, 0, 0, 0x00, 0x01, 0};
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 10; i++)
        scsi_push_data_out_byte(scsi, without[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_status);
    scsi_external_status_byte(scsi);
    scsi_external_message_byte(scsi);
    scsi_external_release(scsi);

    scsi_delete(scsi);
}

// SEEK names an address and moves nothing, so it is bounded as ONE block: the
// last block is reachable, one past it is not.  A zero-length range check
// would have let `BLOCKS` through, because `off + 0 > raw_size` is false at
// exactly the end.  CDU-541 manual S5.2.30.
TEST(seek_is_bounded_at_the_last_block) {
    scsi_t *scsi = attach_disk();
    ASSERT_EQ_INT(seek6(scsi, 0), STATUS_GOOD);
    ASSERT_EQ_INT(seek6(scsi, BLOCKS - 1), STATUS_GOOD);
    ASSERT_EQ_INT(seek6(scsi, BLOCKS), STATUS_CHECK_CONDITION);
    uint8_t key = 0, asc = 0;
    read_sense(scsi, &key, &asc);
    ASSERT_EQ_INT(key, SENSE_ILLEGAL_REQUEST);
    ASSERT_EQ_INT(asc, ASC_LBA_OUT_OF_RANGE);

    ASSERT_EQ_INT(seek10(scsi, BLOCKS - 1), STATUS_GOOD);
    ASSERT_EQ_INT(seek10(scsi, BLOCKS), STATUS_CHECK_CONDITION);
    scsi_delete(scsi);
}

// SEEK(6) packs its address into bytes 1-3 and SEEK(10) into bytes 2-5, so the
// two decodes have to agree about which block they mean.
TEST(both_seek_cdbs_decode_the_same_address) {
    scsi_t *scsi = attach_disk();
    ASSERT_EQ_INT(seek6(scsi, BLOCKS - 1), seek10(scsi, BLOCKS - 1));
    ASSERT_EQ_INT(seek6(scsi, BLOCKS + 1), seek10(scsi, BLOCKS + 1));
    scsi_delete(scsi);
}

// FORMAT UNIT with FmtData clear -- the mandatory form, and the only one
// anything in the corpus sends -- takes no data phase at all.
TEST(format_unit_without_fmtdata_takes_no_data_phase) {
    scsi_t *scsi = attach_disk();
    // Byte for byte what Apple HD SC Setup issues.
    const uint8_t cdb[6] = {0x04, 0x00, 0x00, 0x00, 0x01, 0x00};
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_status);
    ASSERT_EQ_INT(scsi_external_status_byte(scsi), STATUS_GOOD);
    scsi_external_message_byte(scsi);
    scsi_external_release(scsi);
    scsi_delete(scsi);
}

// With FmtData set the target must ask for the four-byte defect list header,
// and then -- inside the same DATA OUT phase, which is what a real one does --
// keep asking for as many descriptor bytes as the header declared.
TEST(format_unit_with_fmtdata_takes_the_header_then_the_list) {
    scsi_t *scsi = attach_disk();
    const uint8_t cdb[6] = {0x04, 0x10, 0x00, 0x00, 0x00, 0x00}; // FmtData
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_out);

    // Header: two reserved bytes, then a length of 8 -- two block-format
    // defect descriptors (X3.131-1986 table 8-5).
    const uint8_t header[4] = {0x00, 0x00, 0x00, 0x08};
    for (int i = 0; i < 4; i++)
        scsi_push_data_out_byte(scsi, header[i]);
    // Still asking: the descriptors have not arrived.
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_out);

    const uint8_t descriptors[8] = {0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x2A};
    for (int i = 0; i < 8; i++)
        scsi_push_data_out_byte(scsi, descriptors[i]);

    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_status);
    ASSERT_EQ_INT(scsi_external_status_byte(scsi), STATUS_GOOD);
    scsi_external_message_byte(scsi);
    scsi_external_release(scsi);
    scsi_delete(scsi);
}

// A zero-length defect list is the header and nothing else -- the form
// X3.131-1994 table 112 marks mandatory once FmtData is set.
TEST(format_unit_with_an_empty_defect_list_ends_at_the_header) {
    scsi_t *scsi = attach_disk();
    const uint8_t cdb[6] = {0x04, 0x18, 0x00, 0x00, 0x00, 0x00}; // FmtData + CmpLst
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 6; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_data_out);

    const uint8_t header[4] = {0x00, 0x00, 0x00, 0x00};
    for (int i = 0; i < 4; i++)
        scsi_push_data_out_byte(scsi, header[i]);

    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_status);
    ASSERT_EQ_INT(scsi_external_status_byte(scsi), STATUS_GOOD);
    scsi_external_message_byte(scsi);
    scsi_external_release(scsi);
    scsi_delete(scsi);
}

// ============================================================
// Empty and sub-block media (F-40)
// ============================================================
// READ CAPACITY reports the address of the LAST block, so a block count has to
// lose one -- and an unsigned zero that loses one is 0xFFFFFFFF.  Two routes
// reached that, and only one of them is an empty drive.

static void read_capacity(scsi_t *scsi, uint8_t out[8], uint8_t *status) {
    const uint8_t cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < 10; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    *status = 0xFF;
    if (scsi_get_bus_phase(scsi) == scsi_data_in) {
        size_t got = 0;
        while (got < 8 && scsi_pop_data_in_byte(scsi, &out[got]))
            got++;
        scsi_external_data_in_complete(scsi);
    }
    if (scsi_get_bus_phase(scsi) == scsi_status) {
        *status = scsi_external_status_byte(scsi);
        scsi_external_message_byte(scsi);
    }
    scsi_external_release(scsi);
}

static uint32_t be32_of(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// The medium is there: the last block is the last block.
TEST(read_capacity_reports_the_last_block) {
    scsi_t *scsi = attach_disk();
    uint8_t cap[8] = {0};
    uint8_t status = 0xFF;
    read_capacity(scsi, cap, &status);
    ASSERT_EQ_INT(status, STATUS_GOOD);
    ASSERT_EQ_INT(be32_of(cap), BLOCKS - 1);
    ASSERT_EQ_INT(be32_of(cap + 4), BLK);
    scsi_delete(scsi);
}

// Route one: a HARD DISK whose image was detached.  The medium gate used to
// test `type == scsi_dev_cdrom`, so every other device type walked past it and
// READ CAPACITY answered GOOD with 0xFFFFFFFF -- four billion blocks.
// scsi.devices[N].eject() takes any ID, which is how a disk gets here.
TEST(an_emptied_hard_disk_is_not_ready) {
    scsi_t *scsi = attach_disk();
    ASSERT_EQ_INT(scsi_eject_device(scsi, TARGET), 1);

    uint8_t cap[8] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    uint8_t status = 0xFF;
    read_capacity(scsi, cap, &status);
    ASSERT_EQ_INT(status, STATUS_CHECK_CONDITION);
    ASSERT_EQ_INT(be32_of(cap), 0xEEEEEEEEu); // nothing was transferred at all

    // A hard disk speaks the standard code, not the Sony CD-ROM's vendor one:
    // X3.131-1994 table 71 lists 3Ah MEDIUM NOT PRESENT for "DTL WRSOM".
    uint8_t key = 0, asc = 0;
    read_sense(scsi, &key, &asc);
    ASSERT_EQ_INT(key, SENSE_NOT_READY);
    ASSERT_EQ_INT(asc, ASC_MEDIUM_NOT_PRESENT);
    scsi_delete(scsi);
}

// ...and the question the command exists to answer must answer it.  This used
// to report GOOD: asked "are you ready?", a drive with no medium said yes.
TEST(test_unit_ready_says_no_when_there_is_no_medium) {
    scsi_t *scsi = attach_disk();
    const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
    ASSERT_EQ_INT(issue(scsi, tur, 6, NULL, 0), STATUS_GOOD);

    ASSERT_EQ_INT(scsi_eject_device(scsi, TARGET), 1);
    ASSERT_EQ_INT(issue(scsi, tur, 6, NULL, 0), STATUS_CHECK_CONDITION);
    uint8_t key = 0, asc = 0;
    read_sense(scsi, &key, &asc);
    ASSERT_EQ_INT(key, SENSE_NOT_READY);
    ASSERT_EQ_INT(asc, ASC_MEDIUM_NOT_PRESENT);
    scsi_delete(scsi);
}

// A CD-ROM keeps its own vocabulary.  The drive we advertise is a SONY
// CDU-8002, whose NOT READY table has no 3Ah in it at all -- an empty bay is
// the vendor code B0h, which is what Apple's CD-ROM driver expects.
TEST(an_empty_cd_bay_still_speaks_sony) {
    scsi_t *scsi = scsi_init(NULL);
    ASSERT_TRUE(scsi != NULL);
    scsi_add_device(scsi, TARGET, "SONY", "CD-ROM CDU-8002", "1.8g", NULL, scsi_dev_cdrom, 2048, true);

    const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
    ASSERT_EQ_INT(issue(scsi, tur, 6, NULL, 0), STATUS_CHECK_CONDITION);
    uint8_t key = 0, asc = 0;
    read_sense(scsi, &key, &asc);
    ASSERT_EQ_INT(key, SENSE_NOT_READY);
    ASSERT_EQ_INT(asc, ASC_SONY_CADDY_NOT_INSERTED);
    scsi_delete(scsi);
}

// Route two, which no medium gate can ever catch: the medium is PRESENT and
// smaller than one block.  1536 / 2048 == 0, and a 1536-byte file really does
// attach as a CD-ROM -- so the subtraction has to be guarded where it happens,
// not only upstream of it.
TEST(a_medium_smaller_than_a_block_does_not_underflow) {
    char tiny[] = "/tmp/gs-subblock-XXXXXX";
    int fd = mkstemp(tiny);
    ASSERT_TRUE(fd >= 0);
    uint8_t pad[1536];
    memset(pad, 0, sizeof pad);
    ASSERT_TRUE(write(fd, pad, sizeof pad) == (ssize_t)sizeof pad);
    close(fd);

    scsi_t *scsi = scsi_init(NULL);
    ASSERT_TRUE(scsi != NULL);
    image_t *img = image_create(tiny, NULL);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ_INT((int)disk_size(img), 1536);
    scsi_add_device(scsi, TARGET, "SONY", "CD-ROM CDU-8002", "1.8g", img, scsi_dev_cdrom, 2048, true);

    // Clear the insertion UNIT ATTENTION the attach raised.
    uint8_t key = 0, asc = 0;
    read_sense(scsi, &key, &asc);
    ASSERT_EQ_INT(key, SENSE_UNIT_ATTENTION);

    uint8_t cap[8] = {0};
    uint8_t status = 0xFF;
    read_capacity(scsi, cap, &status);
    ASSERT_EQ_INT(status, STATUS_GOOD);
    ASSERT_EQ_INT(be32_of(cap), 0u); // NOT 0xFFFFFFFF
    ASSERT_EQ_INT(be32_of(cap + 4), 2048);
    scsi_delete(scsi);
    unlink(tiny);
}

int main(void) {
    make_disk();
    RUN(test_write_in_range_lands);
    RUN(mode_sense_and_read_capacity_agree_about_block_size);
    RUN(mode_sense_changeable_mask_reports_nothing_changeable);
    RUN(mode_sense_saved_values_are_answered_like_defaults);
    RUN(test_write_past_end_is_refused);
    RUN(test_write_beyond_end_is_refused);
    RUN(test_write10_past_end_is_refused);
    RUN(test_read_range);
    RUN(test_large_lba_does_not_wrap);
    RUN(test_read10_large_lba_does_not_wrap);
    RUN(test_cdb_length_by_group_code);
    RUN(test_zero_allocation_length_transfers_nothing);
    RUN(test_rejected_read_does_not_grow_buffer);
    RUN(test_valid_read_still_transfers);
    RUN(verify_up_to_the_final_block_is_accepted);
    RUN(verify_with_a_high_lba_is_refused_not_wrapped);
    RUN(verify_past_the_end_is_refused);
    RUN(verify_of_zero_blocks_is_not_an_error);
    RUN(verify_bytchk_compares_against_the_medium);
    RUN(verify_bytchk_reports_a_miscompare);
    RUN(verify_bytchk_enters_data_out_and_plain_verify_does_not);
    RUN(seek_is_bounded_at_the_last_block);
    RUN(both_seek_cdbs_decode_the_same_address);
    RUN(format_unit_without_fmtdata_takes_no_data_phase);
    RUN(format_unit_with_fmtdata_takes_the_header_then_the_list);
    RUN(format_unit_with_an_empty_defect_list_ends_at_the_header);
    RUN(read_capacity_reports_the_last_block);
    RUN(an_emptied_hard_disk_is_not_ready);
    RUN(test_unit_ready_says_no_when_there_is_no_medium);
    RUN(an_empty_cd_bay_still_speaks_sony);
    RUN(a_medium_smaller_than_a_block_does_not_underflow);
    unlink(g_path);
    printf("All scsi_bounds tests passed\n");
    return 0;
    return true;
}
