// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// StuffIt's methods 3, 13 and 15, tested against encoders built on the
// decoders' own tables and models (moved here from the peeler suite with
// the decoders themselves).  This file #includes sit13.c, for its meta-code
// tables, and sit_arsenic.c, so that its arithmetic *encoder* drives the
// decoder's own model code (model_setup / model_bump): encoder and decoder
// agree by construction.  The Makefile leaves both out of EXTRA_SRCS, or
// their external symbols would be defined twice.  Every input is built here
// rather than committed as a blob, so each fixture reads as the defect it
// provokes; a memory-safety test must fail on the unfixed code.

#include "../../../../src/codec/lib/sit13.c"
#include "../../../../src/codec/lib/sit_arsenic.c"

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void stuffit_tests(void);

// A sized stream decoded whole: exactly `n` bytes (data malloc'd), or
// data NULL and *err the CODEC_ERR_* code.
typedef struct {
    uint8_t *data;
    size_t size;
} dec_buf_t;

static dec_buf_t decode_sized(codec_id_t id, const uint8_t *src, size_t len, size_t n, int64_t *err) {
    dec_buf_t b = {malloc(n ? n : 1), 0};
    ASSERT_TRUE(b.data != NULL);
    int64_t rc = codec_decode(id, src, len, b.data, n);
    *err = rc == (int64_t)n ? 0 : rc < 0 ? rc : CODEC_ERR_TRUNCATED;
    if (*err) {
        free(b.data);
        b.data = NULL;
    } else
        b.size = n;
    return b;
}

// ============================================================================
// An Arsenic (StuffIt method 15) arithmetic *encoder*
// ============================================================================
//
// The inverse of sit_arsenic.c's ac_decode_sym, driving the decoder's own
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

// A zero run of length n, as the selector tokens that encode it: bijective
// base 2, least significant digit first, digit d written as token d - 1.
static void enc_zero_run(ac_enc *e, arsenic_models *mm, unsigned n) {
    ASSERT_TRUE(n > 0);
    while (n > 0) {
        unsigned d = (n % 2 == 0) ? 2 : 1;
        enc_sym(e, &mm->sel, (int)d - 1);
        n = (n - d) / 2;
    }
}

// A one-block stream whose block is a single zero run of length n.
static uint8_t *make_zero_run_stream(unsigned n, size_t *len) {
    ac_enc e;
    arsenic_models mm;
    enc_init(&e);
    enc_header(&e, &mm, 0);
    enc_block_header(&e, &mm, 0);
    enc_zero_run(&e, &mm, n);
    enc_sym(&e, &mm.sel, 10);
    enc_block_footer_eos(&e, &mm);
    return enc_finish(&e, len);
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

    int64_t err = 0;
    dec_buf_t out = decode_sized(CODEC_SIT_ARSENIC, in, len, 1, &err);
    if (err)
        fprintf(stderr, "  sit15: %s\n", codec_strerror(err));
    ASSERT_TRUE(err == 0);
    ASSERT_EQ_INT(1, (int)out.size);
    ASSERT_EQ_INT(0x01, out.data[0]);
    free(out.data);
    free(in);
}

// consume_zero_run accumulated (tok + 1) << bit_pos into a plain int with no
// bound on bit_pos.  Thirty-two zero-run tokens of value 0 sum to 2^0 + ... +
// 2^30 = 2^31 - 1, and the 32nd adds 1 << 31 -- signed overflow, in practice
// INT_MIN -- leaving the total at exactly -1.  The caller's
// `blk_len + run_len > blk_cap` check is false for a negative length, and
// memset(buf, fill, (size_t)-1) follows.  The input must now be rejected.
TEST(test_sit15_zero_run_cannot_overflow) {
    ac_enc e;
    arsenic_models mm;
    enc_init(&e);
    enc_header(&e, &mm, 0);
    enc_block_header(&e, &mm, 0);
    for (int i = 0; i < 32; i++)
        enc_sym(&e, &mm.sel, 0);
    enc_sym(&e, &mm.sel, 10);
    enc_block_footer_eos(&e, &mm);
    size_t len;
    uint8_t *in = enc_finish(&e, &len);

    int64_t err = 0;
    dec_buf_t out = decode_sized(CODEC_SIT_ARSENIC, in, len, 1, &err);
    ASSERT_TRUE(err != 0);
    ASSERT_TRUE(out.data == NULL);
    free(in);
}

// The zero-run bound is exact: a legitimate run may fill the block (512 bytes
// at block exponent 0) and must decode; one byte more must not.  Only 4 output
// bytes are requested: the whole block is decoded before the first byte is
// emitted, so that is enough to exercise the bound, and the final RLE stage (4
// identical bytes then a count) means 512 upstream zeros do not yield 512
// output bytes anyway.  Guards an off-by-one in the bound itself -- `>=`
// instead of `>` fails the first half.
TEST(test_sit15_zero_run_bound_is_exact) {
    size_t len;
    uint8_t *in = make_zero_run_stream(512, &len);
    int64_t err = 0;
    dec_buf_t out = decode_sized(CODEC_SIT_ARSENIC, in, len, 4, &err);
    if (err)
        fprintf(stderr, "  sit15: %s\n", codec_strerror(err));
    ASSERT_TRUE(err == 0);
    ASSERT_EQ_INT(4, (int)out.size);
    for (size_t i = 0; i < out.size; i++)
        ASSERT_EQ_INT(0, out.data[i]); // MTF index 0 on a fresh table is 0x00
    free(out.data);
    free(in);

    in = make_zero_run_stream(513, &len);
    out = decode_sized(CODEC_SIT_ARSENIC, in, len, 4, &err);
    ASSERT_TRUE(err != 0);
    free(in);
}

// ============================================================================
// sit13 (StuffIt method 13) -- a bit writer over the decoder's own meta code
// ============================================================================
//
// The stream is read LSB-first (lsb_get), and a Huffman code is walked
// from its most significant bit down (codec_htree_insert), so a code goes out
// MSB-first, one bit at a time, into an LSB-first stream.

typedef struct {
    uint8_t buf[4096];
    size_t nbits;
} m13_writer;

static void m13w_bits(m13_writer *w, uint32_t v, int n) { // LSB-first field
    ASSERT_TRUE(n >= 0 && n <= 32);
    for (int i = 0; i < n; i++) {
        ASSERT_TRUE(w->nbits < sizeof(w->buf) * 8);
        if ((v >> i) & 1)
            w->buf[w->nbits / 8] |= (uint8_t)(1u << (w->nbits % 8));
        w->nbits++;
    }
}

static void m13w_meta(m13_writer *w, int sym) { // one meta-code symbol
    for (int b = m13_meta_lens[sym] - 1; b >= 0; b--)
        m13w_bits(w, (m13_meta_words[sym] >> b) & 1, 1);
}

// Dynamic-mode header byte: SET = 0, shared second tree, K = 0.
static void m13w_dynamic_header(m13_writer *w) {
    m13w_bits(w, 0x08, 8);
}

// A length of -1 is legal: a decrement from the reset value 0 gives it, and
// m13_build_canonical counts it as absent, like 0.  Real DropStuff 6 streams
// open their distance-tree list with exactly that -- the fix for the -2 case
// (below) first refused it, and the real corpus caught it.  Here the distance
// list is a decrement then nine repeats of -1, and the stream must still
// decode.
TEST(test_sit13_minus_one_is_an_absent_symbol) {
    m13_writer w = {0};
    m13w_dynamic_header(&w);
    m13w_meta(&w, 31); // first tree as in the round trip: 'A' = 0, 'B' = 1
    m13w_meta(&w, 36);
    m13w_bits(&w, 53, 6);
    m13w_meta(&w, 0);
    m13w_meta(&w, 34);
    m13w_bits(&w, 0, 1);
    m13w_meta(&w, 31);
    for (int i = 0; i < 3; i++) {
        m13w_meta(&w, 36);
        m13w_bits(&w, 63, 6);
    }
    m13w_meta(&w, 36);
    m13w_bits(&w, 20, 6);
    m13w_meta(&w, 33); // distance list: L = -1 ...
    m13w_meta(&w, 35);
    m13w_bits(&w, 6, 3); // ... and nine more -1       -> 10
    m13w_bits(&w, 0, 1); // "A"
    m13w_bits(&w, 0, 32);

    int64_t err = 0;
    dec_buf_t out = decode_sized(CODEC_SIT13, w.buf, (w.nbits + 7) / 8, 1, &err);
    if (err)
        fprintf(stderr, "  sit13: %s\n", codec_strerror(err));
    ASSERT_TRUE(err == 0);
    ASSERT_EQ_INT('A', out.data[0]);
    free(out.data);
}

// A length list that decrements to -2.  m13_build_canonical assigns codes by
// walking lengths upward from -1; it never meets a -2, so it never finishes
// (natively the shift in its loop is UB long before that).  Two decrements emit
// -1 then -2; four long repeats and one of 23 fill the remaining 319 entries
// exactly, so the list itself is well-formed in size.
TEST(test_sit13_negative_length_is_rejected) {
    m13_writer w = {0};
    m13w_dynamic_header(&w);
    m13w_meta(&w, 33); // L = -1
    m13w_meta(&w, 33); // L = -2
    for (int i = 0; i < 4; i++) {
        m13w_meta(&w, 36);
        m13w_bits(&w, 63, 6);
    }
    m13w_meta(&w, 36);
    m13w_bits(&w, 12, 6); // 2 + 296 + 23 = 321
    m13w_bits(&w, 0, 32);
    m13w_bits(&w, 0, 32);

    int64_t err = 0;
    dec_buf_t out = decode_sized(CODEC_SIT13, w.buf, (w.nbits + 7) / 8, 16, &err);
    ASSERT_TRUE(err != 0);
    ASSERT_TRUE(out.data == NULL);
}

// A real dynamic-mode stream, so the length decoder is proven on legitimate
// input and not only on hostile input: a two-symbol literal code, 'A' = 0 and
// 'B' = 1, every other symbol absent, the second tree shared, a 10-symbol
// distance tree that is never walked.  The lists use each command kind the
// fix touched -- set (0, 31), conditional repeat (34), short and long repeat
// (35, 36) -- and each must land exactly on its list's end.
TEST(test_sit13_dynamic_round_trip) {
    m13_writer w = {0};
    m13w_dynamic_header(&w); // shared second tree, K = 0: 10 distance symbols
    // First tree, 321 entries: 0 x 65, then 1 for 'A' (65) and 'B' (66), then 0 x 254.
    m13w_meta(&w, 31); //   1 zero                               -> 1
    m13w_meta(&w, 36);
    m13w_bits(&w, 53, 6); // 64 zeros                              -> 65
    m13w_meta(&w, 0); // L = 1 for 'A'                          -> 66
    m13w_meta(&w, 34);
    m13w_bits(&w, 0, 1); // one more L = 1 for 'B'                  -> 67
    m13w_meta(&w, 31); // L = 0                                   -> 68
    for (int i = 0; i < 3; i++) {
        m13w_meta(&w, 36);
        m13w_bits(&w, 63, 6); // 3 x 74 zeros                      -> 290
    }
    m13w_meta(&w, 36);
    m13w_bits(&w, 20, 6); // 31 zeros                             -> 321
    // Distance tree, 10 entries, all absent.
    m13w_meta(&w, 31); //  1                                       -> 1
    m13w_meta(&w, 35);
    m13w_bits(&w, 6, 3); // 9                                      -> 10
    // "ABBA"
    m13w_bits(&w, 0, 1);
    m13w_bits(&w, 1, 1);
    m13w_bits(&w, 1, 1);
    m13w_bits(&w, 0, 1);
    m13w_bits(&w, 0, 32);

    int64_t err = 0;
    dec_buf_t out = decode_sized(CODEC_SIT13, w.buf, (w.nbits + 7) / 8, 4, &err);
    if (err)
        fprintf(stderr, "  sit13: %s\n", codec_strerror(err));
    ASSERT_TRUE(err == 0);
    ASSERT_EQ_INT(4, (int)out.size);
    ASSERT_TRUE(memcmp(out.data, "ABBA", 4) == 0);
    free(out.data);
}

// m13_decode_lengths bounded its loop by nsym but not the repeat commands
// inside it.  Command 36 emits r + 10 entries plus one more, r a 6-bit field --
// up to 74 -- so from index 320 of the 321-entry lengths array it writes 73
// bytes past the end of a stack array.  Four full repeats and one of 24 reach
// index 320 exactly; a fifth full one overruns.
TEST(test_sit13_length_repeat_cannot_overrun) {
    m13_writer w = {0};
    m13w_dynamic_header(&w);
    for (int i = 0; i < 4; i++) { // 4 x 74 = 296
        m13w_meta(&w, 36);
        m13w_bits(&w, 63, 6);
    }
    m13w_meta(&w, 36); // + 24 = 320
    m13w_bits(&w, 13, 6);
    m13w_meta(&w, 36); // + 74 from 320: 73 past the end
    m13w_bits(&w, 63, 6);
    m13w_bits(&w, 0, 32); // padding
    m13w_bits(&w, 0, 32);

    int64_t err = 0;
    dec_buf_t out = decode_sized(CODEC_SIT13, w.buf, (w.nbits + 7) / 8, 16, &err);
    ASSERT_TRUE(err != 0);
    ASSERT_TRUE(out.data == NULL);
}

// sit3 allocated its output, then aborted on a truncated tree -- one set bit
// starts a leaf, whose 8-bit symbol is not there -- and leaked it.
TEST(test_sit3_abort_frees_its_output) {
    static const uint8_t one_byte[] = {0xFF};
    int64_t err = 0;
    dec_buf_t out = decode_sized(CODEC_SIT_HUFFMAN, one_byte, 1, 64, &err);
    ASSERT_TRUE(err != 0);
    ASSERT_TRUE(out.data == NULL);
}

// ---- Characterisation: a method the corpus does not reach ------------------
//
// No archive in the real corpus uses StuffIt method 3 (measured), so this
// pins its behaviour down: a stream built bit by bit, including the edges --
// a tree read across byte boundaries, running out of input, a single leaf.

// An MSB-first bit writer.
typedef struct {
    uint8_t buf[512];
    size_t bit;
} msb_writer;
static void msb_put(msb_writer *w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--, w->bit++)
        if ((v >> i) & 1)
            w->buf[w->bit / 8] |= (uint8_t)(0x80u >> (w->bit % 8));
}

// Method 3: a three-leaf tree -- 'a' = 0, 'b' = 10, 'c' = 11 -- serialised
// pre-order (0 = internal, 1 + 8 bits = leaf), then the message.  The tree
// alone is 29 bits, so everything after it straddles byte boundaries.
TEST(test_sit3_decodes_a_tree_and_message) {
    static msb_writer w;
    memset(&w, 0, sizeof(w));
    msb_put(&w, 0, 1); // root: internal
    msb_put(&w, 1, 1); // zero child: leaf 'a'
    msb_put(&w, 'a', 8);
    msb_put(&w, 0, 1); // one child: internal
    msb_put(&w, 1, 1); //   leaf 'b'
    msb_put(&w, 'b', 8);
    msb_put(&w, 1, 1); //   leaf 'c'
    msb_put(&w, 'c', 8);
    static const char msg[] = "abcacbbaccab";
    for (const char *p = msg; *p; p++) {
        if (*p == 'a')
            msb_put(&w, 0, 1);
        if (*p == 'b')
            msb_put(&w, 2, 2);
        if (*p == 'c')
            msb_put(&w, 3, 2);
    }
    size_t len = (w.bit + 7) / 8;
    size_t n = sizeof(msg) - 1;
    int64_t err = 0;
    dec_buf_t out = decode_sized(CODEC_SIT_HUFFMAN, w.buf, len, n, &err);
    ASSERT_TRUE(err == 0);
    ASSERT_EQ_INT((int)n, (int)out.size);
    ASSERT_TRUE(memcmp(out.data, msg, n) == 0);
    free(out.data);

    // One symbol more than the stream holds: the reader runs out, and
    // method 3 aborts rather than inventing bits.
    out = decode_sized(CODEC_SIT_HUFFMAN, w.buf, len, n + 8, &err);
    ASSERT_TRUE(err != 0 && out.data == NULL);

    // A single-leaf tree codes its symbol as no bits at all.
    memset(&w, 0, sizeof(w));
    msb_put(&w, 1, 1);
    msb_put(&w, 'z', 8);
    err = 0;
    out = decode_sized(CODEC_SIT_HUFFMAN, w.buf, (w.bit + 7) / 8, 5, &err);
    ASSERT_TRUE(err == 0 && out.size == 5 && memcmp(out.data, "zzzzz", 5) == 0);
    free(out.data);
}

// ---- The shared canonical-Huffman pool --------------------------------------

// Walk `bits` (MSB first) from `root`: the symbol reached, or -1.
static int huff_walk(const codec_htree_t *p, int root, const char *bits) {
    int node = root;
    for (; *bits && node >= 0; bits++)
        node = codec_htree_child(p, node, *bits == '1');
    return node < 0 ? -1 : codec_htree_sym(p, node);
}

// Canonical assignment: ascending length, then ascending symbol.  Lengths
// {2, 1, 2}: symbol 1 is "0", symbol 0 "10", symbol 2 "11".  An absent
// symbol (0) gets no code.
TEST(test_huff_canonical_codes) {
    static codec_htree_t pool;
    codec_htree_reset(&pool);
    const int8_t lens[4] = {2, 1, 2, 0};
    int root = codec_htree_build(&pool, lens, 4, 1, 15);
    ASSERT_TRUE(root >= 0);
    ASSERT_EQ_INT(1, huff_walk(&pool, root, "0"));
    ASSERT_EQ_INT(0, huff_walk(&pool, root, "10"));
    ASSERT_EQ_INT(2, huff_walk(&pool, root, "11"));
    // A length outside the format's range is refused, not skipped.
    const int8_t bad[2] = {1, 16};
    ASSERT_EQ_INT(-1, codec_htree_build(&pool, bad, 2, 1, 15));
}

// The pool is bounded: trees that do not fit are refused.  sit13 kept its four
// trees in a 2048-node pool it never bounds-checked; it now shares this one.
// Under ASan an overrun here is a failure.
TEST(test_huff_pool_is_bounded) {
    static codec_htree_t pool;
    codec_htree_reset(&pool);
    // 256 symbols at length 8: a full tree, 511 nodes.  The fifth does not fit.
    static int8_t lens[256];
    memset(lens, 8, sizeof(lens));
    int built = 0;
    while (codec_htree_build(&pool, lens, 256, 1, 15) >= 0)
        built++;
    ASSERT_EQ_INT(4, built);
    ASSERT_TRUE(pool.used <= CODEC_HTREE_CAP);
}

// Drive a decoder one byte at a time, then in odd sizes, and compare with
// what a full decode gives.
static void drive_by_bytes(codec_dec_t *d, const uint8_t *want, size_t n) {
    uint8_t *got = malloc(n ? n : 1);
    size_t at = 0;
    int rc = 0;
    static const size_t caps[] = {1, 1, 1, 2, 3, 5, 7, 13, 100, 1};
    for (int k = 0; rc == 0; k++) {
        size_t step = caps[k % 10], cap = n - at < step ? n - at : step, m = 0;
        rc = codec_dec_run(d, got + at, cap ? cap : 1, &m);
        ASSERT_TRUE(rc >= 0);
        ASSERT_TRUE(m <= cap);
        at += m;
    }
    ASSERT_EQ_INT((int)n, (int)at);
    ASSERT_TRUE(memcmp(got, want, n) == 0);
    codec_dec_free(d);
    free(got);
}

// Method 13 stops wherever the buffer fills and resumes there.
TEST(test_sit13_resumes_byte_by_byte) {
    m13_writer w = {0};
    m13w_dynamic_header(&w); // the "ABBA" trees of test_sit13_dynamic_round_trip
    m13w_meta(&w, 31);
    m13w_meta(&w, 36);
    m13w_bits(&w, 53, 6);
    m13w_meta(&w, 0);
    m13w_meta(&w, 34);
    m13w_bits(&w, 0, 1);
    m13w_meta(&w, 31);
    for (int i = 0; i < 3; i++) {
        m13w_meta(&w, 36);
        m13w_bits(&w, 63, 6);
    }
    m13w_meta(&w, 36);
    m13w_bits(&w, 20, 6);
    m13w_meta(&w, 31);
    m13w_meta(&w, 35);
    m13w_bits(&w, 6, 3);
    size_t n = 20000;
    uint8_t *want = malloc(n);
    for (size_t i = 0; i < n; i++) {
        int bit = (int)((i * 2654435761u) >> 13) & 1;
        want[i] = bit ? 'B' : 'A';
        m13w_bits(&w, (uint32_t)bit, 1);
    }
    m13w_bits(&w, 0, 32);
    int64_t err = 0;
    dec_buf_t whole = decode_sized(CODEC_SIT13, w.buf, (w.nbits + 7) / 8, n, &err);
    ASSERT_TRUE(err == 0 && whole.size == n);
    ASSERT_TRUE(memcmp(whole.data, want, n) == 0);
    free(whole.data);
    codec_dec_opts_t o = {.src = w.buf, .len = (w.nbits + 7) / 8, .out_len = n};
    codec_dec_t *d = codec_dec_new(CODEC_SIT13, &o, NULL);
    ASSERT_TRUE(d != NULL);
    drive_by_bytes(d, want, n);
    free(want);
}

void stuffit_tests(void) {
    RUN(test_sit15_encoder_round_trip);
    RUN(test_sit15_zero_run_cannot_overflow);
    RUN(test_sit15_zero_run_bound_is_exact);
    RUN(test_sit13_dynamic_round_trip);
    RUN(test_sit13_length_repeat_cannot_overrun);
    RUN(test_sit13_minus_one_is_an_absent_symbol);
    RUN(test_sit13_negative_length_is_rejected);
    RUN(test_sit13_resumes_byte_by_byte);
    RUN(test_sit3_abort_frees_its_output);
    RUN(test_sit3_decodes_a_tree_and_message);
    RUN(test_huff_canonical_codes);
    RUN(test_huff_pool_is_bounded);
}
