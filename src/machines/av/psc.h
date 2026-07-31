// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// psc.h
// The PSC (Peripheral Subsystem Controller, 343S1100) — the heart of the AV
// platform: system interrupt controller (the pseudo-VIA2 window + the level
// 3-6 interrupt register pairs), the 7-channel DMA engine, the Singer sound
// engine's register block (modelled as the mandatory free-running `sndPhase`
// counter plus latches), the UTSC time-stamp counter, and the DSP reset
// latch.  Contract: local/gs-docs/840av_660av/docs/psc.md (+ singer.md §7,
// dsp3210.md §8 for the sound/DSP stubs).
//
// The VIA1 function the PSC also implements is NOT here — it is the generic
// 6522 model (src/core/peripherals/via.c) mapped at island offset 0.

#ifndef GS_MACHINES_AV_PSC_H
#define GS_MACHINES_AV_PSC_H

#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct av_psc;
typedef struct av_psc av_psc_t;

// PSC-VIA2 window IFR bit assignments (psc.md §5 item 7; VIA-style layout).
#define AV_PSC_VIA2_SCSI_CA2 0 // SCSI IRQ mirror (level)
#define AV_PSC_VIA2_SLOT_CA1 1 // any slot/VBL source in SInt (level)
#define AV_PSC_VIA2_MUNI_SR  2 // MUNI (never asserted here)
#define AV_PSC_VIA2_SCSI_CB2 3 // SCSI IRQ (level)
#define AV_PSC_VIA2_FDC      5 // New Age command completion (latched)
#define AV_PSC_VIA2_SNDFRM   6 // sound frame (latched)

// SInt slot-interrupt sources (active level tracked internally; the register
// reads active-LOW).  Slots C/D/E on bits 3-5, on-board VBL on bit 6.
#define AV_PSC_SINT_VBL 6

// Level-register indices for av_psc_level_* (0 → IPL 3 … 3 → IPL 6).
#define AV_PSC_L3 0
#define AV_PSC_L4 1
#define AV_PSC_L5 2
#define AV_PSC_L6 3

// === Lifecycle ==============================================================

av_psc_t *av_psc_init(config_t *cfg, checkpoint_t *cp);
void av_psc_delete(av_psc_t *psc);
void av_psc_checkpoint(av_psc_t *psc, checkpoint_t *cp);

// === I/O island handlers (mac030 engine rows) ===============================

// The 3-register VIA2 window at island $02000 ($1A00 IFR / $1C00 IER /
// $1E00 SInt — via1-cuda.md §1).
uint8_t av_psc_via2_read(config_t *cfg, uint32_t addr);
void av_psc_via2_write(config_t *cfg, uint32_t addr, uint8_t value);

// The PSC register block at island $31000-$32FFF (level regs, UTSC, ISR,
// sound block, DMA control + register sets).
uint8_t av_psc_reg_read(config_t *cfg, uint32_t addr);
void av_psc_reg_write(config_t *cfg, uint32_t addr, uint8_t value);

// === Interrupt sources ======================================================

// Drive a level-sensitive VIA2-window source (SCSI bits 0/3).
void av_psc_via2_source(av_psc_t *psc, int bit, bool active);

// Latch a pulse VIA2-window source (FDC bit 5, sound frame bit 6); cleared
// by the guest's write-1-to-clear on the IFR.
void av_psc_via2_latch(av_psc_t *psc, int bit);

// Drive a slot-interrupt source in SInt (VBL bit 6; active = asserting).
void av_psc_slot_source(av_psc_t *psc, int bit, bool active);

// Drive a level-sensitive source on one of the L3-L6 registers
// (level = AV_PSC_L3.., bit 0-6).  MACE→L3 b0, SCC A/B→L4 b1/b2, …
void av_psc_level_source(av_psc_t *psc, int level, int bit, bool active);

// Latch a pulse source on L3-L6 (60.15 Hz → L6 b0); cleared by IR write.
void av_psc_level_latch(av_psc_t *psc, int level, int bit);

// The 60.15 Hz tick (wired from the substrate VBL callback).
void av_psc_tick60(av_psc_t *psc);

// === DMA engine (psc.md §2.6-§3) ============================================

// Channel assignments (psc.md §2.5).
#define AV_PSC_DMA_SCSI     0
#define AV_PSC_DMA_MACE_RX  1
#define AV_PSC_DMA_MACE_TX  2
#define AV_PSC_DMA_FDC      3
#define AV_PSC_DMA_SCCA_RX  4
#define AV_PSC_DMA_SCCB     5
#define AV_PSC_DMA_SCCA_TX  6
#define AV_PSC_DMA_CHANNELS 7

// Guest-physical memory hooks the engine transfers through (the
// sonic_set_memory_hooks pattern — the CPU MMU is deliberately not in the
// path; unit tests install array-backed hooks instead).
typedef uint32_t (*av_psc_mem_read_fn)(void *ctx, uint32_t phys, unsigned width);
typedef void (*av_psc_mem_write_fn)(void *ctx, uint32_t phys, uint32_t value, unsigned width);
void av_psc_set_memory_hooks(av_psc_t *psc, av_psc_mem_read_fn rd, av_psc_mem_write_fn wr, void *ctx);

// Device-side DMA ports: move up to `len` bytes between the channel's
// ACTIVE register set and the device.  `device_in` is device→memory (needs
// DIR = 1), `device_out` is memory→device (DIR = 0).  Honors ENABLED /
// PAUSE / the ≥$40000000 restriction; decrements Cnt; on terminal count
// performs the completion dance (ENABLED clears, TERMCNT + IF set, active
// set flips, PSC_ISR/L4 update).  Returns the byte count actually moved.
int av_psc_dma_device_in(av_psc_t *psc, int chan, const uint8_t *buf, int len);
int av_psc_dma_device_out(av_psc_t *psc, int chan, uint8_t *buf, int len);

// True when the channel's active set is armed and running (a device pump
// uses this to know whether to bother polling its DREQ).
bool av_psc_dma_ready(av_psc_t *psc, int chan);

#endif // GS_MACHINES_AV_PSC_H
