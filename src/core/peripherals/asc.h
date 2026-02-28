// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// asc.h
// Public interface for Apple Sound Chip (ASC) emulation (SE/30).

#ifndef ASC_H
#define ASC_H

// === Includes ===

#include "common.h"
#include "memory.h"
#include "scheduler.h"
#include "via.h"

#include <stdint.h>

// === Forward Declarations ===

struct asc;

// === Type Definitions ===

// Opaque handle for the ASC peripheral state
typedef struct asc asc_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Creates and initialises an ASC instance, registering it in the memory map
asc_t *asc_init(memory_map_t *map, scheduler_t *scheduler, checkpoint_t *checkpoint);

// Frees all resources associated with an ASC instance
void asc_delete(asc_t *asc);

// Saves ASC state to a checkpoint
void asc_checkpoint(asc_t *restrict asc, checkpoint_t *checkpoint);

// === Wiring ===

// Connects the ASC interrupt output to VIA2 CB1 (called after VIA2 creation)
void asc_set_via(asc_t *asc, via_t *via);

// === Operations ===

// Renders audio samples into a stereo interleaved 16-bit signed buffer.
// Called by the platform audio callback; drains FIFO or advances wavetable.
void asc_render(asc_t *asc, int16_t *buffer, int nsamples);

#endif // ASC_H
