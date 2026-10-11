// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_apm.h
// Apple Partition Map parser. Reads the map at block 1 of a 512-byte-block
// image and returns a table of parsed partitions. Consumed by `image partmap`
// (rendering) and by the VFS auto-mount cache (keying per-partition
// filesystem state).

#pragma once

#ifndef IMAGE_APM_H
#define IMAGE_APM_H

#include "image.h"

#include <stdbool.h>
#include <stdint.h>

// Logical block size of the partition map.  The driver descriptor in block 0
// may declare another (sbBlkSize; 2048 on Apple CD-ROMs), but the map
// entries and their start/size fields are in 512-byte blocks on every Apple
// medium, CDs included, so the declared size is not consulted -- rejecting
// a map whose descriptor says 2048 would refuse those discs.
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
    APM_FS_MFS, // a bare MFS volume (never in a partition map)
    APM_FS_ISO9660, // a bare ISO 9660 volume (never in a partition map)
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
// carries the APM signature "PM" and a plausible pmMapBlkCnt. This is the
// cheap sniff the implicit-mount probe calls before committing to a full
// parse; direct callers can skip it and go straight to image_apm_parse.
bool image_apm_probe_magic(const uint8_t *block1);

// Parse the APM from an open image.  Returns NULL and leaves *errmsg
// pointing at a static message on failure.  The caller owns the returned
// table and must free it with image_apm_free.  `errmsg` may be NULL.
apm_table_t *image_apm_parse(image_t *img, const char **errmsg);

// The same over a byte source holding the disk.
struct peel_source;
apm_table_t *image_apm_parse_source(struct peel_source *src, const char **errmsg);

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

// The short label `files.partmap` shows for a kind ("HFS", "drvr", "--").
// Kept beside image_apm_classify_type so the two mappings stay in step.
const char *image_apm_fs_kind_label(enum apm_fs_kind kind);

// The parser's error messages, what *errmsg points at on failure.
extern const char *const image_apm_err_nil; // no image / source
extern const char *const image_apm_err_read; // too short, unreadable, or not 512-byte blocks
extern const char *const image_apm_err_sig; // no "PM" signature on block 1
extern const char *const image_apm_err_count; // pmMapBlkCnt out of range
extern const char *const image_apm_err_alloc; // out of memory

#endif // IMAGE_APM_H
