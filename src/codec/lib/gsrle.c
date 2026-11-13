// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gsrle.c
// Granny Smith's own run lengths, as checkpoint files store RAM and device
// state (see codec.h).  A stream is a sequence of chunks:
//
//   0x00 count:u32le <count bytes>   count literal bytes
//   0x01 count:u32le value:u8        value, count times
//
// The encoder (gsrle_enc.c) makes a run of every 4 or more equal bytes and
// a literal of everything between.  A stream has no end of its own: the container
// states the decoded size, the decoder stops there, and a chunk that would
// go past it is an error.  (Checkpoints before this library wrote the
// count in host order; every host they were written on is little-endian.)

#include "codec_internal.h"

#include <stdlib.h>

// ============================================================================
// Constants
// ============================================================================

#define GSRLE_LIT   0x00
#define GSRLE_RUN   0x01
#define GSRLE_BURST 1 // chunks are emitted a piece at a time

// ============================================================================
// The decoder
// ============================================================================

typedef struct {
    int kind; // the chunk in progress: GSRLE_LIT, GSRLE_RUN, or -1 between chunks
    uint32_t left; // its bytes still to emit
    uint8_t value; // a run's byte
} gsrle_t;

static int gsrle_fill(codec_dec_t *d) {
    gsrle_t *s = d->st;
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    while (codec_room_for(d, GSRLE_BURST)) {
        if (s->kind < 0) {
            // A chunk header: marker, count (and a run's value).
            uint8_t hdr[6];
            int m = codec_in_byte(in);
            if (m < 0)
                return CODEC_END; // short of the size: codec.c says so
            if (m != GSRLE_LIT && m != GSRLE_RUN)
                return codec_fail(d, CODEC_ERR_DATA, "gsrle: bad chunk marker");
            size_t need = m == GSRLE_RUN ? 5 : 4;
            if (codec_in_read(in, hdr, need) != need)
                return codec_fail(d, CODEC_ERR_TRUNCATED, "gsrle: truncated chunk");
            uint32_t count = codec_le32(hdr);
            if (count > codec_left(d))
                return codec_fail(d, CODEC_ERR_DATA, "gsrle: chunk past the decoded size");
            s->kind = m;
            s->left = count;
            s->value = hdr[4];
        }
        size_t room = ring_room(r);
        size_t k = s->left < room ? s->left : room;
        if (s->kind == GSRLE_RUN)
            ring_fill(r, s->value, k);
        else {
            // Literal bytes straight into the ring, up to its wrap.
            size_t done = 0;
            while (done < k) {
                size_t span = r->size - r->w;
                if (span > k - done)
                    span = k - done;
                size_t got = codec_in_read(in, r->buf + r->w, span);
                r->w += got;
                if (r->w == r->size)
                    r->w = 0;
                r->total += got;
                done += got;
                if (got < span) {
                    s->left -= (uint32_t)done;
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "gsrle: truncated literal");
                }
            }
        }
        s->left -= (uint32_t)k;
        if (s->left == 0)
            s->kind = -1;
    }
    return CODEC_MORE; // the ring full, or the size reached (codec.c ends the stream)
}

static int gsrle_init(codec_dec_t *d) {
    ((gsrle_t *)d->st)->kind = -1;
    return 0;
}

const codec_dec_ops_t codec_gsrle_dec = {
    .state_size = sizeof(gsrle_t), .burst = GSRLE_BURST, .sized = true, .init = gsrle_init, .fill = gsrle_fill};
