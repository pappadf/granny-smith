// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// A READ whose backing store fails, in the release profile (see Makefile).
//
// The SCSI range check trusts the image's raw_size; the storage underneath
// refuses any block past the count it was opened with.  Growing raw_size past
// that makes a read the target considers in range fail in storage_read_block
// -- a genuine in-bounds backing-store failure, deterministically.  The
// target must answer CHECK CONDITION with MEDIUM ERROR / UNRECOVERED READ
// ERROR, never GOOD over stale buffer bytes.

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

// Link stubs: scsi.c carries shell-facing helpers that reference the wider
// emulator; none of them is on the path this test drives.
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
    return false;
}
int system_hd_attach_on(struct scsi *bus, const char *path, int scsi_id) {
    (void)bus, (void)path, (void)scsi_id;
    return -1;
}
bool add_scsi_cdrom_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
    (void)config, (void)bus, (void)filename, (void)scsi_id;
    return false;
}

#define BLK    512u
#define BLOCKS 16u
#define TARGET 1

#define STATUS_GOOD            0x00
#define STATUS_CHECK_CONDITION 0x02

static char g_path[] = "/tmp/gs-scsi-short-read-XXXXXX";

static scsi_t *attach_disk(void) {
    int fd = mkstemp(g_path);
    ASSERT_TRUE(fd >= 0);
    uint8_t blk[BLK];
    memset(blk, 0xA5, sizeof blk);
    for (uint32_t lba = 0; lba < BLOCKS; lba++)
        ASSERT_TRUE(write(fd, blk, BLK) == (ssize_t)BLK);
    close(fd);
    scsi_t *scsi = scsi_init(NULL);
    ASSERT_TRUE(scsi != NULL);
    image_t *img = image_create(g_path, NULL);
    ASSERT_TRUE(img != NULL);
    scsi_add_device(scsi, TARGET, "GS", "SCRATCH", "1.0", img, scsi_dev_hd, BLK, false);
    return scsi;
}

// Issue a CDB; collect up to `max` DATA IN bytes into `in`.  Returns the status.
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

// Claim twice the medium the store holds; LBAs past BLOCKS are then in range
// for SCSI and unreadable for storage.
static void overstate_medium(scsi_t *scsi) {
    image_t *img = scsi_device_image(scsi, TARGET);
    ASSERT_TRUE(img != NULL);
    img->raw_size = (size_t)BLOCKS * BLK * 2;
}

static void expect_medium_error(scsi_t *scsi) {
    const uint8_t cdb[6] = {0x03, 0x00, 0x00, 0x00, 18, 0x00}; // REQUEST SENSE
    uint8_t sense[18] = {0};
    ASSERT_EQ_INT(issue(scsi, cdb, 6, sense, sizeof sense), STATUS_GOOD);
    ASSERT_EQ_INT(sense[2] & 0x0F, SENSE_MEDIUM_ERROR);
    ASSERT_EQ_INT(sense[12], ASC_UNRECOVERED_READ_ERROR);
}

TEST(test_read10_backing_failure_is_a_medium_error) {
    scsi_t *scsi = attach_disk();
    overstate_medium(scsi);
    const uint8_t cdb[10] = {0x28, 0, 0, 0, 0, BLOCKS + 2, 0, 0, 1, 0}; // READ(10), one block
    ASSERT_EQ_INT(issue(scsi, cdb, 10, NULL, 0), STATUS_CHECK_CONDITION);
    expect_medium_error(scsi);
    unlink(g_path);
    strcpy(g_path, "/tmp/gs-scsi-short-read-XXXXXX");
}

TEST(test_read6_backing_failure_is_a_medium_error) {
    scsi_t *scsi = attach_disk();
    overstate_medium(scsi);
    const uint8_t cdb[6] = {0x08, 0, 0, BLOCKS + 3, 1, 0}; // READ(6), one block
    ASSERT_EQ_INT(issue(scsi, cdb, 6, NULL, 0), STATUS_CHECK_CONDITION);
    expect_medium_error(scsi);
    unlink(g_path);
    strcpy(g_path, "/tmp/gs-scsi-short-read-XXXXXX");
}

int main(void) {
    RUN(test_read10_backing_failure_is_a_medium_error);
    RUN(test_read6_backing_failure_is_a_medium_error);
    fprintf(stderr, "scsi_short_read: all tests passed\n");
    return 0;
}
