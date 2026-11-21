// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_kind.h
// What a drive takes: the kind of medium a device opens an image as, and the
// one open path every device uses (docs/internals/core/storage/image_kind.md).
// The DEVICE chooses the kind -- a floppy drive opens a floppy, a SCSI disk a
// hard disk -- never the file's size.  The kind fixes the medium's geometry
// (the ProFile's 532-byte block), whether it may be written (a CD never),
// and where a writable medium's delta lives (image_delta_dir).

#ifndef IMAGE_KIND_H
#define IMAGE_KIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct image;

// The kind of medium a device presents an image as.  IMAGE_KIND_NONE is an image
// no device holds: a probe, a VFS mount, a file tool.
typedef enum image_kind {
    IMAGE_KIND_NONE = 0,
    IMAGE_KIND_FLOPPY, // a Sony / SuperDrive floppy (Mac controllers, the Lisa FDC)
    IMAGE_KIND_HD, // a SCSI or ATA hard disk
    IMAGE_KIND_CD, // a SCSI or ATAPI CD-ROM: always read-only
    IMAGE_KIND_PROFILE, // the Lisa's parallel ProFile: 532-byte blocks
    IMAGE_KIND_COUNT
} image_kind_t;

// The floppy layout a medium's size implies, whatever kind it was opened as:
// what a floppy drive presents it as until the guest lays another format
// down.  IMAGE_LAYOUT_NONE for any size that is not a floppy's.
typedef enum image_layout {
    IMAGE_LAYOUT_NONE = 0,
    IMAGE_LAYOUT_GCR400, // 400K GCR, single-sided
    IMAGE_LAYOUT_GCR800, // 800K GCR, double-sided
    IMAGE_LAYOUT_MFM720, // 720K MFM, double-density
    IMAGE_LAYOUT_MFM1440, // 1440K MFM, high-density
} image_layout_t;

// True for the MFM layouts (the ISM frames them; the IWM cannot).
static inline bool image_layout_is_mfm(image_layout_t l) {
    return l == IMAGE_LAYOUT_MFM720 || l == IMAGE_LAYOUT_MFM1440;
}

// The layout a disk of `bytes` implies.
static inline image_layout_t image_layout_of_size(uint64_t bytes) {
    switch (bytes) {
    case 400 * 1024:
        return IMAGE_LAYOUT_GCR400;
    case 800 * 1024:
        return IMAGE_LAYOUT_GCR800;
    case 720 * 1024:
        return IMAGE_LAYOUT_MFM720;
    case 1440 * 1024:
        return IMAGE_LAYOUT_MFM1440;
    default:
        return IMAGE_LAYOUT_NONE;
    }
}

// The wire name of a kind: "none", "floppy", "hd", "cd", "profile".
const char *image_kind_name(image_kind_t kind);

// Bytes per block a medium of `kind` is stored in: 532 for a ProFile (512
// data + its 20-byte tag, tag first), 512 for every other kind.
uint32_t image_kind_block_size(image_kind_t kind);

// Where a writable medium opened from `path` keeps its delta: the active
// machine's directory (it shares state.checkpoint's lifetime), or NULL --
// the image layer's scratch root -- for a base on volatile storage
// (platform_path_is_volatile) or when no machine directory is active.
const char *image_delta_dir(const char *path);

// How to open a medium (image_open_kind).
typedef struct image_open_opts {
    image_kind_t kind;
    bool writable; // guest writes land in a persistent delta; ignored for a CD
    // Reopen this existing delta+journal stem (a quick checkpoint's restore)
    // instead of minting a fresh instance.  NULL for a fresh one.
    const char *instance;
    // A fresh writable instance's directory; NULL = image_delta_dir(path).
    const char *delta_dir;
} image_open_opts_t;

// Open `path` as a medium of `o->kind`: the one open every attach, insert,
// create and restore goes through.  NULL when the file cannot be opened, or
// is not a medium of that kind (a floppy drive takes only floppy sizes);
// the reason is in `err` (may be NULL).
struct image *image_open_kind(const char *path, const image_open_opts_t *o, char *err, size_t err_cap);

// A brand-new all-zero medium of `kind` with no base file: `blocks` blocks of
// image_kind_block_size(kind) bytes (a blank ProFile).
struct image *image_create_blank_kind(image_kind_t kind, uint64_t blocks);

#endif // IMAGE_KIND_H
