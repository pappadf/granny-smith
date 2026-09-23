// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The peeler archive library, tested as an untrusted parser
// (09-WORK-ORDER.md Track A).
//
// Every input here is built by the test rather than committed as a blob, so
// each fixture reads as the defect it provokes.  The rule for a memory-safety
// test is that it must fail on the unfixed code -- a crash counts -- and each
// was verified to by reverting its fix alone.

#include "formats/sit15.c" // for the Arsenic encoder below; see Makefile

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// An Arsenic (StuffIt method 15) arithmetic *encoder*
// ============================================================================
//
// The inverse of sit15.c's ac_decode_sym, driving the decoder's own
// prob_model/model_setup/model_bump, so the two cannot drift apart.
//
// The coded value V is a binary fraction.  The decoder reads its first
// AC_PREC (26) bits as `code`, but its range is AC_ONE (2^25), so a valid
// stream's first bit is always 0; the encoder emits that bit up front and
// then V's expansion.  `low` is the 25-bit window of V not yet emitted; an
// addition that carries past the window ripples into bits already emitted.

typedef struct {
    uint8_t *bits; // one bit per byte, MSB-first stream order
    size_t n, cap;
    uint64_t low;
    int range;
} ac_enc;

static void enc_bit(ac_enc *e, int b) {
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 256;
        e->bits = realloc(e->bits, e->cap);
        ASSERT_TRUE(e->bits != NULL);
    }
    e->bits[e->n++] = (uint8_t)b;
}

static void enc_init(ac_enc *e) {
    memset(e, 0, sizeof(*e));
    e->range = AC_ONE;
    enc_bit(e, 0);
}

static void enc_carry(ac_enc *e) {
    size_t i = e->n;
    while (i > 0 && e->bits[i - 1] == 1)
        e->bits[--i] = 0;
    ASSERT_TRUE(i > 0); // V < 1, so a carry never escapes the stream
    e->bits[i - 1] = 1;
}

static void enc_sym(ac_enc *e, prob_model *m, int sym) {
    int k = sym - m->base_sym;
    ASSERT_TRUE(k >= 0 && k < m->nsyms);
    int cum = 0;
    for (int i = 0; i < k; i++)
        cum += m->freq[i];
    int hi = cum + m->freq[k];
    int scale = e->range / m->total;
    ASSERT_TRUE(scale > 0);

    e->low += (uint64_t)scale * (uint64_t)cum;
    if (e->low >= (uint64_t)AC_ONE) {
        enc_carry(e);
        e->low -= (uint64_t)AC_ONE;
    }
    if (hi == m->total)
        e->range -= scale * cum;
    else
        e->range = m->freq[k] * scale;

    while (e->range <= AC_HALF) {
        enc_bit(e, (int)((e->low >> (AC_PREC - 2)) & 1));
        e->low = (e->low << 1) & ((uint64_t)AC_ONE - 1);
        e->range <<= 1;
    }
    model_bump(m, k);
}

static void enc_field(ac_enc *e, prob_model *m, int n, int value) {
    for (int i = 0; i < n; i++)
        enc_sym(e, m, (value >> i) & 1); // LSB first, as ac_decode_field
}

// Flush: V = the emitted bits followed by `low` exactly, which lies inside the
// final interval; then zero padding so the decoder's renormalisation reads
// never run dry.  Returns a malloc'd byte stream.
static uint8_t *enc_finish(ac_enc *e, size_t *out_len) {
    for (int i = AC_PREC - 2; i >= 0; i--)
        enc_bit(e, (int)((e->low >> i) & 1));
    for (int i = 0; i < 256; i++)
        enc_bit(e, 0);
    size_t nbytes = (e->n + 7) / 8;
    uint8_t *out = calloc(nbytes, 1);
    ASSERT_TRUE(out != NULL);
    for (size_t i = 0; i < e->n; i++)
        if (e->bits[i])
            out[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    free(e->bits);
    e->bits = NULL;
    *out_len = nbytes;
    return out;
}

// The models a stream is decoded against, set up exactly as the decoder does.
typedef struct {
    prob_model primary, sel, grp[7];
} arsenic_models;

static void models_for_header(arsenic_models *mm) {
    model_setup(&mm->primary, 0, 1, 1, 256);
}

static void models_for_block(arsenic_models *mm) {
    model_setup(&mm->sel, 0, 10, 8, 1024);
    for (int g = 0; g < 7; g++)
        model_setup(&mm->grp[g], grp_lo[g], grp_hi[g], grp_step[g], 1024);
}

// Header: signature "As", block exponent B, initial eos = 0.
static void enc_header(ac_enc *e, arsenic_models *mm, int block_exp) {
    models_for_header(mm);
    enc_field(e, &mm->primary, 8, 'A');
    enc_field(e, &mm->primary, 8, 's');
    enc_field(e, &mm->primary, 4, block_exp);
    enc_sym(e, &mm->primary, 0);
}

// Block header: not randomised, BWT origin 0.
static void enc_block_header(ac_enc *e, arsenic_models *mm, int block_exp) {
    models_for_block(mm);
    enc_sym(e, &mm->primary, 0);
    enc_field(e, &mm->primary, block_exp + 9, 0);
}

// Block footer: end of stream, then the 32-bit CRC field (value unchecked).
static void enc_block_footer_eos(ac_enc *e, arsenic_models *mm) {
    enc_sym(e, &mm->primary, 1);
    enc_field(e, &mm->primary, 32, 0);
}

// ============================================================================
// sit15
// ============================================================================

// The encoder is right, or nothing built on it means anything: a block holding
// one literal must decode to that literal.  Selector 2 is MTF index 1, which
// on a fresh table is the byte 0x01.
TEST(test_sit15_encoder_round_trip) {
    ac_enc e;
    arsenic_models mm;
    enc_init(&e);
    enc_header(&e, &mm, 0);
    enc_block_header(&e, &mm, 0);
    enc_sym(&e, &mm.sel, 2);
    enc_sym(&e, &mm.sel, 10);
    enc_block_footer_eos(&e, &mm);
    size_t len;
    uint8_t *in = enc_finish(&e, &len);

    peel_err_t *err = NULL;
    peel_buf_t out = peel_sit15(in, len, 1, &err);
    if (err)
        fprintf(stderr, "  sit15: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(1, (int)out.size);
    ASSERT_EQ_INT(0x01, out.data[0]);
    peel_free(&out);
    free(in);
}

// ============================================================================
// Garbage in, error out
// ============================================================================

// peel()'s contract for input it does not recognise is passthrough: one
// unnamed file carrying the bytes unchanged, and no error (peeler.c
// peel_depth, "fall through to single-file wrap").  The third pattern opens
// with a StuffIt signature but is too short to be one, so it must pass
// through too rather than half-parse.
TEST(test_peel_passes_unrecognised_input_through) {
    static const uint8_t patterns[][8] = {
        {0},
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        {'S', 'I', 'T', '!', 0, 0, 0, 0},
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        peel_err_t *err = NULL;
        peel_file_list_t list = peel(patterns[i], sizeof(patterns[i]), &err);
        ASSERT_TRUE(err == NULL);
        ASSERT_EQ_INT(1, list.count);
        ASSERT_EQ_INT((int)sizeof(patterns[i]), (int)list.files[0].data_fork.size);
        ASSERT_TRUE(memcmp(list.files[0].data_fork.data, patterns[i], sizeof(patterns[i])) == 0);
        peel_file_list_free(&list);
    }
}

int main(void) {
    RUN(test_sit15_encoder_round_trip);
    RUN(test_peel_passes_unrecognised_input_through);
    fprintf(stderr, "All peeler tests passed\n");
    return 0;
}
