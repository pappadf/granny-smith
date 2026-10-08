// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_wrap.c
// Volume wrapper — see image_wrap.h and
// docs/core/storage/bare-volume-wrapper.md.

#include "image_wrap.h"

#include "build_id.h"
#include "common.h"
#include "gs_out.h"
#include "gsdisk_driver.h" // generated: gsdisk_drvr[] (src/core/storage/gsdisk/)
#include "log.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("image")

// Block 0: Driver Descriptor Map field offsets.
#define DDM_SIG        0 // 'ER'
#define DDM_BLK_SIZE   2
#define DDM_BLK_COUNT  4
#define DDM_DEV_TYPE   8
#define DDM_DEV_ID     10
#define DDM_DRVR_COUNT 16
#define DDM_DD_BLOCK   18 // first descriptor: ddBlock.l, ddSize.w, ddType.w
#define DDM_DD_SIZE    22
#define DDM_DD_TYPE    24

// Partition map entry field offsets.
#define PM_SIG          0 // 'PM'
#define PM_MAP_BLK_CNT  4
#define PM_PY_START     8
#define PM_PART_BLK_CNT 12
#define PM_PART_NAME    16
#define PM_PAR_TYPE     48
#define PM_LG_DATA      80
#define PM_DATA_CNT     84
#define PM_STATUS       88
#define PM_LG_BOOT      92
#define PM_BOOT_SIZE    96
#define PM_BOOT_ADDR    100
#define PM_BOOT_ENTRY   108
#define PM_BOOT_CKSUM   116
#define PM_PROCESSOR    120

// pmPartStatus bits.
#define PM_VALID     0x01u
#define PM_ALLOCATED 0x02u
#define PM_IN_USE    0x04u
#define PM_BOOTABLE  0x08u
#define PM_READABLE  0x10u
#define PM_WRITABLE  0x20u
#define PM_BOOT_PIC  0x40u

#define BLK 512u

// Head read for the sniff: block 0 and the longest map the prefix itself
// would hold (entries 1..63).
#define SNIFF_BLOCKS (IMAGE_WRAP_MAP_START + IMAGE_WRAP_MAP_BLOCKS)

// Store a big-endian 16-bit value.
static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

// Store a big-endian 32-bit value.
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Copy a NUL-padded string into a fixed field (never overflows it).
static void put_str(uint8_t *p, size_t cap, const char *s) {
    size_t n = strlen(s);
    memcpy(p, s, n < cap ? n : cap);
}

uint32_t image_wrap_boot_checksum(const uint8_t *code, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint16_t)(sum + code[i]); // add the byte ...
        sum = (uint16_t)((sum << 1) | (sum >> 15)); // ... and rotate left
    }
    return sum ? sum : 0xFFFFu; // the ROM reserves 0 for "no checksum"
}

// Write one partition map entry into `e` (a zeroed 512-byte block).
static void put_entry(uint8_t *e, uint32_t map_entries, uint32_t start, uint32_t blocks, const char *name,
                      const char *type, uint32_t status) {
    put16(e + PM_SIG, 0x504D); // 'PM'
    put32(e + PM_MAP_BLK_CNT, map_entries);
    put32(e + PM_PY_START, start);
    put32(e + PM_PART_BLK_CNT, blocks);
    put_str(e + PM_PART_NAME, 32, name);
    put_str(e + PM_PAR_TYPE, 32, type);
    put32(e + PM_LG_DATA, 0);
    put32(e + PM_DATA_CNT, blocks);
    put32(e + PM_STATUS, status);
}

void image_wrap_build_prefix(uint8_t *out, uint64_t volume_blocks, const char *build_id) {
    const uint32_t prefix = IMAGE_WRAP_PREFIX_BLOCKS;
    const uint32_t vol = volume_blocks > 0xFFFFFFFFu - prefix ? 0xFFFFFFFFu - prefix : (uint32_t)volume_blocks;
    const uint32_t drv_len = (uint32_t)sizeof(gsdisk_drvr);
    const uint32_t drv_blocks = (drv_len + BLK - 1) / BLK;
    memset(out, 0, (size_t)prefix * BLK);

    // The driver, with the emulator's build id stamped after its marker.
    uint8_t *drv = out + (size_t)IMAGE_WRAP_DRIVER_START * BLK;
    memcpy(drv, gsdisk_drvr, drv_len);
    static const char marker[] = "GSDisk build ";
    for (uint32_t i = 0; build_id && i + sizeof(marker) - 1 + 24 <= drv_len; i++) {
        if (memcmp(drv + i, marker, sizeof(marker) - 1) == 0) {
            uint8_t *stamp = drv + i + sizeof(marker) - 1;
            memset(stamp, 0, 24);
            put_str(stamp, 23, build_id); // keep a trailing NUL
            break;
        }
    }

    // Block 0: one 68k driver (ddType 1) at the driver partition.
    put16(out + DDM_SIG, 0x4552); // 'ER'
    put16(out + DDM_BLK_SIZE, BLK);
    put32(out + DDM_BLK_COUNT, prefix + vol);
    put16(out + DDM_DEV_TYPE, 1);
    put16(out + DDM_DEV_ID, 1);
    put16(out + DDM_DRVR_COUNT, 1);
    put32(out + DDM_DD_BLOCK, IMAGE_WRAP_DRIVER_START);
    put16(out + DDM_DD_SIZE, drv_blocks);
    put16(out + DDM_DD_TYPE, 1);

    // Blocks 1..3: the map itself, the driver, the volume.  The driver
    // partition's name must start "Maci" and its boot fields must checksum,
    // or the IIci-and-later SCSI boot code refuses to run it.
    const uint32_t n = 3;
    uint8_t *e = out + (size_t)IMAGE_WRAP_MAP_START * BLK;
    put_entry(e, n, IMAGE_WRAP_MAP_START, IMAGE_WRAP_MAP_BLOCKS, "Apple", "Apple_partition_map",
              PM_VALID | PM_ALLOCATED | PM_IN_USE | PM_READABLE | PM_WRITABLE);
    e += BLK;
    put_entry(e, n, IMAGE_WRAP_DRIVER_START, IMAGE_WRAP_DRIVER_BLOCKS, "Macintosh", "Apple_Driver",
              PM_VALID | PM_ALLOCATED | PM_IN_USE | PM_BOOTABLE | PM_READABLE | PM_WRITABLE | PM_BOOT_PIC);
    put32(e + PM_LG_BOOT, 0);
    put32(e + PM_BOOT_SIZE, drv_len);
    put32(e + PM_BOOT_ADDR, 0);
    put32(e + PM_BOOT_ENTRY, 0);
    put32(e + PM_BOOT_CKSUM, image_wrap_boot_checksum(drv, drv_len));
    put_str(e + PM_PROCESSOR, 16, "68000");
    e += BLK;
    put_entry(e, n, prefix, vol, "MacOS", "Apple_HFS", PM_VALID | PM_ALLOCATED | PM_IN_USE | PM_READABLE | PM_WRITABLE);
}

// Read `size` bytes at `offset` of the image without counting it as guest
// activity (the sniff is not).
static bool sniff(image_t *image, size_t offset, uint8_t *buf, size_t size) {
    uint64_t reads = image->reads;
    size_t got = disk_read_data(image, offset, buf, size);
    image->reads = reads;
    return got == size;
}

int image_wrap_volume(image_t *image) {
    if (!image || image->wrap_prefix || image->block_size != BLK || image->raw_size < 3 * BLK)
        return IMAGE_WRAP_NONE;
    size_t total_blocks = image->raw_size / BLK;
    size_t head_len = (total_blocks < SNIFF_BLOCKS ? total_blocks : SNIFF_BLOCKS) * BLK;
    uint8_t *head = (uint8_t *)malloc(head_len);
    if (!head)
        return -1;
    int kind = IMAGE_WRAP_NONE;
    uint64_t start = 0, blocks = 0;
    if (sniff(image, 0, head, head_len)) {
        if (image_wrap_is_bare_volume(head, head_len)) {
            kind = IMAGE_WRAP_BARE;
            blocks = total_blocks;
        } else if (image_wrap_find_driverless_hfs(head, head_len, &start, &blocks) && start < total_blocks) {
            // The partition must hold an HFS volume where the map says, and
            // is clipped to the file (an image cut short of its map's claim).
            if (blocks > total_blocks - start)
                blocks = total_blocks - start;
            if (blocks >= 3 && sniff(image, (size_t)start * BLK, head, 3 * BLK) &&
                image_wrap_is_bare_volume(head, 3 * BLK))
                kind = IMAGE_WRAP_DRIVERLESS;
        }
    }
    free(head);
    if (kind == IMAGE_WRAP_NONE)
        return IMAGE_WRAP_NONE;
    uint8_t *prefix = (uint8_t *)malloc((size_t)IMAGE_WRAP_PREFIX_BLOCKS * BLK);
    if (!prefix)
        return -1;
    image_wrap_build_prefix(prefix, blocks, build_id_get());
    image->wrap_prefix = prefix;
    image->wrap_blocks = IMAGE_WRAP_PREFIX_BLOCKS;
    image->wrap_base = (size_t)start * BLK;
    image->wrap_storage_size = image->raw_size;
    image->raw_size = (size_t)(IMAGE_WRAP_PREFIX_BLOCKS + blocks) * BLK;
    image->type = image_hd;
    if (kind == IMAGE_WRAP_BARE)
        LOG(1, "wrapped bare volume %s: %u-block partition map + GSDisk driver in front of %llu blocks",
            image->filename ? image->filename : "?", IMAGE_WRAP_PREFIX_BLOCKS, (unsigned long long)blocks);
    else
        LOG(1,
            "wrapped driverless partitioned disk %s: %u-block partition map + GSDisk driver in front of its "
            "Apple_HFS partition (%llu blocks at block %llu)",
            image->filename ? image->filename : "?", IMAGE_WRAP_PREFIX_BLOCKS, (unsigned long long)blocks,
            (unsigned long long)start);
    return kind;
}
