// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lzma_enc.c
// LZMA encoder, writing one xz stream with one LZMA2 block (see codec.h).
//
// The encoder is the decoder in lzma.c run backwards over the same model
// (lzma_internal.h): every decision the decoder will make is coded here, in
// the same order, with the same adaptive probabilities, through a range
// encoder.
//
// Choosing what to code is a heuristic parse, not an optimal one:
//   - matches are found with hash chains over 3-byte prefixes, as deep as
//     the level asks, anywhere in the input (the input is the dictionary);
//   - at each position the four repeat distances are tried as well: a match
//     at a repeat distance costs no distance bits, so it wins unless a new
//     match is clearly longer;
//   - a match is deferred one byte when the next position starts a longer
//     one (lazy matching, from level 2);
//   - a byte equal to the one at the last distance is a short repeat
//     rather than a literal.
//
// The output is LZMA2: chunks of at most 64 KiB coded (the range coder is
// flushed and restarted at each, the model carries over), a chunk that does
// not shrink stored instead (and the model reset after it, as the decoder
// resets it).  Around it the xz container: stream header, block header (one
// LZMA2 filter, its dictionary just covering the input), the CRC-64 check,
// the index and the stream footer.

#include "lzma_internal.h"

#include <stdlib.h>
#include <string.h>

#define ENC_HASH_BITS   16
#define ENC_HASH_SIZE   (1u << ENC_HASH_BITS)
#define ENC_MATCH_MAX   273 // the longest match LZMA codes
#define ENC_CHUNK_PACK  65536 // LZMA2: coded bytes per chunk at most
#define ENC_CHUNK_RAW   (1u << 21) // LZMA2: decoded bytes per chunk at most
#define ENC_SYMBOL_MAX  64 // more than one symbol can add to the coder (~50 bits at ~6 bits each)
#define ENC_STORED_MAX  65536 // LZMA2: bytes per stored chunk at most
#define ENC_PROPS       (3 + 9 * (0 + 5 * 2)) // lc 3, lp 0, pb 2
#define ENC_CHECK_CRC64 0x04 // the xz check type written

struct codec_xz_state {
    size_t max_len;
    int32_t *head; // ENC_HASH_SIZE: newest position per hash
    int32_t *prev; // max_len: the previous position with the same hash
    uint8_t *pack; // one chunk's coded bytes
    lzma_t m; // the model (m.dst is the input being encoded)
};

// ============================================================================
// Range encoder
// ============================================================================

typedef struct {
    uint64_t low; // the bottom of the interval (33 bits: a carry pending)
    uint32_t range;
    uint8_t cache; // the last byte not yet written (a carry may still reach it)
    uint64_t cache_size; // it and the 0xFF bytes after it, all pending
    uint8_t *out;
    size_t pos, cap; // bytes written to out and its size
} enc_rc_t;

static void erc_init(enc_rc_t *rc, uint8_t *out, size_t cap) {
    *rc = (enc_rc_t){.range = 0xFFFFFFFFu, .cache_size = 1, .out = out, .cap = cap};
}

// Move the top byte of `low` out: pending bytes are written once no carry
// can reach them any more.
static void erc_shift_low(enc_rc_t *rc) {
    if ((uint32_t)rc->low < 0xFF000000u || (rc->low >> 32) != 0) {
        uint8_t carry = (uint8_t)(rc->low >> 32);
        uint8_t b = rc->cache;
        do {
            if (rc->pos < rc->cap)
                rc->out[rc->pos] = (uint8_t)(b + carry);
            rc->pos++;
            b = 0xFF;
        } while (--rc->cache_size != 0);
        rc->cache = (uint8_t)(rc->low >> 24);
    }
    rc->cache_size++;
    rc->low = (rc->low & 0x00FFFFFFu) << 8;
}

// Bytes the coder will have written once flushed.
static size_t erc_pending(const enc_rc_t *rc) {
    return rc->pos + (size_t)rc->cache_size + 4;
}

// Write out everything still pending.
static void erc_flush(enc_rc_t *rc) {
    for (int i = 0; i < 5; i++)
        erc_shift_low(rc);
}

// One bit under the adaptive probability *p, adapting it as the decoder
// will (rc_bit in lzma.c).
static inline void erc_bit(enc_rc_t *rc, lzma_prob_t *p, unsigned bit) {
    uint32_t bound = (rc->range >> LZMA_PROB_BITS) * *p;
    if (!bit) {
        rc->range = bound;
        *p = (lzma_prob_t)(*p + (((1u << LZMA_PROB_BITS) - *p) >> LZMA_MOVE_BITS));
    } else {
        rc->low += bound;
        rc->range -= bound;
        *p = (lzma_prob_t)(*p - (*p >> LZMA_MOVE_BITS));
    }
    while (rc->range < LZMA_TOP) {
        rc->range <<= 8;
        erc_shift_low(rc);
    }
}

// The low `n` bits of `v` at a fixed probability of one half, MSB first.
static void erc_direct(enc_rc_t *rc, uint32_t v, int n) {
    while (n--) {
        rc->range >>= 1;
        if ((v >> n) & 1)
            rc->low += rc->range;
        while (rc->range < LZMA_TOP) {
            rc->range <<= 8;
            erc_shift_low(rc);
        }
    }
}

// An n-bit value MSB first down a tree of probabilities (rc_tree).
static void erc_tree(enc_rc_t *rc, lzma_prob_t *probs, int n, uint32_t v) {
    uint32_t m = 1;
    while (n--) {
        unsigned bit = (v >> n) & 1;
        erc_bit(rc, &probs[m], bit);
        m = (m << 1) | bit;
    }
}

// An n-bit value LSB first down a tree of probabilities (rc_tree_reverse).
static void erc_tree_reverse(enc_rc_t *rc, lzma_prob_t *probs, int n, uint32_t v) {
    uint32_t m = 1;
    for (int i = 0; i < n; i++) {
        unsigned bit = (v >> i) & 1;
        erc_bit(rc, &probs[m], bit);
        m = (m << 1) | bit;
    }
}

// ============================================================================
// Coding the symbols (the inverse of lzma_run in lzma.c)
// ============================================================================

// A match length, LZMA_MATCH_MIN .. ENC_MATCH_MAX.
static void enc_len(enc_rc_t *rc, lzma_len_t *l, unsigned ps, uint32_t len) {
    len -= LZMA_MATCH_MIN;
    if (len < 8) {
        erc_bit(rc, &l->choice, 0);
        erc_tree(rc, l->low[ps], 3, len);
    } else if (len < 16) {
        erc_bit(rc, &l->choice, 1);
        erc_bit(rc, &l->choice2, 0);
        erc_tree(rc, l->mid[ps], 3, len - 8);
    } else {
        erc_bit(rc, &l->choice, 1);
        erc_bit(rc, &l->choice2, 1);
        erc_tree(rc, l->high, 8, len - 16);
    }
}

// The index of the highest set bit of v (v > 0).
static int top_bit(uint32_t v) {
    int n = 0;
    while (v >>= 1)
        n++;
    return n;
}

// A match distance (0-based) for a match of `len` bytes.
static void enc_dist(lzma_t *m, enc_rc_t *rc, uint32_t dist, uint32_t len) {
    unsigned ls = len - LZMA_MATCH_MIN < 3 ? len - LZMA_MATCH_MIN : 3;
    uint32_t slot = dist;
    if (dist >= 4) {
        // The slot: the top bit's position, twice, plus the bit below it.
        int n = top_bit(dist);
        slot = (uint32_t)(2 * n) + ((dist >> (n - 1)) & 1);
    }
    erc_tree(rc, m->dist_slot[ls], 6, slot);
    if (slot < 4)
        return;
    int extra = (int)(slot >> 1) - 1;
    uint32_t base = (2 | (slot & 1)) << extra, rem = dist - base;
    if (slot < LZMA_END_POS_MODEL) {
        erc_tree_reverse(rc, m->dist_special + base - slot, extra, rem);
        return;
    }
    erc_direct(rc, rem >> LZMA_ALIGN_BITS, extra - LZMA_ALIGN_BITS);
    erc_tree_reverse(rc, m->dist_align, LZMA_ALIGN_BITS, rem & ((1u << LZMA_ALIGN_BITS) - 1));
}

// The literal at m->pos.
static void enc_literal(lzma_t *m, enc_rc_t *rc) {
    size_t pos = m->pos;
    unsigned ps = (unsigned)pos & ((1u << m->pb) - 1), st = m->state;
    erc_bit(rc, &m->is_match[st][ps], 0);
    unsigned prev = pos ? m->dst[pos - 1] : 0, b = m->dst[pos];
    lzma_prob_t *probs =
        m->lit + 0x300 * (size_t)((((unsigned)pos & ((1u << m->lp) - 1)) << m->lc) + (prev >> (8 - m->lc)));
    uint32_t sym = 1;
    int i = 7;
    if (st >= LZMA_LIT_STATES) {
        // After a match: coded against the byte at rep0 while they agree.
        unsigned mb = m->dst[pos - m->rep[0] - 1];
        for (; i >= 0; i--) {
            unsigned bit = (b >> i) & 1, mbit = (mb >> i) & 1;
            erc_bit(rc, &probs[((1 + mbit) << 8) + sym], bit);
            sym = (sym << 1) | bit;
            if (bit != mbit) {
                i--;
                break;
            }
        }
    }
    for (; i >= 0; i--) {
        unsigned bit = (b >> i) & 1;
        erc_bit(rc, &probs[sym], bit);
        sym = (sym << 1) | bit;
    }
    m->state = st < 4 ? 0 : st < 10 ? st - 3 : st - 6;
    m->pos++;
}

// A match of `len` bytes at a new distance (0-based).
static void enc_match(lzma_t *m, enc_rc_t *rc, uint32_t dist, uint32_t len) {
    unsigned ps = (unsigned)m->pos & ((1u << m->pb) - 1), st = m->state;
    erc_bit(rc, &m->is_match[st][ps], 1);
    erc_bit(rc, &m->is_rep[st], 0);
    enc_len(rc, &m->match_len, ps, len);
    enc_dist(m, rc, dist, len);
    m->rep[3] = m->rep[2];
    m->rep[2] = m->rep[1];
    m->rep[1] = m->rep[0];
    m->rep[0] = dist;
    m->state = st < LZMA_LIT_STATES ? 7 : 10;
    m->pos += len;
}

// A match of `len` bytes at repeat distance `r` (0..3); len 1 at rep0 is
// the short repeat.
static void enc_rep(lzma_t *m, enc_rc_t *rc, unsigned r, uint32_t len) {
    unsigned ps = (unsigned)m->pos & ((1u << m->pb) - 1), st = m->state;
    erc_bit(rc, &m->is_match[st][ps], 1);
    erc_bit(rc, &m->is_rep[st], 1);
    if (r == 0) {
        erc_bit(rc, &m->is_rep_g0[st], 0);
        erc_bit(rc, &m->is_rep0_long[st][ps], len > 1);
        if (len == 1) {
            m->state = st < LZMA_LIT_STATES ? 9 : 11;
            m->pos++;
            return;
        }
    } else {
        erc_bit(rc, &m->is_rep_g0[st], 1);
        uint32_t d = m->rep[r];
        if (r == 1)
            erc_bit(rc, &m->is_rep_g1[st], 0);
        else {
            erc_bit(rc, &m->is_rep_g1[st], 1);
            erc_bit(rc, &m->is_rep_g2[st], r == 3);
            if (r == 3)
                m->rep[3] = m->rep[2];
            m->rep[2] = m->rep[1];
        }
        m->rep[1] = m->rep[0];
        m->rep[0] = d;
    }
    enc_len(rc, &m->rep_len, ps, len);
    m->state = st < LZMA_LIT_STATES ? 8 : 11;
    m->pos += len;
}

// ============================================================================
// Match finding
// ============================================================================

// The hash of the three bytes at p.
static inline uint32_t enc_hash(const uint8_t *p) {
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    return (v * 2654435761u) >> (32 - ENC_HASH_BITS); // Fibonacci hashing: the top bits mix all three
}

// Bytes in common between a and b, at most `max`.
static inline uint32_t common(const uint8_t *a, const uint8_t *b, uint32_t max) {
    uint32_t n = 0;
    while (n < max && a[n] == b[n])
        n++;
    return n;
}

// Enter positions [*next, upto] into the hash chains.
static void mf_insert_upto(codec_xz_state_t *st, size_t *next, size_t upto, size_t len) {
    for (; *next <= upto && *next + 3 <= len; (*next)++) {
        uint32_t h = enc_hash(st->m.dst + *next);
        st->prev[*next] = st->head[h];
        st->head[h] = (int32_t)*next;
    }
    if (*next <= upto)
        *next = upto + 1; // the last two bytes start no 3-byte prefix
}

// The longest match at `pos` among the earlier positions sharing its hash
// (pos itself not yet entered), searched `depth` deep and stopping at
// `nice` bytes.  Returns its length (0 below 3); *dist gets its distance,
// 0-based.
static uint32_t mf_find(const codec_xz_state_t *st, size_t pos, size_t len, int depth, uint32_t nice, uint32_t *dist) {
    if (pos + 3 > len)
        return 0;
    const uint8_t *in = st->m.dst;
    uint32_t max = len - pos < ENC_MATCH_MAX ? (uint32_t)(len - pos) : ENC_MATCH_MAX;
    uint32_t best = 0;
    int32_t c = st->head[enc_hash(in + pos)];
    for (; c >= 0 && depth-- > 0; c = st->prev[c]) {
        const uint8_t *q = in + c;
        if (q[best] != in[pos + best] || q[0] != in[pos])
            continue;
        uint32_t n = common(q, in + pos, max);
        if (n > best) {
            best = n;
            *dist = (uint32_t)(pos - (size_t)c - 1);
            if (n >= nice || n == max)
                break; // long enough, or as long as the input allows (q[best] would read past it)
        }
    }
    return best >= 3 ? best : 0;
}

// The longest match at one of the repeat distances; *r gets which.
static uint32_t rep_find(const lzma_t *m, size_t pos, size_t len, unsigned *r) {
    uint32_t max = len - pos < ENC_MATCH_MAX ? (uint32_t)(len - pos) : ENC_MATCH_MAX, best = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (m->rep[i] >= pos)
            continue;
        uint32_t n = common(m->dst + pos - m->rep[i] - 1, m->dst + pos, max);
        if (n > best) {
            best = n;
            *r = i;
        }
    }
    return best >= 2 ? best : 0;
}

// Whether a new match of `len` bytes `dist` back is worth its distance
// bits: a short match far back costs more than its bytes as literals.
static bool match_worth(uint32_t len, uint32_t dist) {
    return len >= 5 || (len == 4 && dist < (1u << 18)) || (len == 3 && dist < (1u << 12));
}

// ============================================================================
// Chunks and the container
// ============================================================================

typedef struct {
    int depth; // hash-chain depth
    uint32_t nice; // a match this long ends the search
    bool lazy; // try one byte later before taking a match
} enc_params_t;

// Parameters by level, 1..9.
static enc_params_t params_for(int level) {
    static const enc_params_t k[10] = {
        {0,    0,   false},
        {4,    16,  false},
        {8,    32,  true },
        {16,   32,  true },
        {32,   64,  true },
        {64,   64,  true },
        {128,  128, true },
        {256,  273, true },
        {512,  273, true },
        {1024, 273, true },
    };
    return k[level < 1 ? 1 : level > 9 ? 9 : level];
}

// Append `n` bytes to the output; false when they do not fit.
static bool put(uint8_t *out, size_t cap, size_t *at, const void *p, size_t n) {
    if (n > cap - *at)
        return false;
    memcpy(out + *at, p, n);
    *at += n;
    return true;
}

// Append a variable-length integer.
static bool put_varint(uint8_t *out, size_t cap, size_t *at, uint64_t v) {
    uint8_t b[10];
    size_t n = 0;
    do {
        b[n] = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v)
            b[n] |= 0x80;
        n++;
    } while (v);
    return put(out, cap, at, b, n);
}

// Little-endian 32/64-bit stores.
static void wr_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void wr_le64(uint8_t *p, uint64_t v) {
    wr_le32(p, (uint32_t)v);
    wr_le32(p + 4, (uint32_t)(v >> 32));
}

// The LZMA2 dictionary-size byte for the smallest size covering `len`:
// sizes run 4 KiB, 6 KiB, 8 KiB, 12 KiB, ... (2^n and 3 * 2^(n-1)).
static uint8_t dict_byte(size_t len) {
    for (unsigned b = 0; b < 40; b++) {
        uint64_t size = (uint64_t)(2 | (b & 1)) << (b / 2 + 11);
        if (size >= len)
            return (uint8_t)b;
    }
    return 40;
}

size_t codec_xz_bound(size_t len) {
    // Stream header and footer, block header, check, index; per chunk a
    // header of up to six bytes, every chunk holding at least ~60 KiB.
    return len + 6 * (len / 32768 + 2) + 128;
}

codec_xz_state_t *codec_xz_state_new(size_t max_len) {
    codec_xz_state_t *st = calloc(1, sizeof(*st));
    if (!st)
        return NULL;
    st->max_len = max_len;
    st->head = malloc(ENC_HASH_SIZE * sizeof(int32_t));
    st->prev = malloc((max_len ? max_len : 1) * sizeof(int32_t));
    st->pack = malloc(ENC_CHUNK_PACK + ENC_SYMBOL_MAX * 2);
    if (!st->head || !st->prev || !st->pack || codec_lzma_set_props(&st->m, ENC_PROPS, LZMA2_MAX_LCLP) != 0) {
        codec_xz_state_free(st);
        return NULL;
    }
    return st;
}

void codec_xz_state_free(codec_xz_state_t *st) {
    if (!st)
        return;
    free(st->head);
    free(st->prev);
    free(st->pack);
    free(st->m.lit);
    free(st);
}

// The LZMA2 stream for in[0..len) into out[*at..cap).
static bool lzma2_encode(codec_xz_state_t *st, size_t len, enc_params_t prm, uint8_t *out, size_t cap, size_t *at) {
    lzma_t *m = &st->m;
    const uint8_t *in = m->dst;
    for (size_t i = 0; i < ENC_HASH_SIZE; i++)
        st->head[i] = -1;
    m->pos = 0;
    size_t next_ins = 0; // positions below this are in the hash chains
    bool dict_reset = false, props_set = false, state_reset = true;
    // A match found one position ahead (lazy matching), kept for its turn.
    size_t ahead_pos = SIZE_MAX;
    uint32_t ahead_len = 0, ahead_dist = 0;

    while (m->pos < len) {
        size_t start = m->pos;
        if (state_reset)
            codec_lzma_reset_state(m);
        enc_rc_t rc;
        erc_init(&rc, st->pack, ENC_CHUNK_PACK + ENC_SYMBOL_MAX * 2);
        // Symbols until the input ends or the chunk is full.
        while (m->pos < len && erc_pending(&rc) + ENC_SYMBOL_MAX <= ENC_CHUNK_PACK &&
               m->pos - start + ENC_MATCH_MAX <= ENC_CHUNK_RAW) {
            size_t pos = m->pos;
            uint32_t mlen, mdist = 0;
            if (ahead_pos == pos) {
                mlen = ahead_len;
                mdist = ahead_dist;
            } else
                mlen = mf_find(st, pos, len, prm.depth, prm.nice, &mdist);
            mf_insert_upto(st, &next_ins, pos, len);
            if (mlen && !match_worth(mlen, mdist))
                mlen = 0;
            unsigned r = 0;
            uint32_t rlen = rep_find(m, pos, len, &r);
            // A repeat costs no distance: take it unless the new match is
            // clearly longer, and the more so the farther back it is.
            if (rlen && (rlen + 1 >= mlen || (rlen + 2 >= mlen && mdist >= (1u << 9)) ||
                         (rlen + 3 >= mlen && mdist >= (1u << 15)))) {
                mf_insert_upto(st, &next_ins, pos + rlen - 1, len);
                enc_rep(m, &rc, r, rlen);
                continue;
            }
            if (mlen && prm.lazy && mlen < prm.nice && pos + 1 < len) {
                // One byte later: if a longer match starts there, this byte
                // goes out on its own and that match takes its turn.
                uint32_t d2 = 0, l2 = mf_find(st, pos + 1, len, prm.depth, prm.nice, &d2);
                if (l2 && !match_worth(l2, d2))
                    l2 = 0;
                if (l2 > mlen + 1 || (l2 == mlen + 1 && d2 < mdist)) {
                    ahead_pos = pos + 1;
                    ahead_len = l2;
                    ahead_dist = d2;
                    mlen = 0;
                }
            }
            if (mlen) {
                mf_insert_upto(st, &next_ins, pos + mlen - 1, len);
                enc_match(m, &rc, mdist, mlen);
            } else if (m->rep[0] < pos && in[pos] == in[pos - m->rep[0] - 1])
                enc_rep(m, &rc, 0, 1); // the short repeat
            else
                enc_literal(m, &rc);
        }
        erc_flush(&rc);
        size_t unpacked = m->pos - start, packed = rc.pos;
        if (packed >= unpacked) {
            // No gain: stored instead.  The decoder's model never saw these
            // symbols, so both reset theirs before the next coded chunk.
            for (size_t o = start; o < m->pos; o += ENC_STORED_MAX) {
                size_t n = m->pos - o < ENC_STORED_MAX ? m->pos - o : ENC_STORED_MAX;
                uint8_t h[3] = {dict_reset ? 0x02 : 0x01, (uint8_t)((n - 1) >> 8), (uint8_t)(n - 1)};
                if (!put(out, cap, at, h, 3) || !put(out, cap, at, in + o, n))
                    return false;
                dict_reset = true;
            }
            state_reset = true;
            continue;
        }
        // Control: what this chunk resets (3 dictionary + properties +
        // state, 2 properties + state, 1 state, 0 nothing), then the sizes.
        unsigned reset = !dict_reset ? 3 : !props_set ? 2 : state_reset ? 1 : 0;
        uint8_t h[6] = {(uint8_t)(0x80 | reset << 5 | ((unpacked - 1) >> 16)),
                        (uint8_t)((unpacked - 1) >> 8),
                        (uint8_t)(unpacked - 1),
                        (uint8_t)((packed - 1) >> 8),
                        (uint8_t)(packed - 1),
                        ENC_PROPS};
        if (!put(out, cap, at, h, reset >= 2 ? 6 : 5) || !put(out, cap, at, st->pack, packed))
            return false;
        dict_reset = props_set = true;
        state_reset = false;
    }
    static const uint8_t end = 0x00;
    return put(out, cap, at, &end, 1);
}

int64_t codec_xz_compress(codec_xz_state_t *st, const uint8_t *in, size_t len, uint8_t *out, size_t cap, int level) {
    if ((!in && len) || !out)
        return CODEC_ERR_ARG;
    codec_xz_state_t *own = NULL;
    if (!st || st->max_len < len) {
        own = st = codec_xz_state_new(len);
        if (!st)
            return CODEC_ERR_NOMEM;
    }
    st->m.dst = (uint8_t *)(uintptr_t)in; // read only: the model's dictionary
    st->m.cap = len;
    size_t at = 0;
    bool ok = true;

    // Stream header: magic, flags (the check type), their CRC-32.
    uint8_t sh[XZ_HEADER_SIZE];
    memcpy(sh, codec_xz_magic, 6);
    sh[6] = 0;
    sh[7] = ENC_CHECK_CRC64;
    wr_le32(sh + 8, codec_crc32(0, sh + 6, 2));
    ok = put(out, cap, &at, sh, sizeof(sh));

    // Block header: size, flags (one filter, no sizes), LZMA2 with its
    // dictionary-size byte, padding, CRC-32.
    size_t block = at;
    uint8_t bh[12] = {12 / 4 - 1, 0x00, XZ_FILTER_LZMA2, 0x01, dict_byte(len), 0, 0, 0};
    wr_le32(bh + 8, codec_crc32(0, bh, 8));
    ok = ok && put(out, cap, &at, bh, sizeof(bh));

    ok = ok && lzma2_encode(st, len, params_for(level), out, cap, &at);
    size_t unpadded = at - block; // the block header and data, before padding
    static const uint8_t zeros[4] = {0};
    ok = ok && put(out, cap, &at, zeros, (4 - (at & 3)) & 3);
    uint8_t ck[8];
    wr_le64(ck, codec_crc64(0, in, len));
    ok = ok && put(out, cap, &at, ck, sizeof(ck));
    unpadded += sizeof(ck);

    // Index: one record (unpadded block size, decoded size), padding, CRC-32.
    size_t index = at;
    static const uint8_t one[2] = {0x00, 0x01}; // the indicator, one record
    ok = ok && put(out, cap, &at, one, 2) && put_varint(out, cap, &at, unpadded) && put_varint(out, cap, &at, len);
    ok = ok && put(out, cap, &at, zeros, (4 - ((at - index) & 3)) & 3);
    uint8_t crc[4];
    if (ok) {
        wr_le32(crc, codec_crc32(0, out + index, at - index));
        ok = put(out, cap, &at, crc, 4);
    }

    // Footer: CRC-32 of what follows, the index size in fours less one,
    // the stream flags, "YZ".
    uint8_t sf[XZ_HEADER_SIZE];
    wr_le32(sf + 4, (uint32_t)((at - index) / 4 - 1));
    sf[8] = 0;
    sf[9] = ENC_CHECK_CRC64;
    sf[10] = 'Y';
    sf[11] = 'Z';
    wr_le32(sf, codec_crc32(0, sf + 4, 6));
    ok = ok && put(out, cap, &at, sf, sizeof(sf));

    st->m.dst = NULL;
    codec_xz_state_free(own);
    return ok ? (int64_t)at : CODEC_ERR_OVERFLOW;
}
