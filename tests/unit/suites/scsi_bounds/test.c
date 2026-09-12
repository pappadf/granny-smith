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
void add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id) {
    (void)config, (void)filename, (void)scsi_id;
}
// The bus-explicit forms, which `scsi.attach_hd` / `scsi.attach_cdrom` call
// so a machine with more than one visible SCSI bus (the Apple Network
// Servers' two fast/wide channels) can attach to the one it was asked for.
int system_hd_attach_on(struct scsi *bus, const char *path, int scsi_id) {
    (void)bus, (void)path, (void)scsi_id;
    return -1;
}
void add_scsi_cdrom_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
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
    scsi_t *scsi = scsi_init(NULL, NULL);
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

int main(void) {
    make_disk();
    RUN(test_write_in_range_lands);
    RUN(test_write_past_end_is_refused);
    RUN(test_write_beyond_end_is_refused);
    RUN(test_write10_past_end_is_refused);
    RUN(test_read_range);
    RUN(test_large_lba_does_not_wrap);
    RUN(test_read10_large_lba_does_not_wrap);
    unlink(g_path);
    printf("All scsi_bounds tests passed\n");
    return 0;
}
