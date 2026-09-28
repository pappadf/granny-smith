// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// jmfb_family.h
// The JMFB display chip, modelled once for the two cards that carry it.
//
// The Apple Macintosh Display Card 8•24 and the 8•24 GC are the same display
// chip in two products -- the GC is an 8•24 with an accelerator bolted on --
// and the emulator carried the chip twice: `jmfb.c` and the display half of
// `display_card_824gc.c`, the latter opening with "ported from jmfb.c".  Not
// two things that resemble each other: the register maps are identical
// constant for constant, block offset $200000 and register window $400,
// CSR $00 / VideoBase $08 / RowWords $0C / SWICReg $3C / SWClrVInt $48 /
// SWStatusReg $C0 / CLUTAddr $00 / CLUTData $04 / CLUTPBCR $08, with
// MaskSenseLine $F1FF and VRSTB $8000 on both sides.  One map, transcribed
// twice under two prefixes.
//
// The copy had already cost: the same missing VideoBase bounds check had to
// be found twice, once in each copy.  And the two had drifted -- jmfb.c logs
// an unmodelled register access, the GC's dispatcher had a bare `return`, so
// an unmodelled write to the GC's JMFB block was invisible.
//
// WHAT STAYS PER-CARD.  Everything the chip acts ON, because that genuinely
// differs: the 8•24 scans its own 2 MB VRAM, while the GC's registers address
// a different store from the one its accelerator composes into.  That is the
// `jmfb_bind_t` below, and it is bindings only -- no chip state.

#ifndef JMFB_FAMILY_H
#define JMFB_FAMILY_H

#include "display.h"

#include <stddef.h>
#include <stdint.h>

struct nubus_card;

// --- The register map, once -------------------------------------------------
// Offsets within each 256-byte block; the guest reaches the live 16 bits of
// each 32-bit register at +2 (big-endian low half).
#define JMFB_BLK_JMFB      0 // JMFB control block
#define JMFB_BLK_STOPWATCH 1 // Stopwatch (VBL interrupt) block
#define JMFB_BLK_CLUT      2 // RAMDAC block
// The Endeavor PLL block is NOT shared.  It is the one place the two cards
// genuinely model different amounts of the chip: jmfb.c decodes four registers
// and answers an EndeavorID on readback, while the 8*24 GC ignores the block
// outright.  Giving the GC the JMFB's answers would change what its driver
// reads back, and nothing in the sources says what it expects -- so each card
// still handles block 3 itself, and the duplication that IS duplication
// (blocks 0-2) is what moves here.
#define JMFB_BLK_ENDEAVOR 3 // caller's; see above

#define JMFB_REG_CSR        0x00u // Control & Status (sense lines at bits 9-11)
#define JMFB_REG_LSR        0x04u // Load/Sync
#define JMFB_REG_VIDEO_BASE 0x08u // Framebuffer base (units of 32 bytes)
#define JMFB_REG_ROW_WORDS  0x0Cu // Stride encoding (depth-dependent)

#define JMFB_REG_SW_IC      0x3Cu // Interrupt/Control: SRST(b0), ENVERTI(b1)
#define JMFB_REG_SW_CLR_INT 0x48u // Clear pending VBL interrupt
#define JMFB_REG_SW_STATUS  0xC0u // Stopwatch status (VBL toggle)

#define JMFB_REG_CLUT_ADDR 0x00u // RAMDAC palette index (resets the sub-counter)
#define JMFB_REG_CLUT_DATA 0x04u // RAMDAC palette data (three longs = R, G, B)
#define JMFB_REG_CLUT_PBCR 0x08u // Pixel Bus Control (bits 3-4 select depth)

#define JMFB_CSR_MASK_SENSE 0xF1FFu // sense bits live in bits 9-11
#define JMFB_CSR_VRSTB      0x8000u // Master reset
#define JMFB_CSR_REFEN      0x2000u // Refresh enable
#define JMFB_CSR_VIDGO      0x1000u // Video transfer enable

// --- Chip state -------------------------------------------------------------
// PLAIN DATA ONLY.  Both cards checkpoint a contiguous scalar block of their
// private struct, and this is meant to sit inside it -- so nothing here may be
// a pointer, and field order is part of the checkpoint format.
typedef struct jmfb_regs {
    uint16_t csr;
    uint16_t lsr;
    uint16_t video_base; // raw; x32 (or x32x8/3 at 24 bpp) = byte offset
    uint16_t row_words; // raw; x4 (or x32/3 at 24 bpp) = stride
    uint16_t sw_ic;
    uint16_t sw_status;
    uint16_t clut_pbcr;
    uint16_t clut_long_hi; // high half of a CLUTDataReg long, awaiting its low
    uint32_t clut_pending[3]; // the three longs of one palette entry
    uint32_t raster_h; // the sensed monitor's height; see jmfb_apply_scanout
    uint8_t clut_idx;
    uint8_t clut_phase; // 0..2, which of the three longs comes next
    uint8_t sense_code; // 0..7, read back through CSR bits 9-11
} jmfb_regs_t;

// What this instance's registers act on.  Rebuilt at init and after a restore,
// never checkpointed.
typedef struct jmfb_bind {
    display_t *display;
    uint8_t *store; // the scanout store the registers address
    size_t store_size;
    rgba8_t *clut; // 256 entries
    struct nubus_card *card; // for the Stopwatch interrupt acknowledge
    const char *tag; // log prefix ("JMFB" / "8*24 GC")
} jmfb_bind_t;

// --- The chip ---------------------------------------------------------------

// CLUTPBCR's depth field -> pixel format.  The 24 bpp case is NOT here: it is
// depth=3 plus PBCR bit 1, which the write handler applies on top.
pixel_format_t jmfb_depth_to_format(uint16_t pbcr);

// Re-derive the whole descriptor from video_base + row_words + the current
// format, and decide it against the store in one step (display_set_scanout).
// `raster_h` is held in the register state rather than read back out of the
// descriptor because a refused descriptor has height 0, and the next good
// register write has to be able to rebuild the full raster.
void jmfb_apply_scanout(jmfb_regs_t *r, const jmfb_bind_t *b);

// Blocks 0-2 of the register window.  `blk` is JMFB_BLK_*; `off` is the byte
// offset within that block.  JMFB_BLK_ENDEAVOR is the caller's to handle and
// is a no-op / 0 here.
void jmfb_write16(jmfb_regs_t *r, const jmfb_bind_t *b, int blk, uint32_t off, uint16_t val);
uint16_t jmfb_read16(jmfb_regs_t *r, const jmfb_bind_t *b, int blk, uint32_t off);

#endif // JMFB_FAMILY_H
