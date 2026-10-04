// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iw_page.c
// Dot planes of one ImageWriter sheet: see iw_page.h.

#include "iw_page.h"

#include "deflate.h"
#include "inflate.h"

#include <stdlib.h>
#include <string.h>

// Points per inch, the PDF's unit
#define PT_PER_IN 72.0

// The plane that holds band `bit` (an IW_INK_* value) on this sheet.
static int plane_of(const iw_page_t *p, unsigned bit) {
    if (p->nplanes == 1)
        return 0; // a black-only printer has one plane, whatever the mask
    for (int i = 0; i < 4; i++)
        if (bit == (1u << i))
            return i;
    return 3;
}

void iw_page_setup(iw_page_t *p, double width_in, double height_in, unsigned dpi, unsigned nplanes,
                   unsigned dot_shape) {
    iw_page_clear(p);
    p->width_in = width_in;
    p->height_in = height_in;
    p->dpi = (uint16_t)dpi;
    p->nplanes = (uint8_t)(nplanes == 4 ? 4 : 1);
    p->dot_shape = (uint8_t)dot_shape;
    p->w = (uint32_t)(width_in * dpi + 0.5);
    p->h = (uint32_t)(height_in * dpi + 0.5);
    p->stride = (p->w + 7) / 8;
    // The wire is 1/72 in across
    p->dot_px = (uint8_t)(dpi / 72 ? dpi / 72 : 1);
}

void iw_page_clear(iw_page_t *p) {
    for (int i = 0; i < 4; i++) {
        free(p->plane[i]);
        p->plane[i] = NULL;
    }
    p->inks = 0;
    p->dots = 0;
}

// Allocate plane `i` if it is not there yet.
static bool ensure_plane(iw_page_t *p, int i) {
    if (p->plane[i])
        return true;
    p->plane[i] = calloc((size_t)p->stride * p->h, 1);
    return p->plane[i] != NULL;
}

// Whether pixel (dx, dy) of a dot `d` pixels across is inside the disc.
static bool in_disc(unsigned dx, unsigned dy, unsigned d) {
    // Centre at d/2, radius d/2: compare doubled coordinates to stay integral
    int cx = 2 * (int)dx + 1 - (int)d;
    int cy = 2 * (int)dy + 1 - (int)d;
    return cx * cx + cy * cy <= (int)(d * d);
}

bool iw_page_dot(iw_page_t *p, int64_t x, int64_t y, uint8_t mask) {
    if (!mask || p->w == 0)
        return true;
    unsigned d = p->dot_px;
    // Wholly off the sheet: nothing to stamp
    if (x + d <= 0 || y + d <= 0 || x >= (int64_t)p->w || y >= (int64_t)p->h)
        return true;
    for (unsigned bit = IW_INK_Y; bit <= IW_INK_K; bit <<= 1) {
        if (!(mask & bit))
            continue;
        int pi = plane_of(p, bit);
        if (!ensure_plane(p, pi))
            return false;
        uint8_t *plane = p->plane[pi];
        for (unsigned dy = 0; dy < d; dy++) {
            int64_t py = y + dy;
            if (py < 0 || py >= (int64_t)p->h)
                continue;
            uint8_t *row = plane + (size_t)py * p->stride;
            for (unsigned dx = 0; dx < d; dx++) {
                int64_t px = x + dx;
                if (px < 0 || px >= (int64_t)p->w)
                    continue;
                if (p->dot_shape == IW_DOT_DISC && d > 2 && !in_disc(dx, dy, d))
                    continue;
                row[px >> 3] |= (uint8_t)(0x80u >> (px & 7));
            }
        }
        p->inks |= (uint8_t)(p->nplanes == 1 ? IW_INK_K : bit);
    }
    p->dots++;
    return true;
}

void iw_page_palette(const uint8_t inks[4][3], uint8_t palette[16][3]) {
    for (int i = 0; i < 16; i++) {
        if (i & IW_INK_K) {
            // Black over anything stays black
            memcpy(palette[i], inks[3], 3);
            continue;
        }
        // Multiply the transmittances of the inks present
        unsigned rgb[3] = {255, 255, 255};
        for (int b = 0; b < 3; b++) {
            if (!(i & (1 << b)))
                continue;
            for (int c = 0; c < 3; c++)
                rgb[c] = rgb[c] * inks[b][c] / 255;
        }
        for (int c = 0; c < 3; c++)
            palette[i][c] = (uint8_t)rgb[c];
    }
}

bool iw_page_emit(const iw_page_t *p, pdf_writer_t *w, const uint8_t palette[16][3]) {
    double wpt = p->width_in * PT_PER_IN, hpt = p->height_in * PT_PER_IN;
    // Black only (or blank): a 1-bit stencil
    if ((p->inks & ~IW_INK_K) == 0) {
        int pi = p->nplanes == 1 ? 0 : 3;
        if (p->plane[pi])
            return pdf_writer_add_mono_page(w, wpt, hpt, p->w, p->h, p->plane[pi], p->stride);
        uint8_t *blank = calloc((size_t)p->stride * p->h + 1, 1);
        if (!blank)
            return false;
        bool ok = pdf_writer_add_mono_page(w, wpt, hpt, p->w, p->h, blank, p->stride);
        free(blank);
        return ok;
    }
    // Colour: each pixel's index is the set of bands that struck it
    size_t row = (p->w + 1) / 2;
    uint8_t *nib = malloc(row * p->h + 1);
    if (!nib)
        return false;
    for (uint32_t y = 0; y < p->h; y++) {
        uint8_t *out = nib + (size_t)y * row;
        memset(out, 0, row);
        for (int b = 0; b < 4; b++) {
            const uint8_t *src = p->plane[b];
            if (!src)
                continue;
            src += (size_t)y * p->stride;
            for (uint32_t x = 0; x < p->w; x++) {
                if (src[x >> 3] & (0x80u >> (x & 7)))
                    out[x >> 1] |= (uint8_t)((1u << b) << ((x & 1) ? 0 : 4));
            }
        }
    }
    bool ok = pdf_writer_add_indexed4_page(w, wpt, hpt, p->w, p->h, nib, row, palette);
    free(nib);
    return ok;
}

// Append `n` bytes to a growing buffer.
static bool put(uint8_t **buf, size_t *len, size_t *cap, const void *data, size_t n) {
    if (*len + n > *cap) {
        size_t c = *cap ? *cap : 256;
        while (c < *len + n)
            c *= 2;
        uint8_t *b = realloc(*buf, c);
        if (!b)
            return false;
        *buf = b;
        *cap = c;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return true;
}

// Big-endian 32-bit store / load
static void be32(uint8_t out[4], uint32_t v) {
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool iw_page_serialise(const iw_page_t *p, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!iw_page_marked(p))
        return true;
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    // Header: inks, dot count
    uint8_t hdr[5];
    hdr[0] = p->inks;
    be32(hdr + 1, p->dots);
    bool ok = put(&buf, &len, &cap, hdr, sizeof(hdr));
    size_t plane_len = (size_t)p->stride * p->h;
    uint8_t *z = malloc(deflate_bound(plane_len));
    ok = ok && z;
    // Each allocated plane: index, compressed length, zlib stream
    for (int i = 0; ok && i < 4; i++) {
        if (!p->plane[i])
            continue;
        long zl = deflate_zlib(NULL, p->plane[i], plane_len, z, deflate_bound(plane_len), 6);
        if (zl < 0) {
            ok = false;
            break;
        }
        uint8_t ph[5];
        ph[0] = (uint8_t)i;
        be32(ph + 1, (uint32_t)zl);
        ok = put(&buf, &len, &cap, ph, sizeof(ph)) && put(&buf, &len, &cap, z, (size_t)zl);
    }
    free(z);
    if (!ok) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_len = len;
    return true;
}

bool iw_page_deserialise(iw_page_t *p, const uint8_t *data, size_t len) {
    iw_page_clear(p);
    if (len == 0)
        return true;
    if (len < 5)
        return false;
    uint8_t inks = data[0];
    uint32_t dots = rd32(data + 1);
    size_t off = 5;
    size_t plane_len = (size_t)p->stride * p->h;
    while (off + 5 <= len) {
        int i = data[off];
        uint32_t zl = rd32(data + off + 1);
        off += 5;
        if (i >= 4 || zl > len - off || !ensure_plane(p, i)) {
            iw_page_clear(p);
            return false;
        }
        long got = inflate_zlib(data + off, zl, p->plane[i], plane_len);
        if (got != (long)plane_len) {
            iw_page_clear(p);
            return false;
        }
        off += zl;
    }
    p->inks = inks;
    p->dots = dots;
    return off == len;
}
