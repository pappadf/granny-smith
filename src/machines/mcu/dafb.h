// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dafb.h
// DAFB built-in video (Direct Access Frame Buffer): the Quadra family's
// stand-alone video controller — DAFB core registers + Swatch CRTC (+$100)
// + AC842/AC842a RAMDAC + DP8531 pixel clock, driving a dedicated VRAM
// aperture at $F9000000 with registers at $F9800000 (reference §11).
//
// Phase C scope: the register aperture is a stored, logged register file
// (accept-and-log with readback — Trap 24: no silent zeros) plus the VRAM
// buffer, enough for the boot ROM's DAFB probe and RAM-test phases.  The
// real Swatch timing, RAMDAC CLUT, pixel unpacking, and monitor sense land
// in Phase D.

#ifndef GS_MACHINES_MCU_DAFB_H
#define GS_MACHINES_MCU_DAFB_H

#include "checkpoint.h"
#include "memory.h"

#include <stdbool.h>
#include <stdint.h>

// Fixed CPU-visible apertures (reference §5.2 [R])
#define DAFB_VRAM_BASE     0xF9000000u
#define DAFB_VRAM_APERTURE 0x00200000u // 2 MiB CPU window
#define DAFB_REG_BASE      0xF9800000u
#define DAFB_REG_APERTURE  0x00001000u // registers live in the low $400

// Number of longword register slots ($000-$3FF)
#define DAFB_REG_COUNT 256

struct scheduler;

typedef struct dafb dafb_t;

// Create the DAFB with `vram_size` bytes of installed VRAM (512 KiB / 1 MiB /
// 2 MiB; the CPU aperture is fixed at 2 MiB regardless — Trap 12).
dafb_t *dafb_init(uint32_t vram_size, checkpoint_t *cp);

// Attach the scheduler and start the Swatch frame event: the interrupt-status
// register (+$108: bit 0 VBL, bit 2 cursor) gets its pending bits raised once
// per frame so the ROM's scanline-wait polls make progress.  Phase C runs a
// fixed 60.15 Hz cadence; Phase D derives the period from the programmed
// Swatch timing instead (Traps 9/10).
void dafb_attach_scheduler(dafb_t *dafb, struct scheduler *sched);
void dafb_delete(dafb_t *dafb);
void dafb_checkpoint(dafb_t *dafb, checkpoint_t *cp);

// Host pointer to the VRAM buffer (for page-table mapping).
uint8_t *dafb_vram(dafb_t *dafb);
uint32_t dafb_vram_size(dafb_t *dafb);

// Register-aperture memory interface (registered at DAFB_REG_BASE).
const memory_interface_t *dafb_reg_interface(dafb_t *dafb);

// Reset to power-on state (registers cleared; VRAM preserved).
void dafb_reset(dafb_t *dafb);

#endif // GS_MACHINES_MCU_DAFB_H
