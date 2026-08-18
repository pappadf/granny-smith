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

// Optional per-instance port-A data hooks (used by the Lisa parallel hard disk,
// whose ProFile/Widget controller clocks a byte over port A on every access).
// `handshake` is true for register 1 (ORA/IRA — the access pulses CA2/PSTRB, so
// the device advances its data pointer) and false for register 15 (the
// no-handshake port — a plain level read/write of the state/reply byte).
//  - read hook: returns the byte the device drives onto the port-A input pins.
//  - write hook: receives the byte the host drove onto the port-A output pins.
typedef uint8_t (*via_porta_read_fn)(void *context, bool handshake);
typedef void (*via_porta_write_fn)(void *context, uint8_t value, bool handshake);

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Create a VIA instance with per-instance callback routing.
// freq_factor: CPU-to-VIA clock divisor (e.g. 10 for Plus at 7.8 MHz, 20 for SE/30 at 15.7 MHz)
// output_cb: called when a port output value changes
// shift_cb: called when the shift register completes a shift-out
// irq_cb: called when the aggregate interrupt line changes state
// cb_context: opaque pointer passed to all three callbacks
// `name` ("via1" / "via2") tags scheduler events for checkpointing and is
// used as the object-tree node name. Must be a string literal or otherwise
// outlive the via_t.
// The 6522's φ2 clock on every Macintosh that has one: 783,360 Hz (the Plus's
// 7.8336 MHz CPU clock divided by 10).  Timer periods are specified in φ2
// cycles, so a machine's `freq_factor` — the CPU-cycles-per-φ2-cycle divisor
// the scheduler works in — is its CPU clock divided by this.
#define VIA_PHI2_HZ 783360u

// Derive the freq_factor for a CPU clock, rounded to nearest and clamped to the
// uint8_t the constructor takes.  Use this rather than hardcoding a divisor:
// a literal that is right for one member of a machine family is wrong for any
// sibling with a different clock, and the symptom is timers running fast or
// slow by exactly that ratio.
static inline uint8_t via_freq_factor_for_clock(uint32_t cpu_hz) {
    uint32_t f = (cpu_hz + VIA_PHI2_HZ / 2) / VIA_PHI2_HZ;
    if (f < 1)
        f = 1;
    if (f > 255)
        f = 255;
    return (uint8_t)f;
}

via_t *via_init(memory_map_t *map, struct scheduler *scheduler, uint8_t freq_factor, const char *name,
                via_output_fn output_cb, via_shift_out_fn shift_cb, via_irq_fn irq_cb, void *cb_context,
                checkpoint_t *checkpoint);

void via_delete(via_t *via);

void via_checkpoint(via_t *restrict via, checkpoint_t *checkpoint);

// === Operations ===

// Input signals to the VIA
extern void via_input(via_t *via, int port, int pin, bool value);

extern void via_input_sr(via_t *via, uint8_t byte);

extern void via_input_c(via_t *via, int port, int c, bool value);

// Re-drive outputs after initialization of dependent devices (e.g., floppy)
void via_redrive_outputs(via_t *via);

// Install optional port-A data hooks (see via_porta_read_fn / via_porta_write_fn).
// Pass NULL hooks to detach.  `ctx` is passed to both.
void via_set_porta_hooks(via_t *via, via_porta_read_fn read_fn, via_porta_write_fn write_fn, void *ctx);

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

// Install exact-rational φ2 timing (ticks = cycles × 783360/cpu_hz, reduced)
// for machines whose CPU clock is not an integer multiple of VIA_PHI2_HZ —
// the rounded via_freq_factor_for_clock divisor would run their timers
// measurably fast or slow (PDM at 60 MHz: 0.54% slow).  Call after
// via_init; the integer-divisor machines keep the historical arithmetic.
void via_set_exact_clock(via_t *via, uint32_t cpu_hz);

#endif // VIA_H
