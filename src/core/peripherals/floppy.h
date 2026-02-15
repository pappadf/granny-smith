// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy.h
// Public interface for the IWM floppy disk controller module.

#ifndef FLOPPY_H
#define FLOPPY_H

// === Includes ===
#include "common.h"
#include "image.h"
#include "memory.h"
#include "scheduler.h"

#include <stdbool.h>

// === Type Definitions ===
// Opaque floppy controller type
struct floppy;
typedef struct floppy floppy_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===
// Initializes the floppy controller and maps it to memory
floppy_t *floppy_init(memory_map_t *map, struct scheduler *scheduler, checkpoint_t *checkpoint);
// Frees all resources associated with the floppy controller
void floppy_delete(floppy_t *floppy);
// Saves the floppy controller state to a checkpoint
void floppy_checkpoint(floppy_t *restrict floppy, checkpoint_t *checkpoint);

// === Operations ===
// Inserts a disk image into the specified drive
int floppy_insert(floppy_t *floppy, int drive, image_t *disk);
// Returns whether a disk is currently inserted in the specified drive
bool floppy_is_inserted(floppy_t *floppy, int drive);
// Sets the VIA-driven SEL signal for head selection
void floppy_set_sel_signal(floppy_t *floppy, bool sel);

#endif // FLOPPY_H
