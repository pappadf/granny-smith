// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_build.h
// The construction prefix every PowerPC Macintosh family shares (PDM, TNT,
// Gossamer): the memory map, the CPU and the scheduler on the PPC seam; the
// low-speed spine (RTC, SCC, AppleTalk, ImageWriter, VIA1); and ADB with the
// behavioral Cuda on that VIA1.  The 68k twin is m68k_board_build_core /
// m68k_board_build_lowspeed (m68k_board.c).
//
// Each step registers its checkpoint parts as it builds them, with the same
// names in the same order the families used, so a checkpoint's layout is
// unchanged (machine_parts.h).  What differs per family is data: the page
// filler, CPI, the timebase rate, the board's PVR/HID1 straps, the SCC RTxC
// clock and the VIA1 / SCC callbacks.

#ifndef GS_MACHINES_RUNTIME_PPC_BUILD_H
#define GS_MACHINES_RUNTIME_PPC_BUILD_H

#include "checkpoint.h"

#include <stdbool.h>
#include <stdint.h>

struct cuda;
struct config;

// === Type Definitions =======================================================

// The per-family choices of the core step.
typedef struct ppc_core_opts {
    // Host-backed regions core code registers on the bus map (a card's VRAM
    // and declaration ROM) are filled through the family's page filler: no
    // 68k MMU owns a PowerPC machine's page table.
    void (*fill_page)(uint32_t page_index, uint8_t *host_ptr, bool writable);
    uint32_t cpi; // the scheduler's cycles per instruction
    uint32_t tb_hz; // the timebase/decrementer tick (the 601's RTC input)
    // The board's PVR and HID1 (PLL straps), written right after the CPU is
    // built (and restored) when `set_identity` is true.
    bool set_identity;
    uint32_t pvr, hid1;
} ppc_core_opts_t;

// The VIA1 callbacks (Cuda transport) and the SCC IRQ sink of the low-speed
// step, with the SCC's RTxC clock (PCLK is 15.6672 MHz on every board).
typedef struct ppc_lowspeed_opts {
    void (*scc_irq)(void *ctx, bool active);
    uint32_t scc_rtxc_hz;
    void (*via1_output)(void *ctx, uint8_t port, uint8_t value);
    void (*via1_shift_out)(void *ctx, uint8_t byte);
    void (*via1_irq)(void *ctx, bool active);
} ppc_lowspeed_opts_t;

// === Operations =============================================================

// Memory map, CPU and scheduler, with the timebase bound.  Returns 0, or -1
// (logged) when the CPU cannot be built.
int ppc_build_core(struct config *cfg, checkpoint_t *cp, const ppc_core_opts_t *o);

// RTC, SCC, the AppleTalk connection, the ImageWriter and VIA1 at its idle
// input levels (PB3 Cuda TREQ and the CA1/CB1/CB2 lines high).
void ppc_build_lowspeed(struct config *cfg, checkpoint_t *cp, const ppc_lowspeed_opts_t *o);

// ADB and the behavioral Cuda on VIA1 (Mode3Clock tick on).  Returns the
// Cuda, or NULL (logged) when it cannot be built.
struct cuda *ppc_build_cuda(struct config *cfg, checkpoint_t *cp);

#endif // GS_MACHINES_RUNTIME_PPC_BUILD_H
