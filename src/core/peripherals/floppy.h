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

// === M7e — object-model accessors ===========================================
//
// Read-only views over the floppy controller and its two drive slots
// used by `floppy` / `floppy.drives` object classes. Drive index is
// 0 (internal) or 1 (external). Out-of-range indices return zero /
// false / NULL so the object getters can stay branch-free.

int floppy_get_type(const floppy_t *floppy); // FLOPPY_TYPE_IWM | FLOPPY_TYPE_SWIM
bool floppy_get_sel(const floppy_t *floppy); // VIA-driven head-select signal

int floppy_drive_track(const floppy_t *floppy, unsigned drive);
int floppy_drive_side(const floppy_t *floppy, unsigned drive);
bool floppy_drive_motor_on(const floppy_t *floppy, unsigned drive);
const char *floppy_drive_disk_path(const floppy_t *floppy, unsigned drive);
// Eject the disk in the given drive — clears the controller's image
// pointer, drops the cached track buffers. Returns true if a disk was
// removed, false if the drive was already empty / index invalid.
bool floppy_drive_eject(floppy_t *floppy, unsigned drive);

#endif // FLOPPY_H
