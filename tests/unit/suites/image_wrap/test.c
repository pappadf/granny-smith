// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The volume wrapper's layout and sniffs.  See Makefile.

#include "image_apm.h"
#include "image_wrap.h"
#include "test_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- stubs for what image_wrap.c reaches outside the layout code -------------

const char *get_build_id(void) {
    return "test-build";
}

// image_wrap_volume's sniff reads through here; the tests call the pure
// layout and sniff functions directly, so no test reaches it.
size_t disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    (void)disk;
    (void)offset;
    memset(buf, 0, size);
    return size;
}

// --- helpers ---------------------------------------------------------------------

#define BLK 512u

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

// A buffer holding the prefix followed by the first blocks of a volume, so
// the APM parser sees a whole-disk head.
static uint8_t *build(uint64_t vol_blocks) {
    size_t n = (size_t)IMAGE_WRAP_PREFIX_BLOCKS * BLK;
    uint8_t *buf = (uint8_t *)calloc(1, n + 4 * BLK);
    ASSERT_TRUE(buf != NULL);
    image_wrap_build_prefix(buf, vol_blocks, "test-build");
    return buf;
}

// A three-block head of a bare HFS volume (boot blocks, then the MDB at 1024).
static void bare_head(uint8_t *head) {
    memset(head, 0, 3 * BLK);
    head[0] = 'L';
    head[1] = 'K';
    head[2 * BLK] = 'B';
    head[2 * BLK + 1] = 'D';
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Write map entry `i` (1-based) of an `n`-entry map into `head`.
static void map_entry(uint8_t *head, uint32_t i, uint32_t n, uint32_t start, uint32_t blocks, const char *type) {
    uint8_t *e = head + (size_t)i * BLK;
    memset(e, 0, BLK);
    e[0] = 'P';
    e[1] = 'M';
    wr32(e + 4, n);
    wr32(e + 8, start);
    wr32(e + 12, blocks);
    memcpy(e + 48, type, strlen(type));
}

// The head of a Disk Copy / SheepShaver disk: a DDM naming no drivers and a
// three-entry map — the map itself, Apple_HFS at 64, an Apple_Free tail.
// `head` holds DL_HEAD_BLOCKS blocks.
#define DL_HEAD_BLOCKS 64u
static void driverless_head(uint8_t *head) {
    memset(head, 0, DL_HEAD_BLOCKS * BLK);
    head[0] = 'E';
    head[1] = 'R';
    head[2] = 0x02; // sbBlkSize 512
    wr32(head + 4, 245760);
    map_entry(head, 1, 3, 1, 63, "Apple_partition_map");
    map_entry(head, 2, 3, 64, 245680, "Apple_HFS");
    map_entry(head, 3, 3, 245744, 16, "Apple_Free");
}

// --- tests -----------------------------------------------------------------------

// Block 0 is a Driver Descriptor Map naming one 68k driver at the driver
// partition, sized to cover the whole disk.
TEST(test_ddm_names_one_68k_driver) {
    const uint64_t vol = 51200; // a 25 MB volume
    uint8_t *buf = build(vol);
    ASSERT_EQ_INT(rd16(buf + 0), 0x4552); // 'ER'
    ASSERT_EQ_INT(rd16(buf + 2), 512);
    ASSERT_EQ_INT(rd32(buf + 4), (uint32_t)(IMAGE_WRAP_PREFIX_BLOCKS + vol));
    ASSERT_EQ_INT(rd16(buf + 16), 1); // sbDrvrCount
    ASSERT_EQ_INT(rd32(buf + 18), IMAGE_WRAP_DRIVER_START); // ddBlock
    ASSERT_TRUE(rd16(buf + 22) >= 1 && rd16(buf + 22) <= IMAGE_WRAP_DRIVER_BLOCKS); // ddSize
    ASSERT_EQ_INT(rd16(buf + 24), 1); // ddType: Mac OS 68k
    free(buf);
}

// The in-tree parser reads exactly three partitions: the map, the driver and
// the volume, with the volume starting right after the prefix.
TEST(test_partition_map_parses_to_three_entries) {
    const uint64_t vol = 20480;
    uint8_t *buf = build(vol);
    const char *err = NULL;
    apm_table_t *t = image_apm_parse_buffer(buf, (size_t)(IMAGE_WRAP_PREFIX_BLOCKS + 4) * BLK, &err);
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ_INT(t->n_partitions, 3);
    ASSERT_EQ_INT(t->partitions[0].fs_kind, APM_FS_PARTITION_MAP);
    ASSERT_EQ_INT(t->partitions[0].start_block, IMAGE_WRAP_MAP_START);
    ASSERT_EQ_INT(t->partitions[0].size_blocks, IMAGE_WRAP_MAP_BLOCKS);
    ASSERT_EQ_INT(t->partitions[1].fs_kind, APM_FS_DRIVER);
    ASSERT_TRUE(strcmp(t->partitions[1].type, "Apple_Driver") == 0);
    ASSERT_EQ_INT(t->partitions[1].start_block, IMAGE_WRAP_DRIVER_START);
    ASSERT_EQ_INT(t->partitions[2].fs_kind, APM_FS_HFS);
    ASSERT_EQ_INT(t->partitions[2].start_block, IMAGE_WRAP_PREFIX_BLOCKS);
    ASSERT_EQ_INT(t->partitions[2].size_blocks, vol);
    image_apm_free(t);
    free(buf);
}

// What the IIci-and-later SCSI boot code checks before running the driver:
// the entry is named "Maci…", starts at ddBlock, and pmBootCksum matches the
// checksum of pmBootSize bytes of the loaded driver.
TEST(test_driver_partition_passes_the_rom_checks) {
    uint8_t *buf = build(20480);
    const uint8_t *e = buf + 2 * BLK; // second map entry
    ASSERT_TRUE(memcmp(e + 16, "Maci", 4) == 0);
    ASSERT_EQ_INT(rd32(e + 8), rd32(buf + 18));
    uint32_t size = rd32(e + 96);
    ASSERT_TRUE(size > 0 && size <= (uint32_t)rd16(buf + 22) * BLK);
    const uint8_t *drv = buf + (size_t)IMAGE_WRAP_DRIVER_START * BLK;
    ASSERT_EQ_INT(rd32(e + 116), image_wrap_boot_checksum(drv, size));
    ASSERT_TRUE(memcmp(e + 120, "68000", 5) == 0);
    // The ROM JSRs to the first byte: a BRA over the DRVR header.
    ASSERT_EQ_INT(drv[0], 0x60);
    free(buf);
}

// The build id lands after the driver's stamp marker, and changes the
// checksum with it (the checksum is computed after stamping).
TEST(test_build_id_is_stamped_into_the_driver) {
    uint8_t *buf = build(20480);
    const uint8_t *drv = buf + (size_t)IMAGE_WRAP_DRIVER_START * BLK;
    size_t size = rd32(buf + 2 * BLK + 96);
    const uint8_t *hit = NULL;
    for (size_t i = 0; i + 13 < size && !hit; i++)
        if (memcmp(drv + i, "GSDisk build ", 13) == 0)
            hit = drv + i + 13;
    ASSERT_TRUE(hit != NULL);
    ASSERT_TRUE(strcmp((const char *)hit, "test-build") == 0);
    uint8_t *other = (uint8_t *)calloc(1, (size_t)IMAGE_WRAP_PREFIX_BLOCKS * BLK);
    ASSERT_TRUE(other != NULL);
    image_wrap_build_prefix(other, 20480, "another-build");
    ASSERT_TRUE(rd32(other + 2 * BLK + 116) != rd32(buf + 2 * BLK + 116));
    free(other);
    free(buf);
}

// The checksum: add each byte into a 16-bit sum, rotate left one; a zero
// result is reported as 0xFFFF.
TEST(test_boot_checksum_matches_the_rom_algorithm) {
    ASSERT_EQ_INT(image_wrap_boot_checksum(NULL, 0), 0xFFFF);
    const uint8_t one[] = {0x01};
    ASSERT_EQ_INT(image_wrap_boot_checksum(one, 1), 0x0002);
    const uint8_t two[] = {0x80, 0x00};
    // 0x80 -> rol -> 0x0100; +0 -> rol -> 0x0200
    ASSERT_EQ_INT(image_wrap_boot_checksum(two, 2), 0x0200);
    const uint8_t wrap[] = {0xFF, 0xFF};
    // 0xFF -> 0x01FE; +0xFF = 0x02FD -> 0x05FA
    ASSERT_EQ_INT(image_wrap_boot_checksum(wrap, 2), 0x05FA);
}

// Only a bare HFS/HFS+ volume is wrapped: not a partitioned disk, not a
// headerless blank, not something too short to hold an MDB.
TEST(test_bare_volume_sniff) {
    uint8_t head[3 * BLK];
    bare_head(head);
    ASSERT_TRUE(image_wrap_is_bare_volume(head, sizeof(head)));
    head[2 * BLK] = 'H';
    head[2 * BLK + 1] = '+';
    ASSERT_TRUE(image_wrap_is_bare_volume(head, sizeof(head))); // HFS+
    ASSERT_TRUE(!image_wrap_is_bare_volume(head, 2 * BLK)); // too short

    bare_head(head);
    head[0] = 'E';
    head[1] = 'R';
    ASSERT_TRUE(!image_wrap_is_bare_volume(head, sizeof(head))); // has a DDM

    bare_head(head);
    head[BLK] = 'P';
    head[BLK + 1] = 'M';
    ASSERT_TRUE(!image_wrap_is_bare_volume(head, sizeof(head))); // has a map

    memset(head, 0, sizeof(head));
    ASSERT_TRUE(!image_wrap_is_bare_volume(head, sizeof(head))); // blank

    // A wrapped prefix is itself partitioned: never wrapped twice.
    uint8_t *buf = build(20480);
    ASSERT_TRUE(!image_wrap_is_bare_volume(buf, 3 * BLK));
    free(buf);
}

// A partition map with no driver and one Apple_HFS partition is found, and
// the partition's extent is reported; with no DDM at all as well.
TEST(test_driverless_disk_is_found) {
    uint8_t head[DL_HEAD_BLOCKS * BLK];
    uint64_t start = 0, blocks = 0;
    driverless_head(head);
    ASSERT_TRUE(image_wrap_find_driverless_hfs(head, sizeof(head), &start, &blocks));
    ASSERT_EQ_INT(start, 64);
    ASSERT_EQ_INT(blocks, 245680);
    ASSERT_TRUE(!image_wrap_is_bare_volume(head, sizeof(head))); // the other shape

    memset(head, 0, BLK); // no DDM, the map alone
    start = blocks = 0;
    ASSERT_TRUE(image_wrap_find_driverless_hfs(head, sizeof(head), &start, &blocks));
    ASSERT_EQ_INT(start, 64);
}

// Anything that has a driver, more or less than one HFS partition, an A/UX
// partition, or a map the buffer cannot hold whole is left alone.
TEST(test_driverless_sniff_rejects) {
    uint8_t head[DL_HEAD_BLOCKS * BLK];
    uint64_t start = 0, blocks = 0;

    driverless_head(head);
    head[17] = 1; // the DDM names a driver
    ASSERT_TRUE(!image_wrap_find_driverless_hfs(head, sizeof(head), &start, &blocks));

    driverless_head(head);
    map_entry(head, 3, 3, 245744, 16, "Apple_Driver43"); // a driver partition
    ASSERT_TRUE(!image_wrap_find_driverless_hfs(head, sizeof(head), &start, &blocks));

    driverless_head(head);
    map_entry(head, 3, 3, 245744, 16, "Apple_HFS"); // two HFS partitions
    ASSERT_TRUE(!image_wrap_find_driverless_hfs(head, sizeof(head), &start, &blocks));

    driverless_head(head);
    map_entry(head, 2, 3, 64, 245680, "Apple_Free"); // no HFS partition
    ASSERT_TRUE(!image_wrap_find_driverless_hfs(head, sizeof(head), &start, &blocks));

    driverless_head(head);
    map_entry(head, 3, 3, 245744, 16, "Apple_UNIX_SVR2"); // A/UX reads the map
    ASSERT_TRUE(!image_wrap_find_driverless_hfs(head, sizeof(head), &start, &blocks));

    driverless_head(head);
    ASSERT_TRUE(!image_wrap_find_driverless_hfs(head, 3 * BLK, &start, &blocks)); // map cut short

    uint8_t bare[3 * BLK];
    bare_head(bare);
    ASSERT_TRUE(!image_wrap_find_driverless_hfs(bare, sizeof(bare), &start, &blocks)); // no map

    // The wrapper's own prefix has a driver: never wrapped twice.
    uint8_t *buf = build(20480);
    ASSERT_TRUE(!image_wrap_find_driverless_hfs(buf, (size_t)DL_HEAD_BLOCKS * BLK, &start, &blocks));
    free(buf);
}

int main(void) {
    RUN(test_ddm_names_one_68k_driver);
    RUN(test_partition_map_parses_to_three_entries);
    RUN(test_driver_partition_passes_the_rom_checks);
    RUN(test_build_id_is_stamped_into_the_driver);
    RUN(test_boot_checksum_matches_the_rom_algorithm);
    RUN(test_bare_volume_sniff);
    RUN(test_driverless_disk_is_found);
    RUN(test_driverless_sniff_rejects);
    fprintf(stderr, "All image_wrap tests passed\n");
    return 0;
}
