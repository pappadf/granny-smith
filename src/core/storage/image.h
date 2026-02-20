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
enum image_type { image_other, image_fd_ss, image_fd_ds, image_fd_hd, image_hd };

// Image structure (exposed for performance-critical access in floppy controller)
struct image {
    storage_t *storage; // Backing storage engine instance
    char *filename; // Original filename provided by the user
    char *storage_root; // Directory that holds range files for this disk
    size_t raw_size; // Logical size of the image in bytes
    bool writable; // True when the caller requested write access
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

// Open a disk image file
image_t *image_open(const char *filename, bool writeable);

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

// Create a new blank 800K double-sided floppy image file
int image_create_blank_floppy(const char *filename, bool overwrite);

// Setup images from config
extern void setup_images(config_t *config);

#endif // IMAGE_H
