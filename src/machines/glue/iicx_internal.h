// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iicx_internal.h
// Shared internals for the IIcx / IIx pair.  Both machines run the same
// GLUE-driven I/O map, share dual-VIA / 68030 / Universal-ROM peripherals,
// and only diverge in a handful of decisions: slot table, machine-ID bits,
// soft-power policy, sound-jack policy.  iix.c reuses iicx.c's internals via
// this header; the family-shared lifecycle lives in m68k_board.c (the GLUE
// substrate) on the 68k board runtime (runtime/m68k_board*, board_io).

#ifndef IICX_INTERNAL_H
#define IICX_INTERNAL_H

#include "common.h"
#include "m68k_board.h"
#include "memory.h"
#include "mmu.h"
#include "system_internal.h" // for config_t.machine_context
#include <stdbool.h>
#include <stdint.h>

struct adb;
struct asc;
struct floppy;

// IIcx/IIx state is the unified GLUE state struct (m68k_board.h).  The IIcx
// uses soft_power_armed for its PB2 soft-power detector; both machines leave
// the SE/30 vram/vrom/video_card members NULL.
typedef glue_state_t iicx_state_t;

static inline iicx_state_t *iicx_state(config_t *cfg) {
    return (iicx_state_t *)cfg->machine_context;
}

// Family-shared helpers — defined in iicx.c, called from iix.c.

void iicx_set_rom_overlay(struct config *cfg, bool overlay);

// I/O dispatch is the shared GLUE dispatcher (glue_io.c).

// VIA1 callbacks — identical between IIcx and IIx (no PA6 buffer-select).
void iicx_via1_output(void *context, uint8_t port, uint8_t output);

// SCC IRQ (identical).

// The IIcx/IIx memory-layout tail (NuBus card host regions + ROM overlay);
// RAM/ROM/IO are the family's, in glue_memory_layout().
void iicx_memory_layout_tail(struct config *cfg);

// IRQ source bits: the family's M68K_BOARD_IRQ_* (m68k_board.h).

// The RAM sizes the IIcx and IIx offer (KB, 0-terminated) -- one table, as
// both boards take the same SIMMs in the same two banks of four.
extern const uint32_t iicx_iix_ram_options_kb[];

// The board descriptor the IIcx and IIx share byte for byte (iicx.c).
extern const m68k_board_desc_t iicx_iix_board_desc;

// (I/O bus penalties now live with the shared dispatcher, glue_io.c.)

// Address-space constants.
#define IICX_ROM_START 0x40000000UL

#endif // IICX_INTERNAL_H
