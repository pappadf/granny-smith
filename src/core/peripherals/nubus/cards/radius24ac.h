// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// radius24ac.h
// "Apple Macintosh 24AC" — a Radius-built 24-bit colour NuBus display
// card (firmware "Boogie", part 630-0908, © Radius Inc.) with a hardware
// QuickDraw fill/raster accelerator.  See
// local/gs-docs/proposals/proposal-nubus-card-radius-24ac.md and the
// reverse-engineering dossier under local/gs-docs/24AC/.
//
// The model splits into two halves (mirrors the proposal):
//   * Phase 1 — a plain framebuffer + CLUT + VBL display card driven by
//     the genuine declaration ROM's System 7 video driver.  Boots a
//     colour desktop with no accelerator at all.
//   * Phase 2 — the acceleration engine (registers $D00402 / $D40402 /
//     $D40403 + the operand aperture and the +0x400000 active-bank
//     alias) that the Radius cdev drives.  Fully reverse-engineered in
//     the dossier (doc 3); modelled here as a synchronous software-
//     equivalent whose output must match the driver's own CPU fallback.
//
// Register / bit names mirror the dossier's hardware spec.

#ifndef NUBUS_CARDS_RADIUS24AC_H
#define NUBUS_CARDS_RADIUS24AC_H

#include "card.h"
#include <stdbool.h>
#include <stdint.h>

// === Slot-space layout ======================================================
//
// All offsets are relative to the card's slot base (0xFs000000).  We model
// the *large-VRAM* card variant (4 MB passive bank, operand aperture near
// its top at 0x3FE000) so 24-bit colour at the advertised modes fits — see
// the hardware spec §3/§5 (STATUS[3]/CONFIG[0] geometry select) and the
// proposal's open question on the variant bit.

// Passive framebuffer VRAM at offset 0 (4 MB — covers 832×624 at 32 bpp).
#define RADIUS24AC_VRAM_SIZE 0x00400000u // 4 MB

// CPU-visible extent of the passive bank, and the 24-bit-mode framebuffer
// alias.  In 24-bit Memory Manager mode the vrom driver reports the screen
// base as slot+0x900000 (ScrnBase=$F9900000) and QuickDraw dereferences that
// flag-tagged pointer literally, so the card mirrors its VRAM there — same as
// the 8•24.  The visible extent is sized so the alias ends exactly at the
// first register page (0x900000 + 0x380000 = 0xC80000 = the CLUT block) and
// therefore never shadows a card register.  The bytes above it
// (RADIUS24AC_VRAM_VISIBLE .. the engine alias) are served by the operand
// region's passive fall-through, so the whole 4 MB bank stays addressable.
#define RADIUS24AC_VRAM_VISIBLE    0x00380000u // host-mapped + aliased passive bank
#define RADIUS24AC_FB_ALIAS_OFFSET 0x00900000u // 24-bit-mode framebuffer mirror

// The engine's transforming alias of the passive bank, 4 MB higher.  A
// write to (passiveAddr + this) is interpreted by the engine per the
// latched CONTROL mode (fill / copy / ROP).
#define RADIUS24AC_ENGINE_ALIAS_OFFSET 0x00400000u

// Operand aperture (large-VRAM variant).  A latched 32-bit operand
// register: write to load, read to read back; the `4`-twice write to its
// +0x400000 commit alias latches it as the engine's current pattern/colour.
#define RADIUS24AC_OPERAND_APERTURE 0x003FE000u

// Engine registers, high in slot space (hardware spec §2 + vrom RE).  All
// display registers are byte-wide, lane-3 only (spaced by 4).
//   STATUS  read  : byte at 0xD00402 — [2:0] depth/mode (cdev), [3] card
//                   class, [4] VBL/sync busy poll (vrom video driver)
//   VIDCTL  rw    : byte at 0xD00403 — vrom video control latch (see below)
//   CONFIG  read  : byte at 0xD40402 — [0] geometry variant (engine)
//   CONTROL write : byte at 0xD40403 — latch op mode for active-bank writes
#define RADIUS24AC_STATUS_OFFSET  0x00D00402u // STATUS read (byte)
#define RADIUS24AC_VIDCTL_OFFSET  0x00D00403u // VIDCTL read/write (byte)
#define RADIUS24AC_CONFIG_OFFSET  0x00D40402u // CONFIG read (byte, engine)
#define RADIUS24AC_CONTROL_OFFSET 0x00D40403u // CONTROL write (byte, engine)

// STATUS (0xD00402) bit the vrom video driver polls for the CLUT-safe /
// VBL-sync window; we toggle it so the poll loop always sees both edges.
#define RADIUS24AC_STATUS_BUSY 0x10u // bit 4

// VIDCTL (0xD00403) bits the vrom video driver drives (RE doc §"VIDCTL").
#define RADIUS24AC_VIDCTL_VBL_MASK 0x80u // bit 7 — 1 = slot VBL IRQ masked/off
#define RADIUS24AC_VIDCTL_COMMIT   0x20u // bit 5 — config-commit / load strobe
#define RADIUS24AC_VIDCTL_DIRECT   0x08u // bit 3 — direct/"magic" mode enable

// CLUT / RAMDAC byte registers (vrom RE).  Two index/data windows feed the
// same 256-entry palette: an init/clear pair and a runtime pair.  Each
// entry is three sequential data-byte writes in R, G, B order; an index
// write resets the R/G/B sub-counter.
#define RADIUS24AC_RAMDAC_CMD 0x00C80006u // RAMDAC command (#$E3 init) — accept-log
#define RADIUS24AC_CLUT0_DATA 0x00C8000Au // init/clear CLUT data
#define RADIUS24AC_CLUT0_ADDR 0x00C8000Eu // init/clear CLUT index
#define RADIUS24AC_CLUT_CTL   0x00C80016u // CLUT end-of-load strobe — accept-log
#define RADIUS24AC_CLUT_DATA  0x00C8001Au // runtime CLUT data
#define RADIUS24AC_CLUT_ADDR  0x00C8001Eu // runtime CLUT index

// MODE / DEPTH / SENSE byte registers (vrom RE).  MODE bits 7-5 carry the
// pixel-depth code; DEPTH low nibble a parallel depth field; SENSE_CLK is
// the combined monitor-sense read + serial PLL-program line.
#define RADIUS24AC_MODE_REG  0x00D80001u // bits7-5 depth, bits4-0 timing (= CRTC[9])
#define RADIUS24AC_DEPTH_REG 0x00D80005u // low nibble = depth/clock field
#define RADIUS24AC_SENSE_CLK 0x00D8000Du // sense read (bits7-5, inverted) + PLL bit-bang
#define RADIUS24AC_CRTC_LO   0x00D80015u // CRTC[8] (lowest timing-file address)
#define RADIUS24AC_CRTC_HI   0x00D80035u // CRTC[0] (highest); whole file is write-only

// Slot-space pages we register device callbacks over (4 KB each).
#define RADIUS24AC_CLUT_PAGE 0x00C80000u // CLUT / RAMDAC block
#define RADIUS24AC_D00_PAGE  0x00D00000u // STATUS + VIDCTL
#define RADIUS24AC_D40_PAGE  0x00D40000u // CONFIG + CONTROL (engine)
#define RADIUS24AC_D80_PAGE  0x00D80000u // MODE / DEPTH / SENSE / CRTC

// CONTROL (mode) byte values the Radius driver emits (hardware spec §2.3).
#define RADIUS24AC_MODE_FILL    0x01u // pattern / solid fill (replicate operand)
#define RADIUS24AC_MODE_STRETCH 0x03u // stretch / scale (driver-side DDA)
#define RADIUS24AC_MODE_COPY    0x7Fu // fast block copy ("all planes")
// Other values 0x00..0x3F are computed raster-op / transfer-mode codes.

// Operand-commit command written through the aperture's +0x400000 alias.
#define RADIUS24AC_COMMIT_CMD 0x00000004u

// === Declaration ROM ========================================================
// display-card-24ac.vrom: 32 KB chip, byteLanes = $78 (lane 3 only) → 128 KB
// of bus space at slot_base + 0xFE0000 (identical layout to the 8•24).
#define RADIUS24AC_DECLROM_CHIP_SIZE  0x008000u // 32 KB raw chip data (file size)
#define RADIUS24AC_DECLROM_BUS_SIZE   0x020000u // 128 KB bus-space footprint (chip × 4)
#define RADIUS24AC_DECLROM_BUS_OFFSET 0xFE0000u // slot top minus 128 KB

// Per-card kind descriptor — registered in nubus.c's g_card_registry.
extern const nubus_card_kind_t radius_24ac_kind;

// === Engine introspection (object model — slot[N].card.engine) ==============
// True iff `card` is a radius_24ac.  The getters return 0/false and the
// setter is a no-op for any other card kind, so the object-model layer can
// call them without first knowing the card type.  `enabled` is the Phase-2
// acceleration gate (proposal §4 step 4 / §3.5): clearing it forces the
// active-bank alias to behave as plain VRAM (the software-fallback oracle).
bool radius24ac_is_card(const nubus_card_t *card);
bool radius24ac_engine_enabled(const nubus_card_t *card);
void radius24ac_engine_set_enabled(nubus_card_t *card, bool enabled);
uint8_t radius24ac_engine_mode(const nubus_card_t *card);
uint32_t radius24ac_engine_operand(const nubus_card_t *card);

#endif // NUBUS_CARDS_RADIUS24AC_H
