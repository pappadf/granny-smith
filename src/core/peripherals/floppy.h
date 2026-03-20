// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy.h
// Public interface for the unified floppy disk controller module.
// Supports both IWM (Mac Plus) and SWIM (SE/30) controller types.

#ifndef FLOPPY_H
#define FLOPPY_H

// === Includes ===
#include "common.h"
#include "image.h"
#include "memory.h"
#include "scheduler.h"

#include <stdbool.h>

// === Controller Types ===
#define FLOPPY_TYPE_IWM  0 // IWM-only (Mac Plus)
#define FLOPPY_TYPE_SWIM 1 // SWIM dual-mode IWM+ISM (SE/30)

// === Type Definitions ===
// Opaque floppy controller type
struct floppy;
typedef struct floppy floppy_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===
// Initializes a floppy controller of the given type and maps it to memory
floppy_t *floppy_init(int type, memory_map_t *map, struct scheduler *scheduler, checkpoint_t *checkpoint);
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
// Get the memory-mapped I/O interface for machine-level address decode
const memory_interface_t *floppy_get_memory_interface(floppy_t *floppy);

#endif // FLOPPY_H
