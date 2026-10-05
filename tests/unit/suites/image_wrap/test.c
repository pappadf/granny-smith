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

// Set an HFS MDB's size fields: drNmAlBlks, drAlBlkSiz, drAlBlSt.
static void set_mdb(uint8_t *head, uint32_t nm, uint32_t size, uint32_t first) {
    uint8_t *m = head + 2 * BLK;
    m[18] = (uint8_t)(nm >> 8);
    m[19] = (uint8_t)nm;
    wr32(m + 20, size);
    m[28] = (uint8_t)(first >> 8);
    m[29] = (uint8_t)first;
}

// An in-memory image file for image_wrap_extended_blocks.
typedef struct {
    const uint8_t *data;
    uint64_t len;
} mem_file_t;

static bool mem_read(void *ctx, uint64_t offset, uint8_t *buf, size_t size) {
    const mem_file_t *f = (const mem_file_t *)ctx;
    if (offset > f->len || size > f->len - offset)
        return false;
    memcpy(buf, f->data + offset, size);
    return true;
}

// A volume's claimed length: HFS counts its allocation blocks plus the
// alternate MDB and the reserved block; HFS+ its totalBlocks.
TEST(test_volume_blocks) {
    uint8_t head[3 * BLK];
    bare_head(head);
    set_mdb(head, 50314, 1024, 16); // the trimmed archive.org Mac OS 8.1 volume
    ASSERT_EQ_INT(image_wrap_volume_blocks(head, sizeof(head)), 100646);
    set_mdb(head, 50314, 1000, 16); // not a whole number of sectors
    ASSERT_EQ_INT(image_wrap_volume_blocks(head, sizeof(head)), 0);
    set_mdb(head, 50314, 0, 16);
    ASSERT_EQ_INT(image_wrap_volume_blocks(head, sizeof(head)), 0);

    memset(head + 2 * BLK, 0, BLK);
    head[2 * BLK] = 'H';
    head[2 * BLK + 1] = '+';
    wr32(head + 2 * BLK + 40, 4096);
    wr32(head + 2 * BLK + 44, 1000);
    ASSERT_EQ_INT(image_wrap_volume_blocks(head, sizeof(head)), 8000);

    memset(head, 0, sizeof(head));
    ASSERT_EQ_INT(image_wrap_volume_blocks(head, sizeof(head)), 0); // no volume
    ASSERT_EQ_INT(image_wrap_volume_blocks(head, 2 * BLK), 0); // too short
}

// A bare volume trimmed short of its MDB's claim is opened at the claim; a
// whole one, an oversized claim, or a non-volume keeps the file's length.
TEST(test_extended_blocks_bare) {
    const uint64_t file_blocks = 1000;
    uint8_t *img = (uint8_t *)calloc(file_blocks, BLK);
    ASSERT_TRUE(img != NULL);
    mem_file_t f = {img, file_blocks * BLK};
    bare_head(img);
    set_mdb(img, 600, 1024, 16); // 16 + 1200 + 2 = 1218 blocks
    ASSERT_EQ_INT(image_wrap_extended_blocks(mem_read, &f, file_blocks, NULL), 1218);

    set_mdb(img, 480, 1024, 16); // 978 blocks: fits, file has slack
    ASSERT_EQ_INT(image_wrap_extended_blocks(mem_read, &f, file_blocks, NULL), file_blocks);

    set_mdb(img, 65535, 64 * 1024, 16); // ~4 GiB claimed of a 500 KB file
    ASSERT_EQ_INT(image_wrap_extended_blocks(mem_read, &f, file_blocks, NULL), file_blocks);

    memset(img, 0, 3 * BLK); // not a volume
    ASSERT_EQ_INT(image_wrap_extended_blocks(mem_read, &f, file_blocks, NULL), file_blocks);
    ASSERT_EQ_INT(image_wrap_extended_blocks(mem_read, &f, 2, NULL), 2); // too short to sniff
    free(img);
}

// A driverless disk's HFS partition is extended to its volume's claim, but
// never past its map entry.
TEST(test_extended_blocks_driverless) {
    const uint64_t file_blocks = 2000;
    uint8_t *img = (uint8_t *)calloc(file_blocks, BLK);
    ASSERT_TRUE(img != NULL);
    mem_file_t f = {img, file_blocks * BLK};
    img[0] = 'E';
    img[1] = 'R';
    map_entry(img, 1, 2, 1, 63, "Apple_partition_map");
    map_entry(img, 2, 2, 64, 3000, "Apple_HFS");
    uint8_t *vol = img + 64 * BLK;
    bare_head(vol);
    set_mdb(vol, 1100, 1024, 16); // 2218 blocks from 64: 2282
    uint64_t start = 0;
    ASSERT_EQ_INT(image_wrap_extended_blocks(mem_read, &f, file_blocks, &start), 2282);
    ASSERT_EQ_INT(start, 64);

    map_entry(img, 2, 2, 64, 2100, "Apple_HFS"); // the map is shorter still
    ASSERT_EQ_INT(image_wrap_extended_blocks(mem_read, &f, file_blocks, NULL), 2164);
    free(img);
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
    RUN(test_volume_blocks);
    RUN(test_extended_blocks_bare);
    RUN(test_extended_blocks_driverless);
    fprintf(stderr, "All image_wrap tests passed\n");
    return 0;
}
