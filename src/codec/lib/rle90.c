// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rle90.c
// 0x90-escape run lengths: StuffIt's method 1, and BinHex's expansion
// (see codec.h).  Format notes: src/peeler/docs/sit.md § 8, hqx.md § 5.
//
// Every byte is itself, except 0x90, which escapes the byte after it:
//   0x90 0x00   a literal 0x90
//   0x90 N      the previous byte N - 1 more times (N >= 1)
// The two formats differ in two corners.  After an escaped 0x90, StuffIt
// repeats the byte before it, BinHex the 0x90 itself; and a count of 1 (no
// more copies) is StuffIt's no-op but an error in BinHex
// (CODEC_RLE90_BINHEX).  The stream ends with its input; the container
// states the decoded size (out_len, a limit).

#include "codec_internal.h"

#include <stdlib.h>

#define RLE90_MARKER 0x90
#define RLE90_BURST  255 // the most one escape writes

typedef struct {
    uint8_t prev; // the byte a run repeats
} rle90_t;

static int rle90_fill(codec_dec_t *d) {
    rle90_t *s = d->st;
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    bool binhex = d->flags & CODEC_RLE90_BINHEX;
    while (ring_room(r) >= RLE90_BURST) {
        int b = codec_in_byte(in);
        if (b < 0)
            return CODEC_END;
        if (b != RLE90_MARKER) {
            ring_put(r, (uint8_t)b);
            s->prev = (uint8_t)b;
            continue;
        }
        int n = codec_in_byte(in);
        if (n < 0)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "rle90: the stream ends inside an escape");
        if (n == 0) {
            ring_put(r, RLE90_MARKER);
            if (binhex)
                s->prev = RLE90_MARKER;
        } else if (n == 1 && binhex) {
            return codec_fail(d, CODEC_ERR_DATA, "rle90: a count of 1 (BinHex)");
        } else {
            ring_fill(r, s->prev, (size_t)n - 1);
        }
    }
    return CODEC_MORE;
}

static int rle90_init(codec_dec_t *d) {
    d->st = calloc(1, sizeof(rle90_t)); // prev starts at 0
    if (!d->st)
        return CODEC_ERR_NOMEM;
    return codec_ring_alloc(d, 0, RLE90_BURST);
}

static void rle90_free(codec_dec_t *d) {
    free(d->st);
    d->st = NULL;
}

const codec_dec_ops_t codec_rle90_dec = {rle90_init, rle90_fill, rle90_free, false};
