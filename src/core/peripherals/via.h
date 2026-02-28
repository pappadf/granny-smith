// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// via.h
// Public interface for Versatile Interface Adapter (VIA) emulation.

#ifndef VIA_H
#define VIA_H

// === Includes ===
#include "common.h"
#include "memory.h"
#include "scheduler.h"

#include <stdbool.h>
#include <stdint.h>

// === Type Definitions ===
struct via;
typedef struct via via_t;

// Callback function types for per-instance output routing.
// Each VIA instance invokes these through stored function pointers,
// allowing multiple VIA instances (e.g. two in Macintosh IIcx)
// to route outputs to different destinations.
typedef void (*via_output_fn)(void *context, uint8_t port, uint8_t value);
typedef void (*via_shift_out_fn)(void *context, uint8_t byte);
typedef void (*via_irq_fn)(void *context, bool active);

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Create a VIA instance with per-instance callback routing.
// output_cb: called when a port output value changes
// shift_cb: called when the shift register completes a shift-out
// irq_cb: called when the aggregate interrupt line changes state
// cb_context: opaque pointer passed to all three callbacks
via_t *via_init(memory_map_t *map, struct scheduler *scheduler, via_output_fn output_cb, via_shift_out_fn shift_cb,
                via_irq_fn irq_cb, void *cb_context, checkpoint_t *checkpoint);

void via_delete(via_t *via);

void via_checkpoint(via_t *restrict via, checkpoint_t *checkpoint);

// === Operations ===

// Input signals to the VIA
extern void via_input(via_t *via, int port, int pin, bool value);

extern void via_input_sr(via_t *via, uint8_t byte);

extern void via_input_c(via_t *via, int port, int c, bool value);

// Re-drive outputs after initialization of dependent devices (e.g., floppy)
void via_redrive_outputs(via_t *via);

// Get the memory-mapped I/O interface for machine-level address decode
const memory_interface_t *via_get_memory_interface(via_t *via);

#endif // VIA_H
