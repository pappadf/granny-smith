// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fork_decoder.c
// A codec decoder (codec.h) over a range of an archive's source, pulled as
// the decoder reads: how peeler hands every compressed fork to the codec
// library.  The decoder owns the range (and its reference on the source).

#include "internal.h"

#include <stdlib.h>

typedef struct {
    peel_source_t *src; // retained
    uint64_t pos, end; // the packed bytes still to pull
} fork_range_t;

static int64_t range_pull(void *ctx, uint8_t *buf, size_t cap) {
    fork_range_t *r = ctx;
    uint64_t left = r->end - r->pos;
    size_t n = left < cap ? (size_t)left : cap;
    if (n == 0)
        return 0;
    int rc = peel_source_read_exact(r->src, r->pos, buf, n);
    if (rc != 0)
        return rc;
    r->pos += n;
    return (int64_t)n;
}

static void range_close(void *ctx) {
    fork_range_t *r = ctx;
    peel_source_release(r->src);
    free(r);
}

codec_dec_t *peel_fork_decoder(peel_source_t *src, uint64_t off, uint64_t len, codec_id_t id,
                               const codec_dec_opts_t *opts, const char *what, peel_err_t **err) {
    fork_range_t *r = calloc(1, sizeof(*r));
    if (!r) {
        *err = make_err("%s: out of memory", what ? what : "peeler");
        return NULL;
    }
    r->src = peel_source_retain(src);
    r->pos = off;
    r->end = len > UINT64_MAX - off ? UINT64_MAX : off + len;
    codec_dec_opts_t o = *opts;
    o.in.src = NULL;
    o.in.len = 0;
    o.in.pull = range_pull;
    o.in.ctx = r;
    o.in.close = range_close;
    int rc = 0;
    codec_dec_t *d = codec_dec_new(id, &o, &rc);
    if (!d) {
        range_close(r); // still ours: a failed codec_dec_new closes nothing
        *err = make_err("%s%s%s: %s", what ? what : "", what ? ": " : "", codec_name(id), codec_strerror(rc));
    }
    return d;
}
