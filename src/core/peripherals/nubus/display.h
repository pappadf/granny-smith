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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
                      // discards the X byte (per JMFBDriver.a's TFBM30
                      // mode-data and the Designing Cards & Drivers
                      // "24bpp packed-pixel" terminology).  The 24-bit name
                      // describes the visible colour depth, not the storage.
} pixel_format_t;

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
// SetGamma / ProgramCLUT in Apple-341-0868-vrom.asm); on real hardware
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
    const uint8_t *bits; // primary framebuffer; stride * height bytes
    const rgba8_t *clut; // 0/4/16/256-entry palette; NULL for direct formats
    uint32_t clut_len; // entries in clut (0 for direct formats)
    const uint8_t (*crt_response)[256]; // 3 × 256 bytes (R/G/B inverse gamma); NULL = identity

    bool fb_dirty; // `bits` contents may have changed (incl. pointer swap)
    bool shape_dirty; // width/height/stride/format changed — texture needs reallocation
    bool clut_dirty; // CLUT entries changed
    bool response_dirty; // crt_response changed (effectively init-only today)
} display_t;

#endif // NUBUS_DISPLAY_H
