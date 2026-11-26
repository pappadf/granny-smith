// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// record_pixels.c
// The recorder's pixel work.  See record_pixels.h.

#include "record_pixels.h"

#include <string.h>

// ============================================================================
// Formats and classes
// ============================================================================

uint32_t rec_pix_bpp(uint32_t format) {
    switch (format) {
    case REC_PIX_1BPP:
        return 1;
    case REC_PIX_2BPP:
        return 2;
    case REC_PIX_4BPP:
        return 4;
    case REC_PIX_8BPP:
        return 8;
    case REC_PIX_16BPP555:
    case REC_PIX_16BPP565:
        return 16;
    case REC_PIX_32BPP:
        return 32;
    default:
        return 0;
    }
}

bool rec_pix_indexed(uint32_t format) {
    return format <= REC_PIX_8BPP;
}

size_t rec_pix_row_bytes(uint32_t format, uint32_t width) {
    return ((size_t)width * rec_pix_bpp(format) + 7) / 8;
}

int rec_pix_class(uint32_t format, bool tables, bool rgb) {
    if (rec_pix_indexed(format))
        return rgb ? REC_CLASS_D32 : REC_CLASS_INDEXED;
    // A response table's output is 8 bits a channel: 15/16 bpp cannot hold it
    if (!tables && !rgb && format == REC_PIX_16BPP555)
        return REC_CLASS_D15;
    if (!tables && !rgb && format == REC_PIX_16BPP565)
        return REC_CLASS_D16;
    return REC_CLASS_D32;
}

bool rec_pix_class_fits(int cls, uint32_t format, bool tables) {
    // 32 bpp holds every format; the others only their own
    if (cls == REC_CLASS_D32)
        return rec_pix_bpp(format) != 0;
    return rec_pix_class(format, tables, false) == cls;
}

int rec_pix_class_bytes(int cls) {
    switch (cls) {
    case REC_CLASS_INDEXED:
        return 1;
    case REC_CLASS_D15:
    case REC_CLASS_D16:
        return 2;
    default:
        return 4;
    }
}

// ============================================================================
// The emulator thread's pass
// ============================================================================

bool rec_band_scan(const uint8_t *fb, size_t stride, const uint8_t *shadow, size_t row_bytes, uint32_t height,
                   uint32_t *first, uint32_t *end) {
    uint32_t a = 0;
    // The first changed row from the top...
    while (a < height && memcmp(fb + (size_t)a * stride, shadow + (size_t)a * row_bytes, row_bytes) == 0)
        a++;
    if (a == height) {
        *first = *end = 0;
        return false;
    }
    // ...and the last from the bottom; rows between are taken as they are
    uint32_t b = height;
    while (b > a + 1 && memcmp(fb + (size_t)(b - 1) * stride, shadow + (size_t)(b - 1) * row_bytes, row_bytes) == 0)
        b--;
    *first = a;
    *end = b;
    return true;
}

void rec_band_take(const uint8_t *fb, size_t stride, uint8_t *shadow, size_t row_bytes, uint32_t first, uint32_t end,
                   bool xor, uint8_t *out) {
    for (uint32_t y = first; y < end; y++) {
        const uint8_t *src = fb + (size_t)y * stride;
        uint8_t *sh = shadow + (size_t)y * row_bytes;
        if (out) {
            uint8_t *dst = out + (size_t)(y - first) * row_bytes;
            if (xor) {
                // The delta is the payload ZMBV codes for indexed modes
                for (size_t i = 0; i < row_bytes; i++)
                    dst[i] = (uint8_t)(src[i] ^ sh[i]);
            } else {
                memcpy(dst, src, row_bytes);
            }
        }
        memcpy(sh, src, row_bytes);
    }
}

// ============================================================================
// Response tables and palettes
// ============================================================================

void rec_table_compose(uint8_t out[3][256], const uint8_t *dac, const uint8_t *crt) {
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) {
            // The card's DAC first, then the monitor's curve (display.h's order)
            int in = dac ? dac[c * 256 + v] : v;
            out[c][v] = crt ? crt[c * 256 + in] : (uint8_t)in;
        }
}

void rec_palette_build(uint8_t pal[768], uint32_t format, const uint8_t *clut, uint32_t clut_len,
                       const uint8_t table[3][256]) {
    for (uint32_t i = 0; i < 256; i++) {
        uint8_t r = 0, g = 0, b = 0;
        if (format == REC_PIX_1BPP) {
            // 1 = black, 0 = white: the Mac convention, no CLUT involved
            if (i == 0)
                r = g = b = 255;
        } else if (clut && clut_len) {
            const uint8_t *e = clut + (i % clut_len) * 3;
            r = e[0];
            g = e[1];
            b = e[2];
        }
        pal[i * 3] = table[0][r];
        pal[i * 3 + 1] = table[1][g];
        pal[i * 3 + 2] = table[2][b];
    }
}

uint8_t rec_palette_black(const uint8_t pal[768]) {
    uint32_t best = 0, best_d = UINT32_MAX;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t d = (uint32_t)pal[i * 3] * pal[i * 3] + (uint32_t)pal[i * 3 + 1] * pal[i * 3 + 1] +
                     (uint32_t)pal[i * 3 + 2] * pal[i * 3 + 2];
        // Strictly closer only, so the lowest index wins a tie
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

// ============================================================================
// Row conversion
// ============================================================================

// The palette index of pixel x of an indexed raw row.
static inline uint8_t index_at(const uint8_t *raw, uint32_t format, uint32_t x) {
    switch (format) {
    case REC_PIX_1BPP:
        return (uint8_t)((raw[x >> 3] >> (7 - (x & 7))) & 1);
    case REC_PIX_2BPP:
        return (uint8_t)((raw[x >> 2] >> ((3 - (x & 3)) * 2)) & 3);
    case REC_PIX_4BPP:
        return (uint8_t)((raw[x >> 1] >> ((1 - (x & 1)) * 4)) & 15);
    default:
        return raw[x];
    }
}

// 5 bits widened to 8 by bit replication (display.h's expansion).
static inline uint8_t expand5(uint32_t v) {
    return (uint8_t)((v << 3) | (v >> 2));
}

// 6 bits widened to 8 by bit replication.
static inline uint8_t expand6(uint32_t v) {
    return (uint8_t)((v << 2) | (v >> 4));
}

void rec_row_convert(const uint8_t *raw, uint32_t format, uint32_t width, int cls, const uint8_t table[3][256],
                     const uint8_t pal[768], uint8_t *out) {
    if (cls == REC_CLASS_INDEXED) {
        if (format == REC_PIX_8BPP) {
            memcpy(out, raw, width);
            return;
        }
        // One index per byte: DEFLATE takes the redundancy back
        for (uint32_t x = 0; x < width; x++)
            out[x] = index_at(raw, format, x);
        return;
    }
    if (cls == REC_CLASS_D15 || cls == REC_CLASS_D16) {
        // Big-endian words to little-endian: the bit layout is ZMBV's already
        for (uint32_t x = 0; x < width; x++) {
            out[x * 2] = raw[x * 2 + 1];
            out[x * 2 + 1] = raw[x * 2];
        }
        return;
    }
    // 32 bpp: 0x00RRGGBB little-endian, the response applied per pixel
    for (uint32_t x = 0; x < width; x++) {
        uint8_t r, g, b;
        if (rec_pix_indexed(format)) {
            // The palette already went through the table
            const uint8_t *e = pal + (size_t)index_at(raw, format, x) * 3;
            out[x * 4] = e[2];
            out[x * 4 + 1] = e[1];
            out[x * 4 + 2] = e[0];
            out[x * 4 + 3] = 0;
            continue;
        }
        if (format == REC_PIX_16BPP555) {
            uint32_t v = ((uint32_t)raw[x * 2] << 8) | raw[x * 2 + 1];
            r = expand5((v >> 10) & 31);
            g = expand5((v >> 5) & 31);
            b = expand5(v & 31);
        } else if (format == REC_PIX_16BPP565) {
            uint32_t v = ((uint32_t)raw[x * 2] << 8) | raw[x * 2 + 1];
            r = expand5((v >> 11) & 31);
            g = expand6((v >> 5) & 63);
            b = expand5(v & 31);
        } else {
            // [X][R][G][B]: the X byte is not scanned
            r = raw[x * 4 + 1];
            g = raw[x * 4 + 2];
            b = raw[x * 4 + 3];
        }
        out[x * 4] = table[2][b];
        out[x * 4 + 1] = table[1][g];
        out[x * 4 + 2] = table[0][r];
        out[x * 4 + 3] = 0;
    }
}
