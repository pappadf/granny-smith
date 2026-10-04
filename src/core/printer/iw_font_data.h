// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iw_font_data.h
// The generated glyph tables' lookup (iw_font_data.c).

#ifndef IW_FONT_DATA_H
#define IW_FONT_DATA_H

#include "iw_font.h"

// Fill `out` (when not NULL) with the table glyph for `code` under `sel`.
// False when no generated table covers the set or the code.
bool iw_font_data_lookup(const iw_font_sel_t *sel, uint8_t code, iw_glyph_t *out);

#endif // IW_FONT_DATA_H
