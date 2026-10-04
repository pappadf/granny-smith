// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iw_font.h
// The ImageWriter's resident character shapes, as dot columns.  A glyph is
// a run of columns, each a 9-bit wire pattern (bit 0 = wire 1, the top);
// the fixed pitches print every character 8 columns wide (7 of shape and
// one of space) and only the dot spacing changes from pitch to pitch, the
// proportional pitches give each character its own width in 1/144 or
// 1/160 in columns.  The shapes come from the generated tables
// (iw_font_data.c); a set without a table falls back to a built-in 5x7
// placeholder so text still prints, marked as such once per job.

#ifndef IW_FONT_H
#define IW_FONT_H

#include <stdbool.h>
#include <stdint.h>

// Widest glyph, in columns (custom characters may be 16 dots wide)
#define IW_GLYPH_MAX_COLS 16

// Print qualities (the ESC a values)
#define IW_QUALITY_CORRESPONDENCE 0
#define IW_QUALITY_DRAFT          1
#define IW_QUALITY_NLQ            2

// One character's dots.  `pass2` is the NLQ second pass, printed 1/144 in
// below the first; n2 = 0 when the glyph has none.
typedef struct {
    uint8_t n; // columns, which is also the advance
    uint16_t cols[IW_GLYPH_MAX_COLS];
    uint8_t n2;
    uint16_t pass2[IW_GLYPH_MAX_COLS];
    bool placeholder; // drawn from the built-in placeholder font
} iw_glyph_t;

// What selects a glyph besides the code.
typedef struct {
    bool iw1; // the original ImageWriter's character generator
    uint8_t quality; // IW_QUALITY_*
    bool proportional; // a proportional pitch is selected
    uint8_t language; // national variant 0..7 (American .. Spanish)
    bool mousetext; // ESC &: $40..$5F are MouseText
    bool slash_zero; // soft switch B-1
} iw_font_sel_t;

// The glyph for `code` (0x20..0x7F; 0xA0..0xFF when the eighth bit is
// included) under `sel`.  False for a code with no printable shape (the
// caller prints nothing and does not move).
bool iw_font_glyph(const iw_font_sel_t *sel, uint8_t code, iw_glyph_t *out);

// True when the generated tables for `sel`'s set are linked in.
bool iw_font_has_tables(const iw_font_sel_t *sel);

#endif // IW_FONT_H
