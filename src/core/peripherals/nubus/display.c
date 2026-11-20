// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// display.c
// The checkpoint block of a display descriptor's head (display.h), shared by
// every producer that saves one, so the fixed-width form is written once.

#include "display.h"

#include "checkpoint.h"

void display_head_save(checkpoint_t *cp, const display_t *d) {
    // Fixed widths, not a raw struct prefix: the prefix carried a bare
    // pixel_format_t, whose size is implementation-defined (see display.h).
    display_head_t head = display_head_of(d);
    system_write_checkpoint_data(cp, &head, sizeof head, "display.head");
}

void display_head_restore(checkpoint_t *cp, display_t *d) {
    display_head_t head;
    system_read_checkpoint_data(cp, &head, sizeof head, "display.head");
    display_head_apply(d, &head);
}

void display_cursor_overlay(uint8_t *dst, uint32_t stride, uint32_t width, uint32_t height, uint32_t bpp, uint32_t px,
                            uint32_t py, uint32_t ox, uint32_t oy, uint32_t size, display_cursor_row_fn decode,
                            const void *ctx, const uint32_t clr[2]) {
    uint8_t codes[64];
    if (size > 64)
        size = 64;
    for (uint32_t row = oy; row < size; row++) {
        uint32_t y = py + (row - oy);
        if (y >= height)
            break;
        decode(ctx, row, codes); // the chip's encoding, one row at a time
        for (uint32_t col = ox; col < size; col++) {
            uint32_t x = px + (col - ox);
            if (x >= width)
                break;
            uint8_t v = codes[col];
            if (v == DISPLAY_CURSOR_TRANSPARENT)
                continue;
            uint8_t *at = dst + (size_t)y * stride + (size_t)x * bpp;
            for (uint32_t i = 0; i < bpp; i++) {
                uint32_t shift = 8u * (bpp - 1u - i); // big-endian pixel bytes
                at[i] = (v == DISPLAY_CURSOR_INVERT) ? (uint8_t)~at[i] : (uint8_t)(clr[v] >> shift);
            }
        }
    }
}
