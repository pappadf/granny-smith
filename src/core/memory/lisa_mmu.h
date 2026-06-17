// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_mmu.h
// Apple Lisa custom segment MMU (NOT a Motorola PMMU).
//
// A 1K x 12-bit descriptor RAM mapping 128 logical segments x 4 contexts onto
// three disjoint physical spaces (main RAM / I/O / special-I/O), with 512-byte
// pages and a power-on START (setup) mode that bypasses translation.  See
// docs/lisa.md §4-5 and proposal-machine-lisa-xl.md §4.2 for the model.
//
// Integration seam: the Lisa machine routes ALL CPU memory accesses through
// this module via the slow path in memory.c (gated on g_lisa_mmu != NULL).
// The Lisa fast-path SoA cache is intentionally left empty in this first cut,
// so every access takes the slow path and is fully translated here; SoA
// fast-path filling is a later performance optimisation.

#ifndef LISA_MMU_H
#define LISA_MMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

struct memory_interface;
typedef struct memory_interface memory_interface_t;

typedef struct lisa_mmu lisa_mmu_t;

// Global active Lisa MMU.  Non-NULL only while a Lisa/XL machine is active;
// memory.c's slow path delegates to lisa_mmu_* when this is set.  NULL for
// every Mac-architecture machine (which use g_mmu / the direct page table).
extern lisa_mmu_t *g_lisa_mmu;

// === Lifecycle =============================================================

// Create the segment MMU over the machine's physical RAM and 16 KB boot ROM.
// Installs itself as g_lisa_mmu.  Power-on state: START set, descriptors
// cleared (so contexts 1-3 read back as "unprogrammed").
lisa_mmu_t *lisa_mmu_init(uint8_t *ram, uint32_t ram_size, uint8_t *rom, uint32_t rom_size, checkpoint_t *cp);

// Tear down and clear g_lisa_mmu.
void lisa_mmu_delete(lisa_mmu_t *m);

// Save / restore descriptor RAM + latches (checkpoint parity).
void lisa_mmu_checkpoint(lisa_mmu_t *m, checkpoint_t *cp);

// === I/O-space device registration =========================================
//
// Lisa peripherals live at PHYSICAL I/O addresses ($00C000-$00FFFF); the MMU's
// I/O space tag routes a translated access here.  Machine code registers each
// device's physical range so later steps (VIA, SCC, floppy, Widget) can hook
// in.  `base`/`size` are physical I/O addresses; reads/writes are dispatched
// with the offset already subtracted (addr - base), matching memory_interface.
void lisa_mmu_map_io(lisa_mmu_t *m, uint32_t phys_base, uint32_t size, memory_interface_t *iface, void *dev);

// Current video framebuffer physical base, derived from the Video Address
// Latch ($00E800): the latch holds A15-A20, i.e. base = latch << 15, masked
// into installed RAM.  The machine reads this each frame to locate the
// framebuffer (docs/lisa.md §8 / §6.2).
uint32_t lisa_mmu_video_base(const lisa_mmu_t *m);

// True when the vertical-retrace (VBL) interrupt is enabled (VTMSK latch).
bool lisa_mmu_vbl_enabled(const lisa_mmu_t *m);

// Set the level-7 parity-NMI callback.  The MMU asserts it (active=true) when a
// read hits a location written with deliberately-bad parity (the ROM's PARTST),
// and clears it (active=false) on PAROFF.  The machine routes it to the CPU IPL.
void lisa_mmu_set_nmi(lisa_mmu_t *m, void (*cb)(void *ctx, bool active), void *ctx);

// Pulse the Status Register vertical-retrace bit (bit 2) for one frame so the
// ROM's video test / the OS VBL handler observe a retrace.  Called by the
// machine's trigger_vbl.  Returns the prior bit state.
void lisa_mmu_set_vbl_active(lisa_mmu_t *m, bool active);

// Provide the cycle counter the Status Register's vertical-retrace bit is
// derived from.  The video state machine scans continuously, so bit 2 is a pure
// function of the cycle position within the ~60 Hz frame (high during the
// retrace window, low during active scan) — modelled from this clock.
struct scheduler;
void lisa_mmu_set_clock(lisa_mmu_t *m, struct scheduler *sched);

// === CPU slow-path delegates (called from memory.c) =========================
//
// `supervisor` selects the MMU context: supervisor mode forces context 0;
// user mode uses the SEG1/SEG2 latch context.  These perform the full segment
// translation, space routing, and limit/permission checks, raising a 68000
// bus error (via the g_bus_error_* globals) on a violation.
uint8_t lisa_mmu_read8(uint32_t addr, bool supervisor);
uint16_t lisa_mmu_read16(uint32_t addr, bool supervisor);
uint32_t lisa_mmu_read32(uint32_t addr, bool supervisor);
void lisa_mmu_write8(uint32_t addr, bool supervisor, uint8_t value);
void lisa_mmu_write16(uint32_t addr, bool supervisor, uint16_t value);
void lisa_mmu_write32(uint32_t addr, bool supervisor, uint32_t value);

// Side-effect-free read for debug/inspection (memory.peek/.dump): translate and
// read RAM/ROM, return all-ones for unmapped/invalid, never fault or strobe.
uint32_t lisa_mmu_debug_read(uint32_t addr, unsigned size, bool supervisor);

// Side-effect-free debug write (memory.poke): write only when the access
// resolves to main RAM; never touch I/O (no strobes), ROM, or descriptors.
// Returns true if the value landed in RAM.
bool lisa_mmu_debug_write(uint32_t addr, unsigned size, bool supervisor, uint32_t value);

#endif // LISA_MMU_H
