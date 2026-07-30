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
void add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id) {
    (void)config, (void)filename, (void)scsi_id;
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
    scsi_t *scsi = scsi_init(NULL, NULL);
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
TEST(read6_past_end_still_refused) {
    scsi_t *scsi = attach_disc();
    size_t n = read6(scsi, CD_BLOCKS - 4, 200, NULL);
    ASSERT_EQ_INT(0, (int)n);
    scsi_delete(scsi);
}

int main(void) {
    make_disc();
    RUN(read6_at_buf_limit);
    RUN(read6_over_buf_limit_cdrom);
    RUN(read6_max_blocks_cdrom);
    RUN(read6_past_end_still_refused);
    unlink(g_path);
    printf("[scsi_cdrom] all tests passed\n");
    return 0;
}
