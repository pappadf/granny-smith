// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iicx_internal.h
// Shared internals for the IIcx / IIx pair.  Both machines run the same
// GLUE-driven I/O map, share dual-VIA / 68030 / Universal-ROM peripherals,
// and only diverge in a handful of decisions: slot table, machine-ID bits,
// soft-power policy, sound-jack policy.  iix.c reuses iicx.c's internals
// via this header so the family-shared code lives in one place — a
// pragmatic stand-in for the proper §3.1 glue030 extraction that lands
// when a third caller arrives.

#ifndef IICX_INTERNAL_H
#define IICX_INTERNAL_H

#include "common.h"
#include "mac030_glue.h"
#include "memory.h"
#include "mmu.h"
#include "system_config.h" // for config_t.machine_context
#include <stdbool.h>
#include <stdint.h>

struct adb;
struct asc;
struct floppy;

// IIcx/IIx state is the unified GLUE state struct (mac030_glue.h).  The IIcx
// uses last_via2_port_b / soft_power_armed for its PB2 soft-power detector;
// both machines leave the SE/30 vram/vrom/video_card members NULL.
typedef mac030_glue_state_t iicx_state_t;

static inline iicx_state_t *iicx_state(config_t *cfg) {
    return (iicx_state_t *)cfg->machine_context;
}

// Family-shared helpers — defined in iicx.c, called from iix.c.

void iicx_set_rom_overlay(struct config *cfg, bool overlay);

// I/O dispatch is the shared GLUE dispatcher (mac030_glue_io.h).

// VIA1 callbacks — identical between IIcx and IIx (no PA6 buffer-select).
void iicx_via1_output(void *context, uint8_t port, uint8_t output);
void iicx_via1_shift_out(void *context, uint8_t byte);

// SCC IRQ (identical).

// Memory layout (RAM/ROM aliasing + I/O dispatcher registration).
void iicx_memory_layout_init(struct config *cfg);

// IRQ source bit assignments.
#define IICX_IRQ_VIA1 (1 << 0)
#define IICX_IRQ_VIA2 (1 << 1)
#define IICX_IRQ_SCC  (1 << 2)
#define IICX_IRQ_NMI  (1 << 3)

// (I/O bus penalties now live with the shared dispatcher, mac030_glue_io.c.)

// Address-space constants.
#define IICX_ROM_START 0x40000000UL
#define IICX_ROM_END   0x50000000UL
#define IICX_IO_BASE   0x50000000UL
#define IICX_IO_SIZE   0x10000000UL

#endif // IICX_INTERNAL_H
