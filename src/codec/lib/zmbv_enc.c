// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// zmbv_enc.c
// ZMBV encoder.  See codec_zmbv.h for the bitstream (as DOSBox's
// src/libs/zmbv/zmbv.cpp writes it and FFmpeg's libavcodec/zmbv.c reads
// it).
//
// The encoder holds the previous frame and palette.  A keyframe resets the
// zlib stream and compresses the palette and the whole frame; an inter
// frame compresses the palette's XOR (when it changed), the block table
// and the XOR of every changed block, all with zero motion vectors, as the
// continuation of that stream.  Only the band of rows the caller says may
// have changed is looked at: blocks outside it are unchanged by the
// caller's word, and their table entries stay zero.

#include "codec_internal.h"
#include "codec_zmbv.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and types
// ============================================================================

#define ZMBV_VERSION_HI    0
#define ZMBV_VERSION_LO    1
#define ZMBV_COMPRESS_ZLIB 1

// The encoder's state: the stream's geometry, the previous frame and
// palette, the uncompressed body being built, and the zlib stream.
struct codec_zmbv_enc {
    int width, height, format, bpp; // bpp: bytes per pixel
    int bw, bh, bx, by; // block size, and blocks across and down
    size_t row_bytes, frame_bytes, table_bytes;
    uint8_t *prev; // the last frame coded
    uint8_t pal[CODEC_ZMBV_PALETTE]; // the last palette coded
    uint8_t *body; // the frame's data before compression
    size_t body_cap;
    codec_zstream_enc_t *z;
    bool key_due; // the next frame is a keyframe (the first, or forced)
};

// ============================================================================
// Static helpers
// ============================================================================

// Bytes per pixel of a format, or 0 for one not written here.
static int format_bpp(int format) {
    switch (format) {
    case CODEC_ZMBV_FMT_8BPP:
        return 1;
    case CODEC_ZMBV_FMT_15BPP:
    case CODEC_ZMBV_FMT_16BPP:
        return 2;
    case CODEC_ZMBV_FMT_32BPP:
        return 4;
    default:
        return 0;
    }
}

// True if any of `n` bytes at `p` is non-zero.
static bool any_set(const uint8_t *p, size_t n) {
    // Word at a time over the aligned middle: most rows of a delta are zero.
    size_t i = 0;
    for (; i < n && ((uintptr_t)(p + i) & 7); i++)
        if (p[i])
            return true;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        if (v)
            return true;
    }
    for (; i < n; i++)
        if (p[i])
            return true;
    return false;
}

// Fold the band's rows into a copy of the previous frame at `frame`:
// copied, or XORed in.
static void apply_band(const codec_zmbv_enc_t *e, const codec_zmbv_frame_t *f, size_t stride, uint8_t *frame) {
    for (int y = f->row_first; y < f->row_end; y++) {
        const uint8_t *src = f->rows + (size_t)(y - f->row_first) * stride;
        uint8_t *dst = frame + (size_t)y * e->row_bytes;
        if (f->is_xor) {
            for (size_t i = 0; i < e->row_bytes; i++)
                dst[i] ^= src[i];
        } else {
            memcpy(dst, src, e->row_bytes);
        }
    }
}

// The XOR of one block's row segment (`n` bytes from byte `x0` of row `y`)
// against the previous frame, into `out`; zeros outside the band.
static void block_row_delta(const codec_zmbv_enc_t *e, const codec_zmbv_frame_t *f, size_t stride, int y, size_t x0,
                            size_t n, uint8_t *out) {
    if (y < f->row_first || y >= f->row_end) {
        memset(out, 0, n);
        return;
    }
    const uint8_t *src = f->rows + (size_t)(y - f->row_first) * stride + x0;
    if (f->is_xor) {
        memcpy(out, src, n);
        return;
    }
    const uint8_t *old = e->prev + (size_t)y * e->row_bytes + x0;
    for (size_t i = 0; i < n; i++)
        out[i] = src[i] ^ old[i];
}

// True if the block's rows inside the band differ from the previous frame.
static bool block_changed(const codec_zmbv_enc_t *e, const codec_zmbv_frame_t *f, size_t stride, int y0, int y1,
                          size_t x0, size_t n) {
    int a = y0 > f->row_first ? y0 : f->row_first, b = y1 < f->row_end ? y1 : f->row_end;
    for (int y = a; y < b; y++) {
        const uint8_t *src = f->rows + (size_t)(y - f->row_first) * stride + x0;
        if (f->is_xor) {
            if (any_set(src, n))
                return true;
        } else if (memcmp(src, e->prev + (size_t)y * e->row_bytes + x0, n) != 0) {
            return true;
        }
    }
    return false;
}

// The inter frame's body: the palette XOR, the block table and the deltas.
// Returns its length; *changed tells whether anything is in it.
static size_t inter_body(codec_zmbv_enc_t *e, const codec_zmbv_frame_t *f, size_t stride, bool pal_delta,
                         bool *changed) {
    uint8_t *p = e->body;
    if (pal_delta) {
        // The palette as the XOR of the old against the new.
        for (int i = 0; i < CODEC_ZMBV_PALETTE; i++)
            p[i] = e->pal[i] ^ f->palette[i];
        p += CODEC_ZMBV_PALETTE;
    }
    uint8_t *table = p;
    memset(table, 0, e->table_bytes); // zero vectors, no deltas, and the padding
    p += e->table_bytes;
    *changed = pal_delta;
    if (f->row_first >= f->row_end)
        return (size_t)(p - e->body);
    for (int j = 0; j < e->by; j++) {
        int y0 = j * e->bh, y1 = y0 + e->bh < e->height ? y0 + e->bh : e->height;
        // A block row outside the band is unchanged by the caller's word.
        if (y1 <= f->row_first || y0 >= f->row_end)
            continue;
        for (int i = 0; i < e->bx; i++) {
            int x0 = i * e->bw, x1 = x0 + e->bw < e->width ? x0 + e->bw : e->width;
            size_t bx0 = (size_t)x0 * e->bpp, n = (size_t)(x1 - x0) * e->bpp;
            if (!block_changed(e, f, stride, y0, y1, bx0, n))
                continue;
            table[(j * e->bx + i) * 2] = 1; // dx 0, "has delta"
            for (int y = y0; y < y1; y++, p += n)
                block_row_delta(e, f, stride, y, bx0, n, p);
            *changed = true;
        }
    }
    return (size_t)(p - e->body);
}

// ============================================================================
// Lifecycle
// ============================================================================

codec_zmbv_enc_t *codec_zmbv_enc_new(int width, int height, int format, int block_w, int block_h, int level, int *err) {
    int e0 = 0;
    if (!err)
        err = &e0;
    *err = CODEC_ERR_ARG;
    int bpp = format_bpp(format);
    if (block_w == 0)
        block_w = CODEC_ZMBV_BLOCK;
    if (block_h == 0)
        block_h = CODEC_ZMBV_BLOCK;
    if (!bpp || width <= 0 || height <= 0 || width > 32768 || height > 32768 || block_w < 1 || block_w > 255 ||
        block_h < 1 || block_h > 255)
        return NULL;
    codec_zmbv_enc_t *e = calloc(1, sizeof(*e));
    if (!e) {
        *err = CODEC_ERR_NOMEM;
        return NULL;
    }
    e->width = width;
    e->height = height;
    e->format = format;
    e->bpp = bpp;
    e->bw = block_w;
    e->bh = block_h;
    e->bx = (width + block_w - 1) / block_w;
    e->by = (height + block_h - 1) / block_h;
    e->row_bytes = (size_t)width * (size_t)bpp;
    e->frame_bytes = e->row_bytes * (size_t)height;
    // Two bytes per block, padded to a multiple of four (as the decoders skip it).
    e->table_bytes = ((size_t)e->bx * (size_t)e->by * 2 + 3) & ~(size_t)3;
    // The largest body: a palette and either the whole frame (a keyframe)
    // or the table and every block's delta (no more than the frame).
    e->body_cap = CODEC_ZMBV_PALETTE + e->table_bytes + e->frame_bytes;
    e->prev = calloc(1, e->frame_bytes);
    e->body = malloc(e->body_cap);
    e->z = codec_zstream_enc_new(e->body_cap, level, err);
    if (!e->prev || !e->body || !e->z) {
        codec_zmbv_enc_free(e);
        *err = CODEC_ERR_NOMEM;
        return NULL;
    }
    e->key_due = true;
    *err = 0;
    return e;
}

void codec_zmbv_enc_free(codec_zmbv_enc_t *e) {
    if (!e)
        return;
    codec_zstream_enc_free(e->z);
    free(e->prev);
    free(e->body);
    free(e);
}

// ============================================================================
// Operations
// ============================================================================

void codec_zmbv_enc_set_level(codec_zmbv_enc_t *e, int level) {
    if (e)
        codec_zstream_enc_set_level(e->z, level);
}

void codec_zmbv_enc_force_keyframe(codec_zmbv_enc_t *e) {
    if (e)
        e->key_due = true;
}

size_t codec_zmbv_enc_bound(const codec_zmbv_enc_t *e) {
    return e ? CODEC_ZMBV_HEADER_KEY + codec_zstream_enc_bound(e->body_cap) : 0;
}

const uint8_t *codec_zmbv_enc_pixels(const codec_zmbv_enc_t *e) {
    return e ? e->prev : NULL;
}

int64_t codec_zmbv_enc_frame(codec_zmbv_enc_t *e, const codec_zmbv_frame_t *f, uint8_t *out, size_t cap,
                             codec_zmbv_kind_t *kind) {
    if (!e || !f || !out || f->row_first < 0 || f->row_end > e->height || f->row_first > f->row_end ||
        (f->row_first < f->row_end && !f->rows))
        return CODEC_ERR_ARG;
    size_t stride = f->stride ? f->stride : e->row_bytes;
    if (stride < e->row_bytes)
        return CODEC_ERR_ARG;
    bool key = e->key_due || f->keyframe;
    bool is8 = e->format == CODEC_ZMBV_FMT_8BPP;
    bool pal_new = is8 && f->palette && memcmp(f->palette, e->pal, CODEC_ZMBV_PALETTE) != 0;
    size_t head, body_len;
    bool changed = true;
    if (key) {
        if (cap < CODEC_ZMBV_HEADER_KEY)
            return CODEC_ERR_OVERFLOW;
        out[0] = CODEC_ZMBV_FLAG_KEY;
        out[1] = ZMBV_VERSION_HI;
        out[2] = ZMBV_VERSION_LO;
        out[3] = ZMBV_COMPRESS_ZLIB;
        out[4] = (uint8_t)e->format;
        out[5] = (uint8_t)e->bw;
        out[6] = (uint8_t)e->bh;
        head = CODEC_ZMBV_HEADER_KEY;
        // The body is the new palette and frame, built beside the previous
        // ones so that a frame that does not fit changes nothing.
        uint8_t *p = e->body;
        if (is8) {
            memcpy(p, pal_new ? f->palette : e->pal, CODEC_ZMBV_PALETTE);
            p += CODEC_ZMBV_PALETTE;
        }
        memcpy(p, e->prev, e->frame_bytes);
        apply_band(e, f, stride, p);
        body_len = (size_t)(p - e->body) + e->frame_bytes;
        // A keyframe opens a new zlib stream.
        codec_zstream_enc_reset(e->z);
    } else {
        if (cap < 1)
            return CODEC_ERR_OVERFLOW;
        // Deltas against the previous frame, which is updated after.
        body_len = inter_body(e, f, stride, pal_new, &changed);
        out[0] = pal_new ? CODEC_ZMBV_FLAG_DELTAPAL : 0;
        head = 1;
    }
    int64_t n = codec_zstream_enc_chunk(e->z, e->body, body_len, out + head, cap - head);
    if (n < 0) {
        // Nothing is kept of a frame that did not fit, so the caller may
        // retry it; a keyframe's reset stream stays due for a keyframe.
        if (key)
            e->key_due = true;
        return n;
    }
    // The frame is out: it becomes the previous one.
    if (key)
        memcpy(e->prev, e->body + (is8 ? CODEC_ZMBV_PALETTE : 0), e->frame_bytes);
    else
        apply_band(e, f, stride, e->prev);
    if (pal_new)
        memcpy(e->pal, f->palette, CODEC_ZMBV_PALETTE);
    e->key_due = false;
    if (kind)
        *kind = key ? CODEC_ZMBV_KEYFRAME : changed ? CODEC_ZMBV_INTER : CODEC_ZMBV_UNCHANGED;
    return (int64_t)head + n;
}
