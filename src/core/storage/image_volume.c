// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_volume.c
// The disk / volume classifier.  See image_volume.h.

#include "image_volume.h"

#include "common.h"
#include "image_apm.h"
#include "image_hfs.h"
#include "image_mfs.h"
#include "image_ufs.h"
#include "mac_disk_layout.h"

#include <stdlib.h>
#include <string.h>

// The 512-byte block a Mac volume header occupies.
#define HEADER_BLOCK 512u

bool image_apm_probe_magic(const uint8_t *block1) {
    if (!block1)
        return false;
    // Signature "PM" must be present.
    if (RD_BE16(block1 + APM_OFF_SIG) != APM_SIG_PM)
        return false;
    // pmMapBlkCnt must be plausible: non-zero and not absurdly large.
    uint32_t map_blocks = RD_BE32(block1 + APM_OFF_MAP_BLKCNT);
    if (map_blocks == 0 || map_blocks > APM_MAX_PARTITIONS)
        return false;
    return true;
}

// The signature word of the volume header at 1024, or 0 when the volume is
// too small to hold the 512-byte block it starts (the HFS and MFS openers'
// own minimum) or the head does not reach it.
static uint16_t header_sig(const uint8_t *head, size_t len, uint64_t size) {
    if (size < MAC_VOLUME_HEADER_OFF + HEADER_BLOCK || len < MAC_VOLUME_HEADER_OFF + 2)
        return 0;
    return RD_BE16(head + MAC_VOLUME_HEADER_OFF);
}

// A UFS superblock: the magic, big-endian -- the order A/UX writes and
// ufs_open_source reads -- in a volume as large as it needs.
static bool is_ufs(const uint8_t *head, size_t len, uint64_t size) {
    if (size < UFS_SBOFF + 2048 || len < UFS_SBOFF + UFS_SB_MAGIC_OFF + 4)
        return false;
    return RD_BE32(head + UFS_SBOFF + UFS_SB_MAGIC_OFF) == UFS_FS_MAGIC;
}

// An ISO 9660 primary volume descriptor (type 1, "CD001", version 1) at
// sector 16, in a volume that holds the whole descriptor sector.
static bool is_iso(const uint8_t *head, size_t len, uint64_t size) {
    if (size < VOLUME_SNIFF_HEAD || len < ISO9660_VD_OFF + 7)
        return false;
    const uint8_t *vd = head + ISO9660_VD_OFF;
    return vd[0] == 1 && memcmp(vd + 1, ISO9660_ID, 5) == 0 && vd[6] == 1;
}

volume_sniff_t volume_sniff(const uint8_t *head, size_t len, uint64_t size) {
    volume_sniff_t r = {VOL_NONE, false};
    if (!head)
        return r;
    r.iso = is_iso(head, len, size);
    // The map's first entry is block 1.
    uint16_t sig = header_sig(head, len, size);
    if (size >= 2 * APM_BLOCK_SIZE && len >= 2 * APM_BLOCK_SIZE && image_apm_probe_magic(head + APM_BLOCK_SIZE))
        r.kind = VOL_APM;
    else if (sig == HFS_SIG_BD)
        r.kind = VOL_HFS;
    else if (sig == HFS_SIG_HP)
        r.kind = VOL_HFSPLUS;
    else if (sig == HFS_SIG_HX)
        r.kind = VOL_HFSX;
    else if (is_ufs(head, len, size))
        r.kind = VOL_UFS;
    else if (sig == MFS_SIG)
        r.kind = VOL_MFS;
    else if (r.iso)
        r.kind = VOL_ISO;
    return r;
}

volume_sniff_t volume_sniff_source(source_t *src, uint64_t off, uint64_t size) {
    volume_sniff_t r = {VOL_NONE, false};
    if (!src)
        return r;
    // Read no further than the volume (or the source) reaches.
    uint64_t avail = source_size(src);
    avail = avail > off ? avail - off : 0;
    if (avail > size)
        avail = size;
    size_t len = (size_t)(avail < VOLUME_SNIFF_HEAD ? avail : VOLUME_SNIFF_HEAD);
    uint8_t *head = malloc(len ? len : 1);
    if (!head)
        return r;
    if (source_read_exact(src, off, head, len) == 0)
        r = volume_sniff(head, len, size);
    free(head);
    return r;
}

bool volume_kind_is_hfs(volume_kind_t kind) {
    return kind == VOL_HFS || kind == VOL_HFSPLUS || kind == VOL_HFSX;
}

const char *volume_kind_label(volume_kind_t kind) {
    switch (kind) {
    case VOL_APM:
        return "APM";
    case VOL_HFS:
        return "HFS";
    case VOL_HFSPLUS:
        return "HFS+";
    case VOL_HFSX:
        return "HFSX";
    case VOL_MFS:
        return "MFS";
    case VOL_UFS:
        return "UFS";
    case VOL_ISO:
        return "ISO 9660";
    default:
        return NULL;
    }
}

uint64_t volume_hfs_blocks(const uint8_t hdr[512]) {
    if (!hdr)
        return 0;
    uint16_t sig = RD_BE16(hdr + MDB_OFF_SIG);
    if (sig == HFS_SIG_BD) {
        // HFS: the allocation blocks start drAlBlSt blocks in, and the
        // alternate MDB and one reserved block follow the last of them.
        uint32_t nm = RD_BE16(hdr + MDB_OFF_NM_AL_BLKS), al = RD_BE32(hdr + MDB_OFF_AL_BLK_SIZ);
        uint32_t first = RD_BE16(hdr + MDB_OFF_AL_BL_ST);
        if (al == 0 || al % HEADER_BLOCK)
            return 0;
        return (uint64_t)first + (uint64_t)nm * (al / HEADER_BLOCK) + 2;
    }
    if (sig == HFS_SIG_HP || sig == HFS_SIG_HX) {
        // HFS Plus / HFSX: totalBlocks allocation blocks cover the whole
        // volume, the alternate header included.
        uint32_t al = RD_BE32(hdr + VH_OFF_BLOCK_SIZE), total = RD_BE32(hdr + VH_OFF_TOTAL_BLKS);
        if (al == 0 || al % HEADER_BLOCK)
            return 0;
        return (uint64_t)total * (al / HEADER_BLOCK);
    }
    return 0;
}
