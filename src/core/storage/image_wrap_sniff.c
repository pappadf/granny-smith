// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_wrap_sniff.c
// The volume wrapper's sniffs and sizing (image_wrap.h): which images it
// wraps, and how long a trimmed volume really is.  Apart from image_wrap.c
// (which embeds the GSDisk driver) so the image layer, which sizes its
// storage with image_wrap_extended_blocks, links without the driver.

#include "image_apm.h"
#include "image_wrap.h"

#include "common.h"
#include "image_volume.h"
#include "mac_disk_layout.h"

#include <stdlib.h>
#include <string.h>

#define BLK 512u

// Head read for the sniff: block 0 and the longest map the prefix itself
// would hold (entries 1..63).
#define SNIFF_BLOCKS (IMAGE_WRAP_MAP_START + IMAGE_WRAP_MAP_BLOCKS)

bool image_wrap_is_bare_volume(const uint8_t *head, size_t len) {
    if (!head || len < 3 * BLK)
        return false;
    // A partitioned disk: DDM in block 0 or map entries in block 1 (any
    // "PM", even one the classifier finds implausible, rules a volume out).
    if (RD_BE16(head + DDM_OFF_SIG) == DDM_SIG_ER || RD_BE16(head + BLK + APM_OFF_SIG) == APM_SIG_PM)
        return false;
    // An HFS or HFS Plus volume at the bare position.  Not HFSX, which no
    // classic Mac OS the GSDisk driver serves can mount, and not MFS, which
    // the system mounts from floppies only: either is left to read as the
    // raw disk it is.
    volume_kind_t k = volume_sniff(head, len, len).kind;
    return k == VOL_HFS || k == VOL_HFSPLUS;
}

uint64_t image_wrap_volume_blocks(const uint8_t *vol, size_t len) {
    if (!vol || len < 3 * BLK)
        return 0;
    return volume_hfs_blocks(vol + MAC_VOLUME_HEADER_OFF);
}

bool image_wrap_find_driverless_hfs(const uint8_t *head, size_t len, uint64_t *start, uint64_t *blocks) {
    if (!head || len < 2 * BLK || !image_apm_probe_magic(head + BLK))
        return false;
    // A DDM that names a driver means the disk has one (or claims to): leave
    // it to the ROM.
    if (RD_BE16(head + DDM_OFF_SIG) == DDM_SIG_ER && RD_BE16(head + DDM_OFF_DRVR_COUNT) != 0)
        return false;
    apm_table_t *t = image_apm_parse_buffer(head, len, NULL);
    if (!t)
        return false;
    // A map cut short (by the buffer or a bad entry) could hide a driver.
    bool ok = t->n_partitions == t->map_block_count;
    const apm_partition_t *hfs = NULL;
    for (uint32_t i = 0; ok && i < t->n_partitions; i++) {
        const apm_partition_t *p = &t->partitions[i];
        // A driver means the ROM can boot it already; an A/UX partition is
        // reached by A/UX through the map, which the wrapper would hide.
        if (p->fs_kind == APM_FS_DRIVER || p->fs_kind == APM_FS_UFS)
            ok = false;
        else if (p->fs_kind == APM_FS_HFS) {
            if (hfs)
                ok = false; // exactly one: the guest sees only it
            hfs = p;
        }
    }
    ok = ok && hfs && hfs->start_block >= 2 && hfs->size_blocks >= 3;
    if (ok) {
        *start = hfs->start_block;
        *blocks = hfs->size_blocks;
    }
    image_apm_free(t);
    return ok;
}

uint64_t image_wrap_extended_blocks(image_wrap_read_fn read, void *ctx, uint64_t raw_blocks, uint64_t *volume_start) {
    if (volume_start)
        *volume_start = 0;
    if (!read || raw_blocks < 3)
        return raw_blocks;
    size_t head_len = (size_t)(raw_blocks < SNIFF_BLOCKS ? raw_blocks : SNIFF_BLOCKS) * BLK;
    uint8_t *head = (uint8_t *)malloc(head_len);
    if (!head)
        return raw_blocks;
    uint64_t need = 0, start = 0, blocks = 0;
    if (read(ctx, 0, head, head_len)) {
        if (image_wrap_is_bare_volume(head, head_len))
            need = image_wrap_volume_blocks(head, head_len);
        else if (image_wrap_find_driverless_hfs(head, head_len, &start, &blocks) && start + 3 <= raw_blocks &&
                 read(ctx, start * BLK, head, 3 * BLK) && image_wrap_is_bare_volume(head, 3 * BLK)) {
            // The partition holds no more than its map entry says.
            uint64_t vol = image_wrap_volume_blocks(head, 3 * BLK);
            need = vol ? start + (vol < blocks ? vol : blocks) : 0;
        }
    }
    free(head);
    // A header claiming far more than the file has is no trimmed tail but
    // garbage (or a sparse stub): left at the file's own length.
    if (need <= raw_blocks || need - raw_blocks > IMAGE_WRAP_MAX_EXTEND_BLOCKS ||
        need > 0xFFFFFFFFu - IMAGE_WRAP_PREFIX_BLOCKS)
        return raw_blocks;
    if (volume_start)
        *volume_start = start;
    return need;
}
