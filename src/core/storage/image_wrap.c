// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_wrap.c
// Bare-volume wrapper — see image_wrap.h and
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

bool image_wrap_is_bare_volume(const uint8_t *head, size_t len) {
    if (!head || len < 3 * BLK)
        return false;
    // A partitioned disk: DDM in block 0 or map entries in block 1.
    if (head[0] == 'E' && head[1] == 'R')
        return false;
    if (head[BLK] == 'P' && head[BLK + 1] == 'M')
        return false;
    // HFS Master Directory Block or HFS+ volume header at the bare position.
    const uint8_t *mdb = head + 2 * BLK;
    return (mdb[0] == 'B' && mdb[1] == 'D') || (mdb[0] == 'H' && mdb[1] == '+');
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

int image_wrap_bare_volume(image_t *image) {
    if (!image || image->wrap_prefix || image->block_size != BLK || image->raw_size < 3 * BLK)
        return 0;
    uint8_t head[3 * BLK];
    uint64_t reads = image->reads; // the sniff is not guest activity
    size_t got = disk_read_data(image, 0, head, sizeof(head));
    image->reads = reads;
    if (got != sizeof(head))
        return 0;
    if (!image_wrap_is_bare_volume(head, sizeof(head)))
        return 0;
    uint8_t *prefix = (uint8_t *)malloc((size_t)IMAGE_WRAP_PREFIX_BLOCKS * BLK);
    if (!prefix)
        return -1;
    image_wrap_build_prefix(prefix, image->raw_size / BLK, get_build_id());
    image->wrap_prefix = prefix;
    image->wrap_blocks = IMAGE_WRAP_PREFIX_BLOCKS;
    image->raw_size += (size_t)IMAGE_WRAP_PREFIX_BLOCKS * BLK;
    image->type = image_hd;
    LOG(1, "wrapped bare volume %s: %u-block partition map + GSDisk driver in front of %zu blocks",
        image->filename ? image->filename : "?", IMAGE_WRAP_PREFIX_BLOCKS,
        image->raw_size / BLK - IMAGE_WRAP_PREFIX_BLOCKS);
    return 1;
}
