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
// Matches in any block may reach back into earlier blocks' output (the
// decoder's ring keeps 256 KiB of it).  A coded block is read whole -- its
// payloads are read backwards -- and decoded into the ring a triple at a
// time; an LZVN block an opcode at a time; a stored block as the ring takes
// it.
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
#define LZFSE_WINDOW      (1u << 18) // matches reach at most 262 139 bytes back
#define LZFSE_BURST       (315 + 2359) // the most one triple writes (L and M at their largest)
#define LZFSE_BLOCK_MAX   (64u << 20) // a coded block's bytes read into memory, at most
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

// ============================================================================
// The decoder
// ============================================================================

typedef enum { Z_MAGIC, Z_RAW, Z_LZVN, Z_LMD, Z_DONE } lzfse_state_t;

typedef struct {
    lzfse_state_t state;
    uint8_t *blk; // the current block, header and payloads
    size_t blk_cap;
    uint64_t block_end; // ring.total when the current block is complete
    uint32_t raw_left; // a stored block's bytes still to copy
    // An LZVN block: its opcodes, and the cursor.
    const uint8_t *vn;
    size_t vn_len, vn_at, vn_dist;
    // A coded block: its decoded literals and the triples' decoder.
    lzfse_header_t h;
    uint8_t *lits;
    size_t lits_cap;
    uint32_t used, matches_left; // literals consumed, triples to go
    int32_t dist; // the previous distance (none yet)
    uint16_t ls, ms, ds; // the L, M and D decoder states
    fse_in_t in; // the triples' backward bit stream
    fse_entry_t lit_t[LZFSE_LIT_STATES];
    fse_value_entry_t l_t[LZFSE_L_STATES], m_t[LZFSE_M_STATES], d_t[LZFSE_D_STATES];
} lzfse_t;

// Read `n` more bytes of the block into blk at `at`.  0, TRUNCATED or NOMEM.
static int read_block(codec_dec_t *d, lzfse_t *z, size_t at, size_t n) {
    if (at + n > LZFSE_BLOCK_MAX)
        return codec_fail(d, CODEC_ERR_UNSUPPORTED, "lzfse: block too large");
    if (at + n > z->blk_cap) {
        size_t cap = z->blk_cap ? z->blk_cap : 65536;
        while (cap < at + n)
            cap *= 2;
        uint8_t *b = realloc(z->blk, cap);
        if (!b)
            return codec_fail(d, CODEC_ERR_NOMEM, "lzfse: out of memory");
        z->blk = b;
        z->blk_cap = cap;
    }
    if (codec_in_read(&d->in, z->blk + at, n) != n)
        return codec_fail(d, CODEC_ERR_TRUNCATED, "lzfse: truncated block");
    return 0;
}

// Set a coded block up: its literals decoded, the triples' decoder ready.
static int start_coded(codec_dec_t *d, lzfse_t *z) {
    lzfse_header_t *h = &z->h;
    size_t fixed = LZFSE_V2_FIXED;
    // The fixed part has been read (blk[0..32)); the frequency tables and
    // the payloads follow.
    uint64_t w2 = codec_le64(z->blk + 24);
    uint32_t header_size = (uint32_t)(w2 & 0xFFFFFFFFu);
    if (header_size < fixed || header_size > (1u << 16))
        return codec_fail(d, CODEC_ERR_DATA, "lzfse: bad header size");
    int rc = read_block(d, z, fixed, header_size - fixed);
    if (rc)
        return rc;
    if (parse_v2(h, z->blk, header_size) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "lzfse: bad block header");
    size_t n_pay = (size_t)h->lit_bytes + h->lmd_bytes;
    if ((rc = read_block(d, z, header_size, n_pay)) != 0)
        return rc;
    const uint8_t *payload = z->blk + header_size;

    // The literals: four decoders taking turns, so four at a time.
    size_t nlit = ((size_t)h->n_literals + 3) & ~(size_t)3;
    if (nlit > z->lits_cap) {
        uint8_t *l = realloc(z->lits, nlit);
        if (!l)
            return codec_fail(d, CODEC_ERR_NOMEM, "lzfse: out of memory");
        z->lits = l;
        z->lits_cap = nlit;
    }
    if (fse_build(z->lit_t, LZFSE_LIT_STATES, LZFSE_LIT_SYMBOLS, h->lit_freq) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "lzfse: bad literal frequencies");
    fse_in_t in;
    if (fse_in_init(&in, z->blk, payload + h->lit_bytes, h->lit_pad) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "lzfse: bad literal stream");
    uint16_t st[4];
    memcpy(st, h->lit_state, sizeof(st));
    for (uint32_t i = 0; i < h->n_literals; i += 4)
        for (int j = 0; j < 4; j++)
            z->lits[i + (uint32_t)j] = fse_decode(&st[j], z->lit_t, &in);
    if (in.over)
        return codec_fail(d, CODEC_ERR_DATA, "lzfse: literal stream overrun");

    // The triples' tables and their bit stream.
    if (fse_build_values(z->l_t, LZFSE_L_STATES, LZFSE_L_SYMBOLS, h->l_freq, lzfse_l_extra) != 0 ||
        fse_build_values(z->m_t, LZFSE_M_STATES, LZFSE_M_SYMBOLS, h->m_freq, lzfse_m_extra) != 0 ||
        fse_build_values(z->d_t, LZFSE_D_STATES, LZFSE_D_SYMBOLS, h->d_freq, lzfse_d_extra) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "lzfse: bad frequencies");
    const uint8_t *lmd = payload + h->lit_bytes;
    if (fse_in_init(&z->in, z->blk, lmd + h->lmd_bytes, h->lmd_pad) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "lzfse: bad triple stream");
    z->ls = h->l_state;
    z->ms = h->m_state;
    z->ds = h->d_state;
    z->used = 0;
    z->matches_left = h->n_matches;
    z->dist = 0; // the previous distance: none
    z->block_end = d->ring.total + h->n_raw;
    z->state = Z_LMD;
    return 0;
}

// Decode a coded block's triples while the ring has room for one.
static int run_lmd(codec_dec_t *d, lzfse_t *z) {
    codec_ring_t *r = &d->ring;
    while (z->matches_left > 0) {
        if (!codec_room_for(d, LZFSE_BURST))
            return CODEC_MORE;
        int32_t L = fse_decode_value(&z->ls, z->l_t, &z->in);
        int32_t M = fse_decode_value(&z->ms, z->m_t, &z->in);
        int32_t D = fse_decode_value(&z->ds, z->d_t, &z->in);
        z->matches_left--;
        if (z->in.over)
            return codec_fail(d, CODEC_ERR_DATA, "lzfse: triple stream overrun");
        if (D)
            z->dist = D;
        if ((uint32_t)L > z->h.n_literals - z->used || (uint64_t)L + (uint64_t)M > z->block_end - r->total)
            return codec_fail(d, CODEC_ERR_DATA, "lzfse: a triple runs past its block");
        ring_write(r, z->lits + z->used, (size_t)L);
        z->used += (uint32_t)L;
        if (M) {
            if (z->dist <= 0 || (uint64_t)z->dist > r->total)
                return codec_fail(d, CODEC_ERR_DATA, "lzfse: distance before the start of the output");
            ring_copy(r, (size_t)z->dist, (size_t)M);
        }
    }
    if (r->total != z->block_end)
        return codec_fail(d, CODEC_ERR_DATA, "lzfse: a block decodes short of its size");
    z->state = Z_MAGIC;
    return 0;
}

// Decode an LZVN block's opcodes while the ring has room for one.
static int run_lzvn(codec_dec_t *d, lzfse_t *z) {
    codec_ring_t *r = &d->ring;
    const uint8_t *p = z->vn;
    size_t len = z->vn_len;
    for (;;) {
        if (!codec_room_for(d, LZFSE_BURST))
            return CODEC_MORE;
        size_t at = z->vn_at, dist = z->vn_dist;
        if (at >= len)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "lzvn: truncated block");
        unsigned op = p[at];
        size_t L = 0, M = 0, oplen = 1; // literals, match bytes, opcode bytes
        bool match = true; // whether the opcode ends with a match
        unsigned lo = op & 7;
        if (op == 0x06) { // end of stream (an 8-byte opcode)
            if (r->total != z->block_end)
                return codec_fail(d, CODEC_ERR_DATA, "lzvn: a block decodes short of its size");
            z->state = Z_MAGIC;
            return 0;
        } else if (op == 0x0E || op == 0x16) { // no-op
            z->vn_at = at + 1;
            continue;
        } else if ((op >= 0x70 && op <= 0x7F) || (op >= 0xD0 && op <= 0xDF) || (op < 0x40 && lo == 6)) {
            return codec_fail(d, CODEC_ERR_DATA, "lzvn: undefined opcode");
        } else if (op >= 0xF0) { // match at the previous distance
            if (op == 0xF0) {
                oplen = 2;
                if (len - at < 2)
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "lzvn: truncated opcode");
                M = 16 + (size_t)p[at + 1];
            } else
                M = op & 15;
        } else if (op >= 0xE0) { // literals only
            match = false;
            if (op == 0xE0) {
                oplen = 2;
                if (len - at < 2)
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "lzvn: truncated opcode");
                L = 16 + (size_t)p[at + 1];
            } else
                L = op & 15;
        } else if (op >= 0xA0 && op <= 0xBF) { // 101LLMMM DDDDDDMM DDDDDDDD
            oplen = 3;
            if (len - at < 3)
                return codec_fail(d, CODEC_ERR_TRUNCATED, "lzvn: truncated opcode");
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
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "lzvn: truncated opcode");
                dist = codec_le16(p + at + 1);
            } else if (lo != 6) { // 11-bit distance
                oplen = 2;
                if (len - at < 2)
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "lzvn: truncated opcode");
                dist = ((size_t)lo << 8) | p[at + 1];
            }
        }
        at += oplen;
        if (L > len - at)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "lzvn: truncated literals");
        if (L + M > z->block_end - r->total)
            return codec_fail(d, CODEC_ERR_DATA, "lzvn: an opcode runs past its block");
        ring_write(r, p + at, L);
        at += L;
        if (match) {
            if (dist == 0 || dist > r->total)
                return codec_fail(d, CODEC_ERR_DATA, "lzvn: distance before the start of the output");
            ring_copy(r, dist, M);
        }
        z->vn_at = at;
        z->vn_dist = dist;
    }
}

static int lzfse_fill(codec_dec_t *d) {
    lzfse_t *z = d->st;
    codec_ring_t *r = &d->ring;
    for (;;) {
        int rc = 0;
        switch (z->state) {
        case Z_MAGIC: {
            uint8_t m[4];
            if (codec_in_read(&d->in, m, 4) != 4)
                return codec_fail(d, CODEC_ERR_TRUNCATED, "lzfse: truncated stream");
            uint32_t magic = codec_le32(m);
            if (magic == LZFSE_MAGIC_END) {
                z->state = Z_DONE;
                return CODEC_END;
            }
            uint8_t b[8];
            if (magic == LZFSE_MAGIC_RAW) {
                if (codec_in_read(&d->in, b, 4) != 4)
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "lzfse: truncated block header");
                z->raw_left = codec_le32(b);
                z->state = Z_RAW;
            } else if (magic == LZFSE_MAGIC_LZVN) {
                if (codec_in_read(&d->in, b, 8) != 8)
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "lzfse: truncated block header");
                uint32_t n_raw = codec_le32(b);
                size_t n_pay = codec_le32(b + 4);
                if ((rc = read_block(d, z, 0, n_pay)) != 0)
                    return rc;
                z->vn = z->blk;
                z->vn_len = n_pay;
                z->vn_at = 0;
                z->vn_dist = 0;
                z->block_end = r->total + n_raw;
                z->state = Z_LZVN;
            } else if (magic == LZFSE_MAGIC_V2) {
                memcpy(z->blk, m, 4); // blk has room: it starts at 64 KiB
                if ((rc = read_block(d, z, 4, LZFSE_V2_FIXED - 4)) != 0)
                    return rc;
                if ((rc = start_coded(d, z)) != 0)
                    return rc;
            } else if (magic == LZFSE_MAGIC_V1)
                return codec_fail(d, CODEC_ERR_UNSUPPORTED, "lzfse: unpacked (v1) block header");
            else
                return codec_fail(d, CODEC_ERR_DATA, "lzfse: bad block magic");
            break;
        }
        case Z_RAW: {
            size_t room = ring_room(r);
            if (room == 0)
                return CODEC_MORE;
            while (z->raw_left && room) {
                if (d->in.pos == d->in.len && !codec_in_refill(&d->in))
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "lzfse: truncated stored block");
                size_t k = d->in.len - d->in.pos;
                if (k > z->raw_left)
                    k = z->raw_left;
                if (k > room)
                    k = room;
                ring_write(r, d->in.p + d->in.pos, k);
                d->in.pos += k;
                z->raw_left -= (uint32_t)k;
                room -= k;
            }
            if (z->raw_left == 0)
                z->state = Z_MAGIC;
            break;
        }
        case Z_LZVN:
            rc = run_lzvn(d, z);
            if (rc != 0)
                return rc;
            if (z->state == Z_LZVN)
                return CODEC_MORE;
            break;
        case Z_LMD:
            rc = run_lmd(d, z);
            if (rc != 0)
                return rc;
            if (z->state == Z_LMD)
                return CODEC_MORE;
            break;
        case Z_DONE:
            return CODEC_END;
        }
    }
}

static int lzfse_init(codec_dec_t *d) {
    lzfse_t *z = d->st;
    z->blk = malloc(65536);
    if (!z->blk)
        return CODEC_ERR_NOMEM;
    z->blk_cap = 65536;
    return 0;
}

static void lzfse_free(codec_dec_t *d) {
    lzfse_t *z = d->st;
    free(z->blk);
    free(z->lits);
}

const codec_dec_ops_t codec_lzfse_dec = {.state_size = sizeof(lzfse_t),
                                         .window = LZFSE_WINDOW,
                                         .burst = LZFSE_BURST,
                                         .init = lzfse_init,
                                         .fill = lzfse_fill,
                                         .free = lzfse_free};
