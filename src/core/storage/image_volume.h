// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_volume.h
// The one "what is on this disk" classifier: an Apple Partition Map, or a
// bare HFS, HFS Plus, HFSX, MFS, UFS or ISO 9660 volume, from the disk's
// first bytes.  The format registry's disk detector, the VFS disk
// namespace, files.identify, the bare-volume wrapper's sniff and the UDIF
// writer's block-table name all ask it, so a disk one of them claims is one
// the others recognise too.  Each test is the signature check of the
// module that opens that kind (image_apm, image_hfs, image_mfs, image_ufs,
// image_iso9660), under the same minimum size, so a disk classified here
// is one its opener does not refuse on sight.

#ifndef IMAGE_VOLUME_H
#define IMAGE_VOLUME_H

#include "image_iso9660.h"
#include "source.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What a disk (or a partition) starts with.
typedef enum {
    VOL_NONE = 0,
    VOL_APM, // an Apple Partition Map at block 1
    VOL_HFS, // "BD" at 1024
    VOL_HFSPLUS, // "H+" at 1024
    VOL_HFSX, // "HX" at 1024
    VOL_MFS, // 0xD2D7 at 1024
    VOL_UFS, // the big-endian UFS magic in the superblock at 8192
    VOL_ISO, // an ISO 9660 primary volume descriptor at 32 KiB
} volume_kind_t;

// A classification: the kind, and whether an ISO 9660 descriptor is
// present too (always for VOL_ISO; for an APM or HFS-family disk, a hybrid
// disc with both sides).
typedef struct {
    volume_kind_t kind;
    bool iso;
} volume_sniff_t;

// Bytes of head that answer every test (the ISO descriptor is the last).
#define VOLUME_SNIFF_HEAD (ISO9660_VD_OFF + 2048u)

// Classify a disk or volume of `size` bytes from `len` bytes of its head.
// A test whose bytes lie beyond `len`, or whose kind needs a volume larger
// than `size`, answers no.  Precedence: APM, the HFS family, UFS, MFS,
// then ISO 9660.
volume_sniff_t volume_sniff(const uint8_t *head, size_t len, uint64_t size);

// The same for `size` bytes of `src` at `off` (reads up to
// VOLUME_SNIFF_HEAD bytes).  VOL_NONE when the head cannot be read.
volume_sniff_t volume_sniff_source(source_t *src, uint64_t off, uint64_t size);

// True for VOL_HFS, VOL_HFSPLUS and VOL_HFSX.
bool volume_kind_is_hfs(volume_kind_t kind);

// "APM", "HFS", "HFS+", "HFSX", "MFS", "UFS", "ISO 9660"; NULL for VOL_NONE.
const char *volume_kind_label(volume_kind_t kind);

// The length in 512-byte blocks an HFS, HFS Plus or HFSX volume header
// (`hdr`: the 512 bytes at MAC_VOLUME_HEADER_OFF) says its volume has --
// for HFS the allocation blocks plus the alternate MDB and reserved block
// after them.  0 when `hdr` is no such header or names an allocation block
// size that is not a whole number of 512-byte blocks.
uint64_t volume_hfs_blocks(const uint8_t hdr[512]);

#endif // IMAGE_VOLUME_H
