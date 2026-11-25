// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// record_pixels.h
// The recorder's pixel work, apart from any machine: finding the band of
// rows that changed against a shadow copy (the emulator thread's one pass),
// and turning the guest's raw rows into the stored picture (the recorder
// thread's) -- sub-byte indices widened to one per byte, big-endian 16 and
// 32 bpp pixels swapped to the little-endian layout ZMBV stores, and the
// two response tables (the card's DAC table, then the monitor's curve)
// applied: through the palette for indexed pixels, per pixel otherwise.
//
// The pixel formats are display.h's (pixel_format_t, by value), so this
// file needs no machine headers and the unit tier can drive it.

#ifndef RECORD_PIXELS_H
#define RECORD_PIXELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Constants ===

// The guest's pixel formats (display.h's pixel_format_t values).
#define REC_PIX_1BPP     0
#define REC_PIX_2BPP     1
#define REC_PIX_4BPP     2
#define REC_PIX_8BPP     3
#define REC_PIX_16BPP555 4
#define REC_PIX_32BPP    5
#define REC_PIX_16BPP565 6

// What a recording stores, fixed for a segment.
#define REC_CLASS_INDEXED 1 // ZMBV 8 bpp + palette: every indexed guest depth
#define REC_CLASS_D15     2 // ZMBV 15 bpp: x-5-5-5 with no response table
#define REC_CLASS_D16     3 // ZMBV 16 bpp: 5-6-5 with no response table
#define REC_CLASS_D32     4 // ZMBV 32 bpp: any direct colour (or "rgb")

// === Operations ===

// Bits per pixel of a guest format (0 for an unknown one).
uint32_t rec_pix_bpp(uint32_t format);

// True for the indexed formats (1, 2, 4, 8 bpp).
bool rec_pix_indexed(uint32_t format);

// The bytes a row's pixels occupy (not the stride): ceil(width * bpp / 8).
size_t rec_pix_row_bytes(uint32_t format, uint32_t width);

// The class a guest format is stored as: indexed unless `rgb`; 15 / 16 bpp
// stay so only when no response table applies (a table's output is 8 bits
// a channel); everything else is 32 bpp.
int rec_pix_class(uint32_t format, bool tables, bool rgb);

// True when a mode of `format` can go into a segment of class `cls`: the
// same class, or any indexed or direct colour into a 32 bpp one.
bool rec_pix_class_fits(int cls, uint32_t format, bool tables);

// Bytes per pixel ZMBV stores for a class (1, 2 or 4).
int rec_pix_class_bytes(int cls);

// The first and one-past-last rows [*first, *end) of `height` rows whose
// first `row_bytes` bytes differ between the framebuffer (`stride` apart)
// and the shadow (`row_bytes` apart); false (and an empty band) when none do.
bool rec_band_scan(const uint8_t *fb, size_t stride, const uint8_t *shadow, size_t row_bytes, uint32_t height,
                   uint32_t *first, uint32_t *end);

// Rows [first, end) out of the framebuffer into `out` (row_bytes apart),
// XORed with the shadow (xor) or whole, and the shadow brought up to date.
// `out` may be NULL: the shadow alone advances (a frame that was dropped).
void rec_band_take(const uint8_t *fb, size_t stride, uint8_t *shadow, size_t row_bytes, uint32_t first, uint32_t end,
                   bool xor, uint8_t *out);

// The composite response `out`: the DAC table then the monitor's curve,
// each 3 x 256 (R, G, B) or NULL for identity.
void rec_table_compose(uint8_t out[3][256], const uint8_t *dac, const uint8_t *crt);

// The 256-entry palette (768 bytes RGB) an indexed frame is stored with:
// at 1 bpp white and black (0 and 1, as the Mac scans it), otherwise
// `clut` (clut_len RGB triplets, index modulo clut_len, black when empty);
// every entry through the composite response `table`.
void rec_palette_build(uint8_t pal[768], uint32_t format, const uint8_t *clut, uint32_t clut_len,
                       const uint8_t table[3][256]);

// The palette index closest to black (the first on a tie): what borders a
// mode smaller than the canvas.
uint8_t rec_palette_black(const uint8_t pal[768]);

// One raw row of `width` pixels in `format` into the stored layout of
// class `cls` at `out`: indices (INDEXED), little-endian 16-bit words (D15,
// D16) or bytes B, G, R, 0 (D32: direct pixels through `table`, indexed
// ones through `pal`, which already carries the table).
void rec_row_convert(const uint8_t *raw, uint32_t format, uint32_t width, int cls, const uint8_t table[3][256],
                     const uint8_t pal[768], uint8_t *out);

#endif // RECORD_PIXELS_H
