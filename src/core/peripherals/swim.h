// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// swim.h
// Public interface for the SWIM (Sander-Wozniak Integrated Machine) floppy disk
// controller (SE/30). The SWIM is a dual-mode controller: IWM mode for GCR
// 400K/800K disks, and ISM mode for MFM 720K/1440K disks.

#ifndef SWIM_H
#define SWIM_H

// === Includes ===
#include "common.h"
#include "image.h"
#include "memory.h"
#include "scheduler.h"

#include <stdbool.h>

// === Type Definitions ===
// Opaque SWIM controller type
struct swim;
typedef struct swim swim_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Initializes the SWIM controller and maps it to memory at the SE/30 address
swim_t *swim_init(memory_map_t *map, struct scheduler *scheduler, checkpoint_t *checkpoint);

// Frees all resources associated with the SWIM controller
void swim_delete(swim_t *swim);

// Saves the SWIM controller state to a checkpoint
void swim_checkpoint(swim_t *restrict swim, checkpoint_t *checkpoint);

// === Operations ===

// Inserts a disk image into the specified drive
int swim_insert(swim_t *swim, int drive, image_t *disk);

// Returns whether a disk is currently inserted in the specified drive
bool swim_is_inserted(swim_t *swim, int drive);

// Sets the VIA-driven SEL signal for head selection (IWM mode compatibility)
void swim_set_sel_signal(swim_t *swim, bool sel);

// Get the memory-mapped I/O interface for machine-level address decode
const memory_interface_t *swim_get_memory_interface(swim_t *swim);

#endif // SWIM_H
