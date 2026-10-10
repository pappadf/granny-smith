// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// A disk whose backing image cannot serve some blocks it holds (see Makefile).
//
// The image is opened over a byte source that fails every read touching
// blocks [BAD_LO, BAD_HI) with -EIO -- what a corrupt UDIF chunk, a damaged
// archive member or a host I/O error gives the storage engine -- and serves
// a pattern everywhere else.  The guest must see a read error for those
// blocks (never an emulator halt, never silent zeros); an export or a
// consolidated checkpoint of the disk must fail rather than embed zeros.

#include "checkpoint.h"
#include "cpu.h"
#include "image.h"
#include "image_internal.h"
#include "machine_parts.h"
#include "memory.h"
#include "scsi.h"
#include "scsi_internal.h"
#include "source.h"
#include "status.h"
#include "storage.h"
#include "system.h"
#include "test_assert.h"
#include "via.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- Link stubs: scsi.c's shell-facing helpers reach the wider emulator ----

config_t *system_config(void) {
    return NULL;
}
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
int system_hd_attach(const char *path, int scsi_id) {
    (void)path, (void)scsi_id;
    return -1;
}
int system_hd_attach_on(struct scsi *bus, const char *path, int scsi_id) {
    (void)bus, (void)path, (void)scsi_id;
    return -1;
}
bool system_attach_scsi_cdrom(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
    (void)config, (void)bus, (void)filename, (void)scsi_id;
    return false;
}

// ---- An in-memory checkpoint stream (support/stub_checkpoint.c omitted) ----

struct checkpoint {
    uint8_t *buf;
    size_t len, pos, cap;
    bool error;
    checkpoint_kind_t kind;
};

void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)tag, (void)file, (void)line;
    if (cp->error || size > cp->len - cp->pos) {
        cp->error = true;
        memset(data, 0, size);
        return;
    }
    memcpy(data, cp->buf + cp->pos, size);
    cp->pos += size;
}

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)tag, (void)file, (void)line;
    if (cp->error)
        return;
    if (cp->len + size > cp->cap) {
        size_t cap = (cp->len + size) * 2;
        uint8_t *nb = realloc(cp->buf, cap);
        ASSERT_TRUE(nb != NULL);
        cp->buf = nb;
        cp->cap = cap;
    }
    memcpy(cp->buf + cp->len, data, size);
    cp->len += size;
}

bool checkpoint_has_error(checkpoint_t *cp) {
    return cp ? cp->error : true;
}
void checkpoint_set_error(checkpoint_t *cp) {
    if (cp)
        cp->error = true;
}
checkpoint_kind_t checkpoint_get_kind(checkpoint_t *cp) {
    return cp->kind;
}
void checkpoint_write_file_loc(checkpoint_t *cp, const char *path, const char *file, int line) {
    (void)cp, (void)path, (void)file, (void)line;
}
size_t checkpoint_read_file_loc(checkpoint_t *cp, uint8_t *dest, size_t capacity, char **out_path, const char *file,
                                int line) {
    (void)cp, (void)dest, (void)capacity, (void)file, (void)line;
    if (out_path)
        *out_path = NULL;
    return 0;
}
bool checkpoint_read_count(checkpoint_t *cp, uint32_t *out, uint32_t max, const char *what) {
    (void)what;
    system_read_checkpoint_data_loc(cp, out, sizeof *out, NULL, __FILE__, __LINE__);
    if (*out > max) {
        checkpoint_set_error(cp);
        *out = 0;
        return false;
    }
    return true;
}
void machine_part_begin(struct config *cfg, checkpoint_t *cp, const char *name) {
    (void)cfg, (void)cp, (void)name;
}
void machine_part_cancel(struct config *cfg) {
    (void)cfg;
}
void machine_part(struct config *cfg, checkpoint_t *cp, const char *name, machine_part_save_fn save, void *obj) {
    (void)cfg, (void)cp, (void)name, (void)save, (void)obj;
}

static void cp_free(checkpoint_t *cp) {
    free(cp->buf);
    memset(cp, 0, sizeof *cp);
}

// ---- The failing medium ------------------------------------------------------

#define BLK    512u
#define BLOCKS 64u
#define BAD_LO 40u
#define BAD_HI 42u
#define TARGET 1

#define STATUS_GOOD            0x00
#define STATUS_CHECK_CONDITION 0x02

static uint8_t g_medium[BLOCKS * BLK];

static int64_t failing_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    (void)s;
    if (off >= sizeof g_medium)
        return 0;
    if (len > sizeof g_medium - off)
        len = (size_t)(sizeof g_medium - off);
    if (len && off < (uint64_t)BAD_HI * BLK && off + len > (uint64_t)BAD_LO * BLK)
        return -EIO;
    memcpy(buf, g_medium + off, len);
    return (int64_t)len;
}
static uint64_t failing_size(gs_source_t *s) {
    (void)s;
    return sizeof g_medium;
}
static const char *failing_key(gs_source_t *s) {
    (void)s;
    return "test:failing-medium";
}
static gs_tier_t failing_tier(gs_source_t *s) {
    (void)s;
    return GS_TIER_RANDOM;
}
static void failing_close(gs_source_t *s) {
    (void)s;
}
static const gs_source_ops_t failing_ops = {failing_read, failing_size, failing_key, failing_tier, failing_close};

static image_t *open_failing_image(void) {
    for (size_t i = 0; i < sizeof g_medium; i++)
        g_medium[i] = (uint8_t)(i / BLK + i);
    gs_source_t *src = peel_source_new(&failing_ops, NULL, NULL);
    ASSERT_TRUE(src != NULL);
    image_t *img = image_open_readonly_source("failing.img", src, NULL);
    gs_source_release(src);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ_INT((int)disk_size(img), (int)sizeof g_medium);
    return img;
}

// ---- SCSI plumbing (as scsi_short_read drives it) ----------------------------

static uint8_t issue(scsi_t *scsi, const uint8_t *cdb, int cdb_len, uint8_t *in, size_t max) {
    ASSERT_TRUE(scsi_external_select(scsi, TARGET));
    for (int i = 0; i < cdb_len; i++)
        scsi_push_data_out_byte(scsi, cdb[i]);
    if (scsi_get_bus_phase(scsi) == scsi_data_in) {
        size_t n = 0;
        uint8_t b;
        while (scsi_pop_data_in_byte(scsi, &b)) {
            if (in && n < max)
                in[n] = b;
            n++;
        }
        scsi_external_data_in_complete(scsi);
    }
    uint8_t status = 0xFF;
    if (scsi_get_bus_phase(scsi) == scsi_status) {
        status = scsi_external_status_byte(scsi);
        scsi_external_message_byte(scsi);
    }
    scsi_external_release(scsi);
    return status;
}

static void expect_medium_error(scsi_t *scsi) {
    const uint8_t cdb[6] = {0x03, 0x00, 0x00, 0x00, 18, 0x00}; // REQUEST SENSE
    uint8_t sense[18] = {0};
    ASSERT_EQ_INT(issue(scsi, cdb, 6, sense, sizeof sense), STATUS_GOOD);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_MEDIUM_ERROR);
    ASSERT_EQ_INT(sense[12], ASC_UNRECOVERED_READ_ERROR);
}

static scsi_t *attach(image_t *img) {
    scsi_t *scsi = scsi_init_named(NULL, NULL, NULL, "scsi");
    ASSERT_TRUE(scsi != NULL);
    scsi_add_device(scsi, TARGET, "GS", "SCRATCH", "1.0", img, scsi_dev_hd, BLK, true);
    return scsi;
}

// ---- Tests -------------------------------------------------------------------

// The image layer: a failed read is a short count, not a halt, not zeros;
// the blocks around it still read.
TEST(test_disk_read_data_reports_the_failure) {
    image_t *img = open_failing_image();
    uint8_t buf[4 * BLK];
    memset(buf, 0xEE, sizeof buf);
    ASSERT_TRUE(disk_read_data(img, (size_t)BAD_LO * BLK, buf, BLK) != BLK);
    ASSERT_TRUE(disk_read_data(img, (size_t)(BAD_LO - 2) * BLK, buf, 4 * BLK) != 4 * BLK);
    ASSERT_EQ_INT((int)disk_read_data(img, (size_t)(BAD_LO - 1) * BLK, buf, BLK), (int)BLK);
    ASSERT_TRUE(memcmp(buf, g_medium + (BAD_LO - 1) * BLK, BLK) == 0);
    ASSERT_EQ_INT((int)disk_read_data(img, (size_t)BAD_HI * BLK, buf, BLK), (int)BLK);
    ASSERT_TRUE(memcmp(buf, g_medium + BAD_HI * BLK, BLK) == 0);
    ASSERT_EQ_INT((int)img->read_errors, 2);
    image_close(img);
}

// READ(10) of an unreadable block: CHECK CONDITION, MEDIUM ERROR /
// UNRECOVERED READ ERROR.  Its neighbour reads GOOD with its data.
TEST(test_scsi_read_of_unreadable_block_is_a_medium_error) {
    image_t *img = open_failing_image();
    scsi_t *scsi = attach(img);
    const uint8_t bad[10] = {0x28, 0, 0, 0, 0, BAD_LO, 0, 0, 1, 0};
    ASSERT_EQ_INT(issue(scsi, bad, 10, NULL, 0), STATUS_CHECK_CONDITION);
    expect_medium_error(scsi);
    const uint8_t span[6] = {0x08, 0, 0, BAD_LO - 1, 3, 0}; // READ(6) across the bad blocks
    ASSERT_EQ_INT(issue(scsi, span, 6, NULL, 0), STATUS_CHECK_CONDITION);
    expect_medium_error(scsi);
    uint8_t data[BLK];
    const uint8_t good[10] = {0x28, 0, 0, 0, 0, BAD_HI, 0, 0, 1, 0};
    ASSERT_EQ_INT(issue(scsi, good, 10, data, sizeof data), STATUS_GOOD);
    ASSERT_TRUE(memcmp(data, g_medium + BAD_HI * BLK, BLK) == 0);
    scsi_delete(scsi);
    image_close(img);
}

// VERIFY(10) without BytChk is a medium verification: over the unreadable
// blocks it is the same read error; over readable ones, GOOD.
TEST(test_scsi_verify_of_unreadable_block_is_a_medium_error) {
    image_t *img = open_failing_image();
    scsi_t *scsi = attach(img);
    const uint8_t bad[10] = {0x2F, 0, 0, 0, 0, BAD_LO - 4, 0, 0, 8, 0};
    ASSERT_EQ_INT(issue(scsi, bad, 10, NULL, 0), STATUS_CHECK_CONDITION);
    expect_medium_error(scsi);
    const uint8_t good[10] = {0x2F, 0, 0, 0, 0, 0, 0, 0, BAD_LO, 0};
    ASSERT_EQ_INT(issue(scsi, good, 10, NULL, 0), STATUS_GOOD);
    scsi_delete(scsi);
    image_close(img);
}

// An export that cannot read the base fails and leaves no file behind.
TEST(test_export_of_unreadable_disk_fails) {
    image_t *img = open_failing_image();
    char dest[] = "/tmp/gs-read-error-export-XXXXXX";
    int fd = mkstemp(dest);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    unlink(dest); // export refuses an existing file
    int rc = image_export_to(img, dest);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_TRUE(access(dest, F_OK) != 0);
    image_close(img);
}

// A consolidated checkpoint of the disk fails the checkpoint, so its .tmp is
// never renamed over a good one.
TEST(test_consolidated_checkpoint_of_unreadable_disk_fails) {
    image_t *img = open_failing_image();
    checkpoint_t cp = {.kind = CHECKPOINT_KIND_CONSOLIDATED};
    image_checkpoint(img, &cp);
    ASSERT_TRUE(checkpoint_has_error(&cp));
    cp_free(&cp);
    // A quick checkpoint carries no blocks, so it does not read the base.
    cp.kind = CHECKPOINT_KIND_QUICK;
    image_checkpoint(img, &cp);
    ASSERT_TRUE(!checkpoint_has_error(&cp));
    cp_free(&cp);
    image_close(img);
}

static void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

// A crafted consolidated snapshot whose block size is past
// STORAGE_MAX_BLOCK_SIZE, consumed by a restore with no disk to load it
// into: refused, not read into the discard buffer on the stack (ASan would
// report the overflow).
TEST(test_skipped_snapshot_with_oversized_block_is_refused) {
    const uint32_t bs = 64 * 1024;
    checkpoint_t cp = {.kind = CHECKPOINT_KIND_CONSOLIDATED};
    // The snapshot header (storage.c): le32 version (3), u8 has_data at 4,
    // le64 block count at 8, le32 block size at 16; 24 bytes.
    uint8_t h[24] = {0};
    put_le32(h, 3);
    h[4] = 1;
    h[8] = 1;
    put_le32(h + 16, bs);
    system_write_checkpoint_data(&cp, h, sizeof h);
    uint8_t *block = calloc(1, bs);
    ASSERT_TRUE(block != NULL);
    system_write_checkpoint_data(&cp, block, bs);
    free(block);
    status_t rc = storage_restore_from_checkpoint(NULL, &cp);
    ASSERT_EQ_INT(rc, STATUS_E_INVAL);
    cp_free(&cp);

    // A count past what storage_new accepts is refused the same way.
    h[8] = 0;
    h[12] = 1; // 2^32 blocks
    put_le32(h + 16, BLK);
    system_write_checkpoint_data(&cp, h, sizeof h);
    rc = storage_restore_from_checkpoint(NULL, &cp);
    ASSERT_EQ_INT(rc, STATUS_E_INVAL);
    cp_free(&cp);
}

int main(void) {
    RUN(test_disk_read_data_reports_the_failure);
    RUN(test_scsi_read_of_unreadable_block_is_a_medium_error);
    RUN(test_scsi_verify_of_unreadable_block_is_a_medium_error);
    RUN(test_export_of_unreadable_disk_fails);
    RUN(test_consolidated_checkpoint_of_unreadable_disk_fails);
    RUN(test_skipped_snapshot_with_oversized_block_is_refused);
    fprintf(stderr, "storage_read_error: all tests passed\n");
    return 0;
}
