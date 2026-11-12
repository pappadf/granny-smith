// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// codec_producer.c
// Peeler's one adapter onto the codec library (src/codec, codec.h): a
// producer that decodes a fork through one codec, or a chain of them, and
// stops wherever the caller's buffer fills.  Every compressed fork peeler
// reads -- StuffIt's methods, Compact Pro's LZH and RLE, BinHex's RLE --
// decodes through this; peeler itself only parses containers.

#include "internal.h"

#include <stdlib.h>

#define PEEL_CODEC_MAX_STAGES 2

typedef struct {
    peel_producer_t base;
    codec_dec_t *dec[PEEL_CODEC_MAX_STAGES]; // dec[0] reads the packed bytes; the last is the fork
    int nstages;
    bool cut; // output past out_len is dropped, not an error
    char what[32];
} codec_prod_t;

// Report a decoder failure as "<what>: <the codec's message>", naming the
// failing stage (an inner stage's fault reaches the outer one as a pull
// error, and its own message says more).
static int codec_prod_fail(codec_prod_t *p, int rc) {
    const char *msg = "";
    for (int i = 0; i < p->nstages && !*msg; i++)
        msg = codec_dec_message(p->dec[i]);
    if (!*msg)
        msg = codec_strerror(rc);
    snprintf(p->base.err, sizeof(p->base.err), "%s%s%s", p->what, *p->what ? ": " : "", msg);
    return -5;
}

static int codec_prod_run(peel_producer_t *pp, uint8_t *out, size_t cap, size_t *n) {
    codec_prod_t *p = (codec_prod_t *)pp;
    int rc = codec_dec_run(p->dec[p->nstages - 1], out, cap, n);
    if (rc == CODEC_ERR_OVERFLOW && p->cut)
        return 1; // reported once all out_len bytes are out
    return rc < 0 ? codec_prod_fail(p, rc) : rc;
}

static void codec_prod_free(peel_producer_t *pp) {
    codec_prod_t *p = (codec_prod_t *)pp;
    for (int i = p->nstages - 1; i >= 0; i--)
        codec_dec_free(p->dec[i]);
    free(p);
}

peel_producer_t *peel_codec_producer(const codec_id_t *stages, int nstages, uint32_t flags, const uint8_t *src,
                                     size_t len, uint64_t out_len, bool cut, const char *what, peel_err_t **err) {
    if (nstages < 1 || nstages > PEEL_CODEC_MAX_STAGES) {
        *err = make_err("%s: bad codec chain", what ? what : "peeler");
        return NULL;
    }
    codec_prod_t *p = calloc(1, sizeof(*p));
    if (!p) {
        *err = make_err("%s: out of memory", what ? what : "peeler");
        return NULL;
    }
    p->base.run = codec_prod_run;
    p->base.free = codec_prod_free;
    p->cut = cut;
    snprintf(p->what, sizeof(p->what), "%s", what ? what : "");
    for (int i = 0; i < nstages; i++) {
        // An inner stage is limited only by its own stream; the last one
        // makes the fork, of exactly out_len bytes.
        codec_dec_opts_t o = {0};
        if (i == 0) {
            o.src = src;
            o.len = len;
        } else {
            o.pull = codec_dec_pull;
            o.pull_ctx = p->dec[i - 1];
        }
        o.out_len = i == nstages - 1 ? out_len : CODEC_LEN_UNKNOWN;
        o.flags = i == nstages - 1 ? flags : 0;
        int rc = 0;
        p->dec[i] = codec_dec_new(stages[i], &o, &rc);
        if (!p->dec[i]) {
            *err = make_err("%s%s%s: %s", p->what, *p->what ? ": " : "", codec_name(stages[i]), codec_strerror(rc));
            codec_prod_free(&p->base);
            return NULL;
        }
        p->nstages = i + 1;
    }
    return &p->base;
}
