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
    // Reads the backing store failed (a corrupt compressed chunk, a bad
    // archive member, a host I/O error): the guest gets a device-level read
    // error for each; only the first is logged at level 0.
    uint64_t read_errors;

    // Per-sector tags: the 12 bytes a GCR sector carries beside its 512 data
    // bytes.  The Lisa file system keeps its page labels (file id, page
    // links) there and the Lisa boot ROM checks the boot block's FILEID =
    // $AAAA; the Mac file systems write them too.  Loaded from a DiskCopy
    // 4.2 file's tag section, else zero for a 400K/800K GCR disk; NULL for
    // anything else.  Guest writes land here, travel in checkpoints
    // (image_checkpoint, image_set_tags) and leave in a DiskCopy 4.2
    // export -- the base file is never touched.
    uint8_t *tags; // tag_count * tag_bytes bytes, or NULL
    uint32_t tag_bytes; // tag bytes per sector (12 on a Lisa 400 KB disk)
    uint32_t tag_count; // number of tagged sectors

    // How a DiskCopy 4.2 export names and labels the disk: the source
    // header's own values for a DiskCopy image, a machine's for one it
    // knows (image_set_diskcopy_identity), else derived at export.
    uint8_t dc42_name[64]; // the header's name field (Pascal string, padded); length 0 = none
    uint8_t dc42_format_byte; // format byte, or 0 to derive from the size

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
