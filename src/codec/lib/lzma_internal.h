// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lzma_internal.h
// The LZMA model the decoder (lzma.c) and the encoder (lzma_enc.c) share:
// the constants, the probability tables, the state and repeat distances,
// and the xz container constants.  Private to the codec library.

#ifndef CODEC_LZMA_INTERNAL_H
#define CODEC_LZMA_INTERNAL_H

#include "codec_internal.h"

#define LZMA_STATES        12 // the literal/match history machine
#define LZMA_POS_STATES    16 // 1 << pb at most
#define LZMA_PROB_INIT     1024 // half of 1 << 11
#define LZMA_PROB_BITS     11 // probabilities are 11-bit fractions
#define LZMA_MOVE_BITS     5 // adaptation speed
#define LZMA_TOP           (1u << 24) // the range is renormalised below this
#define LZMA_MATCH_MIN     2 // shortest match
#define LZMA_END_POS_MODEL 14 // distance slots below this use modelled extra bits
#define LZMA_FULL_DIST     128 // distances below this need no direct bits
#define LZMA_ALIGN_BITS    4 // low distance bits with their own model
#define LZMA_LIT_STATES    7 // states below this were last a literal

#define LZMA2_MAX_LCLP 4 // LZMA2 limits lc + lp

typedef uint16_t lzma_prob_t;

// A length coder: 0..7 (low, per position state), 8..15 (mid, per position
// state) or 16..271 (high), plus LZMA_MATCH_MIN.
typedef struct {
    lzma_prob_t choice, choice2;
    lzma_prob_t low[LZMA_POS_STATES][1 << 3];
    lzma_prob_t mid[LZMA_POS_STATES][1 << 3];
    lzma_prob_t high[1 << 8];
} lzma_len_t;

typedef struct {
    unsigned lc, lp, pb; // literal context bits, literal position bits, position bits
    lzma_prob_t *lit; // 0x300 probabilities per literal context
    size_t lit_n; // probabilities allocated in `lit`
    lzma_prob_t is_match[LZMA_STATES][LZMA_POS_STATES];
    lzma_prob_t is_rep[LZMA_STATES];
    lzma_prob_t is_rep_g0[LZMA_STATES];
    lzma_prob_t is_rep_g1[LZMA_STATES];
    lzma_prob_t is_rep_g2[LZMA_STATES];
    lzma_prob_t is_rep0_long[LZMA_STATES][LZMA_POS_STATES];
    lzma_prob_t dist_slot[4][1 << 6]; // by length: 2, 3, 4, 5+
    lzma_prob_t dist_special[LZMA_FULL_DIST - LZMA_END_POS_MODEL + 1];
    lzma_prob_t dist_align[1 << LZMA_ALIGN_BITS];
    lzma_len_t match_len, rep_len;
    unsigned state;
    uint32_t rep[4];

    uint8_t *dst; // the decoder's output (and dictionary); the encoder's input
    size_t cap, pos; // its size and the bytes in it (decoded or encoded so far)
    size_t dict_start; // the position of the last dictionary reset
} lzma_t;

// Decode properties byte `d` (lc + 9 * (lp + 5 * pb)) into the model,
// allocating its literal coders; lc + lp may not exceed `max_lclp`.
int codec_lzma_set_props(lzma_t *z, unsigned d, unsigned max_lclp);

// Reset every probability, the state and the repeat distances.
void codec_lzma_reset_state(lzma_t *z);

#define XZ_HEADER_SIZE  12 // stream header and footer
#define XZ_FILTER_LZMA2 0x21
extern const uint8_t codec_xz_magic[6]; // the stream header's first six bytes

#endif // CODEC_LZMA_INTERNAL_H
