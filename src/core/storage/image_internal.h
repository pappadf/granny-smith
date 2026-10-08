// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_internal.h
// The layout of struct image, private to the storage module (image.c and
// its siblings in src/core/storage).  Everyone else holds an opaque image_t
// and reads it through the accessors in image.h.

#ifndef IMAGE_INTERNAL_H
#define IMAGE_INTERNAL_H

#include "image.h"
#include "storage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The image: every field is the storage module's own.
struct image {
    storage_t *storage; // Backing storage engine instance
    char *filename; // The path the caller named (a host path, or one through an image or archive)
    char *source_key; // Key of the source the caller's path opened (source.h)
    char *format; // Wrapper layers peeled to reach the disk: "raw", "dc42", "bin+ndif", ...
    char *source_canon; // Writable only: canonical form of the path the caller named
    struct image *next_writable; // Writable only: the open-writable list (image_path_is_open_writable)
    char *instance_path; // Stem for delta/journal: "<dir>/<id>" — NULL for read-only ghost mounts
    char *delta_path; // Path to delta file (<instance_path>.delta)
    char *journal_path; // Path to preimage journal (<instance_path>.journal)
    size_t raw_size; // Logical size of the image in bytes
    uint32_t block_size; // Bytes per logical block (512 default, 532 for a ProFile)
    bool writable; // True when the caller requested write access
    bool ghost_instance; // True when delta+journal are ephemeral scratch (read-only mounts)
    enum image_type type; // Detected image type (floppy, hd, ...)
    bool from_diskcopy; // True if a DiskCopy 4.2 layer was peeled

    // disk_read_data / disk_write_data calls since open: the drive-activity
    // lights (drive_activity.h) and files.images[i].reads / .writes.
    uint64_t reads;
    uint64_t writes;

    // DiskCopy 4.2 per-sector tags (read-only metadata).  The Lisa boot ROM and
    // OS read these (e.g. the boot block's FILEID = $AAAA); loaded from the
    // file's tag section at open time.  NULL when the image has no tags.
    uint8_t *tags; // tag_count * tag_bytes bytes, or NULL
    uint32_t tag_bytes; // tag bytes per sector (12 on a Lisa 400 KB disk)
    uint32_t tag_count; // number of tagged sectors

    // Volume wrapper (image_wrap.h): a synthesised partition-map + driver
    // prefix served in front of an HFS volume.  wrap_blocks blocks of
    // wrap_prefix precede the volume, which starts wrap_base bytes into
    // `storage` (0 for a bare volume; the Apple_HFS partition's start for a
    // driverless partitioned disk).  raw_size is the prefix plus the volume;
    // wrap_storage_size is the storage's own size.  NULL / 0 for every
    // other image.
    uint8_t *wrap_prefix;
    uint32_t wrap_blocks;
    size_t wrap_base;
    size_t wrap_storage_size;
};

#endif // IMAGE_INTERNAL_H
