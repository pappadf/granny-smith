// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dafb.h
// DAFB built-in video (Direct Access Frame Buffer): the Quadra family's
// stand-alone video controller — DAFB core registers + Swatch CRTC (+$100)
// + AC842/AC842a RAMDAC (+$200) + DP8531 pixel clock (+$300), driving a
// dedicated VRAM aperture at $F9000000 with registers at $F9800000
// (reference §11).  Register semantics follow the reference's [R] tables;
// unknown registers stay accept-and-log with readback (Trap 24).

#ifndef GS_MACHINES_MCU_DAFB_H
#define GS_MACHINES_MCU_DAFB_H

#include "checkpoint.h"
#include "display.h"
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

// Video interrupt output (level; VIA2 PA6 + the /SLOTIRQ aggregate).
typedef void (*dafb_irq_cb)(void *context, bool active);

// Create the DAFB with `vram_size` bytes of installed VRAM (512 KiB / 1 MiB /
// 2 MiB; the CPU aperture is fixed at 2 MiB regardless — Trap 12).
dafb_t *dafb_init(uint32_t vram_size, checkpoint_t *cp);
void dafb_delete(dafb_t *dafb);
void dafb_checkpoint(dafb_t *dafb, checkpoint_t *cp);

// Attach the scheduler: Swatch VBL/cursor events run from the programmed
// timing (a 60.15 Hz fallback covers the pre-mode-set window).
void dafb_attach_scheduler(dafb_t *dafb, struct scheduler *sched);

// Video interrupt output — level-sensitive (ref §11.18).
void dafb_set_irq_callback(dafb_t *dafb, dafb_irq_cb cb, void *context);

// Monitor on the sense lines: the passive 3-bit code (6 = 13" 640×480 RGB).
// Extended-sense tie matrices come with the larger-monitor support.
void dafb_set_monitor_sense(dafb_t *dafb, uint8_t code);

// TurboSCSI DRQ observation (ref §12.4): channel `chan` (0/1) control
// register reads present the controller's live DRQ in bit 9.
typedef bool (*dafb_drq_query_fn)(void *context);
void dafb_set_scsi_drq_query(dafb_t *dafb, int chan, dafb_drq_query_fn fn, void *context);

// Host pointer to the VRAM buffer (for page-table mapping).
uint8_t *dafb_vram(dafb_t *dafb);
uint32_t dafb_vram_size(dafb_t *dafb);

// The scanout display (substrate .display hook).
display_t *dafb_display(dafb_t *dafb);

// Register-aperture memory interface (registered at DAFB_REG_BASE).
const memory_interface_t *dafb_reg_interface(dafb_t *dafb);

// Reset to power-on state (registers cleared; VRAM preserved).
void dafb_reset(dafb_t *dafb);

#endif // GS_MACHINES_MCU_DAFB_H
