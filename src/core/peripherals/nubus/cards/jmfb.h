// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// jmfb.h
// Apple Macintosh Display Card 8•24 (Rev B, ROM `341-0868`).  See
// proposal-machine-iicx-iix.md §3.2.5.  Driver class
// `Display_Video_Apple_MDC` (`catDisplay / typeVideo / DrSwApple /
// DrHwMDC`).  All register / bit-field names mirror Apple's
// JMFBDepVideoEqu.a verbatim.
//
// v1 status (per the proposal's "Steps 1-5 done, Step 6 minimum-viable"
// scope).  The four register blocks (JMFB / Stopwatch / CLUT / Endeavor)
// each have their offsets defined here, and the I/O dispatcher in
// jmfb.c handles every named register; a subset (CLUTAddrReg /
// CLUTDataReg / CLUTPBCR / JMFBVideoBase / JMFBRowWords / SWClrVInt /
// SWICReg / JMFBCSR sense-bits) is *modelled*, the rest are
// *accept-and-log* per §3.2.5's policy.  The card boots far enough to
// run the System 7 JMFB driver's PrimaryInit; full mode-switch
// (`cscSwitchMode`) lands as the JMFB driver's behavioural surface
// expands.

#ifndef NUBUS_CARDS_JMFB_H
#define NUBUS_CARDS_JMFB_H

#include "card.h"
#include <stdbool.h>

// === Block bases — verbatim from JMFBDepVideoEqu.a ==========================
//   JMFB        EQU $200000   ; Offset to JMFB Control Registers
//   Stopwatch   EQU $200100   ; Offset to Stopwatch Registers
//   CLUT        EQU $200200   ; Offset to CLUT Registers
//   Endeavor    EQU $200300   ; Offset to Endeavor Registers
#define JMFB_BLOCK_OFFSET      0x200000u
#define STOPWATCH_BLOCK_OFFSET 0x200100u
#define CLUT_BLOCK_OFFSET      0x200200u
#define ENDEAVOR_BLOCK_OFFSET  0x200300u
#define JMFB_REGISTER_SIZE     0x000400u // covers all four blocks
// Apple-341-0868.vrom is a 32 KB chip with byteLanes = $78 (lane 3 only,
// per "Designing Cards and Drivers for the Macintosh Family" 3rd ed.,
// Table 8-2).  In bus-space layout the chip's bytes appear sparsely:
// each chip byte at lane 3 of a longword, with lanes 0-2 inactive.  So
// the 32 KB chip occupies 128 KB of slot space — JMFB_DECLROM_BUS_SIZE
// is the bus footprint, JMFB_DECLROM_CHIP_SIZE is the file size, and
// JMFB_DECLROM_BUS_OFFSET places the bus region at the top of slot
// space (`slot_base + 0xFFFFFFFF` minus 128KB + 1).
#define JMFB_DECLROM_CHIP_SIZE  0x008000u // 32 KB raw chip data (file size)
#define JMFB_DECLROM_BUS_SIZE   0x020000u // 128 KB bus-space footprint (chip × 4 for byteLanes=$78)
#define JMFB_DECLROM_BUS_OFFSET 0xFE0000u // slot top minus 128 KB
#define JMFB_VRAM_SIZE          0x200000u // 2 MB (defMinorLengthB)

// === In-block offsets — verbatim from JMFBDepVideoEqu.a =====================
// JMFB block
#define JMFBCSR       0x00u // Control & Status
#define JMFBLSR       0x04u // Load/Sync Register
#define JMFBVideoBase 0x08u // Framebuffer base in VRAM (units of 32 bytes)
#define JMFBRowWords  0x0Cu // Stride encoding (depth-dependent)
// Stopwatch block
#define SWICReg     0x3Cu // Interrupt/Control: SRST(b0), ENVERTI(b1)
#define SWClrVInt   0x48u // Clear pending VBL interrupt (write-1-to-clear)
#define SWStatusReg 0xC0u // Stopwatch Status (read of VBL toggle state)
// CLUT block
#define CLUTAddrReg 0x00u // RAMDAC palette index (also resets R/G/B sub-counter)
#define CLUTDataReg 0x04u // RAMDAC palette data; 3 sequential writes load R, G, B
#define CLUTPBCR    0x08u // Pixel Bus Control: bits 3-4 select depth on RAMDAC
// Endeavor block
#define EndeavorM         0x00u // PLL M divider
#define EndeavorN         0x04u // PLL N divider
#define EndeavorExtClkSel 0x08u // External clock select
#define EndeavorReserved  0x0Cu // Apple-reserved
#define EndeavorID        0x0000u // Apple Endeavor identifier (read after reset)

// === Bit-field equates — verbatim from JMFBDepVideoEqu.a ====================
#define MaskSenseLine 0xF1FFu // sense bits live in bits 9-11 (1110_0000_0000)
#define REFEN         0x0008u // Refresh enable
#define VIDGO         0x0040u // Video transfer cycle enable
#define PIXSEL1       0x2000u // Endeavor reset / pixel-clock source
#define VRSTB         0x8000u // Master reset
#define SRST          0x0001u // Soft reset (Stopwatch SWICReg bit 0)
// Vertical-interrupt MASK (Stopwatch SWICReg bit 1).  Active-HIGH disable:
// bit clear = card raises slot IRQ on every VBL, bit set = VBL IRQ masked.
// Confirmed against the Apple driver's InstallSlotInterrupt (writes $5,
// bit 1 clear, to enable) / RemoveSlotInterrupt (writes $7, bit 1 set,
// to disable) — see local/gs-docs/asm/Apple-341-0868-vrom.asm L3017,L3049.
#define VINT_DISABLE 0x0002u

// Per-card kind descriptor — registered in nubus.c's g_card_registry.
extern const nubus_card_kind_t mdc_8_24_kind;

// Pending monitor sense for the next-instantiated JMFB card.  Set
// before `machine.boot` to make the new machine's JMFB report a
// different monitor type to the boot ROM (changes display.width and
// display.height to match — see monitor_for_sense in jmfb.c).  After
// the next factory call the pending slot is reset to the default
// ($6 = 13" RGB).  Valid raw-sense codes per JMFBPrimaryInit.a are
// 0..6; 7 is "no connect / extended sense" and falls back to
// 13" RGB dimensions.
void jmfb_pending_sense_set(uint8_t sense);
uint8_t jmfb_pending_sense_get(void);

// Pending high-level video-mode selection consumed by the next JMFB
// factory call.  The id matches one of the entries enumerated by
// `machine.profile(id).video_modes[].id` (e.g. "13in_rgb_8bpp").
// When set, the factory:
//   1. resolves id → (monitor, depth) via mdc_8_24_monitors[],
//   2. overrides the pending sense to the monitor's sense_code,
//   3. seeds PRAM with the boot-ROM validity tokens ($00=$A8,
//      $0C-$0F='NuMc') and the slot-9 sPRAMRec bytes that name
//      the requested (sister sRsrcID, spDepth) pair, so the Slot
//      Manager's GET_SLOT_DEPTH picks it up at first boot.
// Mirrors how `tests/integration/iicx-video-modes/test.script`
// drives the same dance shell-side.  Passing NULL or "" clears
// the pending selection.
void jmfb_pending_video_mode_set(const char *id);
const char *jmfb_pending_video_mode_get(void);

// Look up a video-mode entry by id ("monitor_Nbpp") in the JMFB
// catalog.  Writes the resolved monitor + depth into *out_monitor
// / *out_depth_bpp on success and returns true; returns false (and
// leaves the out-params untouched) when id doesn't match any
// catalog entry.  Used by the JMFB factory to consume a pending
// video-mode selection.
bool jmfb_video_mode_lookup(const char *id, const nubus_monitor_t **out_monitor, int *out_depth_bpp);

#endif // NUBUS_CARDS_JMFB_H
