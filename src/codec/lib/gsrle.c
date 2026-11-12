// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gsrle.c
// Granny Smith's own run lengths, as checkpoint files store RAM and device
// state (see codec.h).  A stream is a sequence of chunks:
//
//   0x00 count:u32le <count bytes>   count literal bytes
//   0x01 count:u32le value:u8        value, count times
//
// The encoder makes a run of every 4 or more equal bytes and a literal of
// everything between.  A stream has no end of its own: the container
// states the decoded size, the decoder stops there, and a chunk that would
// go past it is an error.  (Checkpoints before this library wrote the
// count in host order; every host they were written on is little-endian.)

#include "codec_internal.h"

#include <stdlib.h>

// ============================================================================
// Constants
// ============================================================================

#define GSRLE_LIT     0x00
#define GSRLE_RUN     0x01
#define GSRLE_MIN_RUN 4 // shorter runs stay in a literal
#define GSRLE_MAX_CNT 0xFFFFFFFFu
#define GSRLE_BURST   1 // chunks are emitted a piece at a time

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
    while (codec_left(d) > 0) {
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
        if (room < GSRLE_BURST)
            return CODEC_MORE;
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
    return CODEC_END;
}

static int gsrle_init(codec_dec_t *d) {
    gsrle_t *s = calloc(1, sizeof(*s));
    if (!s)
        return CODEC_ERR_NOMEM;
    s->kind = -1;
    d->st = s;
    return codec_ring_alloc(d, 0, GSRLE_BURST);
}

static void gsrle_free(codec_dec_t *d) {
    free(d->st);
    d->st = NULL;
}

const codec_dec_ops_t codec_gsrle_dec = {gsrle_init, gsrle_fill, gsrle_free, true};

// ============================================================================
// The encoder
// ============================================================================

// The worst case is a 1-byte literal and a 4-byte run, repeated: 12 bytes
// for every 5; then a final literal's header.
static size_t gsrle_bound(size_t len) {
    size_t groups = len / 5 + 1;
    if (groups > (SIZE_MAX - len - 5) / 7)
        return SIZE_MAX;
    return len + groups * 7 + 5;
}

static int gsrle_enc_init(codec_enc_t *e, size_t max_len) {
    (void)max_len;
    e->st = NULL; // no state
    return 0;
}

static void gsrle_enc_free(codec_enc_t *e) {
    (void)e;
}

// The length of the run of equal bytes at in[i] (at most GSRLE_MAX_CNT).
static size_t run_at(const uint8_t *in, size_t i, size_t len) {
    size_t end = len - i > GSRLE_MAX_CNT ? i + GSRLE_MAX_CNT : len, j = i + 1;
    while (j < end && in[j] == in[i])
        j++;
    return j - i;
}

static int64_t gsrle_enc_run(codec_enc_t *e, const uint8_t *in, size_t len, uint8_t *out, size_t cap) {
    (void)e;
    size_t op = 0, i = 0;
    while (i < len) {
        size_t run = run_at(in, i, len);
        if (run >= GSRLE_MIN_RUN) {
            if (cap - op < 6)
                return CODEC_ERR_OVERFLOW;
            out[op] = GSRLE_RUN;
            codec_wr_le32(out + op + 1, (uint32_t)run);
            out[op + 5] = in[i];
            op += 6;
            i += run;
            continue;
        }
        // A literal, up to the next run of GSRLE_MIN_RUN.
        size_t ls = i;
        while (i < len && i - ls < GSRLE_MAX_CNT) {
            if (len - i >= GSRLE_MIN_RUN && in[i] == in[i + 1] && in[i] == in[i + 2] && in[i] == in[i + 3])
                break;
            i++;
        }
        size_t n = i - ls;
        if (cap - op < 5 || cap - op - 5 < n)
            return CODEC_ERR_OVERFLOW;
        out[op] = GSRLE_LIT;
        codec_wr_le32(out + op + 1, (uint32_t)n);
        memcpy(out + op + 5, in + ls, n);
        op += 5 + n;
    }
    return (int64_t)op;
}

const codec_enc_ops_t codec_gsrle_enc = {gsrle_enc_init, gsrle_enc_run, gsrle_enc_free, gsrle_bound, 0};
