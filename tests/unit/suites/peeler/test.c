// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The peeler archive library, tested as an untrusted parser
// (09-WORK-ORDER.md Track A).
//
// Every input here is built by the test rather than committed as a blob, so
// each fixture reads as the defect it provokes.  The rule for a memory-safety
// test is that it must fail on the unfixed code -- a crash counts -- and each
// was verified to by reverting its fix alone.

#include "formats/sit13.c" // for its meta-code tables; see Makefile
#include "formats/sit15.c" // for the Arsenic encoder below; see Makefile

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Big-endian writers for the archive builders below.
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

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

// F-01: consume_zero_run accumulated (tok + 1) << bit_pos into a plain int with
// no bound on bit_pos.  Thirty-two zero-run tokens of value 0 sum to
// 2^0 + ... + 2^30 = 2^31 - 1, and the 32nd adds 1 << 31 -- signed overflow,
// in practice INT_MIN -- leaving the total at exactly -1.  The caller's
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

    peel_err_t *err = NULL;
    peel_buf_t out = peel_sit15(in, len, 1, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(out.data == NULL);
    peel_err_free(err);
    free(in);
}

// The F-01 bound is exact: a legitimate run may fill the block (512 bytes at
// block exponent 0) and must decode; one byte more must not.  Only 4 output
// bytes are requested: the whole block is decoded before the first byte is
// emitted, so that is enough to exercise the bound, and the final RLE stage
// (4 identical bytes then a count) means 512 upstream zeros do not yield 512
// output bytes anyway.  Guards an off-by-one in the bound itself -- `>=`
// instead of `>` fails the first half.
TEST(test_sit15_zero_run_bound_is_exact) {
    size_t len;
    uint8_t *in = make_zero_run_stream(512, &len);
    peel_err_t *err = NULL;
    peel_buf_t out = peel_sit15(in, len, 4, &err);
    if (err)
        fprintf(stderr, "  sit15: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(4, (int)out.size);
    for (size_t i = 0; i < out.size; i++)
        ASSERT_EQ_INT(0, out.data[i]); // MTF index 0 on a fresh table is 0x00
    peel_free(&out);
    free(in);

    in = make_zero_run_stream(513, &len);
    out = peel_sit15(in, len, 4, &err);
    ASSERT_TRUE(err != NULL);
    peel_err_free(err);
    free(in);
}

// ============================================================================
// sit13 (StuffIt method 13) -- a bit writer over the decoder's own meta code
// ============================================================================
//
// The stream is read LSB-first (m13_br_read), and a Huffman code is walked
// from its most significant bit down (peel_huff_insert), so a code goes out
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
// open their distance-tree list with exactly that -- the fix for F-05 first
// refused it, and the real corpus caught it.  Here the distance list is a
// decrement then nine repeats of -1, and the stream must still decode.
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

    peel_err_t *err = NULL;
    peel_buf_t out = peel_sit13(w.buf, (w.nbits + 7) / 8, 1, &err);
    if (err)
        fprintf(stderr, "  sit13: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT('A', out.data[0]);
    peel_free(&out);
}

// F-05: a length list that decrements to -2.  m13_build_canonical assigns
// codes by walking lengths upward from -1; it never meets a -2, so it never
// finishes (natively the shift in its loop is UB long before that).  Two
// decrements emit -1 then -2; four long repeats and one of 23 fill the
// remaining 319 entries exactly, so the list itself is well-formed in size.
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

    peel_err_t *err = NULL;
    peel_buf_t out = peel_sit13(w.buf, (w.nbits + 7) / 8, 16, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(out.data == NULL);
    peel_err_free(err);
}

// ============================================================================
// Compact Pro
// ============================================================================

// A Compact Pro archive whose directory is a chain of `depth` nested folders,
// each holding the rest (cpt.md §3.2): magic 01 01, the directory offset,
// then at that offset a 4-byte CRC (not checked), the entry count and an
// empty comment.  A folder entry is its name length with the folder bit set
// -- here a zero-length name -- and a 2-byte count of the entries below it.
static uint8_t *make_cpt_nested(unsigned depth, size_t *out_len) {
    size_t len = 8 + 7 + (size_t)depth * 3;
    uint8_t *a = calloc(len, 1);
    ASSERT_TRUE(a != NULL);
    a[0] = 0x01;
    a[1] = 0x01;
    put32(a + 4, 8);
    put16(a + 8 + 4, (uint16_t)depth);
    uint8_t *e = a + 8 + 7;
    for (unsigned i = 0; i < depth; i++, e += 3) {
        e[0] = 0x80; // folder, empty name
        put16(e + 1, (uint16_t)(depth - 1 - i)); // everything below it
    }
    *out_len = len;
    return a;
}

// A Compact Pro archive holding one file entry with the given fork lengths and
// no fork bytes at all: magic, directory offset, directory header, then the
// entry -- its name, and the 45 metadata bytes of cpt.md §3.2.3.
static uint8_t *make_cpt_file(uint32_t file_offset, uint32_t rsrc_comp, uint32_t data_comp, size_t *out_len) {
    const char *name = "F";
    size_t len = 8 + 7 + 1 + 1 + 45 + 64;
    uint8_t *a = calloc(len, 1);
    ASSERT_TRUE(a != NULL);
    a[0] = 0x01;
    a[1] = 0x01;
    put32(a + 4, 8);
    put16(a + 8 + 4, 1); // one entry
    uint8_t *e = a + 8 + 7;
    e[0] = 1; // name length, not a folder
    e[1] = (uint8_t)name[0];
    uint8_t *m = e + 2;
    put32(m + 1, file_offset);
    put32(m + 29, 16); // rsrc_uncomp
    put32(m + 33, 0); // data_uncomp
    put32(m + 37, rsrc_comp); // rsrc_comp
    put32(m + 41, data_comp); // data_comp
    *out_len = len;
    return a;
}

// A Compact Pro archive with one file whose data fork is `fork` (fork_len
// bytes, declared to decode to data_uncomp), flagged LZH or plain RLE.
static uint8_t *make_cpt_data_fork(const uint8_t *fork, uint32_t fork_len, uint32_t data_uncomp, bool lzh,
                                   size_t *out_len) {
    const char *name = "F";
    size_t dir_off = 8 + fork_len; // forks first, directory after
    size_t len = dir_off + 7 + 1 + 1 + 45 + 16;
    uint8_t *a = calloc(len, 1);
    ASSERT_TRUE(a != NULL);
    a[0] = 0x01;
    a[1] = 0x01;
    put32(a + 4, (uint32_t)dir_off);
    memcpy(a + 8, fork, fork_len);
    put16(a + dir_off + 4, 1); // one entry
    uint8_t *e = a + dir_off + 7;
    e[0] = 1;
    e[1] = (uint8_t)name[0];
    uint8_t *m = e + 2;
    put32(m + 1, 8); // file_offset: the forks
    put16(m + 27, lzh ? 0x0004 : 0); // flags: data fork LZH
    put32(m + 33, data_uncomp); // data_uncomp
    put32(m + 41, fork_len); // data_comp (rsrc_comp = 0)
    *out_len = len;
    return a;
}

// An MSB-first bit writer, as cp_bits reads (bytes enter the accumulator's
// high end), for Compact Pro LZH streams.
typedef struct {
    uint8_t buf[4096];
    size_t nbits;
} cpt_writer;

static void cptw_bits(cpt_writer *w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        ASSERT_TRUE(w->nbits < sizeof(w->buf) * 8);
        if ((v >> i) & 1)
            w->buf[w->nbits / 8] |= (uint8_t)(0x80 >> (w->nbits % 8));
        w->nbits++;
    }
}

// One LZH block's three tables (cpt.md §6.4.1: a byte count, then nibble-
// packed code lengths, high nibble first): literals 'A' and 'B' at length 1
// (codes 0 and 1), match lengths 2 and 3 at length 1, offset symbols 0 and 1
// at length 1.  Every code is one bit.
static void cptw_tables(cpt_writer *w) {
    cptw_bits(w, 34, 8); // literal lengths for symbols 0..67
    for (int i = 0; i < 34; i++)
        cptw_bits(w, i == 32 ? 0x01 : i == 33 ? 0x10 : 0x00, 8); // 65 = 'A', 66 = 'B'
    cptw_bits(w, 2, 8); // match lengths 0..3
    cptw_bits(w, 0x00, 8);
    cptw_bits(w, 0x11, 8); // 2 and 3
    cptw_bits(w, 1, 8); // offset symbols 0..1
    cptw_bits(w, 0x11, 8);
}

static void cptw_literal(cpt_writer *w, char c) {
    cptw_bits(w, 1, 1); // literal flag
    cptw_bits(w, c == 'A' ? 0 : 1, 1); // 'A' = 0, 'B' = 1
}

// A match: flag 0, the length's code, the offset symbol's code, 6 low bits.
static void cptw_match(cpt_writer *w, int mlen, unsigned offset) {
    cptw_bits(w, 0, 1);
    cptw_bits(w, (uint32_t)(mlen - 2), 1);
    cptw_bits(w, offset >> 6, 1);
    cptw_bits(w, offset & 63, 6);
}

// The LZH builder is right, and so is the error plumbing on valid input:
// "AB" then a 2-byte match at offset 2 decodes to "ABAB".
TEST(test_cpt_lzh_round_trip) {
    cpt_writer w = {0};
    cptw_tables(&w);
    cptw_literal(&w, 'A');
    cptw_literal(&w, 'B');
    cptw_match(&w, 2, 2);
    size_t len;
    uint8_t *a = make_cpt_data_fork(w.buf, (uint32_t)((w.nbits + 7) / 8), 4, true, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel_cpt(a, len, &err);
    if (err)
        fprintf(stderr, "  cpt: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(1, list.count);
    ASSERT_EQ_INT(4, (int)list.files[0].data_fork.size);
    ASSERT_TRUE(memcmp(list.files[0].data_fork.data, "ABAB", 4) == 0);
    peel_file_list_free(&list);
    free(a);
}

// Offset 0 reaches a full window back (8192 bytes).  09-storage F-16 called it
// invalid, on the strength of cpt.md's "offsets are 1-based", and this suite
// once asserted that it was refused -- until the real corpus
// (src/peeler/test/testfiles) showed both Compact Pro archives use it.  The
// offset field is 13 bits, so 8192 cannot be written any other way, and
// `(wpos - 0) & CP_WIN_MASK` is exactly that slot.  Here: 8192 literals
// "ABAB...", then a 2-byte match at offset 0, which must repeat the first two.
TEST(test_cpt_lzh_offset_zero_is_a_full_window_back) {
    static cpt_writer w; // 8192 two-bit literals: ~2 KB of stream
    memset(&w, 0, sizeof(w));
    cptw_tables(&w);
    for (int i = 0; i < 8192; i++)
        cptw_literal(&w, (i & 1) ? 'B' : 'A');
    cptw_match(&w, 2, 0);
    size_t len;
    uint8_t *a = make_cpt_data_fork(w.buf, (uint32_t)((w.nbits + 7) / 8), 8194, true, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel_cpt(a, len, &err);
    if (err)
        fprintf(stderr, "  cpt: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(8194, (int)list.files[0].data_fork.size);
    ASSERT_TRUE(memcmp(list.files[0].data_fork.data + 8192, "AB", 2) == 0);
    peel_file_list_free(&list);
    free(a);
}

// A fork that ends before its declared length -- here 4 plain bytes declared
// to decode to 16 -- came back as a 4-byte fork with no error: the decoder
// treated running dry as end of file, and nothing compared the result with
// data_uncomp.  (Compact Pro's per-file CRC is also stored and never checked;
// see the commit that added this test for why that is not fixed here.)
TEST(test_cpt_short_fork_is_rejected) {
    static const uint8_t four[] = {'a', 'b', 'c', 'd'};
    size_t len;
    uint8_t *a = make_cpt_data_fork(four, 4, 16, false, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel_cpt(a, len, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_EQ_INT(0, list.count);
    peel_err_free(err);
    free(a);
}

// F-15, Compact Pro half: the fork extents were `file_offset + rsrc_comp > len`
// in size_t -- a sum that wraps on wasm32.  Observed unfixed: native refuses
// the archive; wasm32 ACCEPTS it (no error, one file) with a resource fork
// decoded from an empty source, because cp_memsrc_init's own wrap-safe check
// failed and its result was ignored.  Must be refused on every target.
TEST(test_cpt_fork_extent_is_checked) {
    size_t len;
    uint8_t *a = make_cpt_file(0x40, 0xFFFFFFF0u, 0, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel_cpt(a, len, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(peel_err_msg(err), "resource fork of 'F' extends past archive") != NULL);
    peel_err_free(err);
    peel_file_list_free(&list);
    free(a);
}

// F-07: cp_walk_entries recursed once per nested folder with no depth limit,
// and each frame carries two 256-byte path buffers -- so a few bytes of
// archive per level buy half a kilobyte of stack.  60 000 levels is 180 KB of
// input; it must be refused, not followed down.
TEST(test_cpt_folder_nesting_is_bounded) {
    size_t len;
    uint8_t *a = make_cpt_nested(60000, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel_cpt(a, len, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_EQ_INT(0, list.count);
    peel_err_free(err);
    free(a);
}

// ...while nesting up to the cap still parses -- folders are not files, so a
// chain of empty folders yields an empty archive and no error -- and one level
// past it does not.  The cap is exact: PEEL_MAX_DIR_DEPTH (internal.h), 128, parse.
// (A literal on purpose -- change the cap and this test makes you look.)
TEST(test_cpt_nesting_cap_is_exact) {
    size_t len;
    uint8_t *a = make_cpt_nested(128, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel_cpt(a, len, &err);
    if (err)
        fprintf(stderr, "  cpt: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(0, list.count);
    peel_file_list_free(&list);
    free(a);

    a = make_cpt_nested(129, &len);
    list = peel_cpt(a, len, &err);
    ASSERT_TRUE(err != NULL);
    peel_err_free(err);
    free(a);
}

// ============================================================================
// StuffIt 5 archives
// ============================================================================

// CRC-16/ARC (reflected 0xA001, init 0) -- StuffIt's, per sit.md §3.
static uint16_t crc16_arc(const uint8_t *p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

#define SIT5_TOP_SIZE 100
#define SIT5_H2_SIZE  36 // flags2 .. the version-1 skip; fork info or data follows
#define SIT5_RINFO    14 // resource-fork info block, when flags2 bit 0 is set

// One file in a StuffIt 5 archive, laid out per sit.md §5: the 100-byte top
// header, header 1 (48 fixed bytes + the name), header 2, the resource-fork
// info block if there is a resource fork, then the resource fork's bytes and
// the data fork's.  Both forks are stored (method 0) unless r_algo says
// otherwise.  Every length is written as given, so a test can lie in any of
// them; the builder only ever writes inside the buffer it allocates.
typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t dlen;
    int h1_len; // -1: the true length
    uint32_t raw_len_override; // nonzero: replaces the data fork's raw length
    bool rsrc; // flags2 bit 0
    const uint8_t *rdata;
    uint32_t rdlen; // resource bytes actually present
    uint32_t r_raw_len, r_packed_len;
    uint8_t r_algo;
    uint8_t d_algo; // data fork method; 0 = stored
    const uint8_t *d_raw; // if set: the data fork's uncompressed bytes,
    uint32_t d_raw_len; //   for its raw length and CRC (`data` is then packed)
} sit5_spec;

static uint8_t *build_sit5(const sit5_spec *sp, size_t *out_len) {
    size_t namelen = strlen(sp->name);
    size_t true_h1 = 48 + namelen;
    size_t total = SIT5_TOP_SIZE + true_h1 + SIT5_H2_SIZE + (sp->rsrc ? SIT5_RINFO : 0) + sp->rdlen + sp->dlen + 64;
    uint8_t *a = calloc(total, 1);
    ASSERT_TRUE(a != NULL);

    memcpy(a, "StuffIt (c)1997-2001", 20);
    memcpy(a + 20, " Aladdin Systems, Inc., http://www.aladdinsys.com/StuffIt/", 58);
    a[78] = '\r';
    a[79] = '\n';
    put16(a + 92, 1); // entry count
    put32(a + 94, SIT5_TOP_SIZE); // first entry

    uint8_t *h1 = a + SIT5_TOP_SIZE;
    uint16_t written_h1 = (uint16_t)(sp->h1_len >= 0 ? sp->h1_len : (int)true_h1);
    put32(h1 + 0, 0xA5A5A5A5u);
    h1[4] = 1; // version
    put16(h1 + 6, written_h1);
    put16(h1 + 30, (uint16_t)namelen);
    uint32_t raw_len = sp->d_raw ? sp->d_raw_len : sp->dlen;
    put32(h1 + 34, sp->raw_len_override ? sp->raw_len_override : raw_len);
    put32(h1 + 38, sp->dlen);
    put16(h1 + 42, sp->d_raw ? crc16_arc(sp->d_raw, sp->d_raw_len) : crc16_arc(sp->data, sp->dlen));
    h1[46] = sp->d_algo;
    memcpy(h1 + 48, sp->name, namelen);
    // Header CRC over header 1 as written, with its own two bytes zeroed.
    size_t crc_len = written_h1 <= true_h1 ? written_h1 : true_h1;
    put16(h1 + 32, crc16_arc(h1, crc_len));

    uint8_t *h2 = h1 + true_h1;
    put16(h2 + 0, sp->rsrc ? 1 : 0);
    memcpy(h2 + 4, "TEXT", 4);
    memcpy(h2 + 8, "ttxt", 4);
    uint8_t *p = h2 + SIT5_H2_SIZE;
    if (sp->rsrc) {
        put32(p + 0, sp->r_raw_len);
        put32(p + 4, sp->r_packed_len);
        put16(p + 8, sp->rdlen ? crc16_arc(sp->rdata, sp->rdlen) : 0);
        p[12] = sp->r_algo;
        p += SIT5_RINFO;
        if (sp->rdlen)
            memcpy(p, sp->rdata, sp->rdlen);
        p += sp->rdlen;
    }
    memcpy(p, sp->data, sp->dlen);
    *out_len = total;
    return a;
}

// The common case: one stored data fork, no resource fork.
static uint8_t *make_sit5(const char *name, const uint8_t *data, uint32_t dlen, int h1_len, uint32_t raw_len_override,
                          size_t *out_len) {
    sit5_spec sp = {.name = name, .data = data, .dlen = dlen, .h1_len = h1_len, .raw_len_override = raw_len_override};
    return build_sit5(&sp, out_len);
}

// The builder is right: one stored file comes back byte for byte.
TEST(test_sit5_round_trip) {
    static const uint8_t data[] = "peeler";
    size_t len;
    uint8_t *a = make_sit5("ReadMe", data, 6, -1, 0, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel(a, len, &err);
    if (err)
        fprintf(stderr, "  sit5: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(1, list.count);
    ASSERT_TRUE(strcmp(list.files[0].meta.name, "ReadMe") == 0);
    ASSERT_EQ_INT(6, (int)list.files[0].data_fork.size);
    ASSERT_TRUE(memcmp(list.files[0].data_fork.data, "peeler", 6) == 0);
    peel_file_list_free(&list);
    free(a);
}

// The builder's resource-fork path is right: a stored resource fork comes back.
TEST(test_sit5_round_trip_with_resource_fork) {
    static const uint8_t data[] = "data";
    static const uint8_t rsrc[] = "RSRC";
    sit5_spec sp = {.name = "Both",
                    .data = data,
                    .dlen = 4,
                    .h1_len = -1,
                    .rsrc = true,
                    .rdata = rsrc,
                    .rdlen = 4,
                    .r_raw_len = 4,
                    .r_packed_len = 4};
    size_t len;
    uint8_t *a = build_sit5(&sp, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel(a, len, &err);
    if (err)
        fprintf(stderr, "  sit5: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(1, list.count);
    ASSERT_EQ_INT(4, (int)list.files[0].data_fork.size);
    ASSERT_TRUE(memcmp(list.files[0].data_fork.data, "data", 4) == 0);
    ASSERT_EQ_INT(4, (int)list.files[0].resource_fork.size);
    ASSERT_TRUE(memcmp(list.files[0].resource_fork.data, "RSRC", 4) == 0);
    peel_file_list_free(&list);
    free(a);
}

// LZW (method 2): a stream of 9-bit literal codes, packed least significant
// bit first, decodes to exactly those bytes -- each code after the first adds
// a dictionary entry, but under 255 codes the width stays 9.  Covers the
// LZW bit reader, which F-17 changed from a memcpy into a host-order word
// to explicit little-endian composition.
TEST(test_sit5_lzw_literals_round_trip) {
    static const char text[] = "Hello, LZW!";
    size_t n = sizeof(text) - 1;
    uint8_t packed[32] = {0};
    size_t bit = 0;
    for (size_t i = 0; i < n; i++)
        for (int b = 0; b < 9; b++, bit++)
            if (((unsigned)(uint8_t)text[i] >> b) & 1)
                packed[bit / 8] |= (uint8_t)(1u << (bit % 8));
    sit5_spec sp = {.name = "L",
                    .data = packed,
                    .dlen = (uint32_t)((bit + 7) / 8),
                    .h1_len = -1,
                    .d_algo = 2,
                    .d_raw = (const uint8_t *)text,
                    .d_raw_len = (uint32_t)n};
    size_t len;
    uint8_t *a = build_sit5(&sp, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel(a, len, &err);
    if (err)
        fprintf(stderr, "  lzw: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(1, list.count);
    ASSERT_EQ_INT((int)n, (int)list.files[0].data_fork.size);
    ASSERT_TRUE(memcmp(list.files[0].data_fork.data, text, n) == 0);
    peel_file_list_free(&list);
    free(a);
}

// F-15: a resource fork claiming 0xFFFFFFC0 packed bytes, in an archive of a
// few hundred.  The data fork's start was computed as a pointer, resource
// start + that length, and only the data fork was bounds-checked.  Natively
// the pointer lands far past the end and the check rejects it, by accident.
// On wasm32 it wraps back inside the buffer and the check passes: the unfixed
// build decodes the resource fork straight off the end of the archive into
// adjacent heap until it has its 4096 bytes, and only the fork CRC rejects it
// ("fork CRC mismatch", observed under run-wasm32).  The assertion is on the
// new check's own message, which neither unfixed target produces.
TEST(test_sit5_resource_fork_extent_is_checked) {
    static const uint8_t data[] = "data";
    sit5_spec sp = {.name = "Big",
                    .data = data,
                    .dlen = 4,
                    .h1_len = -1,
                    .rsrc = true,
                    .r_raw_len = 4096,
                    .r_packed_len = 0xFFFFFFC0u,
                    .r_algo = 1};
    size_t len;
    uint8_t *a = build_sit5(&sp, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel(a, len, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(peel_err_msg(err), "fork data extends past archive end") != NULL);
    ASSERT_EQ_INT(0, list.count);
    peel_err_free(err);
    free(a);
}

// F-02: header 1's length was never checked against its own fixed fields.
// The CRC step mallocs h1_len bytes and then zeroes bytes 32 and 33 of the
// copy -- two bytes past the end for any h1_len below 34.  Silent natively
// (the write lands in allocator padding); ASan reports it.  Must be rejected.
TEST(test_sit5_short_header_is_rejected) {
    static const uint8_t data[] = "x";
    size_t len;
    uint8_t *a = make_sit5("ReadMe", data, 1, 20, 0, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel(a, len, &err);
    ASSERT_TRUE(err != NULL);
    // Rejected by the length check itself.  The unfixed parser also failed on
    // this input natively -- further on, for an unrelated reason, after the
    // silent overrun -- so "some error" would pass for the wrong reason.
    ASSERT_TRUE(strstr(peel_err_msg(err), "shorter than its fixed fields") != NULL);
    ASSERT_EQ_INT(0, list.count);
    peel_err_free(err);
    free(a);
}

// F-06: a skip-marker entry (raw length 0xFFFFFFFF) moves the cursor on by
// h1_len and does not count down the entries remaining -- so h1_len == 0 left
// the cursor where it was, forever.  The F-02 bound (h1_len >= 48 + name)
// rules it out; this pins that down in case the bound is ever loosened to
// "just enough for the CRC".
TEST(test_sit5_zero_length_skip_marker_cannot_loop) {
    static const uint8_t data[] = "x";
    size_t len;
    uint8_t *a = make_sit5("ReadMe", data, 1, 0, 0xFFFFFFFFu, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel(a, len, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(peel_err_msg(err), "shorter than its fixed fields") != NULL);
    ASSERT_EQ_INT(0, list.count);
    peel_err_free(err);
    free(a);
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

    peel_err_t *err = NULL;
    peel_buf_t out = peel_sit13(w.buf, (w.nbits + 7) / 8, 4, &err);
    if (err)
        fprintf(stderr, "  sit13: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(4, (int)out.size);
    ASSERT_TRUE(memcmp(out.data, "ABBA", 4) == 0);
    peel_free(&out);
}

// F-03: m13_decode_lengths bounded its loop by nsym but not the repeat
// commands inside it.  Command 36 emits r + 10 entries plus one more, r a
// 6-bit field -- up to 74 -- so from index 320 of the 321-entry lengths array
// it writes 73 bytes past the end of a stack array.  Four full repeats and
// one of 24 reach index 320 exactly; a fifth full one overruns.
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

    peel_err_t *err = NULL;
    peel_buf_t out = peel_sit13(w.buf, (w.nbits + 7) / 8, 16, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(out.data == NULL);
    peel_err_free(err);
}

// ============================================================================
// BinHex 4.0
// ============================================================================

// hqx.c's alphabet (hqx.md §4.1), repeated here: the test encodes with it.
static const char hqx_alpha[] = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";

// A BinHex 4.0 text file (hqx.md §6): the binary stream -- header, data fork,
// resource fork, each followed by its CRC-16/CCITT (peeler's own
// crc16_ccitt) -- 6-bit encoded between colons after the preamble line.  No
// byte of the fixtures is 0x90, so no RLE escaping is needed.  `bad_rsrc_crc`
// corrupts only the resource fork's CRC.
static char *make_hqx_core(const char *name, const char *data, const char *rsrc, bool bad_rsrc_crc,
                           uint32_t declared_dlen) {
    uint8_t bin[512];
    size_t n = 0;
    size_t nl = strlen(name), dl = strlen(data), rl = strlen(rsrc);
    bin[n++] = (uint8_t)nl;
    memcpy(bin + n, name, nl);
    n += nl;
    bin[n++] = 0;
    memcpy(bin + n, "TEXTttxt", 8);
    n += 8;
    bin[n++] = 0;
    bin[n++] = 0; // Finder flags
    put32(bin + n, declared_dlen ? declared_dlen : (uint32_t)dl);
    n += 4;
    put32(bin + n, (uint32_t)rl);
    n += 4;
    put16(bin + n, crc16_ccitt(bin, n));
    n += 2;
    memcpy(bin + n, data, dl);
    put16(bin + n + dl, crc16_ccitt(bin + n, dl));
    n += dl + 2;
    memcpy(bin + n, rsrc, rl);
    put16(bin + n + rl, (uint16_t)(crc16_ccitt(bin + n, rl) ^ (bad_rsrc_crc ? 0x5555 : 0)));
    n += rl + 2;

    char *txt = calloc(1024, 1);
    ASSERT_TRUE(txt != NULL);
    size_t t = (size_t)sprintf(txt, "(This file must be converted with BinHex 4.0)\r\n:");
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v =
            (uint32_t)bin[i] << 16 | (uint32_t)(i + 1 < n ? bin[i + 1] : 0) << 8 | (i + 2 < n ? bin[i + 2] : 0);
        size_t chars = i + 2 < n ? 4 : i + 1 < n ? 3 : 2; // a partial group ends early
        for (size_t c = 0; c < chars; c++)
            txt[t++] = hqx_alpha[(v >> (18 - 6 * c)) & 63];
    }
    txt[t++] = ':';
    txt[t] = '\0';
    return txt;
}

static char *make_hqx(const char *name, const char *data, const char *rsrc, bool bad_rsrc_crc) {
    return make_hqx_core(name, data, rsrc, bad_rsrc_crc, 0);
}

// The same file, but its header declares a data fork of `declared` bytes.
static char *make_hqx_lying(const char *name, const char *data, uint32_t declared) {
    return make_hqx_core(name, data, "", false, declared);
}

// The builder is right: both forks come back.
TEST(test_hqx_round_trip) {
    char *txt = make_hqx("Hi", "data!", "RSRC", false);
    peel_err_t *err = NULL;
    peel_file_t f = peel_hqx_file((const uint8_t *)txt, strlen(txt), &err);
    if (err)
        fprintf(stderr, "  hqx: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(5, (int)f.data_fork.size);
    ASSERT_TRUE(memcmp(f.data_fork.data, "data!", 5) == 0);
    ASSERT_EQ_INT(4, (int)f.resource_fork.size);
    ASSERT_TRUE(memcmp(f.resource_fork.data, "RSRC", 4) == 0);
    peel_free(&f.data_fork);
    peel_free(&f.resource_fork);
    free(txt);
}

// F-12: the data fork decodes and is complete, then the resource fork's CRC
// is wrong.  The abort used to leak the finished data fork; under
// LeakSanitizer that fails the suite.
TEST(test_hqx_resource_fork_failure_frees_the_data_fork) {
    char *txt = make_hqx("Hi", "data!", "RSRC", true);
    peel_err_t *err = NULL;
    peel_file_t f = peel_hqx_file((const uint8_t *)txt, strlen(txt), &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(peel_err_msg(err), "resource fork CRC mismatch") != NULL);
    ASSERT_TRUE(f.data_fork.data == NULL);
    peel_err_free(err);
    free(txt);
}

// sit3.c's entry point, declared (not in a header) where sit.c declares it.
peel_buf_t peel_sit3(const uint8_t *src, size_t len, size_t uncomp_len, peel_err_t **err);

// F-11: sit3 allocated its output, then aborted on a truncated tree -- one
// set bit starts a leaf, whose 8-bit symbol is not there -- and leaked it.
TEST(test_sit3_abort_frees_its_output) {
    static const uint8_t one_byte[] = {0xFF};
    peel_err_t *err = NULL;
    peel_buf_t out = peel_sit3(one_byte, 1, 64, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(out.data == NULL);
    peel_err_free(err);
}

// ---- Characterisation: the decoders the corpus does not reach -------------
//
// No archive in the real corpus uses StuffIt method 3 or method 2 (LZW)
// (measured), so these pin their behaviour down before their bit readers
// are replaced by peeler's shared ones: each decodes a stream built here
// bit by bit, including the edges -- a tree read across byte boundaries,
// codes widening 9 -> 10 -> 11 bits, a clear code mid-block, running out of
// input.

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
    peel_err_t *err = NULL;
    peel_buf_t out = peel_sit3(w.buf, len, n, &err);
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT((int)n, (int)out.size);
    ASSERT_TRUE(memcmp(out.data, msg, n) == 0);
    peel_free(&out);

    // One symbol more than the stream holds: the reader runs out, and
    // method 3 aborts rather than inventing bits.
    out = peel_sit3(w.buf, len, n + 8, &err);
    ASSERT_TRUE(err != NULL && out.data == NULL);
    peel_err_free(err);

    // A single-leaf tree codes its symbol as no bits at all.
    memset(&w, 0, sizeof(w));
    msb_put(&w, 1, 1);
    msb_put(&w, 'z', 8);
    err = NULL;
    out = peel_sit3(w.buf, (w.bit + 7) / 8, 5, &err);
    ASSERT_TRUE(err == NULL && out.size == 5 && memcmp(out.data, "zzzzz", 5) == 0);
    peel_free(&out);
}

// Method 2, beyond 9-bit literals: literal codes only, each always valid
// and each after the first adding a dictionary entry, so the width widens
// exactly as the decoder's table grows (to 10 bits at 512 entries, 11 at
// 1024); then a clear code part-way through an 8-code block, the padding
// codes the decoder skips to realign, and literals at 9 bits again.
typedef struct {
    uint8_t *buf;
    size_t bit;
    int width;
    int table; // next free dictionary slot
    int count; // codes since the last clear
    bool first; // next code is the first after a reset
} lzw_writer;
static void lzw_put(lzw_writer *w, int code) {
    for (int b = 0; b < w->width; b++, w->bit++)
        if ((code >> b) & 1)
            w->buf[w->bit / 8] |= (uint8_t)(1u << (w->bit % 8));
    w->count++;
}
static void lzw_literal(lzw_writer *w, uint8_t byte) {
    lzw_put(w, byte);
    if (w->first) {
        w->first = false; // the first code after a reset adds no entry
        return;
    }
    w->table++;
    if (w->table < (1 << 14) && (w->table & (w->table - 1)) == 0 && w->width < 14)
        w->width++;
}
static void lzw_clear(lzw_writer *w) {
    lzw_put(w, 256);
    while (w->count & 7)
        lzw_put(w, 0x155); // padding the decoder skips, at the current width
    w->width = 9;
    w->table = 257;
    w->count = 0;
    w->first = true;
}

TEST(test_sit5_lzw_widening_and_clear) {
    enum { BEFORE = 1100, AFTER = 60 };
    static uint8_t raw[BEFORE + AFTER];
    for (int i = 0; i < BEFORE + AFTER; i++)
        raw[i] = (uint8_t)(i * 37 + (i >> 5));
    static uint8_t packed[4096];
    memset(packed, 0, sizeof(packed));
    lzw_writer w = {.buf = packed, .width = 9, .table = 257, .first = true};
    for (int i = 0; i < BEFORE; i++)
        lzw_literal(&w, raw[i]);
    ASSERT_EQ_INT(11, w.width); // the stream really reached 11-bit codes
    lzw_clear(&w);
    for (int i = BEFORE; i < BEFORE + AFTER; i++)
        lzw_literal(&w, raw[i]);

    sit5_spec sp = {.name = "W",
                    .data = packed,
                    .dlen = (uint32_t)((w.bit + 7) / 8),
                    .h1_len = -1,
                    .d_algo = 2,
                    .d_raw = raw,
                    .d_raw_len = (uint32_t)sizeof(raw)};
    size_t len;
    uint8_t *a = build_sit5(&sp, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel(a, len, &err);
    if (err)
        fprintf(stderr, "  lzw: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(1, list.count);
    ASSERT_EQ_INT((int)sizeof(raw), (int)list.files[0].data_fork.size);
    ASSERT_TRUE(memcmp(list.files[0].data_fork.data, raw, sizeof(raw)) == 0);
    peel_file_list_free(&list);
    free(a);
}

// ============================================================================
// A3: one cap on what an archive may declare (PEEL_MAX_FORK, internal.h)
// ============================================================================
//
// F-08: each format allocated whatever size its header declared -- a few
// hundred bytes of archive could ask for gigabytes.  Every format must now
// refuse a declared fork over the cap before allocating, and say so; each
// test asserts the cap's own message, because the unfixed code also failed
// these inputs eventually -- after attempting the allocation.

#define OVER_CAP 0x50000000u // 1.25 GiB, over PEEL_MAX_FORK's 1 GiB

TEST(test_sit5_fork_over_the_cap_is_refused) {
    static const uint8_t data[] = "x";
    sit5_spec sp = {.name = "Big", .data = data, .dlen = 1, .h1_len = -1, .raw_len_override = OVER_CAP};
    size_t len;
    uint8_t *a = build_sit5(&sp, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel(a, len, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(peel_err_msg(err), "over the 1024 MiB limit") != NULL);
    ASSERT_EQ_INT(0, list.count);
    peel_err_free(err);
    free(a);
}

TEST(test_cpt_fork_over_the_cap_is_refused) {
    static const uint8_t four[] = {'a', 'b', 'c', 'd'};
    size_t len;
    uint8_t *a = make_cpt_data_fork(four, 4, OVER_CAP, false, &len);
    peel_err_t *err = NULL;
    peel_file_list_t list = peel_cpt(a, len, &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(peel_err_msg(err), "over the 1024 MiB limit") != NULL);
    ASSERT_EQ_INT(0, list.count);
    peel_err_free(err);
    free(a);
}

// hqx declares its fork lengths in the header, and make_hqx writes the true
// ones; this rewrites the data-fork length in the binary stream by building
// it with a lying length directly.
TEST(test_hqx_fork_over_the_cap_is_refused) {
    char *txt = make_hqx_lying("Hi", "data!", OVER_CAP);
    peel_err_t *err = NULL;
    peel_file_t f = peel_hqx_file((const uint8_t *)txt, strlen(txt), &err);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(peel_err_msg(err), "over the 1024 MiB limit") != NULL);
    ASSERT_TRUE(f.data_fork.data == NULL);
    peel_err_free(err);
    free(txt);
}

// F-09: peel_read_file loads a whole file, with no bound.  A sparse file one
// byte over PEEL_MAX_INPUT must be refused before anything is allocated.
// Native only: under node, MEMFS would allocate the gigabyte for real.
TEST(test_read_file_over_the_cap_is_refused) {
#ifndef __EMSCRIPTEN__
    char path[] = "/tmp/peeler-cap-XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    ASSERT_TRUE(ftruncate(fd, (off_t)(1u << 30) + 1) == 0);
    close(fd);
    peel_err_t *err = NULL;
    peel_buf_t b = peel_read_file(path, &err);
    unlink(path);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(peel_err_msg(err), "over the 1024 MiB limit") != NULL);
    ASSERT_TRUE(b.data == NULL);
    peel_err_free(err);
#endif
}

// ============================================================================
// Track B: entry names cannot leave the output directory (F-13)
// ============================================================================

TEST(test_path_is_confined) {
    static const char *ok[] = {"a", "a/b", "_..", "..:escape", "a/_./b", "...", "a.b/c..d"};
    static const char *bad[] = {"", "/etc/passwd", "..", ".", "../x", "a/../b", "a/./b", "a//b", "a/", "a/.."};
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        ASSERT_TRUE(peel_path_is_confined(ok[i]));
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        ASSERT_TRUE(!peel_path_is_confined(bad[i]));
}

// The first entry name a one-file archive yields, and whether it is confined.
static void expect_single_name(const uint8_t *a, size_t len, const char *want) {
    peel_err_t *err = NULL;
    peel_file_list_t list = peel(a, len, &err);
    if (err)
        fprintf(stderr, "  peel: %s\n", peel_err_msg(err));
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(1, list.count);
    if (strcmp(list.files[0].meta.name, want) != 0)
        fprintf(stderr, "  name: got '%s', want '%s'\n", list.files[0].meta.name, want);
    ASSERT_TRUE(strcmp(list.files[0].meta.name, want) == 0);
    ASSERT_TRUE(peel_path_is_confined(list.files[0].meta.name));
    peel_file_list_free(&list);
}

// A Mac name may contain '/' (legal on HFS).  Passed through, "../escape" was
// a traversal path to anything joining it under an output directory.  It
// becomes ':', the macOS convention -- one component, not three.
TEST(test_sit5_slash_in_a_name_cannot_escape) {
    static const uint8_t data[] = "x";
    size_t len;
    uint8_t *a = make_sit5("../escape", data, 1, -1, 0, &len);
    expect_single_name(a, len, "..:escape");
    free(a);
}

// ".." is a legal Mac name too; as a path component it is traversal.
TEST(test_sit5_dots_only_name_is_prefixed) {
    static const uint8_t data[] = "x";
    size_t len;
    uint8_t *a = make_sit5("..", data, 1, -1, 0, &len);
    expect_single_name(a, len, "_..");
    free(a);
}

// The same through a folder, in Compact Pro: folder ".." holding file "x".
TEST(test_cpt_dot_dot_folder_is_prefixed) {
    uint8_t a[8 + 1 + 7 + 3 + 2 + 2 + 45 + 16] = {0};
    a[0] = 0x01;
    a[1] = 0x01;
    put32(a + 4, 9); // one fork byte at 8, directory at 9
    a[8] = 'x';
    uint8_t *d = a + 9;
    put16(d + 4, 2); // two entries: the folder and the file
    uint8_t *e = d + 7;
    e[0] = 0x80 | 2; // folder, 2-byte name
    e[1] = '.';
    e[2] = '.';
    put16(e + 3, 1); // one entry below it
    e += 5;
    e[0] = 1; // file, 1-byte name
    e[1] = 'x';
    uint8_t *m = e + 2;
    put32(m + 1, 8); // file_offset
    put32(m + 33, 1); // data_uncomp
    put32(m + 41, 1); // data_comp (plain RLE: the byte itself)
    expect_single_name(a, sizeof(a), "_../x");
}

// BinHex and MacBinary carry one name each.  peel() treats them as wrappers
// and passes the contents through unnamed, so the name is only exposed by
// peel_hqx_file / peel_bin_file -- which is where it must be safe.
TEST(test_hqx_slash_in_a_name_cannot_escape) {
    char *txt = make_hqx("../x", "data!", "", false);
    peel_err_t *err = NULL;
    peel_file_t f = peel_hqx_file((const uint8_t *)txt, strlen(txt), &err);
    ASSERT_TRUE(err == NULL);
    if (strcmp(f.meta.name, "..:x") != 0)
        fprintf(stderr, "  name: got '%s'\n", f.meta.name);
    ASSERT_TRUE(strcmp(f.meta.name, "..:x") == 0);
    ASSERT_TRUE(peel_path_is_confined(f.meta.name));
    peel_free(&f.data_fork);
    peel_free(&f.resource_fork);
    free(txt);
}

// Classic StuffIt: a folder name of 64 bytes or more skipped the copy into
// the folder stack but still pushed it, leaving that slot uninitialised stack
// for the path builder to strlen.  It is now clamped to the 63 bytes the
// 112-byte entry header can hold.  (Layout per sit.md §4: a 22-byte archive
// header, then 112-byte entry headers; entry headers carry no CRC.)
TEST(test_sit_classic_long_folder_name_is_clamped) {
    uint8_t a[22 + 3 * 112 + 1] = {0};
    memcpy(a, "SIT!", 4);
    put16(a + 4, 1); // one top-level entry: the folder
    put32(a + 6, sizeof(a));
    memcpy(a + 10, "rLau", 4);
    uint8_t *h = a + 22; // folder start, name length 70
    h[0] = h[1] = 0x20;
    h[2] = 70;
    memset(h + 3, 'a', 63);
    h += 112; // the file "f": stored data fork, one byte
    h[2] = 1;
    h[3] = 'f';
    put32(h + 88, 1); // data uncompressed length
    put32(h + 96, 1); // data compressed length
    put16(h + 102, crc16_arc((const uint8_t *)"x", 1));
    h[112] = 'x';
    h += 113; // folder end
    h[0] = h[1] = 0x21;

    char want[80];
    memset(want, 'a', 63);
    memcpy(want + 63, "/f", 3);
    expect_single_name(a, sizeof(a), want);
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

// ---- The shared canonical-Huffman pool (09 A4: F-59, F-04) ------------------

// Walk `bits` (MSB first) from `root`: the symbol reached, or -1.
static int huff_walk(const peel_hpool_t *p, int root, const char *bits) {
    int node = root;
    for (; *bits && node >= 0; bits++)
        node = peel_huff_child(p, node, *bits == '1');
    return node < 0 ? -1 : peel_huff_sym(p, node);
}

// Canonical assignment: ascending length, then ascending symbol.  Lengths
// {2, 1, 2}: symbol 1 is "0", symbol 0 "10", symbol 2 "11".  An absent
// symbol (0) gets no code.
TEST(test_huff_canonical_codes) {
    static peel_hpool_t pool;
    peel_hpool_reset(&pool);
    const int8_t lens[4] = {2, 1, 2, 0};
    int root = peel_huff_build(&pool, lens, 4, 1, 15);
    ASSERT_TRUE(root >= 0);
    ASSERT_EQ_INT(1, huff_walk(&pool, root, "0"));
    ASSERT_EQ_INT(0, huff_walk(&pool, root, "10"));
    ASSERT_EQ_INT(2, huff_walk(&pool, root, "11"));
    // A length outside the format's range is refused, not skipped.
    const int8_t bad[2] = {1, 16};
    ASSERT_EQ_INT(-1, peel_huff_build(&pool, bad, 2, 1, 15));
}

// The pool is bounded: trees that do not fit are refused.  sit13 kept its
// four trees in a 2048-node pool it never bounds-checked (F-04); it now
// shares this one.  Under ASan an overrun here is a failure.
TEST(test_huff_pool_is_bounded) {
    static peel_hpool_t pool;
    peel_hpool_reset(&pool);
    // 256 symbols at length 8: a full tree, 511 nodes.  The fifth does not fit.
    static int8_t lens[256];
    memset(lens, 8, sizeof(lens));
    int built = 0;
    while (peel_huff_build(&pool, lens, 256, 1, 15) >= 0)
        built++;
    ASSERT_EQ_INT(4, built);
    ASSERT_TRUE(pool.used <= PEEL_HUFF_POOL_CAP);
}

int main(void) {
    RUN(test_sit15_encoder_round_trip);
    RUN(test_sit15_zero_run_cannot_overflow);
    RUN(test_sit15_zero_run_bound_is_exact);
    RUN(test_sit13_dynamic_round_trip);
    RUN(test_sit13_length_repeat_cannot_overrun);
    RUN(test_sit13_minus_one_is_an_absent_symbol);
    RUN(test_sit13_negative_length_is_rejected);
    RUN(test_cpt_lzh_round_trip);
    RUN(test_cpt_lzh_offset_zero_is_a_full_window_back);
    RUN(test_cpt_short_fork_is_rejected);
    RUN(test_cpt_fork_extent_is_checked);
    RUN(test_cpt_nesting_cap_is_exact);
    RUN(test_cpt_folder_nesting_is_bounded);
    RUN(test_sit5_round_trip);
    RUN(test_sit5_round_trip_with_resource_fork);
    RUN(test_sit5_lzw_literals_round_trip);
    RUN(test_sit5_resource_fork_extent_is_checked);
    RUN(test_sit5_short_header_is_rejected);
    RUN(test_sit5_zero_length_skip_marker_cannot_loop);
    RUN(test_hqx_round_trip);
    RUN(test_hqx_resource_fork_failure_frees_the_data_fork);
    RUN(test_sit3_abort_frees_its_output);
    RUN(test_sit5_fork_over_the_cap_is_refused);
    RUN(test_cpt_fork_over_the_cap_is_refused);
    RUN(test_hqx_fork_over_the_cap_is_refused);
    RUN(test_read_file_over_the_cap_is_refused);
    RUN(test_path_is_confined);
    RUN(test_sit5_slash_in_a_name_cannot_escape);
    RUN(test_sit5_dots_only_name_is_prefixed);
    RUN(test_cpt_dot_dot_folder_is_prefixed);
    RUN(test_hqx_slash_in_a_name_cannot_escape);
    RUN(test_sit_classic_long_folder_name_is_clamped);
    RUN(test_peel_passes_unrecognised_input_through);
    RUN(test_sit3_decodes_a_tree_and_message);
    RUN(test_sit5_lzw_widening_and_clear);
    RUN(test_huff_canonical_codes);
    RUN(test_huff_pool_is_bounded);
    fprintf(stderr, "All peeler tests passed\n");
    return 0;
}
