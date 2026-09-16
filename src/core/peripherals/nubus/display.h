// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// display.h
// Display descriptor used by all display sources (machine-owned framebuffers
// on Plus, NuBus video cards on the glue030 family).  Consumers (WebGL
// renderer, PNG save/match, screen.* surface) read the descriptor every
// frame; the renderer additionally watches the per-resource dirty flags
// below to decide what to re-upload to the GPU.
//
// All fields are live-mutable: a card may change `bits`, `width`, `height`,
// `stride`, `format`, `clut`, or `clut_len` at any time.  Whenever it does,
// it sets the matching `*_dirty` flag.  The renderer clears the flag after
// consuming it.

#ifndef NUBUS_DISPLAY_H
#define NUBUS_DISPLAY_H

#include "common.h" // GS_UNIMPLEMENTED
#include <stdbool.h>
#include <stddef.h>

#include <stdint.h>
#include <string.h>

// Pixel encodings exposed by display sources.
typedef enum pixel_format {
    PIXEL_1BPP_MSB = 0, // 1 bpp packed, MSB = leftmost pixel (Plus, SE/30 builtin)
    PIXEL_2BPP_MSB, // 2 bpp packed, MSB-first; 4-entry CLUT
    PIXEL_4BPP_MSB, // 4 bpp packed, MSB-first; 16-entry CLUT
    PIXEL_8BPP, // 8 bpp indexed; 256-entry CLUT
    PIXEL_16BPP_555, // direct: 1-5-5-5 RGB, big-endian (Mac convention)
    PIXEL_32BPP_XRGB, // direct: 32 bpp [X][R][G][B] big-endian, X unused.
                      // The Apple 8•24 / JMFB's "millions of colours" mode
                      // uses this storage layout — QuickDraw and the JMFB
                      // driver agree the framebuffer is 4 bytes/pixel, but
                      // the RAMDAC scans only the RGB triple per pixel and
                      // discards the X byte (per the JMFB driver's TFBM30
                      // mode-data and the Designing Cards & Drivers
                      // "24bpp packed-pixel" terminology).  The 24-bit name
                      // describes the visible colour depth, not the storage.
    PIXEL_16BPP_565, // direct: 5-6-5 RGB, big-endian (same byte order as
                     // 5-5-5 above).  The natural format of the 3dfx
                     // Voodoo2's framebuffer and of the Mach64's
                     // CRTC_PIX_WIDTH=4 mode; appended after 32 bpp so the
                     // existing formats keep their values.
} pixel_format_t;

// The byte a display source should fill fresh VRAM with so a cold boot
// scans out BLACK, which is what a real monitor shows before the video
// circuitry starts driving it.
//
// Zero-filled VRAM is not black everywhere: 1 bpp is scanned out inverted
// (1 = black, 0 = white — the Mac convention), so an all-zero 1 bpp buffer
// is a WHITE screen, and that is what the user sees for the first second of
// a cold boot, before the ROM programs the card and paints anything.  Every
// other format already reads black at zero — the direct formats encode
// black as all-zero, and the indexed formats power up with an all-zero
// (i.e. black) CLUT — so this only has to special-case 1 bpp.
static inline uint8_t display_black_fill(pixel_format_t format) {
    return format == PIXEL_1BPP_MSB ? 0xFFu : 0x00u;
}

// Bits per pixel for a format.  A property of the encoding, not of the card
// that scans it out, so it lives with the enum: five display sources each
// carried a private copy of this switch, and two of them had fallen behind
// the enum (Ariel had no 32 bpp case, the RBV built-in had neither 16 nor
// 32), silently answering 8 for anything they had not been taught.
static inline uint32_t display_bpp(pixel_format_t format) {
    switch (format) {
    case PIXEL_1BPP_MSB:
        return 1;
    case PIXEL_2BPP_MSB:
        return 2;
    case PIXEL_4BPP_MSB:
        return 4;
    case PIXEL_8BPP:
        return 8;
    case PIXEL_16BPP_555:
    case PIXEL_16BPP_565:
        return 16;
    case PIXEL_32BPP_XRGB:
        return 32;
    }
    // Not a pixel_format_t this build knows.  Answering a plausible number
    // here is how a bad format becomes a wrong stride three layers away,
    // where it looks like a garbled screen rather than a missing case: the
    // 5-6-5 arm above was absent until 2026-09-16, and -Wswitch had been
    // saying so on every build of both targets for as long as the format
    // existed.  Nothing reached it -- the Mach64 selects 565 and computes its
    // own stride -- but the fix for 04-video F-01 is to route callers HERE,
    // which would have turned a latent 8-for-16 into a live one.
    GS_UNIMPLEMENTED("display_bpp: pixel format %d has no bits-per-pixel rule", (int)format);
    return 8;
}

// Single CLUT entry; rgba layout matches QuickDraw's RGBColor packed for
// host consumption (alpha is always 255 on Mac displays).
typedef struct rgba8 {
    uint8_t r, g, b, a;
} rgba8_t;

// Display descriptor.  Owned by whichever source (machine or NuBus card)
// drives the active display.  Consumers must not retain the pointer across
// frames — read fresh each frame and consume the dirty flags to learn
// which GPU resources need re-uploading.
//
// `crt_response` models the physical response curve of the monitor on the
// far end of the cable.  Mac System 7's video drivers gamma-pre-correct
// every CLUT write per a per-monitor gamma table (see the JMFB driver's
// SetGamma / ProgramCLUT); on real hardware
// the CRT's phosphor/electron-gun gamma applies the inverse and the user
// sees a perceptually-neutral image.  Software displays have no CRT to
// cancel the pre-correction, so without modelling the monitor's response
// the gamma table shows through as a chromatic tint (Kong's blue
// attenuation surfaces as yellow on screen).  `crt_response` is the
// inverse LUT applied per channel at display time; identity means
// "no monitor response model — show what the card put on the bus."
//
// Layout: 3 × 256 bytes.  crt_response[c][v] = the perceptual output
// value when channel c receives byte v on the bus.  Channel order is
// R/G/B = 0/1/2.  The display source owns the storage; consumers read
// const.
//
// Dirty flags: producers set the relevant flag(s) at every mutation
// point; the renderer reads them at refresh time and clears them after
// consuming.  Flags are not mutually exclusive — e.g. an SE/30 alt-buffer
// swap changes only `bits` (fb_dirty); a JMFB depth change re-derives
// stride and format (shape_dirty); a CLUT entry write only touches the
// palette (clut_dirty).  shape_dirty implies the framebuffer texture
// must be reallocated and its contents re-uploaded; the renderer treats
// shape_dirty as fb-implying so producers don't need to set both.
typedef struct display {
    uint32_t width; // pixels
    uint32_t height; // pixels
    uint32_t stride; // bytes per row in `bits`
    pixel_format_t format; // pixel encoding

    // Pixel aspect ratio: the physical shape of ONE framebuffer pixel on the
    // real monitor, as host-pixel width:height (par_w : par_h).  Most displays
    // are square (1:1) — the Macintosh XL screen mod even reshaped the raster to
    // 608x431 specifically to get square pixels.  The Lisa 2's native 720x364
    // raster is NOT square: its pixels are taller than wide, so the renderer
    // must stretch the vertical axis to avoid a squashed image.  0 in either
    // field means "square" (the consumer normalizes 0 -> 1), so producers that
    // don't care leave both zero.  This is display metadata only — `bits`,
    // `width`, `height`, and `stride` are unaffected, so PNG capture and
    // pixel-exact matching see the raw framebuffer regardless.
    uint32_t par_w; // pixel-aspect numerator (display pixel width);  0 => 1
    uint32_t par_h; // pixel-aspect denominator (display pixel height); 0 => 1

    const uint8_t *bits; // primary framebuffer; stride * height bytes
    const rgba8_t *clut; // 0/4/16/256-entry palette; NULL for direct formats
    uint32_t clut_len; // entries in clut (0 for direct formats)
    const uint8_t (*crt_response)[256]; // 3 × 256 bytes (R/G/B inverse gamma); NULL = identity

    bool fb_dirty; // `bits` contents may have changed (incl. pointer swap)
    bool shape_dirty; // width/height/stride/format changed — texture needs reallocation
    bool clut_dirty; // CLUT entries changed
    bool response_dirty; // crt_response changed (effectively init-only today)

    // The frame is being presented by someone else — the Voodoo2 under
    // the WebGPU takeover shows its own frames on an overlay canvas —
    // so `bits` is NOT refreshed per frame and the renderer must skip
    // its pixel upload (shape changes still apply: the canvas keeps
    // the card's geometry).  `sync_pixels` brings `bits` current on
    // demand (a screenshot, a checksum): call display_sync_pixels()
    // before READING bits.  NULL when bits are always current.
    bool presented_externally;
    void (*sync_pixels)(void *ctx);
    void *sync_ctx;
} display_t;

// ============================================================================
// Format authority
// ============================================================================
// Everything below is a property of pixel_format_t, not of the card that
// scans it out, so it lives with the enum.  Before 2026-09-16 each of these
// existed two to eight times across debug.c, nubus_class.c and the WebGL
// renderer, and the copies had drifted: display_bpp answered 8 for a 5-6-5
// format while nubus_class.c's copy answered 16, and the two disagreed about
// what an unknown format means (8 vs 0).  04-video F-01, F-02, F-04.

// 5-bit and 6-bit channels expanded to 8, by bit replication -- (v << 3) |
// (v >> 2) maps 31 to 255 and 0 to 0, which a plain (v * 255 / 31) also does
// but with a divide.  The alternative some code used, a bare (v << 3), tops
// out at 248 and darkens every highlight by one LSB.
static inline uint8_t display_expand5(uint8_t v) {
    return (uint8_t)((v << 3) | (v >> 2));
}
static inline uint8_t display_expand6(uint8_t v) {
    return (uint8_t)((v << 2) | (v >> 4));
}

// The shell-facing name of a format.  One spelling, so machine.screen and a
// card's framebuffer node cannot disagree about what the guest is running.
static inline const char *display_format_name(pixel_format_t format) {
    switch (format) {
    case PIXEL_1BPP_MSB:
        return "1bpp";
    case PIXEL_2BPP_MSB:
        return "2bpp";
    case PIXEL_4BPP_MSB:
        return "4bpp";
    case PIXEL_8BPP:
        return "8bpp_clut";
    case PIXEL_16BPP_555:
        return "16bpp_555";
    case PIXEL_16BPP_565:
        return "16bpp_565";
    case PIXEL_32BPP_XRGB:
        return "32bpp_xrgb";
    }
    return "?";
}

// One pixel of a row, as RGB.
//
// `clut_len` of zero used to reach `idx % clut_len` and divide by zero
// (04-video F-30); a direct format has no CLUT and an indexed one with an
// empty CLUT has nothing to look up, so both answer black.
static inline void display_pixel_rgb(const display_t *d, const uint8_t *src_row, uint32_t x, uint8_t out[3]) {
    const rgba8_t *clut = d->clut;
    const uint32_t clut_len = d->clut_len;
    uint8_t r = 0, g = 0, b = 0;
    uint32_t idx = 0;
    switch (d->format) {
    case PIXEL_1BPP_MSB: {
        int bit = (src_row[x >> 3] >> (7 - (x & 7))) & 1;
        r = g = b = bit ? 0 : 255; // 1 = black, 0 = white (Mac convention)
        goto done;
    }
    case PIXEL_2BPP_MSB:
        idx = (uint32_t)((src_row[x >> 2] >> ((3 - (x & 3)) * 2)) & 0x3);
        break;
    case PIXEL_4BPP_MSB:
        idx = (uint32_t)((src_row[x >> 1] >> ((1 - (x & 1)) * 4)) & 0xF);
        break;
    case PIXEL_8BPP:
        idx = src_row[x];
        break;
    case PIXEL_16BPP_555: {
        uint16_t v = (uint16_t)(((uint16_t)src_row[x * 2] << 8) | src_row[x * 2 + 1]);
        r = display_expand5((uint8_t)((v >> 10) & 0x1F));
        g = display_expand5((uint8_t)((v >> 5) & 0x1F));
        b = display_expand5((uint8_t)(v & 0x1F));
        goto done;
    }
    case PIXEL_16BPP_565: {
        uint16_t v = (uint16_t)(((uint16_t)src_row[x * 2] << 8) | src_row[x * 2 + 1]);
        r = display_expand5((uint8_t)((v >> 11) & 0x1F));
        g = display_expand6((uint8_t)((v >> 5) & 0x3F));
        b = display_expand5((uint8_t)(v & 0x1F));
        goto done;
    }
    case PIXEL_32BPP_XRGB:
        // [X][R][G][B] big-endian; the RAMDAC scans the RGB triple and
        // discards X (see the enum comment).
        r = src_row[x * 4 + 1];
        g = src_row[x * 4 + 2];
        b = src_row[x * 4 + 3];
        goto done;
    }
    if (clut && clut_len) {
        rgba8_t c = clut[idx % clut_len];
        r = c.r;
        g = c.g;
        b = c.b;
    }
done:
    out[0] = r;
    out[1] = g;
    out[2] = b;
}

// One row of a framebuffer as packed RGBA, written at `out_rgba`, which must
// hold at least width*4 bytes.  `out_rgba` is a base pointer rather than a
// row index so the PNG writer can aim it past its per-row filter byte.
static inline void display_row_to_rgba(const display_t *d, uint32_t y, uint8_t *out_rgba) {
    const uint8_t *src_row = d->bits + (size_t)y * d->stride;
    for (uint32_t x = 0; x < d->width; x++) {
        uint8_t *px = out_rgba + x * 4;
        display_pixel_rgb(d, src_row, x, px);
        px[3] = 255;
    }
}

// Make `bits` current before reading them (a no-op for every display
// whose producer keeps them current).
static inline void display_sync_pixels(display_t *d) {
    if (d && d->sync_pixels)
        d->sync_pixels(d->sync_ctx);
}

// Blank the visible raster of a fully-populated descriptor to black.
//
// Only `bits[0 .. stride*height)` is touched — NOT the whole VRAM
// allocation.  A card's buffer is mapped into the slot aperture in one piece
// and the framebuffer usually starts at an offset inside it (the 8•24 puts it
// at +0xA00), so filling the whole allocation writes bytes the guest can read
// that are not pixels at all.  Doing that changed what MacTest's video test
// saw on a IIcx and diverged the run — blank the raster, nothing else.
static inline void display_blank_raster(display_t *d) {
    if (!d || !d->bits || !d->stride || !d->height)
        return;
    memset((uint8_t *)d->bits, display_black_fill(d->format), (size_t)d->stride * d->height);
}

// True when the visible raster still holds nothing but its power-on blank,
// i.e. the guest has not drawn.  A depth change reinterprets every byte, so
// the fill chosen at the old depth stops meaning black at the new one (0xFF
// is black at 1 bpp but index 255 -- white in the seeded ramp -- at 8 bpp).
// Test BEFORE changing the descriptor, re-blank after, so the new raster
// size is the one that gets filled.  A depth switch under a live desktop
// must keep its pixels, which is what this guards.
static inline bool display_raster_is_pristine(const display_t *d) {
    if (!d || !d->bits || !d->stride || !d->height)
        return false;
    uint8_t fill = display_black_fill(d->format);
    size_t n = (size_t)d->stride * d->height;
    for (size_t i = 0; i < n; i++)
        if (d->bits[i] != fill)
            return false;
    return true;
}

#endif // NUBUS_DISPLAY_H
