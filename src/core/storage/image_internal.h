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
    uint64_t raw_size; // Data bytes: sectors x 512 (tags not counted), plus any wrapper prefix
    // Bytes per stored block: a sector stored tag first, [tag_bytes][512 data]
    // -- the unit the base view, the delta and the journal are made of.
    uint32_t block_size;
    // Tag bytes beside each 512-byte sector: 12 for a 400K/800K GCR floppy
    // (the Lisa file system keeps its page labels there and the Lisa boot ROM
    // checks the boot block's FILEID = $AAAA; the Mac file systems write
    // them too), 20 for a ProFile, else 0.  They are in-band, in every
    // block, so base + delta is the whole disk: the delta and a checkpoint
    // carry them with the data, and a DiskCopy 4.2 or MOOF export writes
    // them out.  The base file is never touched.
    uint32_t tag_bytes;
    // The base file itself holds whole blocks, tags in-band (a ProFile's
    // 532-byte layout); otherwise it holds 512-byte sectors and its tags,
    // if any, come from a DiskCopy tag section or are zeros.
    bool units_in_file;
    // Tag bytes per sector the file itself carries (a DiskCopy tag section,
    // a ProFile's in-band 20), whatever the image was opened with: what
    // files.identify reports.
    uint32_t file_tag_bytes;
    bool writable; // True when the caller requested write access
    bool ghost_instance; // True when delta+journal are ephemeral scratch (read-only mounts)
    image_kind_t kind; // What the opening device presents it as (image_open_kind); IMAGE_KIND_NONE unheld
    uint32_t checkpoint_index; // position in the image list the last checkpoint saved (image_ref.c)
    bool from_diskcopy; // True if a DiskCopy 4.2 layer was peeled

    // disk_read_data / disk_write_data calls since open: the drive-activity
    // lights (drive_activity.h) and files.images[i].reads / .writes.
    uint64_t reads;
    uint64_t writes;
    // Reads the backing store failed (a corrupt compressed chunk, a bad
    // archive member, a host I/O error): the guest gets a device-level read
    // error for each; only the first is logged at level 0.
    uint64_t read_errors;

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
    uint64_t wrap_base;
    uint64_t wrap_storage_size;
};

// Per-image geometry: the tags a sector carries.  The kind decides it; only
// the storage module opens with one, through image_open_kind.
typedef struct image_geometry {
    // Fixed tag bytes per sector, which the kind's files hold in-band (a
    // ProFile: 20, its files 532-byte blocks).  0 for none.
    uint32_t tag_bytes;
    // A floppy: 12 tag bytes per sector when the disk is a 400K/800K GCR one,
    // from a DiskCopy tag section or zeros; none for an MFM one.
    bool gcr_tags;
} image_geometry_t;

// Tag bytes per sector a 400K/800K GCR floppy carries.
#define IMAGE_GCR_TAG_BYTES 12u

// The three openers of image.h with a given geometry.
image_t *image_open_readonly_with_geometry(const char *base_path, image_geometry_t geom);
image_t *image_create_with_geometry(const char *base_path, const char *delta_dir, image_geometry_t geom);
image_t *image_open_with_geometry(const char *base_path, const char *instance_path, image_geometry_t geom);

// A brand-new, writable, all-zero image with NO backing base file:
// `block_count` sectors of the geometry's blocks, every block reading back as
// zeros until written (image_create_blank_kind).  Its delta is a fresh
// instance in `delta_dir` (NULL: the scratch root), or the existing
// `instance` stem (a checkpoint restore), so it survives like any other.
image_t *image_create_blank(uint64_t block_count, image_geometry_t geom, const char *delta_dir, const char *instance);

#endif // IMAGE_INTERNAL_H
