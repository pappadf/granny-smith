// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// oss.h
// Public interface for the Macintosh IIfx Operating System Support chip.

#ifndef OSS_H
#define OSS_H

#include "checkpoint.h"
#include "memory.h"

#include <stdbool.h>
#include <stdint.h>

// Opaque handle for the OSS interrupt controller.
typedef struct oss oss_t;

// Called whenever the OSS pending-source or level map changes.
typedef void (*oss_irq_fn)(void *context);

// Called whenever the OSS ROM-control register is written.
typedef void (*oss_control_fn)(void *context, uint8_t value);

// Creates an OSS instance with optional checkpoint restoration.
oss_t *oss_init(oss_irq_fn irq_cb, oss_control_fn control_cb, void *context, checkpoint_t *checkpoint);

// Frees all resources associated with an OSS instance.
void oss_delete(oss_t *oss);

// Saves OSS state to a checkpoint.
void oss_checkpoint(oss_t *oss, checkpoint_t *checkpoint);

// Returns the memory-mapped I/O interface for machine-level address decode.
const memory_interface_t *oss_get_memory_interface(oss_t *oss);

// Sets or clears one interrupt source latch.
void oss_set_source(oss_t *oss, int source, bool active);

// Sets or clears every source present in a bit mask.
void oss_set_source_mask(oss_t *oss, uint16_t mask, bool active);

// Returns the currently pending OSS source bit mask.
uint16_t oss_pending(const oss_t *oss);

// Returns the programmed CPU interrupt level for one source.
uint8_t oss_level(const oss_t *oss, int source);

// Returns the highest active CPU interrupt level after source masking.
uint8_t oss_highest_ipl(const oss_t *oss);

#endif // OSS_H
