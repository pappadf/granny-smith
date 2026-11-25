// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_wrap.c
// Volume wrapper — see image_wrap.h and
// docs/core/storage/bare-volume-wrapper.md.

#include "image_wrap.h"

#include "build_id.h"
#include "common.h"
#include "gsdisk_driver.h" // generated: gsdisk_drvr[] (src/core/storage/gsdisk/)
#include "image_internal.h"
#include "log.h"
#include "mac_disk_layout.h"
#include "out.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("image")

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
    WR_BE16(e + APM_OFF_SIG, APM_SIG_PM);
    WR_BE32(e + APM_OFF_MAP_BLKCNT, map_entries);
    WR_BE32(e + APM_OFF_PY_START, start);
    WR_BE32(e + APM_OFF_PART_BLKCNT, blocks);
    put_str(e + APM_OFF_NAME, 32, name);
    put_str(e + APM_OFF_TYPE, 32, type);
    WR_BE32(e + APM_OFF_LG_DATA, 0);
    WR_BE32(e + APM_OFF_DATA_CNT, blocks);
    WR_BE32(e + APM_OFF_STATUS, status);
}

void image_wrap_build_prefix(uint8_t *out, uint64_t volume_blocks, const char *build_id) {
    image_wrap_build_prefix_boot(out, volume_blocks, build_id, NULL);
}

void image_wrap_build_prefix_boot(uint8_t *out, uint64_t volume_blocks, const char *build_id,
                                  const uint8_t *hfs_entry) {
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
    WR_BE16(out + DDM_OFF_SIG, DDM_SIG_ER);
    WR_BE16(out + DDM_OFF_BLK_SIZE, BLK);
    WR_BE32(out + DDM_OFF_BLK_COUNT, prefix + vol);
    WR_BE16(out + DDM_OFF_DEV_TYPE, 1);
    WR_BE16(out + DDM_OFF_DEV_ID, 1);
    WR_BE16(out + DDM_OFF_DRVR_COUNT, 1);
    WR_BE32(out + DDM_OFF_DD_BLOCK, IMAGE_WRAP_DRIVER_START);
    WR_BE16(out + DDM_OFF_DD_SIZE, drv_blocks);
    WR_BE16(out + DDM_OFF_DD_TYPE, 1);

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
    WR_BE32(e + APM_OFF_LG_BOOT, 0);
    WR_BE32(e + APM_OFF_BOOT_SIZE, drv_len);
    WR_BE32(e + APM_OFF_BOOT_ADDR, 0);
    WR_BE32(e + APM_OFF_BOOT_ENTRY, 0);
    WR_BE32(e + APM_OFF_BOOT_CKSUM, image_wrap_boot_checksum(drv, drv_len));
    put_str(e + APM_OFF_PROCESSOR, 16, "68000");
    e += BLK;
    put_entry(e, n, prefix, vol, "MacOS", "Apple_HFS", PM_VALID | PM_ALLOCATED | PM_IN_USE | PM_READABLE | PM_WRITABLE);
    // The volume's own boot fields, when its map had them: Mac OS X keeps
    // the XCOFF BootX of an Old World Mac in the HFS+ startup file and
    // points the partition entry's boot fields at it (block relative to the
    // partition, so the same volume behind the new map keeps them valid).
    if (hfs_entry) {
        WR_BE32(e + APM_OFF_STATUS, RD_BE32(e + APM_OFF_STATUS) | RD_BE32(hfs_entry + APM_OFF_STATUS));
        memcpy(e + APM_OFF_LG_BOOT, hfs_entry + APM_OFF_LG_BOOT,
               APM_OFF_PROCESSOR + APM_PROCESSOR_LEN - APM_OFF_LG_BOOT);
    }
}

// The map entry of the Apple_HFS partition at `start` in `head` (`len`
// bytes from block 0), or NULL.
static const uint8_t *find_hfs_entry(const uint8_t *head, size_t len, uint64_t start) {
    for (size_t b = IMAGE_WRAP_MAP_START; (b + 1) * BLK <= len && b < SNIFF_BLOCKS; b++) {
        const uint8_t *e = head + b * BLK;
        if (RD_BE16(e + APM_OFF_SIG) != APM_SIG_PM)
            break;
        if (RD_BE32(e + APM_OFF_PY_START) == start && memcmp(e + APM_OFF_TYPE, "Apple_HFS", 10) == 0)
            return e;
    }
    return NULL;
}

// Read `size` bytes at `offset` of the image without counting it as guest
// activity (the sniff is not).
static bool sniff(image_t *image, uint64_t offset, uint8_t *buf, size_t size) {
    uint64_t reads = image->reads;
    size_t got = disk_read_data(image, offset, buf, size);
    image->reads = reads;
    return got == size;
}

int image_wrap_volume(image_t *image) {
    if (!image || image->wrap_prefix || image->block_size != BLK || image->raw_size < 3 * BLK)
        return IMAGE_WRAP_NONE;
    uint64_t total_blocks = image->raw_size / BLK;
    size_t head_len = (size_t)(total_blocks < SNIFF_BLOCKS ? total_blocks : SNIFF_BLOCKS) * BLK;
    uint8_t *head = (uint8_t *)malloc(head_len);
    if (!head)
        return -1;
    int kind = IMAGE_WRAP_NONE;
    uint64_t start = 0, blocks = 0;
    uint8_t hfs_entry[BLK];
    bool have_entry = false;
    if (sniff(image, 0, head, head_len)) {
        if (image_wrap_is_bare_volume(head, head_len)) {
            kind = IMAGE_WRAP_BARE;
            blocks = total_blocks;
        } else if (image_wrap_find_driverless_hfs(head, head_len, &start, &blocks) && start < total_blocks) {
            const uint8_t *e = find_hfs_entry(head, head_len, start);
            if (e) {
                memcpy(hfs_entry, e, BLK);
                have_entry = true;
            }
            // The partition must hold an HFS volume where the map says, and
            // is clipped to the file (an image cut short of its map's claim).
            if (blocks > total_blocks - start)
                blocks = total_blocks - start;
            if (blocks >= 3 && sniff(image, (uint64_t)start * BLK, head, 3 * BLK) &&
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
    image_wrap_build_prefix_boot(prefix, blocks, build_id_get(), have_entry ? hfs_entry : NULL);
    image->wrap_prefix = prefix;
    image->wrap_blocks = IMAGE_WRAP_PREFIX_BLOCKS;
    image->wrap_base = (uint64_t)start * BLK;
    image->wrap_storage_size = image->raw_size;
    image->raw_size = (uint64_t)(IMAGE_WRAP_PREFIX_BLOCKS + blocks) * BLK;
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
