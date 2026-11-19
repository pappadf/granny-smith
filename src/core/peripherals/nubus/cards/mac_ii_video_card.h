// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac_ii_video_card.h
// Apple Macintosh II Video Card (630-0153, "Toby"; declaration ROM 342-0008):
// the NuBus card the Macintosh II shipped with in 1987.  One fixed 640 x 480
// raster at 1, 2, 4 or 8 bpp, drawn by the TFB ("Toby Frame Buffer") chip
// through a Bt453 RAMDAC.  Driver `.Display_Video_Apple_TFB`, which lives in
// the card's 4 KB declaration ROM.
//
// The card's data bus is inverted: every byte the CPU writes reaches VRAM,
// the TFB registers and the RAMDAC complemented, and every read comes back
// complemented again.  The driver compensates by writing ~value wherever a
// register is meant to receive `value`.  For VRAM the two inversions cancel,
// so the model keeps VRAM exactly as the CPU sees it and applies the
// inversion only where it shows -- in what the RAMDAC is indexed by
// (mac_ii_video_card.c, rebuild_display_clut).
//
// Register names follow Apple's TFBDepVideoEqu.a.  The card decodes 20
// address bits; offsets below are within that 1 MB window.

#ifndef NUBUS_CARDS_MAC_II_VIDEO_CARD_H
#define NUBUS_CARDS_MAC_II_VIDEO_CARD_H

#include "card.h"

// === Address map (offsets in the card's 1 MB window) ========================
#define TOBY_VRAM_SIZE   0x80000u // 512 KB: the card with its 8-bit expansion kit
#define TOBY_TFB_BASE    0x80000u // TFB registers: one byte per long word
#define TOBY_CLUT_BASE   0x90000u // Bt453 RAMDAC
#define TOBY_CLRVINT     0xA0000u // write: clear the VBL interrupt and enable it
#define TOBY_DISABLEVINT 0xA0004u // write: disable the VBL interrupt
#define TOBY_INTICON     0xB0000u // write: IntIcOn (unused by Apple's driver)
#define TOBY_INTICOFF    0xC0000u // write: IntIcOff (unused by Apple's driver)
#define TOBY_RDVSYNC     0xD0000u // read: vertical-sync state in bit 0
#define TOBY_RDVINT      0xD0004u // read: VBL interrupt state
#define TOBY_RDINTIC     0xD0008u // read: IntIc state
#define TOBY_IO_END      0xE0000u // end of the decoded register blocks

// The 4 KB declaration ROM sits on byte lane 0 ($E1), so it spans 16 KB of
// bus space ending at the slot top.
#define TOBY_DECLROM_CHIP_SIZE 0x1000u
#define TOBY_DECLROM_BUS_SIZE  0x4000u

// TFB register indices (bus offset (a >> 2) & 0xF within TOBY_TFB_BASE).  The
// driver programs all sixteen from one DepthTbl row per depth; these are the
// ones the model acts on.
#define TFB_REG_BASEHI 2u // wBaseOfstHi: page base in long words, high byte
#define TFB_REG_BASELO 3u // wBaseOfstLo: page base in long words, low byte
#define TFB_REG_MODE   15u // bits 5-4: depth (0..3 = 1/2/4/8 bpp)

// The fixed raster.
#define TOBY_WIDTH  640u
#define TOBY_HEIGHT 480u

extern const nubus_card_kind_t mac_ii_video_card_kind;

#endif // NUBUS_CARDS_MAC_II_VIDEO_CARD_H
