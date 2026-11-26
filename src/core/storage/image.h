// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image.h
// Public interface for disk image management.

#ifndef IMAGE_H
#define IMAGE_H

// === Includes ===
#include "checkpoint.h"
#include "common.h"
#include "image_kind.h"
#include "storage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Forward Declarations ===
struct config;
typedef struct config config_t;

// === Type Definitions ===
// A disk image: opaque outside the storage module (image_internal.h)
struct image;
typedef struct image image_t;

// === Accessors ===
// The kind the device that opened it presents it as (image_open_kind);
// IMAGE_KIND_NONE for an image no device holds
image_kind_t image_get_kind(const image_t *image);
// The floppy layout its size implies (IMAGE_LAYOUT_NONE: not floppy-sized)
image_layout_t image_layout(const image_t *image);
// True when the caller opened the image for writing
bool image_is_writable(const image_t *image);
// Logical size in bytes (a synthesised wrapper prefix included)
uint64_t image_get_raw_size(const image_t *image);
// Wrapper layers peeled to reach the disk: "raw", "dc42", "bin+ndif", ...
const char *image_get_format(const image_t *image);
// Key of the source the image's path opened (source.h), or NULL
const char *image_get_source_key(const image_t *image);
// The delta and journal files of a writable image, or NULL
const char *image_get_delta_path(const image_t *image);
const char *image_get_journal_path(const image_t *image);
// The backing storage engine instance
storage_t *image_get_storage(const image_t *image);
// disk_read_data / disk_write_data calls since open (drive_activity.h)
uint64_t image_get_reads(const image_t *image);
uint64_t image_get_writes(const image_t *image);
// Reads the backing store could not serve (each a guest read error)
uint64_t image_get_read_errors(const image_t *image);
// Tag bytes stored beside each 512-byte sector: 12 (GCR floppy), 20 (ProFile) or 0
uint32_t image_get_tag_bytes(const image_t *image);
// True when sector `block` is in the image's delta (written since its base,
// as far as the storage's modification bitmap knows; storage.h)
bool image_block_in_delta(image_t *image, uint64_t block);

// === Lifecycle (Constructor / Destructor / Checkpoint) ===
//
// The module keeps no state of its own to set up or tear down: each image
// is opened and closed by its owner.

// Open a base image read-only.  Delta and journal are placed in a process-local
// scratch directory and removed when the image is closed.
//
// Every opener takes a path the VFS resolves: a host file, or a file inside
// an image or archive (outer.img/partition1/inner.img, disks.zip/a.dsk).
// Wrapper formats are peeled through the format registry -- UDIF, NDIF,
// QCOW2, MOOF, DiskCopy 4.2, MacBinary, BinHex, gzip, in any nesting -- and the
// storage engine reads the innermost source directly: nothing is decoded to a
// file (a MOOF is decoded to its sectors in memory).
image_t *image_open_readonly(const char *base_path);

// Why this thread's last image open failed when the file is a container
// image (UDIF, NDIF, QCOW2) that cannot be read -- "a QCOW2 image that
// cannot be read in place (it uses ...)" -- or "" (it opened, or was no
// such container).  Valid until the thread's next open.
const char *image_open_error(void);

// The same over forks already open (the VFS mounting a file it holds).
// `name` is what image_get_filename reports.
typedef struct peel_source source_t; // source.h's byte source, without peeler.h
image_t *image_open_readonly_source(const char *name, source_t *data, source_t *rsrc);

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

// Write image metadata to checkpoint, then its storage (blocks, tags in-band)
void image_checkpoint(const image_t *image, checkpoint_t *checkpoint);

// === Operations ===

// The data plane: whole 512-byte sectors' data at `offset` (whole blocks of
// disk_block_size on an image without tags).  A tagged image's sectors keep
// their tags on a write.
size_t disk_read_data(image_t *disk, uint64_t offset, uint8_t *buf, size_t size);

// A sector's tag (the 12 bytes beside a GCR sector, a ProFile block's 20):
// in-band, in the sector's block, so it lands in the delta with the data.
// Returns the bytes copied, at most the image's tag size (0 for an image
// without tags, a sector out of range, or a write to a write-protected
// image).  The floppy controllers read and write them with each sector (the
// Lisa boot ROM validates the boot block's FILEID = $AAAA).
size_t disk_read_tag(image_t *disk, size_t sector, uint8_t *buf, size_t size);
size_t disk_write_tag(image_t *disk, size_t sector, const uint8_t *buf, size_t size);

// Whole blocks as stored, tag first ([tag][512 data]): `count` blocks of
// disk_block_size from block `first`.  What a ProFile moves (its 532-byte
// wire block is its stored one).  Bytes moved, 0 on failure.
size_t disk_read_units(image_t *disk, uint64_t first, uint8_t *buf, size_t count);
size_t disk_write_units(image_t *disk, uint64_t first, const uint8_t *buf, size_t count);

// Label the disk for a DiskCopy 4.2 export: the name and format byte its
// header gets.  A machine whose disks differ from the Mac's defaults (the
// Lisa: "-not a Macintosh disk-", $02) sets them on insert; a DiskCopy
// source already carries its own, which this leaves alone.
void image_set_diskcopy_identity(image_t *image, const char *name, uint8_t format_byte);

size_t disk_write_data(image_t *disk, uint64_t offset, uint8_t *buf, size_t size);

// The disk's data bytes (sectors x 512; a tagged image's tags not counted).
// 0 for a NULL image -- no opened image is empty (the openers refuse a
// zero-length one), so 0 always means "no image".
uint64_t disk_size(image_t *disk);

// Bytes per stored block: 512 plus the tags each sector carries (524 for a
// GCR floppy, 532 for a ProFile).  disk_read_units moves whole ones.
uint32_t disk_block_size(image_t *disk);

// Tick all tracked images (drives storage consolidation)
void image_tick_all(config_t *config);

// Returns the full path+name used to open the image
const char *image_get_filename(const image_t *image);

// A machine's restored images, handed to the media controllers as a
// construction argument: each resolves the media it saved by position.
typedef struct image_list {
    image_t *const *items;
    int n;
} image_list_t;

// What checkpoint_write_image_ref writes for an empty drive.
#define IMAGE_REF_NONE UINT32_MAX

// Record `img`'s position in the image list a checkpoint is saving (the
// images part, before any drive refers to it); IMAGE_REF_NONE for an image
// the list does not hold.
void image_set_checkpoint_index(image_t *img, uint32_t index);

// Write which image a drive holds (image_ref.c): its position in the
// checkpoint's image list, or IMAGE_REF_NONE for NULL.
void checkpoint_write_image_ref(checkpoint_t *cp, const image_t *img);

// Read what checkpoint_write_image_ref wrote and resolve it in `images`.
// NULL for "no image"; a position the list does not hold logs "<what>: the
// checkpoint's image N is not among its images" and flags `cp`.  `held` (may
// be NULL) is set to whether the drive held an image at all.
image_t *checkpoint_read_image_ref(checkpoint_t *cp, const image_list_t *images, const char *what, bool *held);

// Create an empty disk image file of the specified size (for checkpoint restore)
int image_create_empty(const char *filename, uint64_t size);

// Write a blank image of `size` zero bytes as UDIF (udif_writer.h): one zero
// run, a couple of KB whatever the size.  0, or -1 on failure (including an
// existing file).
int image_create_empty_udif(const char *filename, uint64_t size);

// A new all-zero file of `bytes` bytes at `filename` (parent directories
// made), created exclusively: 0, IMAGE_CREATE_EXISTS when the file is there
// already (it is never overwritten), -1 on any other error.  files.create
// (image_create_file_kind) is its caller.
#define IMAGE_CREATE_EXISTS (-2)
int image_create_zeros(const char *filename, uint64_t bytes);

// Export the full disk content (base + delta) of an open image to a new file.
// The destination's name picks the format: .dmg is UDIF; .dc42 and
// .diskcopy are DiskCopy 4.2 with the sector tags (floppies only), as is
// .image for a floppy -- the classic Mac name for a DiskCopy file; .moof
// is an Applesauce MOOF bitstream image (400K/800K floppies only); any
// other name is the flat raw image.  Whatever the format, the destination
// is created exclusively (an existing file is never overwritten) and an
// unreadable base block fails the export.  Returns 0 on success, -1 on
// failure.
int image_export_to(image_t *image, const char *dest_path);

// image_export_to with a codec for a .dmg destination: "zlib" (UDZO, the
// default; NULL or "" mean it too) or "xz" (ULMO, 14-16% smaller, slower to
// read; udif_writer.h).  A codec other than zlib for any other destination
// fails the export.
int image_export_to_codec(image_t *image, const char *dest_path, const char *codec);

// The same export in three steps, so the write can run off the emulator
// thread (an I/O job, io_leaf.h): begin on the emulator thread (refuses an
// existing destination; snapshots the read side and write-locks the
// storage: storage.h), run on any thread (0, -errno, -ECANCELED; a partial
// file is removed on failure), end on the emulator thread.
typedef struct image_export image_export_t;
image_export_t *image_export_begin(image_t *image, const char *dest_path, char *err, size_t err_cap);
// Between begin and run: the codec of a .dmg export (as image_export_to_codec
// takes it).  0, or -1 with the reason in `err`.
int image_export_set_codec(image_export_t *e, const char *codec, char *err, size_t err_cap);
int image_export_run(image_export_t *e, char *err, size_t err_cap);
void image_export_end(image_export_t *e);

#endif // IMAGE_H
