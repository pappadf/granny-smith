// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_apm.h
// Apple Partition Map parser.  Reads the map at block 1 of a 512-byte-block
// image, returns a table of parsed partitions ready for `image partmap` to
// render or (in Phase 2) for the auto-mount cache to key per-partition
// filesystem state on.

#pragma once

#ifndef IMAGE_APM_H
#define IMAGE_APM_H

#include "image.h"

#include <stdbool.h>
#include <stdint.h>

// Logical block size assumed for APM.  Driver descriptor block at block 0
// may declare something different, but every APM media we care about uses
// 512-byte blocks.
#define APM_BLOCK_SIZE 512

// Filesystem kind inferred from the APM partition type string.  The
// strings we probe against match period-correct Apple tooling.
enum apm_fs_kind {
    APM_FS_UNKNOWN = 0,
    APM_FS_HFS, // "Apple_HFS" / "Apple_HFSX"
    APM_FS_UFS, // "Apple_UNIX_SVR2" (A/UX UFS)
    APM_FS_PARTITION_MAP, // "Apple_partition_map" (self-describing entry)
    APM_FS_DRIVER, // "Apple_Driver*" (disk driver code)
    APM_FS_FREE, // "Apple_Free"
    APM_FS_PATCHES, // "Apple_Patches"
};

// One parsed partition.  Offsets are in 512-byte blocks, matching Apple
// tooling and APM on-disk fields exactly.
typedef struct apm_partition {
    uint32_t index; // 1-based, matches Apple convention
    uint64_t start_block; // partition start (pmPyPartStart)
    uint64_t size_blocks; // partition size (pmPartBlkCnt)
    char name[33]; // pmPartName, NUL-terminated
    char type[33]; // pmParType, NUL-terminated
    uint32_t status; // pmPartStatus flags
    enum apm_fs_kind fs_kind;
} apm_partition_t;

// Parsed table.  `partitions` is a heap array of length `n_partitions`;
// `map_block_count` is the self-reported map length (pmMapBlkCnt from
// entry 1).
typedef struct apm_table {
    uint32_t map_block_count;
    uint32_t n_partitions;
    apm_partition_t *partitions;
} apm_table_t;

// Return true if `block1` (a 512-byte buffer read from image offset 512)
// carries the APM signature "PM" and a plausible pmMapBlkCnt.  This is
// the cheap sniff the implicit-mount probe will call (Phase 2); parser
// callers in Phase 1 can skip it and go straight to image_apm_parse.
bool image_apm_probe_magic(const uint8_t *block1);

// Parse the APM from an open image.  Returns NULL and leaves *errmsg
// pointing at a static message on failure.  The caller owns the returned
// table and must free it with image_apm_free.  `errmsg` may be NULL.
apm_table_t *image_apm_parse(image_t *img, const char **errmsg);

// Parse APM directly from a contiguous byte buffer.  Exposed for unit
// tests that want to exercise the parser without dragging in the full
// image/storage stack; production callers should use image_apm_parse.
// `buf` must point at byte 0 of the image (driver descriptor in block 0,
// partition map entries starting at offset 512).
apm_table_t *image_apm_parse_buffer(const uint8_t *buf, size_t buf_size, const char **errmsg);

// Release a parsed table.  Safe on NULL.
void image_apm_free(apm_table_t *table);

// Map a pmParType string to an apm_fs_kind.  Useful for direct probing
// of individual entries when the full table isn't needed.
enum apm_fs_kind image_apm_classify_type(const char *type_string);

#endif // IMAGE_APM_H
