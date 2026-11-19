// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gsrle_enc.c
// The checkpoint file's run lengths, encoding (see gsrle.c for the format):
// a run of every 4 or more equal bytes, a literal of everything between.

#include "codec_internal.h"

// ============================================================================
// Constants
// ============================================================================

#define GSRLE_LIT     0x00
#define GSRLE_RUN     0x01
#define GSRLE_MIN_RUN 4 // shorter runs stay in a literal
#define GSRLE_MAX_CNT 0xFFFFFFFFu

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
