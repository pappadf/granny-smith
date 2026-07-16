// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// display_card_824gc_qd.c
// "GC QuickDraw" — the 8•24 GC's drawing engine, reimplemented in C (HLE).
// This is the half of the card model that ran ON the Am29000 on real
// hardware: the DrawMultiObject opcode interpreter, the pixel cores (boolean
// / arithmetic / hilite transfer modes at 1/8/16/32 bpp), patterns and the
// type-5 PixPat cache, the ROM-exact conic (ovals/arcs/round-rects) and
// polygon rasterizers, region clip masks, the func-$15 blit engine, and the
// font/text machinery (func $30 + ops $67/$06).  The card shell
// (display_card_824gc.c) services the NuBus/CB transport and calls in via
// the gc824_* entry points in display_card_824gc_priv.h.
//
// Coordinate/state conventions, the accept envelopes, and the per-primitive
// algorithm provenance (which ROM QuickDraw sources each rasterizer is a
// port of) are documented inline and in
// local/gs-docs/proposals/proposal-8-24gc-hle-acceleration.md §3.

#include "display_card_824gc_priv.h"

#include "log.h"
#include "memory.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("gc824");

// fwds (text section — used by gc_interp, defined after the rasterizers)
static struct gc_cache_ent *gc_cache_find(struct gc_cache_ent *tab, int n, uint32_t key);
static void gc_draw_text(display_card_824gc_priv_t *p, uint32_t off);

// === Stage 2: DrawMultiObject interpreter + 1-bpp rasterizers (proposal §3.7) =
// The GCQD marshaller stages drawing as a queue of opcode records
// (gc-ipc-protocol.md §10.1) and submits it via func $26/$38.  When func $2D
// (SetPort) is accepted the card interprets the stream over the port's device;
// primitives rasterize into the 1-bpp framebuffer the display surfaces.  The
// per-primitive algorithms follow gc-quickdraw-call-reference.md (§1 line,
// §2 rect, §3 rrect, §7 region).  Depth: 1 bpp only for now (the boot mode);
// other depths still decline (proposal §3.7).

static uint32_t gc_resolve_rgb(display_card_824gc_priv_t *p, uint16_t r, uint16_t g, uint16_t b);

// Arithmetic transfer modes $20-$27 (and their pattern twins $28-$2F) at the
// indexed and direct depths — qd-transfer-modes.md §3.1/§3.2.  The source
// pixel is the colorized pattern pixel (8×8 patterns DO take fg/bk under
// arithmetic modes, §4.1); component math runs on 16-bit components (8 bpp:
// CLUT entries replicated 8→16; 32 bpp: native bytes replicated); the result
// lands via the card-side Color2Index (8 bpp) or packs directly (32 bpp).
// pin EQU weight: one opColor RGB serves blend weight and add/sub pin.
static inline uint16_t gc_c5to16(uint32_t v5) { // replicate 5 bits to 16
    v5 &= 0x1F;
    return (uint16_t)((v5 << 11) | (v5 << 6) | (v5 << 1) | (v5 >> 4));
}
static void gc_px_arith(display_card_824gc_priv_t *p, uint8_t *b, uint32_t srcpix) {
    int is32 = p->display.format == PIXEL_32BPP_XRGB;
    int is16 = p->display.format == PIXEL_16BPP_555;
    uint32_t dstpix = is32 ? LOAD_BE32(b) & 0x00FFFFFFu : is16 ? (LOAD_BE16(b) & 0x7FFFu) : *b;
    int variant = p->gc_mode & 7;
    if (variant == 4) { // transparent: full-pixel compare against transColor
        // (= the port background pixel), skip or plain copy — no math.
        if (srcpix == p->gc_bg)
            return;
        if (is32)
            STORE_BE32(b, srcpix & 0x00FFFFFFu);
        else if (is16)
            STORE_BE16(b, (uint16_t)(srcpix & 0x7FFFu));
        else
            *b = (uint8_t)srcpix;
        return;
    }
    uint16_t cs[3], cd[3], cr[3];
    if (is32) {
        for (int c = 0; c < 3; c++) {
            uint32_t sh = 16 - 8 * c;
            cs[c] = (uint16_t)(((srcpix >> sh) & 0xFF) * 0x101u);
            cd[c] = (uint16_t)(((dstpix >> sh) & 0xFF) * 0x101u);
        }
    } else if (is16) {
        for (int c = 0; c < 3; c++) {
            uint32_t sh = 10 - 5 * c;
            cs[c] = gc_c5to16(srcpix >> sh);
            cd[c] = gc_c5to16(dstpix >> sh);
        }
    } else {
        const rgba8_t *sc = &p->clut[srcpix & 0xFF], *dc = &p->clut[dstpix & 0xFF];
        cs[0] = (uint16_t)(sc->r * 0x101u);
        cs[1] = (uint16_t)(sc->g * 0x101u);
        cs[2] = (uint16_t)(sc->b * 0x101u);
        cd[0] = (uint16_t)(dc->r * 0x101u);
        cd[1] = (uint16_t)(dc->g * 0x101u);
        cd[2] = (uint16_t)(dc->b * 0x101u);
    }
    for (int c = 0; c < 3; c++) {
        uint32_t pin = p->gc_op_rgb[c];
        uint32_t v;
        switch (variant) {
        case 0: { // blend: C' = (Cs·w + Cd·(65536−w)) >> 16, w=0 → 1
            uint32_t w = pin ? pin : 1;
            v = ((uint32_t)cs[c] * w + (uint32_t)cd[c] * (65536u - w)) >> 16;
            break;
        }
        case 1: // addPin: carry or > pin ⇒ pin
            v = (uint32_t)cs[c] + cd[c];
            if (v > 0xFFFFu || v > pin)
                v = pin;
            break;
        case 2: // addOver: wraps
            v = ((uint32_t)cs[c] + cd[c]) & 0xFFFFu;
            break;
        case 3: // subPin: borrow or < pin ⇒ pin
            v = (cd[c] >= cs[c]) ? (uint32_t)cd[c] - cs[c] : pin;
            if (v < pin)
                v = pin;
            break;
        case 5: // adMax
            v = cs[c] > cd[c] ? cs[c] : cd[c];
            break;
        case 6: // subOver: wraps
            v = ((uint32_t)cd[c] - cs[c]) & 0xFFFFu;
            break;
        default: // 7 adMin
            v = cs[c] < cd[c] ? cs[c] : cd[c];
            break;
        }
        cr[c] = (uint16_t)v;
    }
    if (is32) // alpha byte: add/sub/blend write 0; max/min keep dst (ours is 0)
        STORE_BE32(b, ((uint32_t)(cr[0] >> 8) << 16) | ((uint32_t)(cr[1] >> 8) << 8) | (cr[2] >> 8));
    else if (is16)
        STORE_BE16(b, (uint16_t)(((uint32_t)(cr[0] >> 11) << 10) | ((uint32_t)(cr[1] >> 11) << 5) | (cr[2] >> 11)));
    else
        *b = (uint8_t)gc_resolve_rgb(p, cr[0], cr[1], cr[2]);
}

// Apply one source/pattern INK bit `s` (0/1, pre-invert) at (x,y) under the
// current mode + clip, at the screen depth (1 or 8 bpp — func $2D only
// accepts those).  The classic boolean cores of qd-transfer-modes.md §2.1,
// colorized: the invert bit (modes 4-7 / 12-15) flips the source first, then
// Copy paints fg on ink and bg elsewhere, Or paints fg where the source has
// ink, Bic paints bg where the source has ink, and Xor flips the ink pixels'
// index bits (never colorized — at 8 bpp that is dst ^= $FF, the B/W-mask
// expansion of §4.1).  fg/bg are the pixel values from opFg/BkColor's
// pixValue.  Arithmetic modes ($20+) need the CLUT→component→ITab model —
// not implemented; they draw nothing (also the ROM's illegal-mode behavior)
// and log so a scene that needs them is visible.
static inline void gc_px(display_card_824gc_priv_t *p, int x, int y, int s) {
    if (x < 0 || x >= (int)p->display.width || y < 0 || y >= (int)p->display.height || y >= 480)
        return;
    // Clip to the drawable mask (clipRgn ∩ visRgn ∩ device) — region-accurate,
    // so a fill clipped to the desktop region does not paint over icons/windows.
    if (!(p->gc_clipmask[y * 80 + (x >> 3)] & (0x80u >> (x & 7))))
        return;
    if (p->gc_mode & 0x20) { // arithmetic family (incl. hilite $32/$3A)
        if (p->display.format == PIXEL_1BPP_MSB) {
            // 1-bit dst: arithmetic remaps through arithMode[] (§1); every
            // variant lands on an xor/or/bic/copy of the B/W source — the
            // pre-overhaul desktop was pixel-exact treating them via the low
            // bits, so keep that (hilite $32 -> variant 2 -> srcXor = bit1).
        } else if ((p->gc_mode & 0x17) == 0x12) { // hilite $32/$3A (§3.1)
            // Where the source has ink (pattern pixel ≠ bk): swap bk↔hilite
            // in the destination; every other dst pixel is untouched.
            if (s && p->gc_fg != p->gc_bg) {
                if (p->display.format == PIXEL_32BPP_XRGB) {
                    uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + (size_t)x * 4;
                    uint32_t d = LOAD_BE32(b) & 0x00FFFFFFu;
                    if (d == (p->gc_bg & 0x00FFFFFFu))
                        STORE_BE32(b, p->gc_hilite & 0x00FFFFFFu);
                    else if (d == (p->gc_hilite & 0x00FFFFFFu))
                        STORE_BE32(b, p->gc_bg & 0x00FFFFFFu);
                    return;
                }
                if (p->display.format == PIXEL_16BPP_555) {
                    uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + (size_t)x * 2;
                    uint16_t d = LOAD_BE16(b) & 0x7FFFu;
                    if (d == (p->gc_bg & 0x7FFFu))
                        STORE_BE16(b, (uint16_t)(p->gc_hilite & 0x7FFFu));
                    else if (d == (p->gc_hilite & 0x7FFFu))
                        STORE_BE16(b, (uint16_t)(p->gc_bg & 0x7FFFu));
                    return;
                }
                uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + x;
                if (*b == (uint8_t)p->gc_bg)
                    *b = (uint8_t)p->gc_hilite;
                else if (*b == (uint8_t)p->gc_hilite)
                    *b = (uint8_t)p->gc_bg;
            }
            return;
        } else { // arithmetic $20-$27 / $28-$2F (§3.1/§3.2)
            uint32_t srcpix = s ? p->gc_fg : p->gc_bg; // colorized pattern pixel
            uint8_t *b;
            if (p->display.format == PIXEL_32BPP_XRGB)
                b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + (size_t)x * 4;
            else if (p->display.format == PIXEL_16BPP_555)
                b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + (size_t)x * 2;
            else
                b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + x;
            gc_px_arith(p, b, srcpix);
            return;
        }
    } else if ((p->gc_mode & 0x04) != 0) {
        s ^= 1; // notSrc/notPat: invert the source before colorizing
    }
    if (p->display.format == PIXEL_32BPP_XRGB) {
        // Direct colour: the §2.3 conjugation folds into the same net visual
        // rule the cores below implement (Or = fg where ink, Bic = bg where
        // ink, Copy = fg/bg); Xor inverts the RGB bits under the ink, alpha
        // byte excluded (NOPfgColorTable masking).
        uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + (size_t)x * 4;
        switch (p->gc_mode & 3) {
        case 1:
            if (s)
                STORE_BE32(b, p->gc_fg & 0x00FFFFFFu);
            break; // Or: fg where ink
        case 2:
            if (s)
                STORE_BE32(b, LOAD_BE32(b) ^ 0x00FFFFFFu);
            break; // Xor
        case 3:
            if (s)
                STORE_BE32(b, p->gc_bg & 0x00FFFFFFu);
            break; // Bic: bg where ink
        default:
            STORE_BE32(b, (s ? p->gc_fg : p->gc_bg) & 0x00FFFFFFu);
            break; // Copy
        }
        return;
    }
    if (p->display.format == PIXEL_16BPP_555) {
        // Direct 15-bit colour: same net visual rules as the 8/32-bpp cores;
        // Xor flips the colour bits (x-bit excluded).
        uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + (size_t)x * 2;
        switch (p->gc_mode & 3) {
        case 1:
            if (s)
                STORE_BE16(b, (uint16_t)(p->gc_fg & 0x7FFFu));
            break; // Or: fg where ink
        case 2:
            if (s)
                STORE_BE16(b, (uint16_t)(LOAD_BE16(b) ^ 0x7FFFu));
            break; // Xor
        case 3:
            if (s)
                STORE_BE16(b, (uint16_t)(p->gc_bg & 0x7FFFu));
            break; // Bic: bg where ink
        default:
            STORE_BE16(b, (uint16_t)((s ? p->gc_fg : p->gc_bg) & 0x7FFFu));
            break; // Copy
        }
        return;
    }
    if (p->display.format == PIXEL_8BPP) {
        uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + x;
        uint8_t fg = (uint8_t)p->gc_fg, bg = (uint8_t)p->gc_bg;
        switch (p->gc_mode & 3) {
        case 1:
            if (s)
                *b = fg;
            break; // Or: fg where ink ((fg & $FF)|(dst & ~$FF))
        case 2:
            if (s)
                *b ^= 0xFF;
            break; // Xor: flip the index bits where ink
        case 3:
            if (s)
                *b = bg;
            break; // Bic: bg where ink
        default:
            *b = s ? fg : bg;
            break; // Copy: fg on ink, bg elsewhere
        }
        return;
    }
    uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + (x >> 3);
    uint8_t m = (uint8_t)(0x80u >> (x & 7));
    switch (p->gc_mode & 3) {
    case 1:
        if (s) {
            if (p->gc_fg & 1)
                *b |= m;
            else
                *b &= (uint8_t)~m;
        }
        break; // Or: fg where ink
    case 2:
        if (s)
            *b ^= m;
        break; // Xor: flip where ink
    case 3:
        if (s) {
            if (p->gc_bg & 1)
                *b |= m;
            else
                *b &= (uint8_t)~m;
        }
        break; // Bic: bg where ink
    default:
        if ((s ? p->gc_fg : p->gc_bg) & 1)
            *b |= m;
        else
            *b &= (uint8_t)~m;
        break; // Copy: fg on ink, bg elsewhere
    }
}
// The source INK bit at (x,y): the active pattern slot's bit.  (op $73 selects
// the slot; op $71 sets a slot's bytes; gc_px colorizes with fg/bg.)  The
// pattern grid is anchored to the PORT's coordinate space (qd-transfer-modes
// §4.1: dstPix.bounds origin + patAlign) — patterns scroll with SetOrigin'd
// content (the Control Panel's cdev list flips the ltGray phase without this).
static inline int gc_src(display_card_824gc_priv_t *p, int x, int y) {
    const uint8_t *pat = p->gc_pat[p->gc_pat_slot & 3];
    unsigned lx = (unsigned)(x - p->gc_org_x - p->gc_align_x), ly = (unsigned)(y - p->gc_org_y - p->gc_align_y);
    return (pat[ly & 7] >> (7 - (lx & 7))) & 1;
}
// Write a full PIXEL VALUE `v` at (x,y) under the current mode + clip — the
// RGBPat/PixPat path: patType != 0 patterns are never colorized (§2.2), so
// the §2.1 cores run non-colorized with the pattern pixel as S.
static void gc_px_val(display_card_824gc_priv_t *p, int x, int y, uint32_t v) {
    if (x < 0 || x >= (int)p->display.width || y < 0 || y >= (int)p->display.height || y >= 480)
        return;
    if (!(p->gc_clipmask[y * 80 + (x >> 3)] & (0x80u >> (x & 7))))
        return;
    if (p->gc_mode & 0x20) {
        LOG(1, "arithmetic/hilite mode $%02x with a colour pattern not modelled — pixel skipped", p->gc_mode);
        return;
    }
    if (p->display.format == PIXEL_32BPP_XRGB) {
        uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + (size_t)x * 4;
        uint32_t d = LOAD_BE32(b);
        switch (p->gc_mode & 3) {
        case 1:
            d |= v;
            break;
        case 2:
            d ^= v;
            break;
        case 3:
            d &= ~v;
            break;
        default:
            d = v;
            break;
        }
        STORE_BE32(b, d & 0x00FFFFFFu);
        return;
    }
    if (p->display.format == PIXEL_16BPP_555) {
        uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + (size_t)x * 2;
        uint16_t d = LOAD_BE16(b);
        switch (p->gc_mode & 3) {
        case 1:
            d |= (uint16_t)v;
            break;
        case 2:
            d ^= (uint16_t)v;
            break;
        case 3:
            d &= (uint16_t)~v;
            break;
        default:
            d = (uint16_t)v;
            break;
        }
        STORE_BE16(b, d & 0x7FFFu);
        return;
    }
    if (p->display.format == PIXEL_8BPP) {
        uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + x;
        switch (p->gc_mode & 3) {
        case 1:
            *b |= (uint8_t)v;
            break;
        case 2:
            *b ^= (uint8_t)v;
            break;
        case 3:
            *b &= (uint8_t)~v;
            break;
        default:
            *b = (uint8_t)v;
            break;
        }
        return;
    }
    uint8_t *b = (uint8_t *)p->display.bits + (size_t)y * p->display.stride + (x >> 3);
    uint8_t m = (uint8_t)(0x80u >> (x & 7));
    int s = (int)(v & 1);
    switch (p->gc_mode & 3) {
    case 1:
        if (s)
            *b |= m;
        break;
    case 2:
        if (s)
            *b ^= m;
        break;
    case 3:
        if (s)
            *b &= (uint8_t)~m;
        break;
    default:
        if (s)
            *b |= m;
        else
            *b &= (uint8_t)~m;
        break;
    }
}
static void gc_span(display_card_824gc_priv_t *p, int y, int l, int r) {
    if (p->gc_pat_kind[p->gc_pat_slot & 3] == 3) {
        // Cached PixPat tile: port-anchored, power-of-two wrap (QD requires
        // PixPat bounds to be powers of two).  patType != 0 patterns are
        // never colorized (§2.2) — value cores, like the RGB dither.
        const struct gc_pixpat *pp = &p->gc_pixpats[p->gc_pat_pp[p->gc_pat_slot & 3]];
        const uint32_t *row = pp->pix + ((unsigned)(y - p->gc_org_y - p->gc_align_y) & (pp->h - 1u)) * pp->w;
        for (int x = l; x < r; x++)
            gc_px_val(p, x, y, row[(unsigned)(x - p->gc_org_x - p->gc_align_x) & (pp->w - 1u)]);
        return;
    }
    if (p->gc_pat_kind[p->gc_pat_slot & 3] == 2) {
        // RGB 2x2 dither: even rows use cells 0/1, odd rows 2/3, in the
        // PORT's coordinate space (same anchor rule as classic patterns).
        const uint32_t *cell = p->gc_pat_cell[p->gc_pat_slot & 3];
        unsigned ly = (unsigned)(y - p->gc_org_y - p->gc_align_y);
        for (int x = l; x < r; x++) {
            unsigned lx = (unsigned)(x - p->gc_org_x - p->gc_align_x);
            gc_px_val(p, x, y, cell[((ly & 1) << 1) | (lx & 1)]);
        }
        return;
    }
    for (int x = l; x < r; x++)
        gc_px(p, x, y, gc_src(p, x, y));
}
// Resolve a 48-bit QuickDraw RGB to a device pixel at the screen depth — the
// card-side Color2Index.  1 bpp: luminance threshold (dark → black = 1).
// 8 bpp: nearest CLUT entry; exact for black/white and the standard palette.
// Caveat: p->clut holds the DAC (gamma-corrected) values the ACDC was
// programmed with, so a mid-colour can land one index off the ROM's ITab
// answer — revisit with a logical-CLUT capture if a colour scene ever shows
// it in the differential oracle.
static uint32_t gc_resolve_rgb(display_card_824gc_priv_t *p, uint16_t r, uint16_t g, uint16_t b) {
    if (p->display.format == PIXEL_32BPP_XRGB) // direct: 00RRGGBB, no CLUT
        return ((uint32_t)(r >> 8) << 16) | ((uint32_t)(g >> 8) << 8) | (b >> 8);
    if (p->display.format == PIXEL_16BPP_555) // direct: 0RRRRRGGGGGBBBBB
        return ((uint32_t)(r >> 11) << 10) | ((uint32_t)(g >> 11) << 5) | (b >> 11);
    if (p->display.format != PIXEL_8BPP) {
        uint32_t lum = (uint32_t)r + g + b;
        return (lum < 0x18000u) ? 1u : 0u;
    }
    int best = 0;
    uint32_t bestd = 0xFFFFFFFFu;
    for (int i = 0; i < 256; i++) {
        int dr = (int)(r >> 8) - p->clut[i].r;
        int dg = (int)(g >> 8) - p->clut[i].g;
        int db = (int)(b >> 8) - p->clut[i].b;
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return (uint32_t)best;
}
// Cursor shield (gc-cursor-protocol.md §6-§8).  The real card brackets every
// screen-touching op with erase-cursor / redraw-cursor (text_2e7e0/text_2ead0):
// it bus-master-reads the live CrsrRect/CrsrVis through the host addresses the
// driver deposited at CB+$5F8/$5FC, erases the on-screen sprite by restoring
// the ROM's saved under-cursor bits, and reports through two flags the host
// cursor stubs and GACursorTask consume — CB+$5F0 "card hid the cursor" (host
// clears CrsrVis) and CB+$5F4 "cursor changed" (host sets CrsrNew, making the
// ROM redraw with a FRESH under-cursor save).  Both halves matter: without the
// flags the ROM later restores its STALE under-cursor bits over whatever the
// card drew (a 32×6 px desktop-pattern bleed in the menu bar at the boot
// cursor position), and without the erase the part of the sprite the op does
// not repaint stays baked into the framebuffer (a watch-cursor fragment).
static void gc_cursor_shield(display_card_824gc_priv_t *p, int t, int l, int b, int r) {
    uint32_t rect_addr = dram_be32(p, GC824_DRAM_CB + GC824_CB_CRSRRECTP);
    uint32_t vis_addr = dram_be32(p, GC824_DRAM_CB + GC824_CB_CRSRVISP);
    if (!rect_addr || !vis_addr) {
        // The driver seeds these at the end of Open (sub_101A) with the
        // physical addresses of the fixed low-memory cursor globals — but the
        // cursor block at CB+$5F0..$603 lies inside the Boot ACEF's InitMap
        // BSS section ($7400..$77FF), so a post-Open firmware reload zero-fills
        // it (our bring-up provokes one; see findings.md).  The seeded values
        // are architecturally fixed, so recover them — but only once GCQD's
        // cursor patches are armed (L1 anchor at CrsrAddr $0888 carries the
        // magic $075BCD15 at +$20), i.e. the cursor contract is actually live.
        uint32_t l1 = memory_debug_read_uint32(0x0888) & 0x00FFFFFFu;
        if (!l1 || memory_debug_read_uint32(l1 + 0x20) != 0x075BCD15u)
            return; // cursor protocol not armed
        rect_addr = 0x083C; // CrsrRect
        vis_addr = 0x08CC; // CrsrVis
    }
    if (dram_be32(p, GC824_DRAM_CB + GC824_CB_CRSRHID))
        return; // already hidden by us and not yet re-shown (card gate, §6 step 2)
    if (!memory_debug_read_uint8(vis_addr))
        return; // cursor not on screen — nothing to shield
    int ct = (int16_t)memory_debug_read_uint16(rect_addr);
    int cl = (int16_t)memory_debug_read_uint16(rect_addr + 2);
    int cb = (int16_t)memory_debug_read_uint16(rect_addr + 4);
    int cr = (int16_t)memory_debug_read_uint16(rect_addr + 6);
    if (b <= ct || t >= cb || r <= cl || l >= cr)
        return; // op doesn't overlap the sprite
    // Erase the sprite the way the card's text_2e7e0 does: restore the ROM's
    // own saved under-cursor bits.  The CrsrPtr save record holds a DBF-style
    // blit descriptor of the LAST save — +$3E.w rows−1, +$42.w rowLongs−1,
    // +$44.l dest stride advance per row, +$48.l the screen address saved at,
    // and **(rec+$12) = the CCSAVE under-bits buffer.  Restoring the whole
    // cell (not just the op's overlap) is essential: a flags-only hide leaves
    // the un-repainted part of the sprite baked into the framebuffer.
    uint32_t rec = dram_be32(p, GC824_DRAM_CB + 0x1E4); // host addr of the save record
    if (!rec) {
        uint32_t pp = memory_debug_read_uint32(0x0D62) & 0x00FFFFFFu; // CrsrPtr
        if (pp)
            rec = memory_debug_read_uint32(pp);
    }
    rec &= 0x00FFFFFFu; // strip master-pointer flag bits
    if (rec) {
        int rows = (int)(int16_t)memory_debug_read_uint16(rec + 0x3E) + 1;
        int rowlongs = (int)(int16_t)memory_debug_read_uint16(rec + 0x42) + 1;
        uint32_t stride = memory_debug_read_uint32(rec + 0x44);
        uint32_t dest = memory_debug_read_uint32(rec + 0x48);
        uint32_t bufp = memory_debug_read_uint32(rec + 0x12) & 0x00FFFFFFu;
        uint32_t buf = bufp ? (memory_debug_read_uint32(bufp) & 0x00FFFFFFu) : 0;
        uint32_t doff = (dest & 0x0FFFFFFFu) - GC824_DRAM_OFFSET; // ScrnBase is super-slot
        uint32_t pitch = 4u * (uint32_t)rowlongs + stride;
        if (buf && rows > 0 && rows <= 64 && rowlongs > 0 && rowlongs <= 8 &&
            (dest & 0xF0000000u) == (p->super_base & 0xF0000000u) && doff >= GC824_FB_OFFSET &&
            doff + (uint32_t)(rows - 1) * pitch + 4u * (uint32_t)rowlongs <= GC824_DRAM_SIZE) {
            for (int i = 0; i < rows; i++)
                for (int j = 0; j < 4 * rowlongs; j++)
                    p->dram[doff + (uint32_t)i * pitch + (uint32_t)j] =
                        memory_debug_read_uint8(buf + (uint32_t)(i * 4 * rowlongs + j));
            p->display.fb_dirty = true;
        }
    }
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_CRSRHID, 1);
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_CRSRNEW, 1);
}
static void gc_fill_rect(display_card_824gc_priv_t *p, int t, int l, int b, int r) {
    gc_cursor_shield(p, t, l, b, r);
    for (int y = t; y < b; y++)
        gc_span(p, y, l, r);
}
// Frame ring inset by pen (call-reference §2 FrmRect): degenerate → solid fill.
static void gc_frame_rect(display_card_824gc_priv_t *p, int t, int l, int b, int r) {
    int pw = p->gc_pen_w ? p->gc_pen_w : 1, ph = p->gc_pen_h ? p->gc_pen_h : 1;
    if (l + pw >= r - pw || t + ph >= b - ph) {
        gc_fill_rect(p, t, l, b, r);
        return;
    }
    gc_fill_rect(p, t, l, t + ph, r); // top bar
    gc_fill_rect(p, b - ph, l, b, r); // bottom bar
    gc_fill_rect(p, t + ph, l, b - ph, l + pw); // left bar
    gc_fill_rect(p, t + ph, r - pw, b - ph, r); // right bar
}
// Rounded-rect row inset (call-reference §3 DrawArc conic).  For a corner oval
// of ovW×ovH, row dy (0-based within the oval's vertical extent) starts ovW/2
// minus the widest dx whose pixel CENTRE lies inside the ellipse — QuickDraw's
// InitOval/BumpOval march evaluates the conic at half-pixel offsets, which in
// integer form is (2dx+1−W)²·H² + (2dy+1−H)²·W² ≤ W²·H².  Verified pixel-exact
// against the ROM-rendered menu-bar corners (ov 16 fill + ov 22 frame ring).
static int gc_rrect_inset(int ovW, int ovH, int dy) {
    int64_t W = ovW, H = ovH;
    int64_t ny = 2 * dy + 1 - H;
    int64_t lim = W * W * H * H - ny * ny * W * W;
    if (lim < 0)
        return ovW / 2; // row entirely outside the oval
    for (int dx = 0; dx < ovW / 2; dx++) {
        int64_t nx = 2 * dx + 1 - W;
        if (nx * nx * H * H <= lim)
            return dx; // first pixel centre inside the ellipse
    }
    return ovW / 2;
}
// The row inset for a rounded rect (t..b) at row y: nonzero only in the top and
// bottom corner bands (each ovH/2 rows deep).  Radii are pinned to the rect
// dimensions exactly as DrawArc's InitOval does (negative → 0).
static int gc_rrect_row_inset(int t, int b, int ovW, int ovH, int y) {
    if (ovW <= 0 || ovH <= 0)
        return 0;
    if (y - t < (ovH + 1) / 2)
        return gc_rrect_inset(ovW, ovH, y - t);
    if ((b - 1 - y) < (ovH + 1) / 2)
        return gc_rrect_inset(ovW, ovH, ovH - 1 - (b - 1 - y));
    return 0;
}
// opRRect ($04): fill / frame with true rounded corners.  The wire record
// carries ovWd/ovHt (call-reference §3: 16-byte record); the frame's inner
// rrect is the rect inset by the pen with radii shrunk by 2·pen, an empty
// inner degenerating to a solid fill — exactly ROM DrawArc's frame path.
static void gc_fill_rrect(display_card_824gc_priv_t *p, int t, int l, int b, int r, int ovW, int ovH) {
    gc_cursor_shield(p, t, l, b, r);
    if (ovW > r - l)
        ovW = r - l;
    if (ovH > b - t)
        ovH = b - t;
    for (int y = t; y < b; y++) {
        int in = gc_rrect_row_inset(t, b, ovW, ovH, y);
        gc_span(p, y, l + in, r - in);
    }
}
static void gc_frame_rrect(display_card_824gc_priv_t *p, int t, int l, int b, int r, int ovW, int ovH) {
    gc_cursor_shield(p, t, l, b, r);
    int pw = p->gc_pen_w ? p->gc_pen_w : 1, ph = p->gc_pen_h ? p->gc_pen_h : 1;
    int it = t + ph, il = l + pw, ib = b - ph, ir = r - pw;
    if (il >= ir || it >= ib) {
        gc_fill_rrect(p, t, l, b, r, ovW, ovH);
        return;
    }
    if (ovW > r - l)
        ovW = r - l;
    if (ovH > b - t)
        ovH = b - t;
    int iovW = ovW - 2 * pw, iovH = ovH - 2 * ph;
    if (iovW < 0)
        iovW = 0;
    if (iovH < 0)
        iovH = 0;
    if (iovW > ir - il)
        iovW = ir - il;
    if (iovH > ib - it)
        iovH = ib - it;
    for (int y = t; y < b; y++) {
        int oin = gc_rrect_row_inset(t, b, ovW, ovH, y);
        int ol = l + oin, or_ = r - oin;
        if (y < it || y >= ib) {
            gc_span(p, y, ol, or_); // above/below the inner rect: full outer row
            continue;
        }
        int iin = gc_rrect_row_inset(it, ib, iovW, iovH, y);
        int lo = il + iin, ri = ir - iin;
        gc_span(p, y, ol, lo); // left arm of the ring
        gc_span(p, y, ri, or_); // right arm
    }
}
// === Ovals & arcs (opOval $03 / opArc $02) — ROM DrawArc port =================
// QuickDraw/DrawArc.a is the single curve engine: ovals and arcs are rrects
// whose corner-oval radii are the RECT DIMENSIONS (Ovals.a:97-128, Arcs.a:107-
// 131), so every row's span bounds come from the same conic gc_rrect_inset
// evaluates.  Arcs add the wedge machinery (DrawArc.a:286-447, 727-907): each
// of start/stop angle becomes a line through the rect centre whose horizontal
// coordinate at the rect top is LINE = midH·64K − slope·(height div 2), bumped
// by slope every row; slope = FixMul(SlopeFromAngle(angle), FixRatio(w,h)).
// FLAG = angle<180 ? angle−90 : 270−angle marks the vertical half the edge is
// active in (negative = active), negated as the scan crosses the middle.  The
// wedge edges only CLIP each row's spans (frame strokes just the curve band;
// paint fills the wedge to the centre) — they are never stroked themselves.

// SlopeFromAngle (QuickDraw/Angles.a): Fixed −65536·tan(angle), angle mod 180.
// Table-driven exactly like the ROM so the wedge edges land on the ROM's
// pixels: fraction words for 0°..90° (45°+ get an integer part: +1 for
// 45..63, the byte table for 64..90; 90° yields the ±$7FFFFFFF/$80000001
// "infinity" sentinel, whose value is never consumed — a 90°/270° edge has
// FLAG 0 and is never active).
static const uint16_t gc_slope_frac[91] = {
    0x0000, 0x0478, 0x08F1, 0x0D6B, 0x11E7, 0x1666, 0x1AE8, 0x1F6F, 0x23FA, 0x288C, 0x2D24, 0x31C3, 0x366A,
    0x3B1A, 0x3FD4, 0x4498, 0x4968, 0x4E44, 0x532E, 0x5826, 0x5D2D, 0x6245, 0x676E, 0x6CAA, 0x71FB, 0x7760,
    0x7CDC, 0x8270, 0x881E, 0x8DE7, 0x93CD, 0x99D2, 0x9FF7, 0xA640, 0xACAD, 0xB341, 0xB9FF, 0xC0E9, 0xC802,
    0xCF4E, 0xD6CF, 0xDE8A, 0xE681, 0xEEB9, 0xF737, 0x0000, 0x0919, 0x1287, 0x1C51, 0x267F, 0x3117, 0x3C22,
    0x47AA, 0x53B9, 0x605B, 0x6D9B, 0x7B89, 0x8A35, 0x99AF, 0xAA0E, 0xBB68, 0xCDD6, 0xE177, 0xF66E, 0x0CE1,
    0x24FE, 0x3EFC, 0x5B19, 0x799F, 0x9AE7, 0xBF5B, 0xE77A, 0x13E3, 0x4556, 0x7CC7, 0xBB68, 0x02C2, 0x54DB,
    0xB462, 0x2501, 0xABD9, 0x5051, 0x1D88, 0x24F3, 0x83AD, 0x6E17, 0x4CF5, 0x14BD, 0xA2D7, 0x4A30, 0xFFFF,
};
static const uint8_t gc_slope_int[28] = {
    // integer part of tan(a) for a = 63..90 (Angles.a byte table)
    0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03, 0x03, 0x03, 0x04,
    0x04, 0x04, 0x05, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0E, 0x13, 0x1C, 0x39, 0xFF,
};
static int32_t gc_slope_from_angle(int angle) {
    int a = angle % 180;
    if (a < 0)
        a += 180;
    int neg = 1; // 0..90 → negative result (the $8000 flag in Angles.a)
    if (a > 90) {
        neg = 0;
        a = 180 - a;
    }
    if (a == 90)
        return neg ? (int32_t)0x80000001 : INT32_MAX; // "infinity" sentinel
    uint32_t mag = gc_slope_frac[a];
    if (a >= 64)
        mag |= (uint32_t)gc_slope_int[a - 63] << 16;
    else if (a >= 45)
        mag |= 0x10000u;
    return neg ? -(int32_t)mag : (int32_t)mag;
}
// FixMul: middle 32 bits of the 64-bit product, rounded with the 33rd bit
// (FixMathAsm.a header).  Overflow wraps (the only overflowing input is the
// 90° sentinel, whose product is never consumed).
static int32_t gc_fix_mul(int32_t a, int32_t b) {
    return (int32_t)(uint32_t)(((int64_t)a * b + 0x8000) >> 16);
}
// FixRatio: (n<<16)/d, DIVS semantics — truncation toward zero, div-by-zero
// pins to ±$7FFFFFFF by sign of the numerator.
static int32_t gc_fix_ratio(int n, int d) {
    if (d == 0)
        return n >= 0 ? INT32_MAX : INT32_MIN;
    return (int32_t)(((int64_t)n << 16) / d);
}

// The DrawArc body: fill/frame the arc of the oval inscribed in (t,l,b,r) from
// `start` spanning `arc` degrees (oval = 0/360).  Faithful transcription of
// DrawArc.a's DOARC/HARC/SARC/SOVAL/HOVAL row cases; span bounds and LINE
// comparisons use the 16-bit-wrapped high word exactly like the ROM's .W
// arithmetic.  Rows outside the clip are masked per-pixel by gc_px (the ROM
// pre-clips via MINRECT — same pixels).
static void gc_draw_arc(display_card_824gc_priv_t *p, int t, int l, int b, int r, int start, int arc, int frame) {
    if (r <= l || b <= t)
        return; // empty dst rect
    // Angle normalization (DrawArc.a:286-297): quit on 0, flip negative arcs.
    if (arc == 0)
        return;
    if (arc < 0) {
        start += arc;
        arc = -arc;
    }
    int arcflag = arc < 360;
    int32_t line1 = 0, line2 = 0, slope1 = 0, slope2 = 0;
    int flag1 = 0, flag2 = 0, skip = 0;
    if (arcflag) {
        start %= 360;
        if (start < 0)
            start += 360; // DIVS remainder fixup (DrawArc.a:304-311)
        int stop = start + arc;
        if (stop >= 360)
            stop -= 360; // single subtract suffices: start+arc < 720
        int w = r - l, h = b - t;
        int32_t ratio = gc_fix_ratio(w, h); // aspect: 45° points at the corner
        slope1 = gc_fix_mul(gc_slope_from_angle(start), ratio);
        slope2 = gc_fix_mul(gc_slope_from_angle(stop), ratio);
        int midh = (l + r) >> 1, hd2 = h >> 1;
        line1 = (int32_t)((uint32_t)midh << 16) - (int32_t)(uint32_t)((int64_t)slope1 * hd2);
        line2 = (int32_t)((uint32_t)midh << 16) - (int32_t)(uint32_t)((int64_t)slope2 * hd2);
        flag1 = (start < 180) ? start - 90 : 270 - start;
        flag2 = (stop < 180) ? stop - 90 : 270 - stop;
        if (arc > 180)
            skip = 0;
        else if (arc == 180)
            skip = (start == 90); // bottom-half wedge: skip the top half
        else
            skip = (flag1 >= 0 && flag2 >= 0);
    }
    int midv = (t + b) >> 1;
    int ovW = r - l, ovH = b - t; // radii = rect dims (Ovals.a:97-103)
    // Inner oval (frame): rect inset by the pen, radii shrunk by 2·pen; an
    // empty inner rect degenerates to a solid fill (DrawArc.a:482-510).
    int pw = p->gc_pen_w ? p->gc_pen_w : 1, ph = p->gc_pen_h ? p->gc_pen_h : 1;
    int it = t + ph, il = l + pw, ib = b - ph, ir = r - pw;
    int hollow = frame && il < ir && it < ib;
    int iovW = 0, iovH = 0;
    if (hollow) {
        iovW = ovW - 2 * pw;
        iovH = ovH - 2 * ph;
        if (iovW < 0)
            iovW = 0;
        if (iovH < 0)
            iovH = 0;
        if (iovW > ir - il)
            iovW = ir - il;
        if (iovH > ib - it)
            iovH = ib - it;
    }
    gc_cursor_shield(p, t, l, b, r);
    for (int y = t; y < b; y++) {
        // Mid-crossing (DrawArc.a:727-757): negate the flags, recompute
        // SKIPFLAG (arc==180 keys on start==270 here — the top-half wedge is
        // finished), quit if it trips, then swap the line/slope/flag pairs.
        if (arcflag && y == midv) {
            flag1 = -flag1;
            flag2 = -flag2;
            skip = 0;
            if (arc == 180) {
                if (start == 270)
                    break;
            } else if (arc < 180) {
                if (flag1 >= 0 && flag2 >= 0)
                    break;
            }
            int tf = flag1;
            flag1 = flag2;
            flag2 = tf;
            int32_t tv = line1;
            line1 = line2;
            line2 = tv;
            tv = slope1;
            slope1 = slope2;
            slope2 = tv;
        }
        if (!skip) {
            int in = gc_rrect_row_inset(t, b, ovW, ovH, y);
            int oOvL = l + in, oOvR = r - in; // the row's outer oval bounds
            int l1i = (int16_t)((uint32_t)line1 >> 16); // .W compares, as ROM
            int l2i = (int16_t)((uint32_t)line2 >> 16);
            int oL = oOvL, oR = oOvR;
            if (arcflag) { // DOARC: clip to the active wedge edges
                if (flag1 < 0 && oL < l1i)
                    oL = l1i;
                if (flag2 < 0 && oR > l2i)
                    oR = l2i;
            }
            int inner_row = hollow && y >= it && y < ib;
            int iOvL = 0, iOvR = 0;
            if (inner_row) {
                int iin = gc_rrect_row_inset(it, ib, iovW, iovH, y);
                iOvL = il + iin;
                iOvR = ir - iin;
            }
            if (!arcflag) {
                if (inner_row) { // HOVAL: ring row, two slabs
                    gc_span(p, y, oOvL, iOvL);
                    gc_span(p, y, iOvR, oOvR);
                } else { // SOVAL
                    gc_span(p, y, oOvL, oOvR);
                }
            } else if (!inner_row) { // SARC
                if (oL < oR)
                    gc_span(p, y, oL, oR);
                else if (flag1 < 0 && flag2 < 0 && arc > 180) {
                    // wedge wraps the vertical midline: two slabs
                    gc_span(p, y, oOvL, oR);
                    gc_span(p, y, oL, oOvR);
                }
            } else { // HARC: ring row clipped by the wedge edges
                int iL = iOvL, iR = iOvR;
                if (flag2 < 0 && iL > l2i)
                    iL = l2i;
                if (flag1 < 0 && iR < l1i)
                    iR = l1i;
                if (oL < oR) {
                    gc_span(p, y, oL, iL);
                    gc_span(p, y, iR, oR);
                } else if (flag1 < 0 && flag2 < 0 && arc > 180) {
                    // wraparound, with the ROM's third-slab coalescing
                    if (oR == iL)
                        gc_span(p, y, oL, iOvL);
                    else if (oL == iR)
                        gc_span(p, y, iOvR, oR);
                    gc_span(p, y, oOvL, iL);
                    gc_span(p, y, iR, oOvR);
                }
            }
        }
        line1 += slope1; // bumped EVERY row, drawn or not (DrawArc.a NODRAW)
        line2 += slope2;
    }
}

// opLine ($01): pen-sized line from the pen loc to `pt`; pen := pt.  For the
// axis-aligned lines the desktop draws this reduces to a pen-thick bar; a
// general Bresenham covers the rest (call-reference §1).
static void gc_line_to(display_card_824gc_priv_t *p, int x1, int y1) {
    int x0 = p->gc_pen_x, y0 = p->gc_pen_y;
    int pw = p->gc_pen_w ? p->gc_pen_w : 1, ph = p->gc_pen_h ? p->gc_pen_h : 1;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx >= ady) {
        int lo = dx >= 0 ? x0 : x1, hi = dx >= 0 ? x1 : x0, yy = dx >= 0 ? y0 : y1;
        int sy = (dy != 0) ? ((dx >= 0) == (dy > 0) ? 1 : -1) : 0;
        int err = adx / 2;
        for (int x = lo; x < hi; x++) {
            gc_fill_rect(p, yy, x, yy + ph, x + pw);
            err += ady;
            if (err >= adx) {
                err -= adx;
                yy += sy;
            }
        }
    } else {
        int lo = dy >= 0 ? y0 : y1, hi = dy >= 0 ? y1 : y0, xx = dy >= 0 ? x0 : x1;
        int sx = (dx != 0) ? ((dy >= 0) == (dx > 0) ? 1 : -1) : 0;
        int err = ady / 2;
        for (int y = lo; y < hi; y++) {
            gc_fill_rect(p, y, xx, y + ph, xx + pw);
            err += adx;
            if (err >= ady) {
                err -= ady;
                xx += sx;
            }
        }
    }
    p->gc_pen_x = x1;
    p->gc_pen_y = y1;
}
// opRgn ($08): fill a QuickDraw region.  Rectangular regions (rgnSize 0x0A)
// are their bbox; complex regions are the classic band/inversion list
// {y, x-pairs, 0x7FFF, …, 0x7FFF} (call-reference §7).
static void gc_fill_rgn(display_card_824gc_priv_t *p, uint32_t off, int ox, int oy) {
    uint16_t size = (uint16_t)(dram_be32(p, off) >> 16);
    int top = (int16_t)(dram_be32(p, off) & 0xFFFF) + oy;
    int left = (int16_t)(dram_be32(p, off + 4) >> 16) + ox;
    int bot = (int16_t)(dram_be32(p, off + 4) & 0xFFFF) + oy;
    int right = (int16_t)(dram_be32(p, off + 8) >> 16) + ox;
    if (size <= 0x0A) {
        gc_fill_rect(p, top, left, bot, right);
        return;
    }
    gc_cursor_shield(p, top, left, bot, right);
    uint32_t d = off + 10; // band data follows the 10-byte header
    uint32_t dend = off + size;
    int inv[128];
    int ninv = 0;
    int y0raw = (int16_t)dram_be16(p, d);
    int y = (y0raw == 0x7FFF) ? 0x7FFF : y0raw + oy;
    int guard = 0;
    while (y != 0x7FFF && d + 2 <= dend && guard++ < 2000) {
        d += 2;
        for (;;) {
            int x = (int16_t)dram_be16(p, d);
            d += 2;
            if (x == 0x7FFF)
                break;
            x += ox;
            // XOR-toggle x into the sorted inversion set.
            int i = 0;
            while (i < ninv && inv[i] < x)
                i++;
            if (i < ninv && inv[i] == x) {
                for (int k = i; k + 1 < ninv; k++)
                    inv[k] = inv[k + 1];
                ninv--;
            } else if (ninv < 128) {
                for (int k = ninv; k > i; k--)
                    inv[k] = inv[k - 1];
                inv[i] = x;
                ninv++;
            }
        }
        int nyraw = (int16_t)dram_be16(p, d);
        int ny = (nyraw == 0x7FFF) ? 0x7FFF : nyraw + oy;
        int bandBot = (ny == 0x7FFF) ? bot : ny;
        for (int yy = y; yy < bandBot; yy++)
            for (int s = 0; s + 1 < ninv; s += 2)
                gc_span(p, yy, inv[s], inv[s + 1]);
        y = ny;
    }
}

// === Polygons (opPoly $07) — ROM DrawPoly/PaintVector port ====================
// Fill = the vector scan converter of QuickDraw/QuickPolys.a ("does not use
// QuickDraw regions"): one {upperPoint, dy, dx} vector per NON-horizontal edge
// (QuickPolys.a:1009-1026), auto-closed if the poly is open (1028-1049), sorted
// by (y,x); then an X-sorted active-edge-list walk per row with hard-wired
// even-odd toggling.  Edges enter with the PutLine bias — "excerpted from the
// PutLine code … to match the old polygon code precisely" (630-653):
//   XValue = (x<<16) + $8000 + slope/2,  slope = FixRatio(dx,dy)
//   + slope  if 0 <= slope < 1.0
//   + 1.0    if slope < -1.0
// Slab bounds are the .W high words; a slab draws only when right > left; an
// edge leaves the list when LastY (= upperY + |dy|) <= currentY; after each
// row's use XValue += slope with a backward re-sort on crossing.

typedef struct {
    int16_t y, x; // upper endpoint
    int16_t dy, dx; // directed deltas (old → new), dy ≠ 0
} gc_pvec_t;

static int gc_pvec_cmp(const void *a, const void *b) {
    // SortVectors: ascending by the packed {y,x} long (y major, x minor).
    const gc_pvec_t *va = a, *vb = b;
    int32_t ka = ((int32_t)va->y << 16) | (uint16_t)va->x;
    int32_t kb = ((int32_t)vb->y << 16) | (uint16_t)vb->x;
    return (ka > kb) - (ka < kb);
}

#define GC_POLY_MAXPTS 1024 // > any poly record the 4 KB queue can carry

static void gc_fill_poly(display_card_824gc_priv_t *p, uint32_t off, int npts, int ox, int oy) {
    // `off` = DRAM offset of the Polygon record: size.w, bbox Rect, points.
    int bbT = (int16_t)dram_be16(p, off + 2) + oy, bbB = (int16_t)dram_be16(p, off + 6) + oy;
    int bbL = (int16_t)dram_be16(p, off + 4) + ox, bbR = (int16_t)dram_be16(p, off + 8) + ox;
    if (npts > GC_POLY_MAXPTS)
        return; // can't happen off the wire; guard the stack
    gc_pvec_t v[GC_POLY_MAXPTS + 1];
    int nv = 0;
    int16_t py = (int16_t)((int16_t)dram_be16(p, off + 10) + oy), px = (int16_t)((int16_t)dram_be16(p, off + 12) + ox);
    int16_t fy = py, fx = px;
    for (int i = 1; i <= npts; i++) {
        int16_t ny, nx;
        if (i < npts) {
            ny = (int16_t)((int16_t)dram_be16(p, off + 10 + 4 * i) + oy);
            nx = (int16_t)((int16_t)dram_be16(p, off + 12 + 4 * i) + ox);
        } else { // auto-close (skipped when already closed)
            if (py == fy && px == fx)
                break;
            ny = fy;
            nx = fx;
        }
        int dy = ny - py, dx = nx - px;
        if (dy != 0) { // horizontal edges dropped; store the upper endpoint
            v[nv].y = dy > 0 ? py : ny;
            v[nv].x = dy > 0 ? px : nx;
            v[nv].dy = (int16_t)dy;
            v[nv].dx = (int16_t)dx;
            nv++;
        }
        py = ny;
        px = nx;
    }
    if (nv == 0)
        return;
    qsort(v, (size_t)nv, sizeof(v[0]), gc_pvec_cmp);
    gc_cursor_shield(p, bbT, bbL, bbB, bbR);
    // Active edge list: doubly linked by index, ascending 32-bit XValue.
    int32_t xv[GC_POLY_MAXPTS + 1], xs[GC_POLY_MAXPTS + 1];
    int lasty[GC_POLY_MAXPTS + 1], nxt[GC_POLY_MAXPTS + 1], prv[GC_POLY_MAXPTS + 1];
    int head = -1, iv = 0;
    int y = v[0].y;
    while (y < bbB) {
        // Enter the edges starting on this row, insertion-sorted by XValue.
        for (; iv < nv && v[iv].y == y; iv++) {
            int32_t slope = gc_fix_ratio(v[iv].dx, v[iv].dy);
            int32_t x = (int32_t)(((uint32_t)(uint16_t)v[iv].x << 16) | 0x8000u) + (slope >> 1);
            if (slope >= 0 && slope < 0x10000)
                x += slope;
            else if (slope < -0x10000)
                x += 0x10000;
            xv[iv] = x;
            xs[iv] = slope;
            lasty[iv] = y + (v[iv].dy < 0 ? -v[iv].dy : v[iv].dy);
            int q = head, pr = -1;
            while (q != -1 && xv[q] <= x) {
                pr = q;
                q = nxt[q];
            }
            prv[iv] = pr;
            nxt[iv] = q;
            if (pr == -1)
                head = iv;
            else
                nxt[pr] = iv;
            if (q != -1)
                prv[q] = iv;
        }
        // Walk the list: even-odd toggle, slab, bump + backward re-sort.
        int parity = 0, sx = 0;
        for (int e = head, nx_ = -1; e != -1; e = nx_) {
            nx_ = nxt[e]; // captured, as the ROM's A4 (re-sorts don't disturb it)
            if (lasty[e] <= y) { // expire the edge
                if (prv[e] != -1)
                    nxt[prv[e]] = nxt[e];
                else
                    head = nxt[e];
                if (nxt[e] != -1)
                    prv[nxt[e]] = prv[e];
                continue;
            }
            int xi = (int16_t)((uint32_t)xv[e] >> 16); // .W high word, as ROM
            if ((parity ^= 1) != 0)
                sx = xi; // slab opens
            else if (xi > sx)
                gc_span(p, y, sx, xi); // slab closes: [sx, xi)
            xv[e] += xs[e]; // bump to the next row
            if (prv[e] != -1 && xv[e] < xv[prv[e]]) { // crossing: re-sort backward
                int q = prv[e];
                nxt[q] = nxt[e];
                if (nxt[e] != -1)
                    prv[nxt[e]] = q;
                while (prv[q] != -1 && xv[prv[q]] > xv[e])
                    q = prv[q];
                prv[e] = prv[q];
                nxt[e] = q;
                if (prv[q] == -1)
                    head = e;
                else
                    nxt[prv[q]] = e;
                prv[q] = e;
            }
        }
        y++;
        if (head == -1) { // list drained: hop to the next vector's start row
            if (iv >= nv)
                break;
            if (v[iv].y > y)
                y = v[iv].y;
        }
    }
}

// === Clip mask (region-accurate) ============================================
// gc_clipmask is 1 bit/pixel over 640x480 (stride 80): 1 = drawable.  Reset to
// all-drawable at a cycle start, then AND'd by opClipRgn/opVisRgn so a fill
// clipped to the desktop region leaves icons/windows untouched (call-ref §2:
// "rect ∩ device rect then the cached clip/vis region masks").
static void gc_clip_reset(display_card_824gc_priv_t *p) {
    memset(p->gc_clipmask, 0xFF, (size_t)GC824_CLIP_STRIDE * GC824_CLIP_ROWS);
}
// AND a QuickDraw region (raw bytes: rgnSize.w, bbox Rect, band data) into a
// 1-bpp mask (stride GC824_CLIP_STRIDE, GC824_CLIP_ROWS rows): clear every
// pixel OUTSIDE the region.  Serves both the queue's opClipRgn/opVisRgn (region
// bytes in card DRAM) and func $15's rgnA/B/C (bytes fetched from guest RAM).
static void gc_mask_and_region_row(uint8_t *mask, int y, int x0, int x1) {
    if (x0 < 0)
        x0 = 0;
    if (x1 > GC824_CLIP_STRIDE * 8)
        x1 = GC824_CLIP_STRIDE * 8;
    uint8_t *row = &mask[(size_t)y * GC824_CLIP_STRIDE];
    for (int x = x0; x < x1; x++)
        row[x >> 3] &= (uint8_t) ~(0x80u >> (x & 7));
}
static void gc_mask_and_region(uint8_t *mask, const uint8_t *rgn, uint32_t maxlen, int ox, int oy) {
    if (maxlen < 10)
        return;
    uint16_t size = LOAD_BE16(rgn);
    int top = (int16_t)LOAD_BE16(rgn + 2) + oy;
    int left = (int16_t)LOAD_BE16(rgn + 4) + ox;
    int bot = (int16_t)LOAD_BE16(rgn + 6) + oy;
    int right = (int16_t)LOAD_BE16(rgn + 8) + ox;
    if (top < 0)
        top = 0;
    if (bot > GC824_CLIP_ROWS)
        bot = GC824_CLIP_ROWS;
    // Everything outside the bounding box is outside the region.
    for (int y = 0; y < top && y < GC824_CLIP_ROWS; y++)
        memset(&mask[(size_t)y * GC824_CLIP_STRIDE], 0, GC824_CLIP_STRIDE);
    for (int y = (bot < 0 ? 0 : bot); y < GC824_CLIP_ROWS; y++)
        memset(&mask[(size_t)y * GC824_CLIP_STRIDE], 0, GC824_CLIP_STRIDE);
    if (size <= 0x0A) { // rectangular region: clear the columns outside [left,right)
        for (int y = top; y < bot; y++) {
            gc_mask_and_region_row(mask, y, 0, left);
            gc_mask_and_region_row(mask, y, right, GC824_CLIP_STRIDE * 8);
        }
        return;
    }
    // Complex region: band/inversion list — clear the gaps between spans.
    uint32_t d = 10, dend = size < maxlen ? size : maxlen;
    int inv[128], ninv = 0, guard = 0;
    int y0raw = (int16_t)LOAD_BE16(rgn + d);
    int y = (y0raw == 0x7FFF) ? 0x7FFF : y0raw + oy;
    while (y != 0x7FFF && d + 2 <= dend && guard++ < 2000) {
        d += 2;
        for (;;) {
            if (d + 2 > dend)
                return;
            int x = (int16_t)LOAD_BE16(rgn + d);
            d += 2;
            if (x == 0x7FFF)
                break;
            x += ox;
            int i = 0;
            while (i < ninv && inv[i] < x)
                i++;
            if (i < ninv && inv[i] == x) {
                for (int k = i; k + 1 < ninv; k++)
                    inv[k] = inv[k + 1];
                ninv--;
            } else if (ninv < 128) {
                for (int k = ninv; k > i; k--)
                    inv[k] = inv[k - 1];
                inv[i] = x;
                ninv++;
            }
        }
        if (d + 2 > dend)
            return;
        int nyraw = (int16_t)LOAD_BE16(rgn + d);
        int ny = (nyraw == 0x7FFF) ? 0x7FFF : nyraw + oy;
        int bandBot = (ny == 0x7FFF) ? bot : ny;
        if (bandBot > GC824_CLIP_ROWS)
            bandBot = GC824_CLIP_ROWS;
        for (int yy = (y < 0 ? 0 : y); yy < bandBot; yy++) {
            int prev = 0; // clear the gap [prev, inv[s]) then jump past the span
            for (int s = 0; s + 1 < ninv; s += 2) {
                gc_mask_and_region_row(mask, yy, prev, inv[s]);
                prev = inv[s + 1];
            }
            gc_mask_and_region_row(mask, yy, prev, GC824_CLIP_STRIDE * 8);
        }
        y = ny;
    }
}
// Rebuild the effective drawable mask = clipRgn ∩ visRgn (each region is the
// CURRENT one — the ops replace, not accumulate).
static void gc_clip_rebuild(display_card_824gc_priv_t *p) {
    memset(p->gc_clipmask, 0xFF, (size_t)GC824_CLIP_STRIDE * GC824_CLIP_ROWS);
    if (p->gc_cliprgn_len)
        gc_mask_and_region(p->gc_clipmask, p->gc_cliprgn, p->gc_cliprgn_len, p->gc_rgn_ox, p->gc_rgn_oy);
    if (p->gc_visrgn_len)
        gc_mask_and_region(p->gc_clipmask, p->gc_visrgn, p->gc_visrgn_len, p->gc_rgn_ox, p->gc_rgn_oy);
}
// Capture an opClipRgn/opVisRgn record (region bytes at DRAM `off`) as the
// port's CURRENT clip or vis, then rebuild the mask.
static void gc_clip_set_region(display_card_824gc_priv_t *p, int is_vis, uint32_t off, int ox, int oy) {
    if (off + 10 > GC824_DRAM_SIZE)
        return;
    uint16_t size = (uint16_t)((p->dram[off] << 8) | p->dram[off + 1]);
    uint8_t *dst = is_vis ? p->gc_visrgn : p->gc_cliprgn;
    uint32_t *len = is_vis ? &p->gc_visrgn_len : &p->gc_cliprgn_len;
    if (size >= 10 && size <= GC824_RGN_MAX && off + size <= GC824_DRAM_SIZE) {
        memcpy(dst, p->dram + off, size);
        *len = size;
    } else {
        *len = 0; // unparseable — fail open (the safety net favours drawing)
    }
    p->gc_rgn_ox = (int16_t)ox;
    p->gc_rgn_oy = (int16_t)oy;
    gc_clip_rebuild(p);
}

// Compute the byte advance for an opcode record at DRAM offset `off`.
static uint32_t gc_op_adv(display_card_824gc_priv_t *p, uint32_t off, uint16_t op) {
    uint16_t w2 = (uint16_t)(dram_be32(p, off) & 0xFFFF); // halfword at +2
    uint16_t w4 = (uint16_t)(dram_be32(p, off + 4) >> 16); // halfword at +4
    switch (op) {
    case 0x69:
    case 0x73:
    case 0x6F:
        return 4;
    case 0x01:
    case 0x68:
    case 0x6E:
    case 0x72:
    case 0x75:
        return 8;
    case 0x03:
    case 0x05:
    case 0x64:
    case 0x6B:
    case 0x71:
    case 0x74:
        return 0xC;
    case 0x02:
    case 0x04:
    case 0x66:
    case 0x6D:
    case 0x70:
        return 0x10;
    case 0x67:
        return 0x20;
    case 0x06:
        return ((uint32_t)w2 + 0x13) & ~3u; // byteLen.w at +2
    case 0x07:
    case 0x08:
        return ((uint32_t)w4 + 7) & ~3u; // poly/rgn size.w at +4
    case 0x09:
        // opBits (inline blit): dataLen.L at +0x20 (protocol §10.1).  Dead in
        // Sys 7 (emitter removed) and never emitted by Sys 6 either (func $22
        // is dead code) — but give it an advance so a stream carrying it
        // doesn't abort the interpreter.
        return (dram_be32(p, off + 0x20) + 0x27) & ~3u;
    case 0x6A:
    case 0x6C:
        return ((uint32_t)w2 + 5) & ~3u; // rgnSize.w at +2
    default:
        return 0;
    }
}

// Interpret a DrawMultiObject opcode stream at gcp address `base` for `count`
// bytes.  The gcp window maps `base` → DRAM_CB + (base - gcp_base).
void gc824_interp(display_card_824gc_priv_t *p, uint32_t base, uint32_t count) {
    if (base < p->gcp_base || count == 0)
        return;
    uint32_t off = GC824_DRAM_CB + (base - p->gcp_base);
    uint32_t end = off + count;
    // The accepted port's local -> global origin (func $2D staged bounds):
    // every queued coordinate is port-LOCAL and must be shifted.  KNOWN
    // LIMIT: GCQD's SetOrigin stub ($23AC) flushes the queue (func $26) but
    // never re-stages the port record, so a mid-session origin change (the
    // cdev list LDEF) leaves these stale until the next $2D — live-re-reading
    // the guest port at drain time was tried and reads unstable structures.
    int ox = p->gc_org_x, oy = p->gc_org_y;
    while (off + 2 <= end) {
        uint16_t op = (uint16_t)(dram_be32(p, off) >> 16);
        uint32_t adv = gc_op_adv(p, off, op);
        if (adv == 0) {
            LOG(1, "DrawMultiObject: unknown opCode $%02x @dram $%05x", op, off);
            break;
        }
        switch (op) {
        case 0x6F:
            break; // buffer header — skip
        case 0x69:
            p->gc_mode = (uint16_t)(dram_be32(p, off) & 0xFFFF);
            break; // opMode
        case 0x68: // opPenLoc: {pnLocHFrac.w at +2, Point at +4 = {v.w, h.w}}
            p->gc_pen_hfrac = dram_be16(p, off + 2);
            p->gc_pen_y = (int16_t)((int16_t)(dram_be32(p, off + 4) >> 16) + oy);
            p->gc_pen_x = (int16_t)((int16_t)(dram_be32(p, off + 4) & 0xFFFF) + ox);
            break;
        case 0x67: { // opSwapFont: font-info 26 B at +2, width-table key at +$1C
            if (off + 0x20 > GC824_DRAM_SIZE)
                break;
            memcpy(p->gc_font_info, p->dram + off + 2, sizeof(p->gc_font_info));
            uint32_t wtkey = dram_be32(p, off + 0x1C);
            struct gc_cache_ent *wt = gc_cache_find(p->gc_wtabs, 4, wtkey);
            p->gc_cur_wt = wt ? wt->data : NULL;
            // The strike key lives in the width table's trailer (+$410); the
            // font handle at rec+4 is the fallback lookup (card 0xE448).
            struct gc_cache_ent *stk = wt ? gc_cache_find(p->gc_fonts, 8, LOAD_BE32(wt->data + 0x410)) : NULL;
            if (!stk)
                stk = gc_cache_find(p->gc_fonts, 8, dram_be32(p, off + 4));
            p->gc_cur_strike = stk ? stk->data : NULL;
            p->gc_cur_strike_size = stk ? stk->size : 0;
            if (!wt || !stk)
                LOG(0, "opSwapFont: cache miss (wt key $%08x) — text will drop", wtkey);
            if (dram_be32(p, off + 0x14) != dram_be32(p, off + 0x18))
                LOG(0, "opSwapFont: scaled text (numer != denom) not modelled — drawn unscaled");
            break;
        }
        case 0x06: // opText
            gc_draw_text(p, off);
            break;
        case 0x6E: // opPenSize: Point at +4 = {v.w, h.w}
            p->gc_pen_h = (int16_t)(dram_be32(p, off + 4) >> 16);
            p->gc_pen_w = (int16_t)(dram_be32(p, off + 4) & 0xFFFF);
            break;
        case 0x71: { // opPattern: {which.w slot, 8-byte Pattern} — set slot bytes
            // Sets the named slot's bytes ONLY; the active slot is chosen solely
            // by opWhichPat ($73).  (Setting the active slot here would let a
            // later bkPat load steal the slot an earlier opWhichPat selected —
            // e.g. the desktop's gray fillPat, painting the desktop black.)
            int slot = (int)(dram_be32(p, off) & 0xFFFF) & 3;
            for (int i = 0; i < 8; i++)
                p->gc_pat[slot][i] = (uint8_t)(dram_be32(p, off + 4 + (i & ~3)) >> (24 - 8 * (i & 3)));
            p->gc_pat_kind[slot] = 0;
            break;
        }
        case 0x74: { // opRGBPat: {which.w at +4, RGB at +6} — MakeRGBPat's
            // 2x2 ordered dither (Patterns.a PatDither): each component is
            // quantized to 13 levels (>= 8 bpp; 5 below) and cell pixel k
            // takes column k of the dither table, then the card's
            // Color2Index resolves each cell to a device pixel.
            static const uint16_t d13[13][4] = {
                {0x0000, 0x0000, 0x0000, 0x0000},
                {0x5555, 0x0000, 0x0000, 0x0000},
                {0x5555, 0x0000, 0x0000, 0x5555},
                {0x5555, 0x5555, 0x0000, 0x5555},
                {0x5555, 0x5555, 0x5555, 0x5555},
                {0xAAAA, 0x5555, 0x5555, 0x5555},
                {0xAAAA, 0x5555, 0x5555, 0xAAAA},
                {0xAAAA, 0xAAAA, 0x5555, 0xAAAA},
                {0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA},
                {0xFFFF, 0xAAAA, 0xAAAA, 0xAAAA},
                {0xFFFF, 0xAAAA, 0xAAAA, 0xFFFF},
                {0xFFFF, 0xFFFF, 0xAAAA, 0xFFFF},
                {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
            };
            static const uint16_t d5[5][4] = {
                {0x0000, 0x0000, 0x0000, 0x0000},
                {0xFFFF, 0x0000, 0x0000, 0x0000},
                {0xFFFF, 0x0000, 0x0000, 0xFFFF},
                {0xFFFF, 0xFFFF, 0x0000, 0xFFFF},
                {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
            };
            int slot = (int)dram_be16(p, off + 4) & 3;
            int deep = p->display.format == PIXEL_8BPP || p->display.format == PIXEL_32BPP_XRGB;
            uint32_t div = deep ? 0x13B13B13u : 0x33333333u;
            uint32_t lr = (uint32_t)(((uint64_t)dram_be16(p, off + 6) << 16) / div);
            uint32_t lg = (uint32_t)(((uint64_t)dram_be16(p, off + 8) << 16) / div);
            uint32_t lb = (uint32_t)(((uint64_t)dram_be16(p, off + 10) << 16) / div);
            for (int k = 0; k < 4; k++) {
                uint16_t r = deep ? d13[lr > 12 ? 12 : lr][k] : d5[lr > 4 ? 4 : lr][k];
                uint16_t g = deep ? d13[lg > 12 ? 12 : lg][k] : d5[lg > 4 ? 4 : lg][k];
                uint16_t b = deep ? d13[lb > 12 ? 12 : lb][k] : d5[lb > 4 ? 4 : lb][k];
                p->gc_pat_cell[slot][k] = gc_resolve_rgb(p, r, g, b);
            }
            p->gc_pat_kind[slot] = 2;
            break;
        }
        case 0x73: // opWhichPat: select the active pattern slot (1/2/3)
            p->gc_pat_slot = (uint8_t)(dram_be32(p, off) & 0xFFFF) & 3;
            break;
        case 0x64:
        case 0x66: // opFgColor(Idx): {RGB at +2, pixValue.L at +8}.  Resolve
            // the pixel from the RGB — NOT from pixValue: for old-style
            // GrafPorts the host ships port->fgColor verbatim, which is a
            // CLASSIC COLOUR CONSTANT (33 = blackColor…), not a pixel (the
            // Finder desktop port does exactly this; §2.2 "old ports map
            // classic planar constants").  The RGB words are always the truth.
            p->gc_fg = gc_resolve_rgb(p, dram_be16(p, off + 2), dram_be16(p, off + 4), dram_be16(p, off + 6));
            break;
        case 0x6B:
        case 0x6D: // opBkColor(Idx)
            p->gc_bg = gc_resolve_rgb(p, dram_be16(p, off + 2), dram_be16(p, off + 4), dram_be16(p, off + 6));
            break;
        case 0x6A:
        case 0x6C: { // opClipRgn / opVisRgn: intersect the region into the mask
            int t = (int16_t)(dram_be32(p, off + 4) >> 16);
            int l = (int16_t)(dram_be32(p, off + 4) & 0xFFFF);
            int b = (int16_t)(dram_be32(p, off + 8) >> 16);
            int r = (int16_t)(dram_be32(p, off + 8) & 0xFFFF);
            if (t > p->gc_clip_t)
                p->gc_clip_t = (int16_t)t;
            if (l > p->gc_clip_l)
                p->gc_clip_l = (int16_t)l;
            if (b < p->gc_clip_b)
                p->gc_clip_b = (int16_t)b;
            if (r < p->gc_clip_r)
                p->gc_clip_r = (int16_t)r;
            gc_clip_set_region(p, op == 0x6C, off + 2, ox, oy); // region follows the opcode word
            break;
        }
        case 0x01: { // opLine
            int y = (int16_t)(dram_be32(p, off + 4) >> 16) + oy;
            int x = (int16_t)(dram_be32(p, off + 4) & 0xFFFF) + ox;
            gc_line_to(p, x, y);
            p->draw_count++;
            break;
        }
        case 0x05: { // opRect
            uint16_t frame = (uint16_t)(dram_be32(p, off) & 0xFFFF);
            int t = (int16_t)(dram_be32(p, off + 4) >> 16) + oy;
            int l = (int16_t)(dram_be32(p, off + 4) & 0xFFFF) + ox;
            int b = (int16_t)(dram_be32(p, off + 8) >> 16) + oy;
            int r = (int16_t)(dram_be32(p, off + 8) & 0xFFFF) + ox;
            if (frame)
                gc_frame_rect(p, t, l, b, r);
            else
                gc_fill_rect(p, t, l, b, r);
            p->draw_count++;
            break;
        }
        case 0x04: { // opRRect: {frameFlag.w, Rect, ovWd.w, ovHt.w} (§3)
            uint16_t frame = (uint16_t)(dram_be32(p, off) & 0xFFFF);
            int t = (int16_t)(dram_be32(p, off + 4) >> 16) + oy;
            int l = (int16_t)(dram_be32(p, off + 4) & 0xFFFF) + ox;
            int b = (int16_t)(dram_be32(p, off + 8) >> 16) + oy;
            int r = (int16_t)(dram_be32(p, off + 8) & 0xFFFF) + ox;
            int ovW = (int16_t)(dram_be32(p, off + 12) >> 16);
            int ovH = (int16_t)(dram_be32(p, off + 12) & 0xFFFF);
            if (ovW < 0)
                ovW = 0; // DrawArc pins negative radii to 0
            if (ovH < 0)
                ovH = 0;
            if (frame)
                gc_frame_rrect(p, t, l, b, r, ovW, ovH);
            else
                gc_fill_rrect(p, t, l, b, r, ovW, ovH);
            p->draw_count++;
            break;
        }
        case 0x03: { // opOval: {frameFlag.w, Rect} — DrawArc with radii = rect dims
            uint16_t frame = (uint16_t)(dram_be32(p, off) & 0xFFFF);
            int t = (int16_t)(dram_be32(p, off + 4) >> 16) + oy;
            int l = (int16_t)(dram_be32(p, off + 4) & 0xFFFF) + ox;
            int b = (int16_t)(dram_be32(p, off + 8) >> 16) + oy;
            int r = (int16_t)(dram_be32(p, off + 8) & 0xFFFF) + ox;
            gc_draw_arc(p, t, l, b, r, 0, 360, frame);
            p->draw_count++;
            break;
        }
        case 0x02: { // opArc: {frameFlag.w, Rect, startAngle.w, arcAngle.w}
            uint16_t frame = (uint16_t)(dram_be32(p, off) & 0xFFFF);
            int t = (int16_t)(dram_be32(p, off + 4) >> 16) + oy;
            int l = (int16_t)(dram_be32(p, off + 4) & 0xFFFF) + ox;
            int b = (int16_t)(dram_be32(p, off + 8) >> 16) + oy;
            int r = (int16_t)(dram_be32(p, off + 8) & 0xFFFF) + ox;
            int start = (int16_t)(dram_be32(p, off + 12) >> 16);
            int arc = (int16_t)(dram_be32(p, off + 12) & 0xFFFF);
            gc_draw_arc(p, t, l, b, r, start, arc, frame);
            p->draw_count++;
            break;
        }
        case 0x07: { // opPoly: {frameFlag.w} + the Polygon verbatim at +4
            uint16_t frame = (uint16_t)(dram_be32(p, off) & 0xFFFF);
            int polySize = dram_be16(p, off + 4);
            int npts = (polySize - 10) / 4;
            if (npts <= 0)
                break; // pen untouched (== ROM / card 0xC688)
            // Sys 6 card: pen := points[0] UNCONDITIONALLY (0xC694) — even for
            // fills (the divergence Sys 7 fixes; call-reference §6).
            p->gc_pen_y = (int16_t)((int16_t)dram_be16(p, off + 0x0E) + oy);
            p->gc_pen_x = (int16_t)((int16_t)dram_be16(p, off + 0x10) + ox);
            if (frame) {
                // FrPoly: one pen line per edge, pen := last vertex — the same
                // line engine as op $01; no auto-close (Polygons.a:421-454).
                for (int i = 1; i < npts; i++)
                    gc_line_to(p, (int16_t)dram_be16(p, off + 0x10 + 4 * i) + ox,
                               (int16_t)dram_be16(p, off + 0x0E + 4 * i) + oy);
            } else {
                gc_fill_poly(p, off + 4, npts, ox, oy);
            }
            p->draw_count++;
            break;
        }
        case 0x08: // opRgn
            gc_fill_rgn(p, off + 4, ox, oy);
            p->draw_count++;
            break;
        case 0x72: { // opPixPat: {which.w at +2, PixPat host Handle.L at +4} —
            // select a tile pre-downloaded via func $0C.  The host only emits
            // this after a verified download, so a miss here means the entry
            // was evicted or the depth changed under it — fall back to the
            // classic slot bits and say so loudly.
            int slot = (int)(dram_be32(p, off) & 0xFFFF) & 3;
            uint32_t key = dram_be32(p, off + 4);
            int hit = -1;
            for (int i = 0; i < 4; i++)
                if (p->gc_pixpats[i].key == key && p->gc_pixpats[i].fmt == (uint8_t)p->display.format)
                    hit = i;
            if (hit >= 0) {
                p->gc_pat_pp[slot] = (uint8_t)hit;
                p->gc_pat_kind[slot] = 3;
            } else {
                // The real card aborts the WHOLE DrawMultiObject stream here
                // ("can't find PixPat in cache" → epilogue), so the batch's
                // remaining records are dropped, not drawn with wrong fills.
                LOG(0, "op $72: PixPat $%08x not in the type-5 cache — batch aborted", key);
                p->gc_pat_kind[slot] = 0;
                return;
            }
            break;
        }
        case 0x70: // opSpecialColors: rgbOpColor at +4, rgbHiliteColor at +$A
            p->gc_op_rgb[0] = dram_be16(p, off + 4);
            p->gc_op_rgb[1] = dram_be16(p, off + 6);
            p->gc_op_rgb[2] = dram_be16(p, off + 8);
            p->gc_op_color = gc_resolve_rgb(p, p->gc_op_rgb[0], p->gc_op_rgb[1], p->gc_op_rgb[2]);
            p->gc_hilite = gc_resolve_rgb(p, dram_be16(p, off + 0xA), dram_be16(p, off + 0xC), dram_be16(p, off + 0xE));
            break;
        case 0x75: // opQDGlobal — SetPatAlign.  The card handler ($BA28) reads
            // the Point.L at +4 and stores it verbatim to the patAlign global
            // (UNDEF+$E0, feeding text_16d44's phase); it NEVER inspects the
            // word at +2.  So apply it UNCONDITIONALLY — the word at +2 is a
            // host-side global index the card ignores (1 for a phased bar like
            // the Trash window's {7,3}, 12 for the Control Panel's {0,1}; an
            // earlier `which==1` gate silently dropped every other window's
            // patAlign, so a title bar whose non-zero v-phase was carried under
            // a different index landed its racing stripes one row off around
            // the close box).  patAlign shifts the anchor of every pattern
            // kind (classic 8×8, RGB dither, PixPat) — reset to 0 per cycle.
            p->gc_align_y = (int16_t)(dram_be32(p, off + 4) >> 16);
            p->gc_align_x = (int16_t)(dram_be32(p, off + 4) & 0xFFFF);
            break;
        default:
            break; // $09 opBits (dead) skipped
        }
        off += adv;
    }
    p->display.fb_dirty = true;
}

// Read one byte of blit source at `base + off`.  `base` is a QuickDraw
// baseAddr: a super-slot CARD address (the screen FB or card DRAM — read the
// card's own memory directly: a memory_debug_read of $9xxxxxxx resolves
// through the guest's CURRENT addressing mode, and at RPC time that is
// usually 24-bit, where it would silently read host RAM at addr&0xFFFFFF —
// noise; this is exactly what screen→screen ScrollRect copies hit) or a host
// master pointer whose top byte carries HandleMgr lock/purge flags (strip to
// the low 24 bits, then read host RAM).
static inline uint8_t gc_src_read8(display_card_824gc_priv_t *p, uint32_t base, uint32_t off) {
    uint32_t cl = base - p->super_base;
    if (cl < 0x10000000u) { // card-local: serve from the card's memory
        cl = (cl - GC824_DRAM_OFFSET) + off;
        return (cl < GC824_DRAM_SIZE) ? p->dram[cl] : 0xFF;
    }
    return memory_debug_read_uint8((base & 0x00FFFFFFu) + off);
}

// Read one 1-bpp pixel (MSB-first) at (x,y) from a guest bitmap.
static inline int gc_bmp_pixel(display_card_824gc_priv_t *p, uint32_t base, int y, int x, int rowbytes) {
    uint8_t b = gc_src_read8(p, base, (uint32_t)y * (uint32_t)rowbytes + (uint32_t)(x >> 3));
    return (b >> (7 - (x & 7))) & 1;
}

// Fetch a guest QuickDraw region (deref'd handle in host RAM) into `buf`.
// Returns the byte count, or 0 on a reject (implausible size).
static uint32_t gc_fetch_guest_rgn(uint32_t addr, uint16_t size, uint8_t *buf) {
    addr &= 0x00FFFFFFu; // strip master-pointer flag bits (24-bit mode)
    if (!addr || size < 10 || size > GC824_RGN_MAX)
        return 0;
    for (uint32_t i = 0; i < size; i++)
        buf[i] = memory_debug_read_uint8(addr + i);
    return size;
}

// func $15 StretchBits (CopyBits) — the blit.  Stage-2 accept envelope
// (proposal §3.8 v1 + 1-bit masks): the classic boolean modes 0-7 (colorized
// at 1 bpp from the request's fg/bk indices; §2.1 cores), no stretch, 1-bpp,
// destination = the screen framebuffer; everything else declines to the ROM
// path (result 0).  Request block at CB+$58 (protocol §9.1); the blit is
// clipped to rgnA ∩ rgnB ∩ rgnC — the request block's own deref'd region
// pointers (mask/clip/vis, +0xC0/C4/C8, sizes +0x104/6/8), which is what the
// card's blit lane (text_28c14) consumes — NOT the queue-port clip mask.
// Returns 1 if drawn, 0 to decline.
// Resolve a request PixMap's pixel-image base through its pmVersion tag
// (+$E in the copied record at `pm`).  Returns 0 to decline.
static uint32_t gc_offscreen_base(display_card_824gc_priv_t *p, uint32_t pm, uint32_t base) {
    if (!(dram_be16(p, pm + 4) & 0x8000))
        return base; // plain BitMap — baseAddr is a pointer
    uint16_t pmver = dram_be16(p, pm + 0xE);
    switch (pmver) {
    case 0:
    case 1: // screen / locked offscreen: baseAddr is a real pointer
        return base;
    case 2: { // unlocked offscreen: baseAddr is the pixel-image HANDLE
        uint32_t h = ((base & 0xF0000000u) == 0x90000000u) ? base : (base & 0x00FFFFFFu);
        uint32_t master = memory_debug_read_uint32(h);
        return master ? master : 0; // purged/empty handle → decline
    }
    default: // 4 = transient $4842 swizzle (or unknown) → ROM path
        LOG(2, "blit: pmVersion %u source declined", pmver);
        return 0;
    }
}

int gc824_stretchbits(display_card_824gc_priv_t *p) {
    uint32_t rb = GC824_DRAM_CB + 0x58;
    uint16_t mode = dram_be16(p, rb + 0x00);
    uint32_t dstBase = dram_be32(p, rb + 0x02);
    int dstBnT = (int16_t)dram_be16(p, rb + 0x08), dstBnL = (int16_t)dram_be16(p, rb + 0x0A);
    uint32_t srcBase = dram_be32(p, rb + 0x36);
    int srcRB = dram_be16(p, rb + 0x3A) & 0x3FFF;
    int srcBnT = (int16_t)dram_be16(p, rb + 0x3C), srcBnL = (int16_t)dram_be16(p, rb + 0x3E);
    uint32_t maskBase = dram_be32(p, rb + 0x6A);
    int maskRB = dram_be16(p, rb + 0x6E) & 0x3FFF;
    int maskBnT = (int16_t)dram_be16(p, rb + 0x70), maskBnL = (int16_t)dram_be16(p, rb + 0x72);
    int dRt = (int16_t)dram_be16(p, rb + 0xA8), dRl = (int16_t)dram_be16(p, rb + 0xAA);
    int dRb = (int16_t)dram_be16(p, rb + 0xAC), dRr = (int16_t)dram_be16(p, rb + 0xAE);
    int sRt = (int16_t)dram_be16(p, rb + 0xB0), sRl = (int16_t)dram_be16(p, rb + 0xB2);
    int mRt = (int16_t)dram_be16(p, rb + 0xB8), mRl = (int16_t)dram_be16(p, rb + 0xBA);

    // Accept only: boolean modes, destination is the screen FB, 1:1 (no stretch).
    uint32_t screen = p->super_base | (GC824_DRAM_OFFSET + GC824_FB_OFFSET);
    if (mode > 7 || dstBase != screen) {
        LOG(2, "blit declined: mode $%x dst $%08x (screen $%08x)", mode, dstBase, screen);
        return 0; // arithmetic/hilite (incl. pending-hilite 50) → ROM
    }
    if ((dRb - dRt) != (int16_t)dram_be16(p, rb + 0xB4) - sRt ||
        (dRr - dRl) != (int16_t)dram_be16(p, rb + 0xB6) - sRl) {
        LOG(2, "blit declined: stretched %dx%d -> %dx%d", (int16_t)dram_be16(p, rb + 0xB4) - sRt,
            (int16_t)dram_be16(p, rb + 0xB6) - sRl, dRb - dRt, dRr - dRl);
        return 0; // stretched → decline
    }
    // Depths: the raw rowBytes word's bit 15 marks a PixMap (pixelSize at
    // +0x20 in the copied map) vs a plain BitMap (1 bpp).
    int depth = (p->display.format == PIXEL_8BPP)         ? 8
                : (p->display.format == PIXEL_16BPP_555)  ? 16
                : (p->display.format == PIXEL_32BPP_XRGB) ? 32
                                                          : 1;
    int dstPS = (dram_be16(p, rb + 0x06) & 0x8000) ? dram_be16(p, rb + 0x02 + 0x20) : 1;
    int srcPS = (dram_be16(p, rb + 0x3A) & 0x8000) ? dram_be16(p, rb + 0x36 + 0x20) : 1;
    if (dstPS != depth || (depth == 1 && srcPS != 1) || (depth == 8 && srcPS != 8 && srcPS != 1) ||
        (depth == 16 && srcPS != 16 && srcPS != 1) || (depth == 32 && srcPS != 32 && srcPS != 1)) {
        // dst must match the screen depth; sources may be same-depth or
        // 1-bit-expanded — cross-depth blits route to the ROM path.
        LOG(2, "blit declined: depth %d dstPS %d srcPS %d", depth, dstPS, srcPS);
        return 0;
    }
    // Offscreen (GWorld) sources: pmVersion tags the baseAddr form the GCQD
    // marshaller shipped (protocol §13 / marshaller §7.1) — 1 = locked
    // (baseAddr is a real pointer), 2 = unlocked (baseAddr IS the pixel-image
    // Handle: dereference the master pointer), 4 = the transient
    // $4842-swizzle state (mid-ROM-fallback; never in a live request —
    // decline).  The real card copies the image into its type-6 cache and
    // blits from the copy; the HLE reads the live host pixels instead —
    // coherent by construction, because queued draws only ever target the
    // SCREEN here, so the host copy is always the authority (which is also
    // why func $0D correctly returns 0 and $19/$33 stay no-ops).
    srcBase = gc_offscreen_base(p, rb + 0x36, srcBase);
    if (!srcBase)
        return 0;
    if (maskBase) {
        maskBase = gc_offscreen_base(p, rb + 0x6A, maskBase);
        if (!maskBase)
            return 0;
    }
    if (maskBase && (dram_be16(p, rb + 0x6E) & 0x8000) && dram_be16(p, rb + 0x6A + 0x20) != 1) {
        LOG(2, "blit declined: deep mask (%u bpp)", dram_be16(p, rb + 0x6A + 0x20));
        return 0; // only 1-bit masks (deep CopyDeepMask blends -> ROM)
    }
    if (depth == 8 || depth == 16 || depth == 32) {
        // v1 colorize envelope at 8 bpp: only the B/W port (fg black, bk
        // white — the colorize-NOP case, §2.3).  Anything colorized routes
        // through MakeScaleTbl in RGB space on the ROM path.
        if (dram_be16(p, rb + 0xE8) != 0 || dram_be16(p, rb + 0xEA) != 0 || dram_be16(p, rb + 0xEC) != 0 ||
            dram_be16(p, rb + 0xEE) != 0xFFFF || dram_be16(p, rb + 0xF0) != 0xFFFF ||
            dram_be16(p, rb + 0xF2) != 0xFFFF) {
            LOG(2, "blit declined: colorized fg %04x/%04x/%04x bk %04x/%04x/%04x", dram_be16(p, rb + 0xE8),
                dram_be16(p, rb + 0xEA), dram_be16(p, rb + 0xEC), dram_be16(p, rb + 0xEE), dram_be16(p, rb + 0xF0),
                dram_be16(p, rb + 0xF2));
            return 0;
        }
    }

    // Blit clip = ∩ of the request's regions (0 = absent).  A region we can't
    // fetch faithfully declines the whole blit — the ROM path is the safety net.
    memset(p->gc_blitmask, 0xFF, (size_t)GC824_CLIP_STRIDE * GC824_CLIP_ROWS);
    for (int i = 0; i < 3; i++) {
        uint32_t rgn = dram_be32(p, rb + 0xC0 + 4u * (uint32_t)i);
        uint16_t rsz = dram_be16(p, rb + 0x104 + 2u * (uint32_t)i);
        if (!rgn)
            continue;
        uint8_t buf[GC824_RGN_MAX];
        if (!gc_fetch_guest_rgn(rgn, rsz, buf)) {
            LOG(2, "blit declined: region %d @$%08x size %u unfetchable", i, rgn, rsz);
            return 0;
        }
        // The regions are PORT-LOCAL like every other coordinate in the
        // request; the dst PixMap bounds give the local->global shift.
        gc_mask_and_region(p->gc_blitmask, buf, rsz, -dstBnL, -dstBnT);
    }

    // fg/bk PIXEL values resolved from the request's RGBs (+0xE8/+0xEE) — the
    // index fields (+0xE0/+0xE4) hold classic colour CONSTANTS for old-style
    // ports (33 = blackColor…), so the RGB is the only reliable source.
    int fg = (int)gc_resolve_rgb(p, dram_be16(p, rb + 0xE8), dram_be16(p, rb + 0xEA), dram_be16(p, rb + 0xEC));
    int bk = (int)gc_resolve_rgb(p, dram_be16(p, rb + 0xEE), dram_be16(p, rb + 0xF0), dram_be16(p, rb + 0xF2));
    int inv = (mode >> 2) & 1; // notSrc*: invert the source before colorizing
    gc_cursor_shield(p, dRt - dstBnT, dRl - dstBnL, dRb - dstBnT, dRr - dstBnL);
    // Overlapping screen->screen copies (ScrollRect) must run away from the
    // overlap: copy bottom-up when moving content down, right-to-left when
    // moving it right — otherwise source rows are clobbered before they are
    // read.
    bool down = srcBase == dstBase && dRt > sRt + (dstBnT - srcBnT);
    bool right = srcBase == dstBase && dRl > sRl + (dstBnL - srcBnL);
    for (int i = 0; i < dRb - dRt; i++) {
        int dy = down ? dRb - 1 - i : dRt + i;
        int y = dy - dstBnT;
        if (y < 0 || y >= (int)p->display.height)
            continue;
        int sy = sRt + (dy - dRt) - srcBnT;
        int my = mRt + (dy - dRt) - maskBnT;
        uint8_t *row = (uint8_t *)p->display.bits + (size_t)y * p->display.stride;
        for (int j = 0; j < dRr - dRl; j++) {
            int dx = right ? dRr - 1 - j : dRl + j;
            int x = dx - dstBnL;
            if (x < 0 || x >= (int)p->display.width)
                continue;
            if (!(p->gc_blitmask[y * GC824_CLIP_STRIDE + (x >> 3)] & (0x80u >> (x & 7))))
                continue; // outside rgnA∩rgnB∩rgnC
            int sx = sRl + (dx - dRl) - srcBnL;
            if (maskBase && !gc_bmp_pixel(p, maskBase, my, mRl + (dx - dRl) - maskBnL, maskRB))
                continue; // outside the CopyMask 1-bit mask → leave the dst
            if (depth == 8) {
                // 8-bpp dst: same-depth source = bitwise index math on bytes
                // (§2.1, colorize-NOP guaranteed by the envelope); 1-bit
                // source expands 1→fg pixel / 0→bk pixel first (§2.4).
                uint8_t s;
                if (srcPS == 8) {
                    s = gc_src_read8(p, srcBase, (uint32_t)sy * (uint32_t)srcRB + (uint32_t)sx);
                    if (inv)
                        s = (uint8_t)~s;
                } else {
                    int b1 = gc_bmp_pixel(p, srcBase, sy, sx, srcRB) ^ inv;
                    s = (uint8_t)(b1 ? fg : bk);
                }
                switch (mode & 3) {
                case 1:
                    row[x] |= s;
                    break; // Or
                case 2:
                    row[x] ^= s;
                    break; // Xor
                case 3:
                    row[x] &= (uint8_t)~s;
                    break; // Bic
                default:
                    row[x] = s;
                    break; // Copy
                }
                continue;
            }
            if (depth == 16) {
                // 16-bpp dst: same rules on 15-bit direct values.
                uint16_t sv;
                if (srcPS == 16) {
                    uint32_t off = (uint32_t)sy * (uint32_t)srcRB + (uint32_t)sx * 2;
                    sv = (uint16_t)(((uint16_t)gc_src_read8(p, srcBase, off) << 8) | gc_src_read8(p, srcBase, off + 1));
                    if (inv)
                        sv = (uint16_t)~sv;
                } else {
                    int b1 = gc_bmp_pixel(p, srcBase, sy, sx, srcRB) ^ inv;
                    sv = (uint16_t)(b1 ? fg : bk);
                }
                uint8_t *px = row + (size_t)x * 2;
                uint16_t d = LOAD_BE16(px);
                switch (mode & 3) {
                case 1:
                    d |= sv;
                    break; // Or
                case 2:
                    d ^= sv;
                    break; // Xor
                case 3:
                    d &= (uint16_t)~sv;
                    break; // Bic
                default:
                    d = sv;
                    break; // Copy
                }
                STORE_BE16(px, d & 0x7FFFu);
                continue;
            }
            if (depth == 32) {
                // 32-bpp dst: same rules on 00RRGGBB longs (alpha excluded).
                uint32_t sv;
                if (srcPS == 32) {
                    uint32_t off = (uint32_t)sy * (uint32_t)srcRB + (uint32_t)sx * 4;
                    sv = ((uint32_t)gc_src_read8(p, srcBase, off) << 24) |
                         ((uint32_t)gc_src_read8(p, srcBase, off + 1) << 16) |
                         ((uint32_t)gc_src_read8(p, srcBase, off + 2) << 8) | gc_src_read8(p, srcBase, off + 3);
                    if (inv)
                        sv = ~sv;
                } else {
                    int b1 = gc_bmp_pixel(p, srcBase, sy, sx, srcRB) ^ inv;
                    sv = (uint32_t)(b1 ? fg : bk);
                }
                uint8_t *px = row + (size_t)x * 4;
                uint32_t d = LOAD_BE32(px);
                switch (mode & 3) {
                case 1:
                    d |= sv;
                    break; // Or
                case 2:
                    d ^= sv;
                    break; // Xor
                case 3:
                    d &= ~sv;
                    break; // Bic
                default:
                    d = sv;
                    break; // Copy
                }
                STORE_BE32(px, d & 0x00FFFFFFu);
                continue;
            }
            uint8_t bit = (uint8_t)(0x80u >> (x & 7));
            int s = gc_bmp_pixel(p, srcBase, sy, sx, srcRB) ^ inv;
            switch (mode & 3) {
            case 1: // Or: fg where ink
                if (s) {
                    if (fg)
                        row[x >> 3] |= bit;
                    else
                        row[x >> 3] &= (uint8_t)~bit;
                }
                break;
            case 2: // Xor: flip where ink (never colorized)
                if (s)
                    row[x >> 3] ^= bit;
                break;
            case 3: // Bic: bk where ink
                if (s) {
                    if (bk)
                        row[x >> 3] |= bit;
                    else
                        row[x >> 3] &= (uint8_t)~bit;
                }
                break;
            default: // Copy: fg on ink, bk elsewhere
                if (s ? fg : bk)
                    row[x >> 3] |= bit;
                else
                    row[x >> 3] &= (uint8_t)~bit;
                break;
            }
        }
    }
    p->draw_count++;
    p->display.fb_dirty = true;
    return 1;
}

// === Text (func $30 FontDownload + ops $67/$06; proposal §3.10) ==============
// The host measures (StdTxMeas) and downloads font data; the card only draws.
// Func $30 args (packing byte-verified from the host's sub_4A44 emitter,
// gc24--4048.s $4D80-$4E5C): arg0 = group mask, then in order —
//   bit 0:        {WidthTabHandle, ptr}        type-10 width table (0x434 B;
//                 the host stamps its checksum into wt+$432 first)
//   bit 1/bit 3:  {strikeH, ptr, size} / {strikeH, ptr}   type-8 strike,
//                 RAM (sized copy) / ROM-resident (size computed from FontRec)
//   bit 2/bit 4:  {fontH, ptr[, size]}         second type-8 entry, same rules
// Any failing sub-op → result 0 (the host rolls the checksum back and draws
// text unaccelerated — the safety net).
// op $67 opSwapFont selects: width table by key rec+$1C; strike by the handle
// the width table carries at +$410 (fallback: the font handle at rec+4).
// op $06 opText renders: per-char advances from the width table's Fixed
// entries (spaces = long A, stored into wt+$80), glyphs from the classic
// FontRec strike (bitImage / locTable / owTable, $FFFF = missing).

#define GC824_WTAB_SIZE  0x434u // Font Manager width table incl. trailer
#define GC824_STRIKE_MAX 0x40000u // sanity cap on a strike copy

static struct gc_cache_ent *gc_cache_find(struct gc_cache_ent *tab, int n, uint32_t key) {
    for (int i = 0; i < n; i++)
        if (key && tab[i].key == key)
            return &tab[i];
    return NULL;
}
// Copy `size` guest bytes at host pointer `ptr` (24-bit master-pointer rules,
// as gc_bmp_pixel) into a fresh cache entry for `key`, replacing round-robin.
static struct gc_cache_ent *gc_cache_put(struct gc_cache_ent *tab, int n, int *rr, uint32_t key, uint32_t ptr,
                                         uint32_t size) {
    if (!key || !size || size > GC824_STRIKE_MAX)
        return NULL;
    struct gc_cache_ent *e = gc_cache_find(tab, n, key);
    if (!e) {
        e = &tab[*rr];
        *rr = (*rr + 1) % n;
    }
    uint8_t *buf = malloc(size);
    if (!buf)
        return NULL;
    uint32_t a = ((ptr & 0xF0000000u) == 0x90000000u) ? ptr : (ptr & 0x00FFFFFFu);
    for (uint32_t i = 0; i < size; i++)
        buf[i] = memory_debug_read_uint8(a + i);
    free(e->data);
    e->key = key;
    e->data = buf;
    e->size = size;
    return e;
}
void gc824_font_caches_flush(display_card_824gc_priv_t *p) {
    for (size_t i = 0; i < sizeof(p->gc_fonts) / sizeof(p->gc_fonts[0]); i++) {
        free(p->gc_fonts[i].data);
        p->gc_fonts[i] = (struct gc_cache_ent){0};
    }
    for (size_t i = 0; i < sizeof(p->gc_wtabs) / sizeof(p->gc_wtabs[0]); i++) {
        free(p->gc_wtabs[i].data);
        p->gc_wtabs[i] = (struct gc_cache_ent){0};
    }
    p->gc_cur_wt = NULL;
    p->gc_cur_strike = NULL;
    p->gc_cur_strike_size = 0;
}
// Func $0C CachePixPat: expand a patType-1 PixPat into a device-pixel tile
// (the card's PATCONVERT).  Args (6 longs in the args area): {PixPat host
// Handle (key), patType, host ptr to the patMap PixMap record, host ptr to
// the pattern image bits, host ptr to its ColorTable or 0, ctSize or 0}.
// The host copies nothing but this block — the card fetches the records
// through the passed host pointers.  Result 0 = decline: the host then
// declines the WHOLE draw to the ROM original (sub_1530 -> -1), so any
// unsupported shape falls back safely.
static uint32_t gc_host_addr(uint32_t ptr) {
    return ((ptr & 0xF0000000u) == 0x90000000u) ? ptr : (ptr & 0x00FFFFFFu);
}
int gc824_cache_pixpat(display_card_824gc_priv_t *p) {
    uint32_t a = GC824_DRAM_CB + GC824_CB_ARGSAREA;
    uint32_t key = dram_be32(p, a);
    uint32_t ptype = dram_be32(p, a + 4);
    uint32_t pmap = gc_host_addr(dram_be32(p, a + 8));
    uint32_t pdata = gc_host_addr(dram_be32(p, a + 12));
    uint32_t ctab = dram_be32(p, a + 16) ? gc_host_addr(dram_be32(p, a + 16)) : 0;
    int ctsize = (int)dram_be32(p, a + 20);
    if (!key || ptype != 1 || !pmap || !pdata) {
        LOG(1, "func $0C: bad args (key $%08x type %u) — declined", key, ptype);
        return 0;
    }
    uint32_t rowbytes = memory_debug_read_uint16(pmap + 4) & 0x3FFFu;
    int t = (int16_t)memory_debug_read_uint16(pmap + 6), l = (int16_t)memory_debug_read_uint16(pmap + 8);
    int b = (int16_t)memory_debug_read_uint16(pmap + 10), r = (int16_t)memory_debug_read_uint16(pmap + 12);
    int depth = (int16_t)memory_debug_read_uint16(pmap + 0x20);
    int w = r - l, h = b - t;
    // Envelope: power-of-two tile up to 64x64, indexed source depths with a
    // ColorTable, or 32-bpp direct.  (16 bpp has no rasterizer anywhere in
    // the model yet; PixPat bounds outside QD's power-of-two contract or a
    // missing CLUT would tile wrongly — decline them all to the ROM path.)
    if (w <= 0 || h <= 0 || w > 64 || h > 64 || (w & (w - 1)) || (h & (h - 1)) ||
        !((depth == 1 || depth == 2 || depth == 4 || depth == 8) ? ctab != 0 : depth == 32)) {
        LOG(1, "func $0C: PixPat %dx%d depth %d outside envelope — declined", w, h, depth);
        return 0;
    }
    uint32_t *pix = malloc((size_t)w * h * sizeof(uint32_t));
    if (!pix)
        return 0;
    for (int yy = 0; yy < h; yy++) {
        uint32_t rowa = pdata + (uint32_t)yy * rowbytes;
        for (int xx = 0; xx < w; xx++) {
            uint16_t cr, cg, cb;
            if (depth == 32) {
                uint32_t v = memory_debug_read_uint32(rowa + (uint32_t)xx * 4);
                cr = (uint16_t)(((v >> 16) & 0xFF) * 0x101u);
                cg = (uint16_t)(((v >> 8) & 0xFF) * 0x101u);
                cb = (uint16_t)((v & 0xFF) * 0x101u);
            } else {
                uint32_t idx;
                if (depth == 8) {
                    idx = memory_debug_read_uint8(rowa + (uint32_t)xx);
                } else {
                    uint8_t byte = memory_debug_read_uint8(rowa + (uint32_t)(xx * depth) / 8u);
                    int sh = 8 - depth - ((xx * depth) & 7);
                    idx = (byte >> sh) & ((1u << depth) - 1u);
                }
                // ColorTable entry: {value.w, r.w, g.w, b.w} at CT+8+i*8.
                // Match on the value field (pixpat CTabs are usually the
                // identity but QD does not require it); fall back to the
                // index position clamped to ctSize.
                uint32_t ent = 0xFFFFFFFFu;
                for (int i = 0; i <= ctsize && i < 256; i++)
                    if ((memory_debug_read_uint16(ctab + 8 + (uint32_t)i * 8) & ((1u << depth) - 1u)) == idx) {
                        ent = ctab + 8 + (uint32_t)i * 8;
                        break;
                    }
                if (ent == 0xFFFFFFFFu)
                    ent = ctab + 8 + (uint32_t)(idx > (uint32_t)ctsize ? (uint32_t)ctsize : idx) * 8;
                cr = memory_debug_read_uint16(ent + 2);
                cg = memory_debug_read_uint16(ent + 4);
                cb = memory_debug_read_uint16(ent + 6);
            }
            pix[yy * w + xx] = gc_resolve_rgb(p, cr, cg, cb);
        }
    }
    struct gc_pixpat *e = NULL;
    for (int i = 0; i < 4; i++)
        if (p->gc_pixpats[i].key == key)
            e = &p->gc_pixpats[i];
    if (!e) {
        e = &p->gc_pixpats[p->gc_pixpat_rr];
        p->gc_pixpat_rr = (p->gc_pixpat_rr + 1) % 4;
    }
    free(e->pix);
    *e = (struct gc_pixpat){
        .key = key, .w = (uint16_t)w, .h = (uint16_t)h, .fmt = (uint8_t)p->display.format, .pix = pix};
    LOG(2, "func $0C: cached PixPat $%08x %dx%d depth %d", key, w, h, depth);
    return 1;
}
void gc824_pixpats_flush(display_card_824gc_priv_t *p, uint32_t key) {
    for (int i = 0; i < 4; i++)
        if (p->gc_pixpats[i].key && (key == 0 || p->gc_pixpats[i].key == key)) {
            free(p->gc_pixpats[i].pix);
            p->gc_pixpats[i] = (struct gc_pixpat){0};
        }
    for (int sl = 0; sl < 4; sl++)
        if (p->gc_pat_kind[sl] == 3 && !p->gc_pixpats[p->gc_pat_pp[sl]].key)
            p->gc_pat_kind[sl] = 0;
}

// The dense FontRec size for a ROM-resident (size-less) strike: header $1A,
// owTable at $10 + owTLoc*2, plus lastChar-firstChar+3 trailing word entries.
static uint32_t gc_fontrec_size(uint32_t ptr) {
    uint32_t a = ((ptr & 0xF0000000u) == 0x90000000u) ? ptr : (ptr & 0x00FFFFFFu);
    int first = (int16_t)memory_debug_read_uint16(a + 2);
    int last = (int16_t)memory_debug_read_uint16(a + 4);
    uint32_t owtloc = memory_debug_read_uint16(a + 16);
    if (last < first)
        return 0;
    return 0x10u + owtloc * 2u + (uint32_t)(last - first + 3) * 2u;
}
// Func $30 FontDownload: parse the group mask + packed args (see above).
int gc824_font_download(display_card_824gc_priv_t *p) {
    uint32_t a = GC824_DRAM_CB + GC824_CB_ARGSAREA;
    uint32_t mask = dram_be32(p, a);
    a += 4;
    if (mask & 1) { // width table (type 10)
        uint32_t h = dram_be32(p, a), ptr = dram_be32(p, a + 4);
        a += 8;
        if (!gc_cache_put(p->gc_wtabs, 4, &p->gc_wtab_rr, h, ptr, GC824_WTAB_SIZE))
            return 0;
    }
    for (int g = 0; g < 2; g++) { // strike group, then font group (type 8)
        uint32_t ram_bit = g == 0 ? 0x2u : 0x4u, rom_bit = g == 0 ? 0x8u : 0x10u;
        if (mask & ram_bit) {
            uint32_t h = dram_be32(p, a), ptr = dram_be32(p, a + 4), size = dram_be32(p, a + 8);
            a += 12;
            if (!gc_cache_put(p->gc_fonts, 8, &p->gc_font_rr, h, ptr, size))
                return 0;
        } else if (mask & rom_bit) {
            uint32_t h = dram_be32(p, a), ptr = dram_be32(p, a + 4);
            a += 8;
            if (!gc_cache_put(p->gc_fonts, 8, &p->gc_font_rr, h, ptr, gc_fontrec_size(ptr)))
                return 0;
        }
    }
    LOG(2, "func $30 FontDownload mask $%02x cached", mask);
    return 1;
}

// op $06 opText: render a measured run with the current font (card 0xE660 /
// renderer text_115c8 — a DrText port).  `off` = DRAM offset of the record.
static void gc_draw_text(display_card_824gc_priv_t *p, uint32_t off) {
    int len = (int16_t)dram_be16(p, off + 2);
    uint32_t spaceW = dram_be32(p, off + 4); // long A: space-glyph Fixed width
    uint32_t rawAdv = dram_be32(p, off + 8); // long B: raw advance; 0 = no-op
    int32_t scaledAdv = (int32_t)dram_be32(p, off + 0xC);
    int32_t acc = ((int32_t)p->gc_pen_x << 16) + (int32_t)p->gc_pen_hfrac;
    int32_t accEnd = acc + scaledAdv;
    const uint8_t *st = p->gc_cur_strike;
    if (len > 0 && rawAdv != 0 && st && p->gc_cur_wt && off + 0x10 + (uint32_t)len <= GC824_DRAM_SIZE) {
        // The card stores the run's space width into the cached width table's
        // char-$20 slot, so per-space advance follows the host (0xE690).
        STORE_BE32(p->gc_cur_wt + 0x80, spaceW);
        int first = (int16_t)LOAD_BE16(st + 2), last = (int16_t)LOAD_BE16(st + 4);
        int widMax = (int16_t)LOAD_BE16(st + 6);
        int kern = (int16_t)LOAD_BE16(st + 8);
        int height = (int16_t)LOAD_BE16(st + 14);
        uint32_t owtloc = LOAD_BE16(st + 16);
        int ascent = (int16_t)LOAD_BE16(st + 18);
        uint32_t rowbytes = (uint32_t)LOAD_BE16(st + 24) * 2u;
        const uint8_t *bits = st + 26;
        const uint8_t *loct = st + 26 + (size_t)rowbytes * (size_t)height;
        const uint8_t *owt = st + 16 + (size_t)owtloc * 2;
        int nglyph = last - first + 1; // owTable/locTable index of the missing glyph
        // Bounds: bitImage + both tables (incl. the missing glyph's end column)
        // must lie inside the cached strike bytes.
        bool sane = first >= 0 && last >= first && height > 0 && rowbytes > 0 &&
                    26 + rowbytes * (size_t)height + (size_t)(nglyph + 2) * 2 <= p->gc_cur_strike_size &&
                    16 + (size_t)owtloc * 2 + (size_t)(nglyph + 1) * 2 <= p->gc_cur_strike_size;
        if (!sane) {
            LOG(0, "opText: cached strike fails sanity (first %d last %d h %d rb %u size %u) — run dropped", first,
                last, height, rowbytes, p->gc_cur_strike_size);
        } else {
            if (p->gc_font_info[6] || p->gc_font_info[7])
                LOG(1, "opText: on-card style synthesis (bold/italic) not modelled — drawn plain");
            int top = p->gc_pen_y - ascent;
            gc_cursor_shield(p, top, (acc >> 16) + kern - widMax, top + height, (accEnd >> 16) + kern + 2 * widMax);
            if ((p->gc_mode & 0x27) == 0) {
                // Copy-class text: the ROM renders the run into a buffer and
                // blits the measured box, so [pen, pen+advance) ×
                // [pen−ascent, +fRectHeight) is painted BACKGROUND before the
                // ink lands — erasing what was there (the General cdev's
                // ticking clock repaints its seconds digits this way).
                for (int row = 0; row < height; row++)
                    for (int ex = acc >> 16; ex < (accEnd >> 16); ex++)
                        gc_px(p, ex, top + row, 0);
            }
            for (int i = 0; i < len; i++) {
                uint8_t c = p->dram[off + 0x10 + (uint32_t)i];
                int idx = (c >= first && c <= last) ? c - first : nglyph;
                uint16_t ow = LOAD_BE16(owt + (size_t)idx * 2);
                if (ow == 0xFFFF && idx != nglyph) {
                    idx = nglyph;
                    ow = LOAD_BE16(owt + (size_t)idx * 2);
                }
                if (ow != 0xFFFF) {
                    int goff = ow >> 8; // left-side bearing (with kernMax)
                    int c0 = LOAD_BE16(loct + (size_t)idx * 2);
                    int c1 = LOAD_BE16(loct + (size_t)idx * 2 + 2);
                    int left = (acc >> 16) + kern + goff;
                    if (c1 > c0 && (uint32_t)c1 <= rowbytes * 8u) {
                        for (int row = 0; row < height; row++) {
                            const uint8_t *rb = bits + (size_t)row * rowbytes;
                            for (int col = c0; col < c1; col++)
                                if (rb[col >> 3] & (0x80u >> (col & 7)))
                                    gc_px(p, left + (col - c0), top + row, 1);
                        }
                    }
                }
                acc += (int32_t)LOAD_BE32(p->gc_cur_wt + (size_t)c * 4);
            }
            p->draw_count++;
        }
    } else if (len > 0 && rawAdv != 0) {
        LOG(0, "opText with no cached font — run dropped");
    }
    // Mirror the host's eager pen advance in the card pen (0xE830-0xE848).
    p->gc_pen_x = (int16_t)(accEnd >> 16);
    p->gc_pen_hfrac = (uint16_t)(accEnd & 0xFFFF);
}

// Reset the interpreter's per-cycle state to QuickDraw defaults at the start of
// a drawing cycle (a fresh queue buffer / a new accepted port).  State opcodes
// in the stream then set the live values; clip only ever narrows from full.
void gc824_reset_draw_state(display_card_824gc_priv_t *p) {
    p->gc_align_x = 0;
    p->gc_align_y = 0;
    p->gc_clip_t = 0;
    p->gc_clip_l = 0;
    p->gc_clip_b = (int16_t)p->display.height;
    p->gc_clip_r = (int16_t)p->display.width;
    gc_clip_reset(p); // all-drawable; opClipRgn/opVisRgn replace into it
    p->gc_cliprgn_len = 0;
    p->gc_visrgn_len = 0;
    // Default port colours as PIXEL VALUES for the screen depth: black fg,
    // white bg (1 bpp: 1/0; 8 bpp standard CLUT: $FF/$00; 16 bpp direct:
    // $0000/$7FFF; 32 bpp direct: $000000/$FFFFFF).
    p->gc_fg = (p->display.format == PIXEL_8BPP)         ? 0xFFu
               : (p->display.format == PIXEL_32BPP_XRGB) ? 0u
               : (p->display.format == PIXEL_16BPP_555)  ? 0u
                                                         : 1u;
    p->gc_bg = (p->display.format == PIXEL_32BPP_XRGB)  ? 0x00FFFFFFu
               : (p->display.format == PIXEL_16BPP_555) ? 0x7FFFu
                                                        : 0u;
    // Default hilite = the architectural low-mem HiliteRGB ($0DA0, 3 words) —
    // what an old-style port uses when no op $70 arrives (§3.1).
    p->gc_hilite = gc_resolve_rgb(p, memory_debug_read_uint16(0x0DA0), memory_debug_read_uint16(0x0DA2),
                                  memory_debug_read_uint16(0x0DA4));
    p->gc_op_color = 0;
    // Old-port arithmetic defaults (§3.1): blend weight = 50% gray $7FFF
    // (addPin pins to white, subPin to black — mode-specific fallbacks in
    // gc_px_arith when the weight is the default).
    p->gc_op_rgb[0] = p->gc_op_rgb[1] = p->gc_op_rgb[2] = 0x7FFF;
    p->gc_pen_hfrac = 0x8000; // pnLocHFrac default (a half pixel)
    p->gc_pat_slot = 1;
    memset(p->gc_pat, 0, sizeof(p->gc_pat));
    memset(p->gc_pat_kind, 0, sizeof(p->gc_pat_kind));
}
