// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// zmbv.c
// ZMBV decoder.  See codec_zmbv.h for the bitstream.
//
// Decodes what DOSBox's encoder (src/libs/zmbv/zmbv.cpp) and FFmpeg's
// decoder (libavcodec/zmbv.c) agree on at 8, 15, 16 and 32 bpp: keyframes,
// inter frames with any motion vectors in range, palette deltas, and the
// uncompressed variant (compression 0).  A keyframe restarts the zlib
// stream; each inter frame's data is the next sync-flushed chunk of it.
// The input is untrusted: every vector, delta and size is checked against
// the frame, and a frame that does not fit is an error.

#include "codec_internal.h"
#include "codec_zmbv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and types
// ============================================================================

#define ZMBV_COMPRESS_NONE 0
#define ZMBV_COMPRESS_ZLIB 1

// The decoder's state: the frame size, the stream's parameters from its
// last keyframe, the current and previous frames, and the zlib stream.
struct codec_zmbv_dec {
    int width, height;
    int format, bpp, bw, bh, bx, by, comp; // from the last keyframe (format 0: none yet)
    size_t row_bytes, frame_bytes;
    uint8_t *cur, *prev; // the frame being built, and the one before it
    uint8_t pal[CODEC_ZMBV_PALETTE];
    uint8_t *body; // one frame's decompressed data
    size_t body_cap;
    codec_zstream_dec_t *z;
    char msg[64];
};

// ============================================================================
// Static helpers
// ============================================================================

// Fail the frame with `rc` and a message.
static int zfail(codec_zmbv_dec_t *d, int rc, const char *msg) {
    snprintf(d->msg, sizeof(d->msg), "zmbv: %s", msg);
    return rc;
}

// Bytes per pixel of a format, or 0 for one not decoded here.
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

// The frame's data, decompressed into d->body: the bytes, or a code.
static int64_t frame_data(codec_zmbv_dec_t *d, const uint8_t *src, size_t len) {
    if (d->comp == ZMBV_COMPRESS_NONE) {
        if (len > d->body_cap)
            return zfail(d, CODEC_ERR_DATA, "frame larger than its picture");
        memcpy(d->body, src, len);
        return (int64_t)len;
    }
    int64_t n = codec_zstream_dec_chunk(d->z, src, len, d->body, d->body_cap);
    if (n < 0)
        zfail(d, (int)n, n == CODEC_ERR_OVERFLOW ? "frame larger than its picture" : codec_zstream_dec_message(d->z));
    return n;
}

// Apply an inter frame's block table and deltas (body[0..n)) to the
// previous frame, into the current one.  0 or CODEC_ERR_DATA.
static int apply_inter(codec_zmbv_dec_t *d, const uint8_t *body, size_t n) {
    size_t table = ((size_t)d->bx * (size_t)d->by * 2 + 3) & ~(size_t)3;
    if (n < table)
        return zfail(d, CODEC_ERR_DATA, "block table cut short");
    const uint8_t *mv = body, *delta = body + table, *end = body + n;
    for (int j = 0; j < d->by; j++) {
        int y0 = j * d->bh, h = d->bh < d->height - y0 ? d->bh : d->height - y0;
        for (int i = 0; i < d->bx; i++) {
            int x0 = i * d->bw, w = d->bw < d->width - x0 ? d->bw : d->width - x0;
            // dx and dy are signed 7-bit values above the "has delta" bit.
            int b = (j * d->bx + i) * 2;
            bool has_delta = mv[b] & 1;
            int dx = (int8_t)mv[b] >> 1, dy = (int8_t)mv[b + 1] >> 1;
            size_t seg = (size_t)w * d->bpp;
            for (int y = 0; y < h; y++) {
                uint8_t *dst = d->cur + (size_t)(y0 + y) * d->row_bytes + (size_t)x0 * d->bpp;
                int sy = y0 + y + dy;
                // The source row, pixel by pixel where the vector leaves the
                // frame (those pixels read as zero, as FFmpeg's decoder has it).
                if (dx == 0 && sy >= 0 && sy < d->height) {
                    memcpy(dst, d->prev + (size_t)sy * d->row_bytes + (size_t)x0 * d->bpp, seg);
                } else {
                    for (int x = 0; x < w; x++) {
                        int sx = x0 + x + dx;
                        uint8_t *px = dst + (size_t)x * d->bpp;
                        if (sx < 0 || sx >= d->width || sy < 0 || sy >= d->height)
                            memset(px, 0, (size_t)d->bpp);
                        else
                            memcpy(px, d->prev + (size_t)sy * d->row_bytes + (size_t)sx * d->bpp, (size_t)d->bpp);
                    }
                }
                if (has_delta) {
                    if ((size_t)(end - delta) < seg)
                        return zfail(d, CODEC_ERR_DATA, "block delta cut short");
                    for (size_t k = 0; k < seg; k++)
                        dst[k] ^= delta[k];
                    delta += seg;
                }
            }
        }
    }
    return 0;
}

// ============================================================================
// Lifecycle
// ============================================================================

codec_zmbv_dec_t *codec_zmbv_dec_new(int width, int height, int *err) {
    int e0 = 0;
    if (!err)
        err = &e0;
    *err = CODEC_ERR_ARG;
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768)
        return NULL;
    codec_zmbv_dec_t *d = calloc(1, sizeof(*d));
    if (!d) {
        *err = CODEC_ERR_NOMEM;
        return NULL;
    }
    d->width = width;
    d->height = height;
    // Room for the largest format: 4 bytes a pixel.
    size_t max_frame = (size_t)width * (size_t)height * 4;
    // A palette, a table for 1x1 blocks at worst, and every pixel's delta.
    d->body_cap = CODEC_ZMBV_PALETTE + (((size_t)width * (size_t)height * 2 + 3) & ~(size_t)3) + max_frame;
    d->cur = calloc(1, max_frame);
    d->prev = calloc(1, max_frame);
    d->body = malloc(d->body_cap);
    d->z = codec_zstream_dec_new(err);
    if (!d->cur || !d->prev || !d->body || !d->z) {
        codec_zmbv_dec_free(d);
        *err = CODEC_ERR_NOMEM;
        return NULL;
    }
    *err = 0;
    return d;
}

void codec_zmbv_dec_free(codec_zmbv_dec_t *d) {
    if (!d)
        return;
    codec_zstream_dec_free(d->z);
    free(d->cur);
    free(d->prev);
    free(d->body);
    free(d);
}

// ============================================================================
// Operations
// ============================================================================

int codec_zmbv_dec_frame(codec_zmbv_dec_t *d, const uint8_t *pkt, size_t len) {
    if (!d || (!pkt && len))
        return CODEC_ERR_ARG;
    d->msg[0] = '\0';
    // An empty packet: the picture is unchanged.
    if (len == 0)
        return d->format ? 0 : zfail(d, CODEC_ERR_DATA, "frame before the first keyframe");
    uint8_t flags = pkt[0];
    const uint8_t *src = pkt + 1;
    size_t left = len - 1;
    if (flags & CODEC_ZMBV_FLAG_KEY) {
        if (left < CODEC_ZMBV_HEADER_KEY - 1)
            return zfail(d, CODEC_ERR_DATA, "keyframe header cut short");
        int hi = src[0], lo = src[1], comp = src[2], format = src[3], bw = src[4], bh = src[5];
        src += CODEC_ZMBV_HEADER_KEY - 1;
        left -= CODEC_ZMBV_HEADER_KEY - 1;
        int bpp = format_bpp(format);
        if (hi != 0 || lo != 1)
            return zfail(d, CODEC_ERR_UNSUPPORTED, "version not 0.1");
        if (comp != ZMBV_COMPRESS_NONE && comp != ZMBV_COMPRESS_ZLIB)
            return zfail(d, CODEC_ERR_UNSUPPORTED, "unknown compression");
        if (!bpp)
            return zfail(d, CODEC_ERR_UNSUPPORTED, "pixel format not handled");
        if (bw == 0 || bh == 0)
            return zfail(d, CODEC_ERR_DATA, "zero block size");
        d->format = 0; // until the keyframe has decoded
        d->comp = comp;
        d->bpp = bpp;
        d->bw = bw;
        d->bh = bh;
        d->bx = (d->width + bw - 1) / bw;
        d->by = (d->height + bh - 1) / bh;
        d->row_bytes = (size_t)d->width * (size_t)bpp;
        d->frame_bytes = d->row_bytes * (size_t)d->height;
        codec_zstream_dec_reset(d->z);
        int64_t n = frame_data(d, src, left);
        if (n < 0)
            return (int)n;
        size_t pal = format == CODEC_ZMBV_FMT_8BPP ? CODEC_ZMBV_PALETTE : 0;
        if ((size_t)n != pal + d->frame_bytes)
            return zfail(d, CODEC_ERR_DATA, "keyframe is not the size of its picture");
        memcpy(d->pal, d->body, pal);
        memcpy(d->cur, d->body + pal, d->frame_bytes);
        d->format = format;
        return 0;
    }
    if (!d->format)
        return zfail(d, CODEC_ERR_DATA, "frame before the first keyframe");
    int64_t n = frame_data(d, src, left);
    if (n < 0) {
        d->format = 0; // the stream is lost until the next keyframe
        return (int)n;
    }
    const uint8_t *body = d->body;
    size_t blen = (size_t)n;
    if ((flags & CODEC_ZMBV_FLAG_DELTAPAL) && d->format == CODEC_ZMBV_FMT_8BPP) {
        if (blen < CODEC_ZMBV_PALETTE)
            return zfail(d, CODEC_ERR_DATA, "palette delta cut short");
        for (int i = 0; i < CODEC_ZMBV_PALETTE; i++)
            d->pal[i] ^= body[i];
        body += CODEC_ZMBV_PALETTE;
        blen -= CODEC_ZMBV_PALETTE;
    }
    // The new frame is built from the old: swap, then rebuild into cur.
    uint8_t *t = d->prev;
    d->prev = d->cur;
    d->cur = t;
    int rc = apply_inter(d, body, blen);
    if (rc < 0)
        d->format = 0;
    return rc;
}

int codec_zmbv_dec_format(const codec_zmbv_dec_t *d) {
    return d ? d->format : 0;
}

int codec_zmbv_dec_bytes_per_pixel(const codec_zmbv_dec_t *d) {
    return d && d->format ? d->bpp : 0;
}

const uint8_t *codec_zmbv_dec_pixels(const codec_zmbv_dec_t *d) {
    return d ? d->cur : NULL;
}

const uint8_t *codec_zmbv_dec_palette(const codec_zmbv_dec_t *d) {
    return d ? d->pal : NULL;
}

int codec_zmbv_dec_rgb24(const codec_zmbv_dec_t *d, uint8_t *out) {
    if (!d || !out || !d->format)
        return CODEC_ERR_DATA;
    size_t n = (size_t)d->width * (size_t)d->height;
    for (size_t i = 0; i < n; i++, out += 3) {
        const uint8_t *p = d->cur + i * (size_t)d->bpp;
        unsigned r, g, b;
        switch (d->format) {
        case CODEC_ZMBV_FMT_8BPP:
            r = d->pal[p[0] * 3];
            g = d->pal[p[0] * 3 + 1];
            b = d->pal[p[0] * 3 + 2];
            break;
        case CODEC_ZMBV_FMT_15BPP: {
            // x-5-5-5, each channel widened by repeating its top bits.
            unsigned v = codec_le16(p), r5 = (v >> 10) & 31, g5 = (v >> 5) & 31, b5 = v & 31;
            r = r5 << 3 | r5 >> 2;
            g = g5 << 3 | g5 >> 2;
            b = b5 << 3 | b5 >> 2;
            break;
        }
        case CODEC_ZMBV_FMT_16BPP: {
            unsigned v = codec_le16(p), r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;
            r = r5 << 3 | r5 >> 2;
            g = g6 << 2 | g6 >> 4;
            b = b5 << 3 | b5 >> 2;
            break;
        }
        default: // 32 bpp: B, G, R, unused
            r = p[2];
            g = p[1];
            b = p[0];
            break;
        }
        out[0] = (uint8_t)r;
        out[1] = (uint8_t)g;
        out[2] = (uint8_t)b;
    }
    return 0;
}

const char *codec_zmbv_dec_message(const codec_zmbv_dec_t *d) {
    return d ? d->msg : "no decoder";
}
