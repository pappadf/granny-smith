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

int main(void) {
    fill_invert();
    RUN(test_32bpp_channels_go_through_the_table);
    RUN(test_16bpp_expands_then_looks_up);
    RUN(test_8bpp_uses_the_clut_and_no_table);
    printf("[PASS] All display_dac tests passed\n");
    return 0;
}
