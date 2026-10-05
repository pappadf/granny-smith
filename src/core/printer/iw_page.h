// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iw_page.h
// One sheet of an ImageWriter job as dots: one 1-bit plane per ribbon band
// (yellow, magenta, cyan, black; just black on a printer without colour) at
// a fixed resolution, allocated on the first dot.  A dot is stamped as a
// disc of the print wire's diameter (1/72 in) into every plane the ribbon
// mask selects, so a dot struck through two bands is both colours at once,
// as the ribbon makes it.  A finished sheet is handed to the PDF writer:
// a black-only sheet as a 1-bit stencil, a coloured one as a 4-bit indexed
// image whose index is the ribbon mask.

#ifndef IW_PAGE_H
#define IW_PAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pdf_writer.h"

// Ribbon bands, as mask bits and as palette index bits
#define IW_INK_Y 0x01
#define IW_INK_M 0x02
#define IW_INK_C 0x04
#define IW_INK_K 0x08

// Dot shapes
#define IW_DOT_DISC   0
#define IW_DOT_SQUARE 1

typedef struct iw_page {
    // Geometry, fixed by iw_page_setup
    double width_in, height_in; // the sheet
    uint16_t dpi; // raster resolution
    uint8_t nplanes; // 1 (black) or 4 (Y, M, C, K)
    uint8_t dot_shape; // IW_DOT_*
    uint32_t w, h; // pixels
    uint32_t stride; // bytes per row of one plane
    uint8_t dot_px; // dot diameter in pixels
    // Content
    uint8_t *plane[4]; // NULL until the first dot (plane 0 is black on a 1-plane page)
    uint8_t inks; // the ribbon bands used so far (IW_INK_* bits)
    uint32_t dots; // dots stamped (an empty sheet has none)
} iw_page_t;

// Set the geometry; frees any content.
void iw_page_setup(iw_page_t *p, double width_in, double height_in, unsigned dpi, unsigned nplanes, unsigned dot_shape);

// Free the content (the geometry stays): the sheet is blank again.
void iw_page_clear(iw_page_t *p);

// True when something has been printed on the sheet.
static inline bool iw_page_marked(const iw_page_t *p) {
    return p->dots != 0;
}

// Stamp one dot whose top-left corner is pixel (x, y), through the ribbon
// bands in `mask`.  Dots off the sheet are clipped.  False when memory for
// the planes runs out.
bool iw_page_dot(iw_page_t *p, int64_t x, int64_t y, uint8_t mask);

// The 16-colour palette for ink colours `inks[4]` (Y, M, C, K as RGB):
// entry i is the colour of a dot struck through the bands in mask i,
// mixed subtractively; any mix with black is black.
void iw_page_palette(const uint8_t inks[4][3], uint8_t palette[16][3]);

// Add the sheet to `w` as one page.  False when the writer fails.
bool iw_page_emit(const iw_page_t *p, pdf_writer_t *w, const uint8_t palette[16][3]);

// The sheet's content as one buffer for a checkpoint: `inks`, then each
// used plane, deflated.  *out is malloc'd.  An empty sheet gives 0 bytes.
bool iw_page_serialise(const iw_page_t *p, uint8_t **out, size_t *out_len);

// Restore content written by iw_page_serialise onto a sheet with the same
// geometry.  False when the data does not fit.
bool iw_page_deserialise(iw_page_t *p, const uint8_t *data, size_t len);

#endif // IW_PAGE_H
