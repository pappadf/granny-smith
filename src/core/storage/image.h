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
enum image_type { image_other, image_fd_ss, image_fd_ds, image_fd_hd, image_hd, image_cdrom };

// Image structure (exposed for performance-critical access in floppy controller)
struct image {
    storage_t *storage; // Backing storage engine instance
    char *filename; // Original filename provided by the user (base image)
    char *instance_path; // Stem for delta/journal: "<dir>/<id>" — NULL for read-only ghost mounts
    char *delta_path; // Path to delta file (<instance_path>.delta)
    char *journal_path; // Path to preimage journal (<instance_path>.journal)
    size_t raw_size; // Logical size of the image in bytes
    bool writable; // True when the caller requested write access
    bool ghost_instance; // True when delta+journal are ephemeral scratch (read-only mounts)
    enum image_type type; // Detected image type (floppy, hd, ...)
    bool from_diskcopy; // True if the source file was DiskCopy 4.2
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
image_t *image_open_readonly(const char *base_path);

// Create a new writable image instance.  The image subsystem mints an opaque
// 16-hex-char id internally and creates two files at <delta_dir>/<id>.delta
// and <delta_dir>/<id>.journal.  If `delta_dir` is NULL, the directory of
// `base_path` is used (legacy adjacent-to-base layout).
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

// Write image metadata to checkpoint
void image_checkpoint(const image_t *image, checkpoint_t *checkpoint);

// === Operations ===

// Read/write raw data from/to the disk image
size_t disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size);

size_t disk_write_data(image_t *disk, size_t offset, uint8_t *buf, size_t size);

// Get the size of the disk image in bytes
size_t disk_size(image_t *disk);

// Save modified data to the underlying storage
size_t image_save(image_t *image);

// Add an image to the config for tracking
void add_image(config_t *sim, image_t *image);

// Tick all tracked images (drives storage consolidation)
void image_tick_all(config_t *config);

// Returns the full path+name used to open the image
const char *image_get_filename(const image_t *image);

// Create an empty disk image file of the specified size (for checkpoint restore)
int image_create_empty(const char *filename, size_t size);

// Create a new blank floppy image file (800K or 1440K)
int image_create_blank_floppy(const char *filename, bool overwrite, bool high_density);

// Export the full disk content (base + delta) of an open image to a new file.
// Returns 0 on success, -1 on failure.
int image_export_to(image_t *image, const char *dest_path);

// If `path` is volatile (/tmp/ or /fd/), copy the file to /images/<hash>.img
// and return the persistent path (caller must free).  If already persistent,
// returns a copy of the original path.  Returns NULL on error.
char *image_persist_volatile(const char *path);

// Setup images from config
extern void setup_images(config_t *config);

#endif // IMAGE_H
