// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// jmfb_family.c
// See jmfb_family.h.  This is jmfb.c's model, moved -- not a merge of the two
// copies.  Where they differed it was only ever that the 8*24 GC's port had
// dropped things: the log lines, the LSR decode, the comments recording WHY a
// bus convention is what it is.  Taking the poorer copy's silence into the
// shared model would have thrown away the thing that made sharing worth doing.

#include "jmfb_family.h"

#include "log.h"
#include "nubus.h"

LOG_USE_CATEGORY_NAME("nubus");

pixel_format_t jmfb_depth_to_format(uint16_t pbcr) {
    // 24 bpp is depth=3 plus PBCR bit 1, and the System 7 driver only toggles
    // bit 1 once it is already at 8 bpp -- so this result is the right
    // starting point and the write handler upgrades it.
    switch ((pbcr >> 3) & 0x3) {
    case 0:
        return PIXEL_1BPP_MSB;
    case 1:
        return PIXEL_2BPP_MSB;
    case 2:
        return PIXEL_4BPP_MSB;
    case 3:
    default:
        return PIXEL_8BPP;
    }
}

void jmfb_apply_scanout(jmfb_regs_t *r, const jmfb_bind_t *b) {
    if (!r || !b || !b->display)
        return;
    if (r->row_words == 0)
        return; // chip-reset sentinel; preserve the last good descriptor

    uint32_t stride, width;
    if (b->display->format == PIXEL_32BPP_XRGB) {
        stride = (uint32_t)r->row_words * 32u / 3u;
        width = stride / 4u;
    } else {
        stride = (uint32_t)r->row_words * 4u;
        width = (uint32_t)r->row_words * 32u / display_bpp(b->display->format);
    }

    // Byte offset into the store is depth-dependent.  For <=8 bpp the encoded
    // value x 32 = byte offset.  For 24 bpp the JMFB driver writes
    // `(defmBaseOffset * 3/4) >> 5 >> 1` (its TFBM30 parms) -- inverted, that
    // is `value * 32 * 8/3`, which is the x32/3 stride formula scaled by 8 to
    // get from row-stride units back to bytes.
    uint64_t offset = (b->display->format == PIXEL_32BPP_XRGB) ? (uint64_t)r->video_base * 32u * 8u / 3u
                                                               : (uint64_t)r->video_base * 32u;
    if (offset > UINT32_MAX)
        offset = UINT32_MAX; // guaranteed to fail the fit test below

    display_set_scanout(b->display, b->store, b->store_size, (uint32_t)offset, stride, width, r->raster_h, NULL, 0);
}

// Decode the three completed long writes into one palette entry.  Two
// protocols share the register: at 24 bpp the driver puts all three components
// in the last long, otherwise each long's LSB is one component.
static void clut_finalize_entry(jmfb_regs_t *r, const jmfb_bind_t *b) {
    uint32_t w0 = r->clut_pending[0];
    uint32_t w1 = r->clut_pending[1];
    uint32_t w2 = r->clut_pending[2];
    rgba8_t e;
    if (w0 == 0 && w1 == 0 && (w2 & 0xFFFFFF00u) != 0) {
        // 24bpp variant: w2 carries 0x00BBGGRR all in one long.
        e.r = (uint8_t)(w2 & 0xFFu);
        e.g = (uint8_t)((w2 >> 8) & 0xFFu);
        e.b = (uint8_t)((w2 >> 16) & 0xFFu);
    } else {
        // 8/16bpp variant: each long's LSB is one component (R, G, B).
        e.r = (uint8_t)(w0 & 0xFFu);
        e.g = (uint8_t)(w1 & 0xFFu);
        e.b = (uint8_t)(w2 & 0xFFu);
    }
    e.a = 255;
    if (b->clut)
        b->clut[r->clut_idx] = e;
    r->clut_idx++; // auto-increment for run-write
    r->clut_phase = 0;
    if (b->display)
        b->display->clut_dirty = true;
}

// --- JMFB block -------------------------------------------------------------
//
// LSR / VideoBase / RowWords are 16-bit registers occupying the LOW half of a
// 32-bit-aligned slot -- Apple's bus convention for half-word registers in
// long-aligned slot space.  When the driver writes one with `move.l #N,
// (slot)`, the card's io_write32 splits it into io_write16(slot, hi=0) and
// io_write16(slot+2, lo=N): the meaningful value lands at slot+2 and slot is a
// no-op write of zero.  Reads mirror it -- slot+2 returns the register, slot
// returns the high half's zero.
//
// The header keeps Apple's spec naming (the slot offset), and the handlers
// dispatch on slot+2 for data while accepting the high-half pass.  Without the
// split, the OS's `move.l #$50, (VideoBase)` clobbered video_base to 0 -- the
// high-half write hit the slot case while the meaningful $50 fell through to
// the unmodelled default -- pointing display.bits at the store's base instead
// of +$A00 and shifting the rendered framebuffer ~32 rows up.

static void jmfb_block_write16(jmfb_regs_t *r, const jmfb_bind_t *b, uint32_t off, uint16_t val) {
    switch (off) {
    case JMFB_REG_CSR:
        // High 16 bits of the 32-bit CSR.  No documented soft-controlled bits
        // modelled here -- accept-and-ignore.
        return;
    case JMFB_REG_CSR + 2:
        // Low 16 bits -- software-writable control bits live here.
        r->csr = (uint16_t)((val & ~JMFB_CSR_MASK_SENSE) | (r->csr & JMFB_CSR_MASK_SENSE));
        if (val & JMFB_CSR_VRSTB) {
            // Master reset clears software-controlled bits and stops the
            // RAMDAC sub-counter.  Sense lines are a hardware property and are
            // NOT cleared.
            r->csr &= JMFB_CSR_MASK_SENSE;
            r->clut_phase = 0;
            LOG(2, "%s CSR: VRSTB master reset", b->tag);
        }
        if (val & JMFB_CSR_REFEN)
            LOG(3, "%s CSR: REFEN set", b->tag);
        if (val & JMFB_CSR_VIDGO)
            LOG(3, "%s CSR: VIDGO set (video transfer enabled)", b->tag);
        return;
    case JMFB_REG_LSR:
    case JMFB_REG_VIDEO_BASE:
    case JMFB_REG_ROW_WORDS:
        return; // high half of a long write -- bus discards
    case JMFB_REG_LSR + 2:
        r->lsr = val;
        LOG(3, "%s LSR write %04x (accept-and-log)", b->tag, val);
        return;
    case JMFB_REG_VIDEO_BASE + 2:
        r->video_base = val;
        jmfb_apply_scanout(r, b);
        if (b->display)
            b->display->fb_dirty = true;
        return;
    case JMFB_REG_ROW_WORDS + 2:
        r->row_words = val;
        jmfb_apply_scanout(r, b);
        if (b->display)
            b->display->shape_dirty = true;
        return;
    default:
        LOG(2, "%s block write at +%02x = %04x (unmodeled)", b->tag, off, val);
        return;
    }
}

static uint16_t jmfb_block_read16(jmfb_regs_t *r, const jmfb_bind_t *b, uint32_t off) {
    switch (off) {
    case JMFB_REG_CSR:
        // The CSR is a 32-bit register at offset $00.  Apple's PrimaryInit
        // reads sense via `BFEXTU (A1,D1.L){20:3},D4` -- bits 20..22 of the
        // 32-bit memory value, which is the LOW 16-bit half (bits 9..11
        // LSB-numbered).  So the sense lines live at offset $02, and offset
        // $00 is the high half, which has no modelled bits.
        return 0;
    case JMFB_REG_CSR + 2:
        return (uint16_t)((r->csr & JMFB_CSR_MASK_SENSE) | ((r->sense_code & 7) << 9));
    case JMFB_REG_LSR:
    case JMFB_REG_VIDEO_BASE:
    case JMFB_REG_ROW_WORDS:
        return 0; // high half of a long read -- bus drives zero
    case JMFB_REG_LSR + 2:
        return r->lsr;
    case JMFB_REG_VIDEO_BASE + 2:
        return r->video_base;
    case JMFB_REG_ROW_WORDS + 2:
        return r->row_words;
    default:
        LOG(2, "%s block read at +%02x (unmodeled, returning 0)", b->tag, off);
        return 0;
    }
}

// --- Stopwatch block --------------------------------------------------------
//
// Same 16-bit-in-32-bit-slot convention.  SWStatusReg's +2 read path was
// always right because the Apple driver's `BFEXTU (A0){#$1D:#$1}` test made
// the convention obvious there; SWICReg / SWClrVInt writes were silently lost
// on long-write paths until it was applied to them too.

static void stopwatch_write16(jmfb_regs_t *r, const jmfb_bind_t *b, uint32_t off, uint16_t val) {
    switch (off) {
    case JMFB_REG_SW_IC:
    case JMFB_REG_SW_CLR_INT:
    case JMFB_REG_SW_STATUS:
        return; // high half of a long write -- bus discards
    case JMFB_REG_SW_IC + 2:
        // Bit 1 = VINT_DISABLE (active-high mask): cleared = VBL IRQ on every
        // VBL, set = masked.  The card's on_vbl hook gates on this.
        r->sw_ic = val;
        if (val & 0x0001u)
            LOG(2, "%s SWICReg: soft reset", b->tag);
        return;
    case JMFB_REG_SW_CLR_INT + 2:
        // Write any value clears the pending VBL and de-asserts the slot's IRQ
        // on the bus controller.
        if (b->card)
            nubus_deassert_irq(b->card);
        return;
    case JMFB_REG_SW_STATUS + 2:
        // Read-mostly on hardware; the System 7 driver writes here to clear
        // bits.  Accept-and-log.
        r->sw_status = val;
        return;
    default:
        LOG(2, "%s Stopwatch block write at +%02x = %04x (unmodeled)", b->tag, off, val);
        return;
    }
}

static uint16_t stopwatch_read16(jmfb_regs_t *r, const jmfb_bind_t *b, uint32_t off) {
    switch (off) {
    case JMFB_REG_SW_STATUS:
        // Top half of the 32-bit status word.  Real hardware exposes the VBL
        // toggle at LONG-word bit 2 (= byte $C3 bit 2, big-endian), which the
        // Apple driver polls via BFEXTU (A0){#$1D:#$1}.  The toggle is in the
        // +2 path below; this half is a stable 0.
        return 0;
    case JMFB_REG_SW_STATUS + 2:
        // Bit 2 of this half is the VBL toggle the OS polls.  Flip it on every
        // read so the poll sees both edges.
        r->sw_status ^= 0x0004u;
        return r->sw_status & 0x0004u;
    case JMFB_REG_SW_IC:
        return 0; // high half -- bus drives zero
    case JMFB_REG_SW_IC + 2:
        return r->sw_ic;
    default:
        LOG(2, "%s Stopwatch block read at +%02x (unmodeled)", b->tag, off);
        return 0;
    }
}

// --- CLUT block -------------------------------------------------------------
//
// The RAMDAC registers follow the same 16-bit-in-32-bit-slot convention as the
// JMFB block: the meaningful half is at slot+2.  Without that, the driver's
// cscSetEntries writes land on the unmodelled default while the slot+0 cases
// catch only the high-half zero -- the palette stays pinned to the init
// grayscale ramp -- and cscSetMode's CLUTPBCR writes are likewise lost, so
// depth changes from the Monitors control panel never reach display.format.

static void clut_write16(jmfb_regs_t *r, const jmfb_bind_t *b, uint32_t off, uint16_t val) {
    switch (off) {
    case JMFB_REG_CLUT_ADDR:
    case JMFB_REG_CLUT_PBCR:
        return; // high half of a long write -- bus discards
    case JMFB_REG_CLUT_DATA:
        // High half of a CLUTDataReg long write -- stash it so the low half
        // can reassemble the full 32-bit value below.
        r->clut_long_hi = val;
        return;
    case JMFB_REG_CLUT_ADDR + 2:
        // The index maps into the low byte; a write resets the R/G/B
        // sub-counter so the next three data writes load the new entry.
        r->clut_idx = (uint8_t)(val & 0xFFu);
        r->clut_phase = 0;
        return;
    case JMFB_REG_CLUT_DATA + 2: {
        uint32_t full = ((uint32_t)r->clut_long_hi << 16) | val;
        r->clut_long_hi = 0;
        if (r->clut_phase < 3)
            r->clut_pending[r->clut_phase++] = full;
        if (r->clut_phase == 3)
            clut_finalize_entry(r, b);
        return;
    }
    case JMFB_REG_CLUT_PBCR + 2: {
        r->clut_pbcr = val;
        pixel_format_t f = jmfb_depth_to_format(val);
        // Bit 1 = 24 bpp packed (RAMDAC bypass) on top of the depth=3 case.
        if ((val & 0x0002u) && f == PIXEL_8BPP)
            f = PIXEL_32BPP_XRGB;
        if (b->display && b->display->format != f) {
            b->display->format = f;
            // A depth change moves BOTH halves -- the 24 bpp offset formula
            // and the stride -- so re-decide the whole descriptor.
            jmfb_apply_scanout(r, b);
            b->display->shape_dirty = true;
        }
        return;
    }
    default:
        LOG(2, "%s CLUT block write at +%02x = %04x (unmodeled)", b->tag, off, val);
        return;
    }
}

static uint16_t clut_read16(jmfb_regs_t *r, const jmfb_bind_t *b, uint32_t off) {
    switch (off) {
    case JMFB_REG_CLUT_ADDR:
    case JMFB_REG_CLUT_PBCR:
        return 0; // high half of a long read -- bus drives zero
    case JMFB_REG_CLUT_ADDR + 2:
        return r->clut_idx;
    case JMFB_REG_CLUT_PBCR + 2:
        return r->clut_pbcr;
    default:
        LOG(2, "%s CLUT block read at +%02x (unmodeled)", b->tag, off);
        return 0;
    }
}

// --- Dispatch ---------------------------------------------------------------

void jmfb_write16(jmfb_regs_t *r, const jmfb_bind_t *b, int blk, uint32_t off, uint16_t val) {
    if (!r || !b)
        return;
    switch (blk) {
    case JMFB_BLK_JMFB:
        jmfb_block_write16(r, b, off, val);
        return;
    case JMFB_BLK_STOPWATCH:
        stopwatch_write16(r, b, off, val);
        return;
    case JMFB_BLK_CLUT:
        clut_write16(r, b, off, val);
        return;
    default:
        return; // Endeavor is the caller's (jmfb_family.h)
    }
}

uint16_t jmfb_read16(jmfb_regs_t *r, const jmfb_bind_t *b, int blk, uint32_t off) {
    if (!r || !b)
        return 0;
    switch (blk) {
    case JMFB_BLK_JMFB:
        return jmfb_block_read16(r, b, off);
    case JMFB_BLK_STOPWATCH:
        return stopwatch_read16(r, b, off);
    case JMFB_BLK_CLUT:
        return clut_read16(r, b, off);
    default:
        return 0; // Endeavor is the caller's (jmfb_family.h)
    }
}
