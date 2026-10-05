// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// builtin_rbv_video.h
// Macintosh IIci built-in video, presented as a NuBus pseudo-card in slot
// $0.  Unlike the SE/30's slot-$E framebuffer (a dedicated VRAM region with
// a declaration ROM), the IIci's RBV reads its framebuffer directly out of
// main RAM; the boot ROM has the video driver baked in and references the
// framebuffer at the logical base $FBB08000 (the slot-$B aperture) from the
// hard-coded VideoInfoMDU record — so this card needs no declaration ROM.
//
// v1 models the framebuffer pointer + depth/CLUT (driven by RvMonP and the
// VDAC), not the cycle-stealing DMA.  The card owns the display_t and the
// 256-entry CLUT; the machine points it at the bottom of Bank A (physical 0,
// where the ROM's tables map $FBB08000) and routes the VDAC ($50F24000)
// window here.  The
// monitor-sense + depth register lives on the RBV chip (rbv.c).

#ifndef NUBUS_CARDS_BUILTIN_RBV_VIDEO_H
#define NUBUS_CARDS_BUILTIN_RBV_VIDEO_H

#include "card.h"
#include "rbv.h"
#include <stdbool.h>
#include <stdint.h>

// Size of the window the card scans out of, and of the private buffer it
// starts with before the machine points it at main RAM (both the IIci and the
// IIsi do: the RBV/V8 frame buffer is the bottom of Bank A).
#define BUILTIN_RBV_VRAM_SIZE     0x00100000UL // 1 MB — covers 640×480×8bpp + offset
#define BUILTIN_RBV_SCREEN_OFFSET 0x8000UL // screen offset within the private buffer

// Card-kind descriptor — registered in nubus.c's g_card_registry under the
// id "builtin_rbv_video"; the IIci machine names it in its slot-$0 decl.
extern const nubus_card_kind_t builtin_rbv_video_kind;

// === Machine-facing hooks (outside the nubus_card_ops_t vtable) =============

// Point the card's framebuffer at a machine-owned aperture (a window into main
// RAM) rather than the card's private buffer.  Used by the IIci and the IIsi,
// whose RBV / V8 read the framebuffer directly out of main DRAM.  `aperture` is
// the base of the framebuffer in host memory; `screen_offset` is where the
// active screen begins within it (both put the screen at the very start of
// Bank A — physical 0 — so their offset is 0).  The machine owns the memory
// (not freed by the card).  `blank` clears the visible window: true on a cold
// boot, false on a checkpoint restore, whose RAM image already holds the screen.
void builtin_rbv_video_set_framebuffer(nubus_card_t *card, uint8_t *aperture, uint32_t screen_offset, bool blank);

// Wire the RBV chip to the card so the card can assert the slot-0 video
// VBL interrupt and read the current depth.  Called from iici_init after
// both the card and the RBV exist.
void builtin_rbv_video_set_rbv(nubus_card_t *card, rbv_t *rbv);

// VDAC (Bt450 RAMDAC) register access.  The IIci's VDAC lives in machine
// I/O space at $50F24000, not in the card's slot space, so the machine's
// I/O dispatcher forwards reads/writes here.  `off` is relative to the
// VDAC base; the modelled registers are $0 (write-addr), $4 (write-data,
// R/G/B sequential), $8 (pixel-read-mask), $C (read-addr).
void builtin_rbv_video_vdac_write(nubus_card_t *card, uint32_t off, uint8_t val);
uint8_t builtin_rbv_video_vdac_read(nubus_card_t *card, uint32_t off);
// The same register without the read's side effect (memory_interface_t.peek_*).
uint8_t builtin_rbv_video_vdac_peek(nubus_card_t *card, uint32_t off);

// Apply a depth change from the RBV mode callback (depth_code 0..3 =
// 1/2/4/8 bpp).  Recomputes display.format/stride and sets shape_dirty.
void builtin_rbv_video_set_depth(nubus_card_t *card, int depth_code);

// RvMonP's RvVIDOff bit: present a black raster without touching the
// framebuffer, which on the IIsi is live guest memory.
void builtin_rbv_video_set_blank(nubus_card_t *card, bool video_off);

#endif // NUBUS_CARDS_BUILTIN_RBV_VIDEO_H
