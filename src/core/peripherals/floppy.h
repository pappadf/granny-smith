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
#define FLOPPY_TYPE_IWM   0 // IWM-only (Mac Plus)
#define FLOPPY_TYPE_SWIM  1 // SWIM dual-mode IWM+ISM (SE/30)
#define FLOPPY_TYPE_SWIM3 2 // SWIM III, controller-driven (PDM 6100/7100/8100)

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

int floppy_get_type(const floppy_t *floppy); // FLOPPY_TYPE_IWM | _SWIM | _SWIM3
bool floppy_get_sel(const floppy_t *floppy); // VIA-driven head-select signal

int floppy_drive_track(const floppy_t *floppy, unsigned drive);
int floppy_drive_side(const floppy_t *floppy, unsigned drive);
bool floppy_drive_motor_on(const floppy_t *floppy, unsigned drive);
const char *floppy_drive_disk_path(const floppy_t *floppy, unsigned drive);
// Eject the disk in the given drive — clears the controller's image
// pointer, drops the cached track buffers. Returns true if a disk was
// removed, false if the drive was already empty / index invalid.
bool floppy_drive_eject(floppy_t *floppy, unsigned drive);

// Returns the disk image currently in the given drive (NULL when empty
// or index is out of range). Used by the SWIM IOP behavioural model to
// service block-level read/write/format requests issued by the host's
// .Sony driver through XmtMsg[2].
image_t *floppy_drive_image(const floppy_t *floppy, unsigned drive);

// === SWIM III drive controls ================================================
//
// SWIM3 (PDM) owns the Sony sense/strobe protocol itself — there are no IWM
// state lines to drive the head through — so its controller model moves the
// head, spins the motor and latches the side directly.  Media, geometry and
// the object tree stay here; only these three pieces of drive state are
// written from outside.

// Move the head by `count` tracks, outward (towards 0) or inward; clamps at
// the 0 and NUM_TRACKS-1 stops the way a real drive's head does.
void floppy_swim3_step(floppy_t *floppy, unsigned drive, bool outward, int count);
// Spindle motor latch (the wMotorOn / wMotorOff drive-register strobes).
void floppy_swim3_set_motor(floppy_t *floppy, unsigned drive, bool on);
// Head-select latch (the sense address routes head 0 or head 1's RdData).
void floppy_swim3_set_side(floppy_t *floppy, unsigned drive, int side);

#endif // FLOPPY_H
