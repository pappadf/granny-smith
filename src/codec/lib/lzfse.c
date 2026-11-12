// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lzfse.c
// LZFSE decoder, with the LZVN and stored blocks an LZFSE stream may hold
// (see codec.h).
//
// A stream is a run of blocks, each starting with a four-byte magic:
//   "bvx-"  stored: 32-bit size, then the bytes
//   "bvxn"  LZVN: 32-bit decoded size, 32-bit payload size, then the payload
//   "bvx2"  LZFSE with a packed header (below)
//   "bvx1"  LZFSE with an unpacked header -- an encoder-internal form never
//           written to a stream; refused as unsupported
//   "bvx$"  end of stream
// Matches in any block may reach back into earlier blocks' output.
//
// An LZFSE block is LZ77 coded as literals plus (L, M, D) triples -- L
// literals, then M bytes from D back, D = 0 repeating the previous
// distance -- and entropy-coded with finite-state entropy (tANS):
//   - the literals by four interleaved decoders over one 1024-state table;
//   - L, M and D by one decoder each (64, 64 and 256 states), whose symbol
//     is a bucket: a base value plus that many extra bits.
// Each table is defined by its symbols' frequencies (they sum to at most the
// state count).  A symbol of frequency f owns f consecutive states; every
// state names the symbol and how to reach the next state: read k bits and
// add a base.  For f, k is the shift that brings f << k into
// [states, 2 * states); the first ((2 * states) >> k) - f of its states read
// k bits, the rest k - 1, so together they cover the whole state range.
//
// The v2 header packs the counts, initial states and bit offsets into three
// 64-bit words, then codes the frequency tables with a small prefix code.
// Each payload (literals, then triples) is a bit stream read backwards from
// its end, most recently written bits first.
//
// LZVN is byte-oriented LZ77 with one opcode byte per step: a literal run,
// a match at a new or repeated distance, or both (up to 3 literals and a
// match in one opcode), or end of stream.

#include "codec_internal.h"

#include <stdlib.h>
#include <string.h>

#define LZFSE_MAGIC_END   0x24787662u // "bvx$"
#define LZFSE_MAGIC_RAW   0x2d787662u // "bvx-"
#define LZFSE_MAGIC_V1    0x31787662u // "bvx1"
#define LZFSE_MAGIC_V2    0x32787662u // "bvx2"
#define LZFSE_MAGIC_LZVN  0x6e787662u // "bvxn"
#define LZFSE_V2_FIXED    32 // the v2 header before its frequency tables
#define LZFSE_LZVN_HEADER 12 // magic, decoded size, payload size

#define LZFSE_L_SYMBOLS   20
#define LZFSE_M_SYMBOLS   20
#define LZFSE_D_SYMBOLS   64
#define LZFSE_LIT_SYMBOLS 256
#define LZFSE_L_STATES    64
#define LZFSE_M_STATES    64
#define LZFSE_D_STATES    256
#define LZFSE_LIT_STATES  1024

// Extra bits per L, M and D bucket; the bucket base values follow from them
// (each bucket starts where the previous one's range ends).
static const uint8_t lzfse_l_extra[LZFSE_L_SYMBOLS] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 5, 8};
static const uint8_t lzfse_m_extra[LZFSE_M_SYMBOLS] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 5, 8, 11};
static const uint8_t lzfse_d_extra[LZFSE_D_SYMBOLS] = {0,  0,  0,  0,  1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  3,
                                                       4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,  6,  7,  7,  7,  7,
                                                       8,  8,  8,  8,  9,  9,  9,  9,  10, 10, 10, 10, 11, 11, 11, 11,
                                                       12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15};

// ============================================================================
// Backward bit input
// ============================================================================

// Bits are taken from the end of a payload towards its start; a byte read
// earlier is older, so newer bytes are shifted in below it and the oldest
// bits are taken first, from the top.
typedef struct {
    const uint8_t *start; // the first byte reads may reach (the stream start)
    const uint8_t *p; // bytes before p are not yet loaded
    uint64_t acc; // the loaded bits not yet taken, in the low n bits
    int n;
    bool over; // a bit before the payload start was needed
} fse_in_t;

// Start at `end`, a payload's end.  The first word holds `pad` (0..7)
// unused top bits, which must be zero: eight bytes when there are none, else
// seven.  A payload shorter than that word (a block of four literals that
// all decode from zero-bit states has none at all) borrows the bytes before
// it -- the block header -- which a valid stream never consumes; `start`
// bounds how far back reads may go: the start of the LZFSE stream.
static int fse_in_init(fse_in_t *in, const uint8_t *start, const uint8_t *end, int pad) {
    int bytes = pad ? 7 : 8;
    if (end - start < bytes)
        return CODEC_ERR_DATA;
    *in = (fse_in_t){.start = start, .p = end - bytes};
    for (int i = bytes - 1; i >= 0; i--)
        in->acc = (in->acc << 8) | in->p[i];
    in->n = bytes * 8 - pad;
    if (in->n < 64 && (in->acc >> in->n) != 0)
        return CODEC_ERR_DATA;
    return 0;
}

// Take the next `k` (0..32) bits.
static inline uint32_t fse_pull(fse_in_t *in, int k) {
    if (k == 0)
        return 0; // (and a full 64-bit accumulator must not shift by 64)
    while (in->n < k) {
        if (in->p == in->start) {
            in->over = true;
            return 0;
        }
        in->acc = (in->acc << 8) | *--in->p;
        in->n += 8;
    }
    in->n -= k;
    uint32_t v = (uint32_t)(in->acc >> in->n);
    in->acc &= in->n ? (~0ull >> (64 - in->n)) : 0;
    return v;
}

// ============================================================================
// Decoding tables
// ============================================================================

// One state: the symbol, and the next state's base and bit count.
typedef struct {
    uint8_t symbol;
    int8_t k; // bits to read for the next state
    int16_t delta; // the next state is delta + those bits
} fse_entry_t;

// One state of an L, M or D table: the next state as above, plus the
// symbol's bucket (vbase and vbits extra bits), read together.
typedef struct {
    uint8_t total; // k + vbits
    uint8_t vbits;
    int16_t delta;
    int32_t vbase;
} fse_value_entry_t;

// The states of `nsym` symbols with frequencies `freq` over `nstates`
// states (a power of two): fills t[0 .. sum of freq).  The rest of `t` is
// cleared, so an out-of-range state decodes as symbol 0 with no successor.
static int fse_build(fse_entry_t *t, int nstates, int nsym, const uint16_t *freq) {
    memset(t, 0, sizeof(*t) * (size_t)nstates);
    int at = 0;
    for (int s = 0; s < nsym; s++) {
        int f = freq[s];
        if (f == 0)
            continue;
        if (f > nstates - at)
            return CODEC_ERR_DATA; // frequencies over-subscribe the states
        int k = 0;
        while ((f << k) < nstates)
            k++;
        int j0 = ((2 * nstates) >> k) - f; // states that read k bits
        for (int j = 0; j < f; j++, at++) {
            t[at].symbol = (uint8_t)s;
            if (j < j0) {
                t[at].k = (int8_t)k;
                t[at].delta = (int16_t)(((f + j) << k) - nstates);
            } else {
                t[at].k = (int8_t)(k - 1);
                t[at].delta = (int16_t)((j - j0) << (k - 1));
            }
        }
    }
    return 0;
}

// A value table: fse_build's states, with each symbol's bucket folded in.
static int fse_build_values(fse_value_entry_t *t, int nstates, int nsym, const uint16_t *freq, const uint8_t *extra) {
    fse_entry_t tmp[LZFSE_D_STATES]; // the largest value table
    int rc = fse_build(tmp, nstates, nsym, freq);
    if (rc)
        return rc;
    int32_t base[LZFSE_D_SYMBOLS];
    int32_t v = 0;
    for (int s = 0; s < nsym; s++) {
        base[s] = v;
        v += 1 << extra[s];
    }
    for (int i = 0; i < nstates; i++) {
        int s = tmp[i].symbol;
        t[i] = (fse_value_entry_t){
            .total = (uint8_t)(tmp[i].k + extra[s]), .vbits = extra[s], .delta = tmp[i].delta, .vbase = base[s]};
    }
    return 0;
}

// Step a literal decoder: its symbol, and the next state.
static inline uint8_t fse_decode(uint16_t *state, const fse_entry_t *t, fse_in_t *in) {
    fse_entry_t e = t[*state];
    *state = (uint16_t)(e.delta + (int)fse_pull(in, e.k));
    return e.symbol;
}

// Step an L, M or D decoder: its value, and the next state.  The state bits
// come first in the stream, the bucket's extra bits after them.
static inline int32_t fse_decode_value(uint16_t *state, const fse_value_entry_t *t, fse_in_t *in) {
    fse_value_entry_t e = t[*state];
    uint32_t b = fse_pull(in, e.total);
    *state = (uint16_t)(e.delta + (int)(b >> e.vbits));
    return e.vbase + (int32_t)(b & ((1u << e.vbits) - 1));
}

// ============================================================================
// Output
// ============================================================================

typedef struct {
    uint8_t *dst;
    size_t cap, pos; // the output and bytes written
} lzfse_out_t;

// Append `n` bytes from `src`.
static int out_bytes(lzfse_out_t *o, const uint8_t *src, size_t n) {
    if (n > o->cap - o->pos)
        return CODEC_ERR_OVERFLOW;
    memcpy(o->dst + o->pos, src, n);
    o->pos += n;
    return 0;
}

// Append `n` bytes copied from `d` back (byte by byte: they may overlap).
static int out_match(lzfse_out_t *o, size_t d, size_t n) {
    if (d == 0 || d > o->pos)
        return CODEC_ERR_DATA; // reaches before the start of the output
    if (n > o->cap - o->pos)
        return CODEC_ERR_OVERFLOW;
    uint8_t *q = o->dst + o->pos;
    for (size_t i = 0; i < n; i++)
        q[i] = q[(ptrdiff_t)i - (ptrdiff_t)d];
    o->pos += n;
    return 0;
}

// ============================================================================
// LZFSE blocks
// ============================================================================

typedef struct {
    uint32_t n_raw; // decoded bytes
    uint32_t n_literals, n_matches;
    uint32_t lit_bytes, lmd_bytes; // payload sizes
    int lit_pad, lmd_pad; // unused top bits of each payload's first word
    uint16_t lit_state[4];
    uint16_t l_state, m_state, d_state;
    uint32_t header_size;
    uint16_t l_freq[LZFSE_L_SYMBOLS], m_freq[LZFSE_M_SYMBOLS];
    uint16_t d_freq[LZFSE_D_SYMBOLS], lit_freq[LZFSE_LIT_SYMBOLS];
} lzfse_header_t;

// Bits `at` .. `at + n` of `w`.
static uint32_t field(uint64_t w, int at, int n) {
    return (uint32_t)((w >> at) & ((1ull << n) - 1));
}

// One frequency from the low bits of `bits`: a prefix code of 2, 3, 5, 8 or
// 14 bits (*n set), read from the least significant bit.
static uint32_t freq_code(uint32_t bits, int *n) {
    if ((bits & 1) == 0) {
        *n = 2; // x0: 0 or 1
        return (bits >> 1) & 1;
    }
    if ((bits & 3) == 1) {
        *n = 3; // x01: 2 or 3
        return 2 + ((bits >> 2) & 1);
    }
    if ((bits & 7) == 3) {
        *n = 5; // xx011: 4..7
        return 4 + ((bits >> 3) & 3);
    }
    if ((bits & 15) == 7) {
        *n = 8; // xxxx0111: 8..23
        return 8 + ((bits >> 4) & 15);
    }
    *n = 14; // x..x1111: 24..1047
    return 24 + ((bits >> 4) & 0x3FF);
}

// Parse a v2 header at b[0..len).
static int parse_v2(lzfse_header_t *h, const uint8_t *b, size_t len) {
    if (len < LZFSE_V2_FIXED)
        return CODEC_ERR_TRUNCATED;
    uint64_t w0 = codec_le64(b + 8), w1 = codec_le64(b + 16), w2 = codec_le64(b + 24);
    h->n_raw = codec_le32(b + 4);
    h->n_literals = field(w0, 0, 20);
    h->lit_bytes = field(w0, 20, 20);
    h->n_matches = field(w0, 40, 20);
    h->lit_pad = 7 - (int)field(w0, 60, 3); // stored as bit count + 7, 0..-7
    for (int i = 0; i < 4; i++)
        h->lit_state[i] = (uint16_t)field(w1, 10 * i, 10);
    h->lmd_bytes = field(w1, 40, 20);
    h->lmd_pad = 7 - (int)field(w1, 60, 3);
    h->header_size = field(w2, 0, 32);
    h->l_state = (uint16_t)field(w2, 32, 10);
    h->m_state = (uint16_t)field(w2, 42, 10);
    h->d_state = (uint16_t)field(w2, 52, 10);
    if (h->header_size < LZFSE_V2_FIXED || h->header_size > len)
        return CODEC_ERR_DATA;
    if (h->lit_pad < 0 || h->lmd_pad < 0)
        return CODEC_ERR_DATA; // only -7..0 are bit counts
    if (h->l_state >= LZFSE_L_STATES || h->m_state >= LZFSE_M_STATES || h->d_state >= LZFSE_D_STATES)
        return CODEC_ERR_DATA;
    for (int i = 0; i < 4; i++)
        if (h->lit_state[i] >= LZFSE_LIT_STATES)
            return CODEC_ERR_DATA;

    // The frequency tables, L, M, D, then literals, as prefix codes read
    // from the least significant bit of a byte stream.
    uint16_t *dest[4] = {h->l_freq, h->m_freq, h->d_freq, h->lit_freq};
    const int count[4] = {LZFSE_L_SYMBOLS, LZFSE_M_SYMBOLS, LZFSE_D_SYMBOLS, LZFSE_LIT_SYMBOLS};
    const uint8_t *p = b + LZFSE_V2_FIXED, *end = b + h->header_size;
    uint32_t acc = 0;
    int n = 0;
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < count[t]; i++) {
            while (n <= 24 && p < end) {
                acc |= (uint32_t)*p++ << n;
                n += 8;
            }
            int used;
            uint32_t f = freq_code(acc, &used);
            if (used > n)
                return CODEC_ERR_DATA; // the tables end mid-code
            acc >>= used;
            n -= used;
            dest[t][i] = (uint16_t)f;
        }
    }
    return 0;
}

// Decode one LZFSE block (header already parsed) whose payloads begin at
// `payload`, in a stream that begins at `stream`.
static int lzfse_block(lzfse_out_t *o, const lzfse_header_t *h, const uint8_t *stream, const uint8_t *payload) {
    size_t want = o->pos + h->n_raw; // where this block's output ends
    if (h->n_raw > o->cap - o->pos)
        return CODEC_ERR_OVERFLOW;

    // The literals: four decoders taking turns, so four at a time.
    fse_entry_t *lit_t = malloc(sizeof(fse_entry_t) * LZFSE_LIT_STATES);
    uint8_t *lits = malloc(((size_t)h->n_literals + 3) & ~(size_t)3);
    int rc = (!lit_t || !lits) ? CODEC_ERR_NOMEM : 0;
    if (rc == 0)
        rc = fse_build(lit_t, LZFSE_LIT_STATES, LZFSE_LIT_SYMBOLS, h->lit_freq);
    fse_in_t in;
    if (rc == 0)
        rc = fse_in_init(&in, stream, payload + h->lit_bytes, h->lit_pad);
    if (rc == 0) {
        uint16_t st[4];
        memcpy(st, h->lit_state, sizeof(st));
        for (uint32_t i = 0; i < h->n_literals; i += 4)
            for (int j = 0; j < 4; j++) {
                uint8_t c = fse_decode(&st[j], lit_t, &in);
                if (i + (uint32_t)j < h->n_literals)
                    lits[i + j] = c;
            }
        if (in.over)
            rc = CODEC_ERR_DATA;
    }
    free(lit_t);

    // The triples: L literals, then M bytes from D back.
    fse_value_entry_t l_t[LZFSE_L_STATES], m_t[LZFSE_M_STATES], d_t[LZFSE_D_STATES];
    if (rc == 0)
        rc = fse_build_values(l_t, LZFSE_L_STATES, LZFSE_L_SYMBOLS, h->l_freq, lzfse_l_extra);
    if (rc == 0)
        rc = fse_build_values(m_t, LZFSE_M_STATES, LZFSE_M_SYMBOLS, h->m_freq, lzfse_m_extra);
    if (rc == 0)
        rc = fse_build_values(d_t, LZFSE_D_STATES, LZFSE_D_SYMBOLS, h->d_freq, lzfse_d_extra);
    const uint8_t *lmd = payload + h->lit_bytes;
    if (rc == 0)
        rc = fse_in_init(&in, stream, lmd + h->lmd_bytes, h->lmd_pad);
    if (rc == 0) {
        uint16_t ls = h->l_state, ms = h->m_state, ds = h->d_state;
        uint32_t used = 0; // literals consumed
        int32_t dist = 0; // the previous distance (none yet)
        for (uint32_t i = 0; i < h->n_matches && rc == 0; i++) {
            int32_t L = fse_decode_value(&ls, l_t, &in);
            int32_t M = fse_decode_value(&ms, m_t, &in);
            int32_t D = fse_decode_value(&ds, d_t, &in);
            if (in.over) {
                rc = CODEC_ERR_DATA;
                break;
            }
            if (D)
                dist = D;
            if ((uint32_t)L > h->n_literals - used || (size_t)L + (size_t)M > want - o->pos) {
                rc = CODEC_ERR_DATA; // more than the block holds
                break;
            }
            rc = out_bytes(o, lits + used, (size_t)L);
            used += (uint32_t)L;
            if (rc == 0 && M)
                rc = out_match(o, (size_t)dist, (size_t)M);
        }
        if (rc == 0 && o->pos != want)
            rc = CODEC_ERR_DATA; // the block decodes short of its size
    }
    free(lits);
    return rc;
}

// ============================================================================
// LZVN blocks
// ============================================================================

// Decode LZVN opcodes from p[0..len) until the end-of-stream opcode, the
// output growing by exactly `n_raw` bytes.
static int lzvn_block(lzfse_out_t *o, const uint8_t *p, size_t len, uint32_t n_raw) {
    size_t want = o->pos + n_raw, at = 0, dist = 0;
    if (n_raw > o->cap - o->pos)
        return CODEC_ERR_OVERFLOW;
    for (;;) {
        if (at >= len)
            return CODEC_ERR_TRUNCATED;
        unsigned op = p[at];
        size_t L = 0, M = 0, oplen = 1; // literals, match bytes, opcode bytes
        bool match = true; // whether the opcode ends with a match
        unsigned lo = op & 7;
        if (op == 0x06) { // end of stream (an 8-byte opcode)
            return o->pos == want ? 0 : CODEC_ERR_DATA;
        } else if (op == 0x0E || op == 0x16) { // no-op
            at++;
            continue;
        } else if ((op >= 0x70 && op <= 0x7F) || (op >= 0xD0 && op <= 0xDF) || (op < 0x40 && lo == 6)) {
            return CODEC_ERR_DATA; // undefined
        } else if (op >= 0xF0) { // match at the previous distance
            if (op == 0xF0) {
                oplen = 2;
                if (len - at < 2)
                    return CODEC_ERR_TRUNCATED;
                M = 16 + (size_t)p[at + 1];
            } else
                M = op & 15;
        } else if (op >= 0xE0) { // literals only
            match = false;
            if (op == 0xE0) {
                oplen = 2;
                if (len - at < 2)
                    return CODEC_ERR_TRUNCATED;
                L = 16 + (size_t)p[at + 1];
            } else
                L = op & 15;
        } else if (op >= 0xA0 && op <= 0xBF) { // 101LLMMM DDDDDDMM DDDDDDDD
            oplen = 3;
            if (len - at < 3)
                return CODEC_ERR_TRUNCATED;
            uint32_t w = codec_le16(p + at + 1);
            L = (op >> 3) & 3;
            M = 3 + ((((size_t)op & 7) << 2) | (w & 3));
            dist = w >> 2;
        } else { // LLMMMDDD: a new distance or (DDD = 110) the previous
            L = op >> 6;
            M = 3 + ((op >> 3) & 7);
            if (lo == 7) { // 16-bit distance
                oplen = 3;
                if (len - at < 3)
                    return CODEC_ERR_TRUNCATED;
                dist = codec_le16(p + at + 1);
            } else if (lo != 6) { // 11-bit distance
                oplen = 2;
                if (len - at < 2)
                    return CODEC_ERR_TRUNCATED;
                dist = ((size_t)lo << 8) | p[at + 1];
            }
        }
        at += oplen;
        if (L > len - at)
            return CODEC_ERR_TRUNCATED;
        if (L + M > want - o->pos)
            return CODEC_ERR_DATA; // more than the block holds
        int rc = out_bytes(o, p + at, L);
        at += L;
        if (rc == 0 && match)
            rc = out_match(o, dist, M);
        if (rc)
            return rc;
    }
}

// ============================================================================
// The stream
// ============================================================================

int64_t codec_lzfse_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    if (!src || (!dst && cap))
        return CODEC_ERR_ARG;
    lzfse_out_t o = {dst, cap, 0};
    size_t at = 0;
    lzfse_header_t *h = malloc(sizeof(*h));
    if (!h)
        return CODEC_ERR_NOMEM;
    int rc = 0;
    for (;;) {
        if (len - at < 4) {
            rc = CODEC_ERR_TRUNCATED;
            break;
        }
        uint32_t magic = codec_le32(src + at);
        size_t left = len - at;
        if (magic == LZFSE_MAGIC_END) {
            break;
        } else if (magic == LZFSE_MAGIC_RAW) {
            if (left < 8) {
                rc = CODEC_ERR_TRUNCATED;
                break;
            }
            size_t n = codec_le32(src + at + 4);
            if (n > left - 8) {
                rc = CODEC_ERR_TRUNCATED;
                break;
            }
            rc = out_bytes(&o, src + at + 8, n);
            at += 8 + n;
        } else if (magic == LZFSE_MAGIC_LZVN) {
            if (left < LZFSE_LZVN_HEADER) {
                rc = CODEC_ERR_TRUNCATED;
                break;
            }
            uint32_t n_raw = codec_le32(src + at + 4);
            size_t n_pay = codec_le32(src + at + 8);
            if (n_pay > left - LZFSE_LZVN_HEADER) {
                rc = CODEC_ERR_TRUNCATED;
                break;
            }
            rc = lzvn_block(&o, src + at + LZFSE_LZVN_HEADER, n_pay, n_raw);
            at += LZFSE_LZVN_HEADER + n_pay;
        } else if (magic == LZFSE_MAGIC_V2) {
            rc = parse_v2(h, src + at, left);
            if (rc)
                break;
            size_t n_pay = (size_t)h->lit_bytes + h->lmd_bytes;
            if (n_pay > left - h->header_size) {
                rc = CODEC_ERR_TRUNCATED;
                break;
            }
            rc = lzfse_block(&o, h, src, src + at + h->header_size);
            at += h->header_size + n_pay;
        } else if (magic == LZFSE_MAGIC_V1) {
            rc = CODEC_ERR_UNSUPPORTED;
        } else {
            rc = CODEC_ERR_DATA;
        }
        if (rc)
            break;
    }
    free(h);
    return rc < 0 ? rc : (int64_t)o.pos;
}
