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
// freq_factor: CPU-to-VIA clock divisor (e.g. 10 for Plus at 7.8 MHz, 20 for SE/30 at 15.7 MHz)
// output_cb: called when a port output value changes
// shift_cb: called when the shift register completes a shift-out
// irq_cb: called when the aggregate interrupt line changes state
// cb_context: opaque pointer passed to all three callbacks
via_t *via_init(memory_map_t *map, struct scheduler *scheduler, uint8_t freq_factor, via_output_fn output_cb,
                via_shift_out_fn shift_cb, via_irq_fn irq_cb, void *cb_context, checkpoint_t *checkpoint);

// Set a custom instance name for event type registration (e.g. "via1", "via2").
// Must be called before scheduler_start() if using checkpoints on multi-VIA machines.
void via_set_instance_name(via_t *via, const char *name);

void via_delete(via_t *via);

void via_checkpoint(via_t *restrict via, checkpoint_t *checkpoint);

// === Operations ===

// Input signals to the VIA
extern void via_input(via_t *via, int port, int pin, bool value);

extern void via_input_sr(via_t *via, uint8_t byte);

extern void via_input_c(via_t *via, int port, int c, bool value);

// Re-drive outputs after initialization of dependent devices (e.g., floppy)
void via_redrive_outputs(via_t *via);

// Read the current shift register value (used by ADB to capture command bytes)
uint8_t via_read_sr(via_t *via);

// Cancel any pending shift-out completion callback.  Called by the ADB module
// when it reads VIA SR directly (CMD/Listen transitions), so the generic
// sr_shift_complete timer does not fire a spurious IFR_SR interrupt.
void via_cancel_pending_shift(via_t *via);

// Get the memory-mapped I/O interface for machine-level address decode
const memory_interface_t *via_get_memory_interface(via_t *via);

// === M7c — object-model accessors ===========================================
//
// Read-only views over the VIA register file used by the `via1` /
// `via2` object classes. Port `which` is 0 (A) or 1 (B); timer
// `which` is 0 (T1) or 1 (T2). All accessors return 0 / false when
// `via` is NULL or the index is out of range, so the object getters
// can ignore those edge cases.

uint8_t via_get_ifr(const via_t *via);
uint8_t via_get_ier(const via_t *via);
uint8_t via_get_acr(const via_t *via);
uint8_t via_get_pcr(const via_t *via);
uint8_t via_get_sr(const via_t *via);

uint8_t via_port_output(const via_t *via, unsigned which);
uint8_t via_port_input(const via_t *via, unsigned which);
uint8_t via_port_direction(const via_t *via, unsigned which);

uint16_t via_timer_counter(const via_t *via, unsigned which);
uint16_t via_timer_latch(const via_t *via, unsigned which);

uint8_t via_get_freq_factor(const via_t *via);

#endif // VIA_H
