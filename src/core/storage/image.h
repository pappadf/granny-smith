// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image.h
// Public interface for disk image management.

#ifndef IMAGE_H
#define IMAGE_H

// === Includes ===
#include "common.h"
#include "storage.h"

#include <stdbool.h>
#include <stdint.h>

// === Forward Declarations ===
struct config;
typedef struct config config_t;

// === Type Definitions ===
// Floppy kinds are named by capacity because that is what distinguishes them
// on the wire; the encoding follows from it.  image_fd_dd_mfm (720K) was
// missing, so a 737,280-byte floppy classified as a hard disk and every
// consumer got a wrong answer.  Ask "is this MFM media?" with
// image_is_mfm_floppy(), never `type == image_fd_hd`.
enum image_type {
    image_other,
    image_fd_ss, // 400K GCR, single-sided
    image_fd_ds, // 800K GCR, double-sided
    image_fd_dd_mfm, // 720K MFM, double-density
    image_fd_hd, // 1440K MFM, high-density
    image_hd,
    image_cdrom
};

// True for media the drive reads with MFM framing rather than Apple GCR.
static inline bool image_is_mfm_floppy(enum image_type t) {
    return t == image_fd_dd_mfm || t == image_fd_hd;
}

// True for any floppy geometry, GCR or MFM.
static inline bool image_is_floppy(enum image_type t) {
    return t == image_fd_ss || t == image_fd_ds || image_is_mfm_floppy(t);
}

// Per-image geometry.  The default openers use { .block_size = 512 }; devices
// with a different on-disk block (e.g. the Lisa ProFile's 532-byte block) open
// with the matching size via the *_with_geometry variants.  Has room to grow
// (tag size, sector-header layout) without churning the opener signatures.
typedef struct image_geometry {
    uint32_t block_size; // Bytes per block; 0 is treated as STORAGE_BLOCK_SIZE (512)
} image_geometry_t;

// Image structure (exposed for performance-critical access in floppy controller)
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

struct image;
typedef struct image image_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Module initialization (registers shell commands)
void image_init(checkpoint_t *checkpoint);

// Module destructor (no-op for now)
void image_delete(void);

// Open a base image read-only.  Delta and journal are placed in a process-local
// scratch directory and removed when the image is closed.
//
// Every opener takes a path the VFS resolves: a host file, or a file inside
// an image or archive (outer.img/partition1/inner.img, disks.zip/a.dsk).
// Wrapper formats are peeled through the format registry -- UDIF, NDIF,
// DiskCopy 4.2, MacBinary, BinHex, gzip, in any nesting -- and the storage
// engine reads the innermost source directly: nothing is decoded to a file.
image_t *image_open_readonly(const char *base_path);

// The same over forks already open (the VFS mounting a file it holds).
// `name` is what image_get_filename reports.
struct peel_source;
image_t *image_open_readonly_source(const char *name, struct peel_source *data, struct peel_source *rsrc);

// Create a new writable image instance.  The image subsystem mints an opaque
// 16-hex-char id internally and creates two files at <delta_dir>/<id>.delta
// and <delta_dir>/<id>.journal.  If `delta_dir` is NULL, the GS_STORAGE_CACHE
// environment directory is used when set (the integration runner's per-test
// sidecar routing), else the directory of `base_path` (legacy
// adjacent-to-base layout).
image_t *image_create(const char *base_path, const char *delta_dir);

// Reopen an existing writable image instance.  `instance_path` is the stem
// (no extension) returned by image_path() when the instance was created.
// Fails if the delta+journal files are missing.
image_t *image_open(const char *base_path, const char *instance_path);

// Geometry-aware variants of the three openers above.  Identical behaviour but
// the base is interpreted as `geom.block_size`-byte blocks (0 ⇒ 512).  The
// default openers are thin wrappers over these with { .block_size = 512 }.
image_t *image_open_readonly_with_geometry(const char *base_path, image_geometry_t geom);
image_t *image_create_with_geometry(const char *base_path, const char *delta_dir, image_geometry_t geom);
image_t *image_open_with_geometry(const char *base_path, const char *instance_path, image_geometry_t geom);

// Create a brand-new, writable, all-zero image with NO backing base file:
// `block_count` blocks of geom.block_size bytes, every block reading back as
// zeros until written.  The delta+journal live in a process-local scratch dir
// and are removed when the image is closed (like a read-only ghost mount), so
// the image is ephemeral unless its contents are written out via
// image_export_to().  Used for a blank ProFile (no source file to open).
image_t *image_create_blank(uint64_t block_count, image_geometry_t geom);

// Returns the instance path stem for a writable image, suitable for
// persisting in checkpoints and feeding back into image_open().
// Returns NULL for read-only images.
const char *image_path(const image_t *image);

// Close a disk image and release resources
void image_close(image_t *image);

// True while an image opened writable (image_create / image_open) from
// `canonical_path` is still open.  The image VFS mounts files read-only, and
// guest writes to a writable image land in its delta, so the VFS asks this
// before serving a file and refuses with -EBUSY rather than serve the stale
// base.  The key is the path the caller named, canonicalised with realpath()
// (or taken as given when that fails), not a decoded scratch copy.
bool image_path_is_open_writable(const char *path);

// True while a writable image's source key is `key` or contains it (a key
// naming something inside that image: "<key>/partition1/...", "<key>#dc42").
// The VFS asks this of every mount, whatever path reached it.
bool image_key_is_open_writable(const char *key);

// Bits of the per-image flags byte image_checkpoint writes.
#define IMAGE_CKPT_WRITABLE 0x01
#define IMAGE_CKPT_WRAPPED  0x02 // re-wrap on restore (image_wrap.h)

// Write image metadata to checkpoint
void image_checkpoint(const image_t *image, checkpoint_t *checkpoint);

// === Operations ===

// Read/write raw data from/to the disk image
size_t disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size);

// Read a logical sector's DiskCopy tag (per-sector metadata) into `buf`.
// Returns the number of tag bytes copied (0 if the image has no tags or the
// sector is out of range).  Used by the Lisa floppy controller to populate the
// boot-block header the ROM validates (FILEID = $AAAA).
size_t disk_read_tag(image_t *disk, size_t sector, uint8_t *buf, size_t size);
size_t disk_write_tag(image_t *disk, size_t sector, const uint8_t *buf, size_t size);

size_t disk_write_data(image_t *disk, size_t offset, uint8_t *buf, size_t size);

// Get the size of the disk image in bytes
size_t disk_size(image_t *disk);

// Bytes per block the image was opened with (512 unless a geometry said
// otherwise); disk_read_data/disk_write_data work in whole blocks of it.
uint32_t disk_block_size(image_t *disk);

// Add an image to the config for tracking
void add_image(config_t *sim, image_t *image);

// Tick all tracked images (drives storage consolidation)
void image_tick_all(config_t *config);

// Returns the full path+name used to open the image
const char *image_get_filename(const image_t *image);

// A machine's restored images, handed to the media controllers as a
// construction argument: each resolves the media it saved by name.
typedef struct image_list {
    image_t *const *items;
    int n;
} image_list_t;

// The image in `images` whose image_get_filename is `name`, or NULL (also
// when `images` is NULL).
image_t *images_find(const image_list_t *images, const char *name);

// Create an empty disk image file of the specified size (for checkpoint restore)
int image_create_empty(const char *filename, size_t size);

// Write a blank image of `size` zero bytes as UDIF (udif_writer.h): one zero
// run, a couple of KB whatever the size.  0, or -1 on failure (including an
// existing file).
int image_create_empty_udif(const char *filename, uint64_t size);

// Create a new blank floppy image file (800K or 1440K)
int image_create_blank_floppy(const char *filename, bool overwrite, bool high_density);

// On-the-wire ProFile block size: 512 data bytes + a 20-byte inline tag (see
// src/machines/lisa/lisa_profile.c).  A raw ProFile image is simply
// block_count × this, all zero for a blank disk.
#define PROFILE_BLOCK_BYTES 532u

// Create a new blank ProFile (Lisa/XL parallel hard disk) image file: a raw,
// all-zero file of block_count × PROFILE_BLOCK_BYTES bytes.  The controller's
// synthesized device-info block reports this block count, so the OS sizes the
// volume from it (5 MB ProFile = 9728 blocks; 10 MB Widget ≈ 19448 blocks).
// Refuses to overwrite an existing file.  Returns 0 on success, -2 if the file
// already exists, -1 on any other error.
int image_create_blank_profile(const char *filename, uint32_t block_count);

// Export the full disk content (base + delta) of an open image to a new file.
// Returns 0 on success, -1 on failure.
int image_export_to(image_t *image, const char *dest_path);

// The same export in three steps, so the write can run off the emulator
// thread (an I/O job, io_leaf.h): begin on the emulator thread (refuses an
// existing destination; snapshots the read side and write-locks the
// storage: storage.h), run on any thread (0, -errno, -ECANCELED; a partial
// file is removed on failure), end on the emulator thread.
typedef struct image_export image_export_t;
image_export_t *image_export_begin(image_t *image, const char *dest_path, char *err, size_t err_cap);
int image_export_run(image_export_t *e, char *err, size_t err_cap);
void image_export_end(image_export_t *e);

// Setup images from config
extern void setup_images(config_t *config);

#endif // IMAGE_H
