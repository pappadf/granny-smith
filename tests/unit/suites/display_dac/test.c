// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// display.h's pixel decode with a direct-colour DAC table (#147).
//
// Control's RaDACal keeps its 256-entry table in the DAC path at 16 and
// 32 bpp, one lookup per channel; the descriptor publishes it as `dac_lut`
// and both consumers -- the renderer and the capture path behind
// screen.save / screen.match -- apply it.  This pins the capture half:
// display_row_to_rgba maps each channel through the table in the direct
// formats, leaves it alone with no table, and does not touch the 8 bpp CLUT
// path, where the table IS the CLUT.

#include "display.h"

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// A table that maps every channel value v to 255 - v (an inverting DAC), so
// a wrong channel order or a missed lookup shows in the numbers.
static uint8_t s_invert[3][256];

static void fill_invert(void) {
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++)
            s_invert[c][v] = (uint8_t)(255 - v);
}

TEST(test_32bpp_channels_go_through_the_table) {
    // One [X][R][G][B] pixel: R=4, G=15, B=200.
    static const uint8_t row[4] = {0x00, 4, 15, 200};
    display_t d = {.width = 1, .height = 1, .stride = 4, .format = PIXEL_32BPP_XRGB, .bits = row};
    uint8_t rgba[4];

    display_row_to_rgba(&d, 0, rgba);
    ASSERT_EQ_INT(4, rgba[0]);
    ASSERT_EQ_INT(15, rgba[1]);
    ASSERT_EQ_INT(200, rgba[2]);

    d.dac_lut = s_invert;
    display_row_to_rgba(&d, 0, rgba);
    ASSERT_EQ_INT(251, rgba[0]);
    ASSERT_EQ_INT(240, rgba[1]);
    ASSERT_EQ_INT(55, rgba[2]);
    ASSERT_EQ_INT(255, rgba[3]);
}

TEST(test_16bpp_expands_then_looks_up) {
    // 555: R=31, G=0, B=16 -> expanded 255, 0, 132.
    static const uint8_t row[2] = {0x7C, 0x10};
    display_t d = {.width = 1, .height = 1, .stride = 2, .format = PIXEL_16BPP_555, .bits = row, .dac_lut = s_invert};
    uint8_t rgba[4];

    display_row_to_rgba(&d, 0, rgba);
    ASSERT_EQ_INT(0, rgba[0]);
    ASSERT_EQ_INT(255, rgba[1]);
    ASSERT_EQ_INT(255 - display_expand5(16), rgba[2]);
}

TEST(test_8bpp_uses_the_clut_and_no_table) {
    static const uint8_t row[1] = {7};
    static rgba8_t clut[256];
    clut[7] = (rgba8_t){.r = 10, .g = 20, .b = 30, .a = 255};
    display_t d = {
        .width = 1, .height = 1, .stride = 1, .format = PIXEL_8BPP, .bits = row, .clut = clut, .clut_len = 256};
    uint8_t rgba[4];

    display_row_to_rgba(&d, 0, rgba);
    ASSERT_EQ_INT(10, rgba[0]);
    ASSERT_EQ_INT(20, rgba[1]);
    ASSERT_EQ_INT(30, rgba[2]);
}

// A cursor whose row r reads: col 0 colour 0, col 1 colour 1, col 2
// transparent, col 3 invert, the rest transparent.
static void test_cursor_row(const void *ctx, uint32_t row, uint8_t *codes) {
    (void)ctx, (void)row;
    memset(codes, DISPLAY_CURSOR_TRANSPARENT, 64);
    codes[0] = DISPLAY_CURSOR_CLR0;
    codes[1] = DISPLAY_CURSOR_CLR1;
    codes[3] = DISPLAY_CURSOR_INVERT;
}

// The shared cursor clip walk: colours land big-endian in the frame's
// depth, transparent leaves the frame, invert flips its bytes, the
// definition offset shifts the cursor, and the frame's edges clip it.
TEST(test_cursor_overlay_codes_offset_and_clip) {
    uint8_t frame[4 * 3 * 2]; // 4 x 3 pixels at 16 bpp
    memset(frame, 0x5A, sizeof frame);
    const uint32_t clr[2] = {0x1234, 0xABCD};
    // Definition (0,0) at frame (1,1): cols 0..2 of the definition fit.
    display_cursor_overlay(frame, 8, 4, 3, 2, 1, 1, 0, 0, 64, test_cursor_row, NULL, clr);
    ASSERT_EQ_INT(frame[1 * 8 + 2], 0x12); // (1,1) colour 0, high byte first
    ASSERT_EQ_INT(frame[1 * 8 + 3], 0x34);
    ASSERT_EQ_INT(frame[1 * 8 + 4], 0xAB); // (2,1) colour 1
    ASSERT_EQ_INT(frame[1 * 8 + 5], 0xCD);
    ASSERT_EQ_INT(frame[1 * 8 + 6], 0x5A); // (3,1) transparent; col 3 is clipped
    ASSERT_EQ_INT(frame[0 * 8 + 2], 0x5A); // row 0 untouched
    ASSERT_EQ_INT(frame[2 * 8 + 2], 0x12); // row 2 drawn, then the bottom edge
    // An offset of 3 makes the visible cursor start at the invert column.
    memset(frame, 0x5A, sizeof frame);
    display_cursor_overlay(frame, 8, 4, 3, 2, 0, 0, 3, 0, 64, test_cursor_row, NULL, clr);
    ASSERT_EQ_INT(frame[0], 0xA5); // (0,0) inverted
    ASSERT_EQ_INT(frame[1], 0xA5);
    ASSERT_EQ_INT(frame[2], 0x5A); // the rest transparent
}

int main(void) {
    fill_invert();
    RUN(test_32bpp_channels_go_through_the_table);
    RUN(test_16bpp_expands_then_looks_up);
    RUN(test_8bpp_uses_the_clut_and_no_table);
    RUN(test_cursor_overlay_codes_offset_and_clip);
    printf("[PASS] All display_dac tests passed\n");
    return 0;
}
