// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lzma.c
// LZMA decoder, with the LZMA2 chunking and the .xz container around it,
// and the legacy .lzma header (see codec.h).
//
// LZMA is LZ77 whose every decision is coded by a binary range coder with
// adaptive probabilities.  The decoder keeps
//   - a 12-state machine recording the recent mix of literals, matches,
//     repeated matches and short repeats, which selects the probabilities;
//   - the four most recent match distances (rep0..rep3);
//   - the probability models: is-match/is-rep flags, literal coders indexed
//     by the previous byte's top lc bits and the position's low lp bits,
//     length coders (2..273, in three ranges), and the distance coder (a
//     6-bit slot, then extra bits: modelled, direct or 4 aligned).
// Positions are counted from the last dictionary reset; pb low bits of the
// position select among the is-match and length models.
//
// The decoder's output ring is the dictionary: as large as the stream's
// dictionary size says, and no larger than its output when that is known
// (a one-shot decode of a small chunk allocates no 8 MiB window).  Symbols
// decode into it while it has room for the longest match; the range
// decoder reads the input directly, held to an LZMA2 chunk's size.
//
// LZMA2 wraps LZMA in chunks of at most 2 MiB decoded: a control byte says
// whether the chunk is stored or LZMA, and which of the dictionary, the
// state and the properties (lc/lp/pb) it resets.  Each LZMA chunk starts a
// fresh range coder and decodes exactly its stated size.
//
// The .xz container: a stream header (magic, check type, CRC-32), blocks
// (a header listing the filters, the LZMA2 data, padding to four bytes, the
// check of the decoded block), an index of the blocks and a stream footer.
// Streams may be concatenated, with zero padding in multiples of four.

#include "codec_internal.h"
#include "lzma_internal.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// Range decoder
// ============================================================================

typedef struct {
    codec_in_t *in;
    uint64_t left; // coded bytes it may still read (an LZMA2 chunk's), or UINT64_MAX
    uint32_t range, code;
    bool over; // a byte past its end was needed
} lzma_rc_t;

// The next coded byte; zero, and the overrun noted, past the end.
static inline uint8_t rc_byte(lzma_rc_t *rc) {
    if (rc->left == 0) {
        rc->over = true;
        return 0;
    }
    rc->left--;
    int b = codec_in_byte(rc->in);
    if (b < 0) {
        rc->over = true;
        return 0;
    }
    return (uint8_t)b;
}

// Start decoding `left` coded bytes of `in`: a zero byte, then the 32-bit
// code.
static int rc_init(lzma_rc_t *rc, codec_in_t *in, uint64_t left) {
    *rc = (lzma_rc_t){.in = in, .left = left, .range = 0xFFFFFFFFu};
    if (left < 5)
        return CODEC_ERR_DATA;
    if (rc_byte(rc) != 0)
        return rc->over ? CODEC_ERR_TRUNCATED : CODEC_ERR_DATA;
    for (int i = 0; i < 4; i++)
        rc->code = (rc->code << 8) | rc_byte(rc);
    if (rc->over)
        return CODEC_ERR_TRUNCATED;
    return rc->code == rc->range ? CODEC_ERR_DATA : 0;
}

// Keep the range at least 2^24, shifting in a byte when it drops below.
static inline void rc_normalize(lzma_rc_t *rc) {
    if (rc->range < LZMA_TOP) {
        rc->range <<= 8;
        rc->code = (rc->code << 8) | rc_byte(rc);
    }
}

// One bit under the adaptive probability *p (the chance of a 0).
static inline unsigned rc_bit(lzma_rc_t *rc, lzma_prob_t *p) {
    uint32_t bound = (rc->range >> LZMA_PROB_BITS) * *p;
    unsigned bit;
    if (rc->code < bound) {
        rc->range = bound;
        *p = (lzma_prob_t)(*p + (((1u << LZMA_PROB_BITS) - *p) >> LZMA_MOVE_BITS));
        bit = 0;
    } else {
        rc->range -= bound;
        rc->code -= bound;
        *p = (lzma_prob_t)(*p - (*p >> LZMA_MOVE_BITS));
        bit = 1;
    }
    rc_normalize(rc);
    return bit;
}

// `n` bits at a fixed probability of one half, most significant first.
static uint32_t rc_direct(lzma_rc_t *rc, int n) {
    uint32_t v = 0;
    while (n--) {
        rc->range >>= 1;
        unsigned bit = rc->code >= rc->range;
        if (bit)
            rc->code -= rc->range;
        v = (v << 1) | bit;
        rc_normalize(rc);
    }
    return v;
}

// An n-bit value coded MSB first down a binary tree of probabilities
// (probs[1 .. 2^n - 1]).
static uint32_t rc_tree(lzma_rc_t *rc, lzma_prob_t *probs, int n) {
    uint32_t m = 1;
    for (int i = 0; i < n; i++)
        m = (m << 1) | rc_bit(rc, &probs[m]);
    return m - (1u << n);
}

// An n-bit value coded LSB first down a binary tree (probs[1 ..]).
static uint32_t rc_tree_reverse(lzma_rc_t *rc, lzma_prob_t *probs, int n) {
    uint32_t m = 1, v = 0;
    for (int i = 0; i < n; i++) {
        unsigned bit = rc_bit(rc, &probs[m]);
        m = (m << 1) | bit;
        v |= (uint32_t)bit << i;
    }
    return v;
}

// ============================================================================
// The LZMA model
// ============================================================================

// Decode properties byte `d` (lc + 9 * (lp + 5 * pb)) into the model,
// allocating its literal coders.
int codec_lzma_set_props(lzma_t *z, unsigned d, unsigned max_lclp) {
    if (d >= 9 * 5 * 5)
        return CODEC_ERR_DATA;
    unsigned lc = d % 9, lp = (d / 9) % 5, pb = d / 45;
    if (lc + lp > max_lclp)
        return CODEC_ERR_DATA;
    size_t need = (size_t)0x300 << (lc + lp);
    if (need > z->lit_n) {
        lzma_prob_t *p = realloc(z->lit, need * sizeof(lzma_prob_t));
        if (!p)
            return CODEC_ERR_NOMEM;
        z->lit = p;
        z->lit_n = need;
    }
    z->lc = lc;
    z->lp = lp;
    z->pb = pb;
    return 0;
}

// Fill `n` probabilities with one half.
static void probs_init(lzma_prob_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        p[i] = LZMA_PROB_INIT;
}

// Reset every probability, the state and the repeat distances.
void codec_lzma_reset_state(lzma_t *z) {
    probs_init(z->lit, (size_t)0x300 << (z->lc + z->lp));
    probs_init(&z->is_match[0][0], sizeof(z->is_match) / sizeof(lzma_prob_t));
    probs_init(z->is_rep, LZMA_STATES);
    probs_init(z->is_rep_g0, LZMA_STATES);
    probs_init(z->is_rep_g1, LZMA_STATES);
    probs_init(z->is_rep_g2, LZMA_STATES);
    probs_init(&z->is_rep0_long[0][0], sizeof(z->is_rep0_long) / sizeof(lzma_prob_t));
    probs_init(&z->dist_slot[0][0], sizeof(z->dist_slot) / sizeof(lzma_prob_t));
    probs_init(z->dist_special, sizeof(z->dist_special) / sizeof(lzma_prob_t));
    probs_init(z->dist_align, sizeof(z->dist_align) / sizeof(lzma_prob_t));
    probs_init((lzma_prob_t *)&z->match_len, sizeof(lzma_len_t) / sizeof(lzma_prob_t));
    probs_init((lzma_prob_t *)&z->rep_len, sizeof(lzma_len_t) / sizeof(lzma_prob_t));
    z->state = 0;
    memset(z->rep, 0, sizeof(z->rep));
}

// ============================================================================
// The decoder: symbols
// ============================================================================

#define LZMA_BURST (LZMA_MATCH_MIN + 271) // the longest match: the most one symbol writes

typedef enum {
    X_STREAM, // .xz: the stream header (or stream padding, then another)
    X_BLOCK, // .xz: a block header, or the index
    X_CHUNK, // LZMA2: a chunk's control byte
    X_STORED, // LZMA2: a stored chunk's bytes
    X_LZMA, // LZMA2 or .lzma: coded symbols
    X_BLOCK_END, // .xz: block padding and check
    X_DONE
} xz_state_t;

typedef struct {
    lzma_t m; // the model
    lzma_rc_t rc;
    xz_state_t state;
    bool alone; // .lzma (no container, no LZMA2)
    bool marker; // .lzma of unknown size: ends at the end marker
    uint64_t dict_start; // ring.total at the last dictionary reset
    uint64_t chunk_end; // ring.total where the current chunk (or .lzma stream) ends
    bool need_dict, need_props; // LZMA2: the next chunk must reset them
    // .xz
    uint8_t flags[2]; // the stream flags (the footer repeats them)
    unsigned check; // the check type
    uint64_t block_start; // ring.total where the block's output began
    uint64_t block_in; // input offset where the block began
    size_t block_hsize; // its header's size
    uint32_t streams; // streams begun
    uint64_t csize, usize; // the block header's sizes, or UINT64_MAX
    uint64_t records; // blocks in this stream (the index must agree)
    uint64_t check_at; // ring.total the running check covers
    uint32_t crc32;
    uint64_t crc64;
    uint64_t dict_size; // the ring's history (the dictionary it serves)
} xz_t;

// A match length (LZMA_MATCH_MIN ..).
static uint32_t lzma_len(lzma_rc_t *rc, lzma_len_t *l, unsigned ps) {
    if (!rc_bit(rc, &l->choice))
        return LZMA_MATCH_MIN + rc_tree(rc, l->low[ps], 3);
    if (!rc_bit(rc, &l->choice2))
        return LZMA_MATCH_MIN + 8 + rc_tree(rc, l->mid[ps], 3);
    return LZMA_MATCH_MIN + 16 + rc_tree(rc, l->high, 8);
}

// A match distance (0-based) for a match of `len` bytes.
static uint32_t lzma_dist(lzma_t *z, lzma_rc_t *rc, uint32_t len) {
    unsigned ls = len - LZMA_MATCH_MIN < 3 ? len - LZMA_MATCH_MIN : 3;
    uint32_t slot = rc_tree(rc, z->dist_slot[ls], 6);
    if (slot < 4)
        return slot;
    // The slot gives the top two bits and how many follow.
    int extra = (int)(slot >> 1) - 1;
    uint32_t dist = (2 | (slot & 1)) << extra;
    if (slot < LZMA_END_POS_MODEL)
        return dist + rc_tree_reverse(rc, z->dist_special + dist - slot, extra);
    dist += rc_direct(rc, extra - LZMA_ALIGN_BITS) << LZMA_ALIGN_BITS;
    return dist + rc_tree_reverse(rc, z->dist_align, LZMA_ALIGN_BITS);
}

// Decode symbols into the ring until the chunk ends (CODEC_END), the ring
// has no room for another (CODEC_MORE), or -- .lzma of unknown size -- the
// end marker.
static int lzma_symbols(codec_dec_t *d, xz_t *x) {
    lzma_t *z = &x->m;
    lzma_rc_t *rc = &x->rc;
    codec_ring_t *r = &d->ring;
    unsigned pb_mask = (1u << z->pb) - 1, lp_mask = (1u << z->lp) - 1;
    for (;;) {
        if (rc->over)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "lzma: truncated stream");
        if (r->total == x->chunk_end && !x->marker)
            return CODEC_END;
        if (ring_room(r) < LZMA_BURST)
            return CODEC_MORE;
        uint64_t rel = r->total - x->dict_start; // position since the dictionary reset
        unsigned ps = (unsigned)rel & pb_mask;
        unsigned st = z->state;

        if (!rc_bit(rc, &z->is_match[st][ps])) {
            // A literal, coded under the previous byte's top lc bits and
            // the position's low lp bits.
            if (r->total >= x->chunk_end)
                return codec_fail(d, CODEC_ERR_OVERFLOW, "lzma: decodes past its size");
            unsigned prev = rel ? ring_back(r, 1) : 0;
            lzma_prob_t *probs =
                z->lit + 0x300 * (size_t)((((unsigned)rel & lp_mask) << z->lc) + (prev >> (8 - z->lc)));
            uint32_t sym = 1;
            if (st >= LZMA_LIT_STATES) {
                // After a match: follow the byte at rep0 while it agrees.
                if (z->rep[0] >= rel)
                    return codec_fail(d, CODEC_ERR_DATA, "lzma: distance before the dictionary");
                unsigned mb = ring_back(r, (size_t)z->rep[0] + 1);
                while (sym < 0x100) {
                    unsigned mbit = (mb >> 7) & 1;
                    mb <<= 1;
                    unsigned bit = rc_bit(rc, &probs[((1 + mbit) << 8) + sym]);
                    sym = (sym << 1) | bit;
                    if (bit != mbit)
                        break;
                }
            }
            while (sym < 0x100)
                sym = (sym << 1) | rc_bit(rc, &probs[sym]);
            ring_put(r, (uint8_t)sym);
            z->state = st < 4 ? 0 : st < 10 ? st - 3 : st - 6;
            continue;
        }

        uint32_t len;
        if (!rc_bit(rc, &z->is_rep[st])) {
            // A new match: the distances shift down.
            len = lzma_len(rc, &z->match_len, ps);
            uint32_t dist = lzma_dist(z, rc, len);
            if (dist == 0xFFFFFFFFu) {
                if (rc->over)
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "lzma: truncated stream");
                if (x->marker)
                    return CODEC_END;
                return codec_fail(d, CODEC_ERR_DATA, "lzma: unexpected end marker");
            }
            z->rep[3] = z->rep[2];
            z->rep[2] = z->rep[1];
            z->rep[1] = z->rep[0];
            z->rep[0] = dist;
            z->state = st < LZMA_LIT_STATES ? 7 : 10;
        } else {
            if (!rc_bit(rc, &z->is_rep_g0[st])) {
                if (!rc_bit(rc, &z->is_rep0_long[st][ps])) {
                    // A short repeat: one byte from rep0.
                    if (z->rep[0] >= rel)
                        return codec_fail(d, CODEC_ERR_DATA, "lzma: distance before the dictionary");
                    if (r->total >= x->chunk_end)
                        return codec_fail(d, CODEC_ERR_OVERFLOW, "lzma: decodes past its size");
                    ring_put(r, ring_back(r, (size_t)z->rep[0] + 1));
                    z->state = st < LZMA_LIT_STATES ? 9 : 11;
                    continue;
                }
            } else {
                // rep1..rep3 moves to the front.
                uint32_t dd;
                if (!rc_bit(rc, &z->is_rep_g1[st]))
                    dd = z->rep[1];
                else {
                    if (!rc_bit(rc, &z->is_rep_g2[st]))
                        dd = z->rep[2];
                    else {
                        dd = z->rep[3];
                        z->rep[3] = z->rep[2];
                    }
                    z->rep[2] = z->rep[1];
                }
                z->rep[1] = z->rep[0];
                z->rep[0] = dd;
            }
            len = lzma_len(rc, &z->rep_len, ps);
            z->state = st < LZMA_LIT_STATES ? 8 : 11;
        }
        if (rc->over)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "lzma: truncated stream");
        if ((uint64_t)z->rep[0] >= rel || (uint64_t)z->rep[0] >= x->dict_size)
            return codec_fail(d, CODEC_ERR_DATA, "lzma: distance before the dictionary");
        if (len > x->chunk_end - r->total)
            return codec_fail(d, x->marker ? CODEC_ERR_OVERFLOW : CODEC_ERR_DATA, "lzma: a match runs past its chunk");
        ring_copy(r, (size_t)z->rep[0] + 1, len);
    }
}

// ============================================================================
// The decoder: containers
// ============================================================================

#define XZ_DICT_MAX ((uint64_t)1 << 30) // the largest dictionary a ring is made for

const uint8_t codec_xz_magic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};

// The dictionary-size byte of an LZMA2 filter (or a .lzma header's size)
// as a ring: no larger than the output, when that is known.
static int dict_ring(codec_dec_t *d, xz_t *x, uint64_t dict) {
    if (d->out_len != CODEC_LEN_UNKNOWN && dict > d->out_len)
        dict = d->out_len;
    if (dict > XZ_DICT_MAX)
        return codec_fail(d, CODEC_ERR_UNSUPPORTED, "xz: dictionary too large");
    if (dict < 4096)
        dict = 4096;
    if (d->ring.buf && dict <= x->dict_size)
        return 0; // the ring serves it already
    // A larger dictionary for a later stream or block: a fresh ring, the
    // counts carried over (no history is needed across a dictionary reset).
    uint64_t total = d->ring.total;
    int rc = codec_ring_alloc(d, (size_t)dict, LZMA_BURST);
    if (rc)
        return codec_fail(d, rc, "xz: out of memory for the dictionary");
    d->ring.total = d->ring.drained = total;
    x->dict_size = dict;
    return 0;
}

// Fold the bytes decoded since the last call into the block check.
static void check_update(codec_dec_t *d, xz_t *x) {
    codec_ring_t *r = &d->ring;
    while (x->check_at < r->total) {
        size_t pending = (size_t)(r->total - x->check_at);
        size_t at = r->w >= pending ? r->w - pending : r->w + r->size - pending;
        size_t k = r->size - at < pending ? r->size - at : pending;
        if (x->check == 1)
            x->crc32 = codec_crc32(x->crc32, r->buf + at, k);
        else if (x->check == 4)
            x->crc64 = codec_crc64(x->crc64, r->buf + at, k);
        x->check_at += k;
    }
}

// Read `n` bytes, adding them to *crc (the index's running CRC-32).
static bool read_crc(codec_in_t *in, uint8_t *dst, size_t n, uint32_t *crc) {
    if (codec_in_read(in, dst, n) != n)
        return false;
    if (crc)
        *crc = codec_crc32(*crc, dst, n);
    return true;
}

// A variable-length integer (7 bits a byte, low first) from b[*at..end).
static int varint(const uint8_t *b, size_t end, size_t *at, uint64_t *v) {
    *v = 0;
    for (int i = 0; i < 9; i++) {
        if (*at >= end)
            return CODEC_ERR_DATA;
        uint8_t c = b[(*at)++];
        *v |= (uint64_t)(c & 0x7F) << (7 * i);
        if (!(c & 0x80))
            return (c == 0 && i > 0) ? CODEC_ERR_DATA : 0; // no redundant zero bytes
    }
    return CODEC_ERR_DATA;
}

// The same, read from the input with the bytes added to *crc.
static int varint_in(codec_in_t *in, uint64_t *v, uint32_t *crc) {
    *v = 0;
    for (int i = 0; i < 9; i++) {
        uint8_t c;
        if (!read_crc(in, &c, 1, crc))
            return CODEC_ERR_TRUNCATED;
        *v |= (uint64_t)(c & 0x7F) << (7 * i);
        if (!(c & 0x80))
            return (c == 0 && i > 0) ? CODEC_ERR_DATA : 0;
    }
    return CODEC_ERR_DATA;
}

// Bytes in the check field for check type `t`.
static size_t check_size(unsigned t) {
    return t == 0 ? 0 : (size_t)4 << ((t - 1) / 3);
}

// The stream header: magic, flags, their CRC-32.  `first` is the first
// byte when it has been read (stream padding ended at it), else -1.
static int stream_header(codec_dec_t *d, xz_t *x, int first) {
    uint8_t h[XZ_HEADER_SIZE];
    size_t at = 0;
    if (first >= 0)
        h[at++] = (uint8_t)first;
    if (codec_in_read(&d->in, h + at, sizeof(h) - at) != sizeof(h) - at)
        return codec_fail(d, CODEC_ERR_TRUNCATED, "xz: truncated stream header");
    if (memcmp(h, codec_xz_magic, sizeof(codec_xz_magic)) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "xz: bad magic");
    if (codec_crc32(0, h + 6, 2) != codec_le32(h + 8))
        return codec_fail(d, CODEC_ERR_CHECKSUM, "xz: stream header CRC mismatch");
    if (h[6] != 0 || (h[7] & 0xF0))
        return codec_fail(d, CODEC_ERR_UNSUPPORTED, "xz: unknown stream flags");
    memcpy(x->flags, h + 6, 2);
    x->streams++;
    x->check = h[7] & 0x0F;
    x->records = 0;
    x->state = X_BLOCK;
    return 0;
}

// A block header (its first byte read: its size), or the index and footer.
static int block_or_index(codec_dec_t *d, xz_t *x) {
    codec_in_t *in = &d->in;
    uint64_t at0 = codec_in_consumed(in);
    int first = codec_in_byte(in);
    if (first < 0)
        return codec_fail(d, CODEC_ERR_TRUNCATED, "xz: truncated stream");
    if (first == 0) {
        // The index: its records, padding, CRC-32; then the footer.
        uint32_t crc = codec_crc32(0, "\0", 1);
        uint64_t n, v1, v2;
        int rc = varint_in(in, &n, &crc);
        if (rc)
            return codec_fail(d, rc, "xz: bad index");
        if (n != x->records)
            return codec_fail(d, CODEC_ERR_DATA, "xz: the index does not list every block");
        for (uint64_t i = 0; i < n; i++)
            if ((rc = varint_in(in, &v1, &crc)) != 0 || (rc = varint_in(in, &v2, &crc)) != 0)
                return codec_fail(d, rc, "xz: bad index");
        while ((codec_in_consumed(in) - at0) & 3) {
            uint8_t z;
            if (!read_crc(in, &z, 1, &crc))
                return codec_fail(d, CODEC_ERR_TRUNCATED, "xz: truncated index");
            if (z)
                return codec_fail(d, CODEC_ERR_DATA, "xz: index padding is not zero");
        }
        uint64_t index_size = codec_in_consumed(in) - at0 + 4;
        uint8_t t[4 + XZ_HEADER_SIZE];
        if (codec_in_read(in, t, sizeof(t)) != sizeof(t))
            return codec_fail(d, CODEC_ERR_TRUNCATED, "xz: truncated index or footer");
        if (codec_le32(t) != crc)
            return codec_fail(d, CODEC_ERR_CHECKSUM, "xz: index CRC mismatch");
        // Footer: CRC-32 of (backward size, flags), backward size, flags, "YZ".
        const uint8_t *f = t + 4;
        if (f[10] != 'Y' || f[11] != 'Z' || f[8] != x->flags[0] || f[9] != x->flags[1])
            return codec_fail(d, CODEC_ERR_DATA, "xz: bad stream footer");
        if (codec_crc32(0, f + 4, 6) != codec_le32(f))
            return codec_fail(d, CODEC_ERR_CHECKSUM, "xz: stream footer CRC mismatch");
        if (((uint64_t)codec_le32(f + 4) + 1) * 4 != index_size)
            return codec_fail(d, CODEC_ERR_DATA, "xz: the footer's index size is wrong");
        x->state = X_STREAM;
        return 0;
    }
    size_t hsize = ((size_t)first + 1) * 4;
    uint8_t h[1024];
    h[0] = (uint8_t)first;
    if (codec_in_read(in, h + 1, hsize - 1) != hsize - 1)
        return codec_fail(d, CODEC_ERR_TRUNCATED, "xz: truncated block header");
    if (codec_crc32(0, h, hsize - 4) != codec_le32(h + hsize - 4))
        return codec_fail(d, CODEC_ERR_CHECKSUM, "xz: block header CRC mismatch");
    size_t end = hsize - 4, p = 1;
    unsigned flags = h[p++];
    if (flags & 0x3C)
        return codec_fail(d, CODEC_ERR_UNSUPPORTED, "xz: reserved block flags");
    x->csize = x->usize = UINT64_MAX;
    if ((flags & 0x40) && varint(h, end, &p, &x->csize) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "xz: bad block header");
    if ((flags & 0x80) && varint(h, end, &p, &x->usize) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "xz: bad block header");
    // One filter, LZMA2; anything else (BCJ, delta) is not implemented.
    uint64_t id = 0, psize = 0;
    if ((flags & 3) != 0)
        return codec_fail(d, CODEC_ERR_UNSUPPORTED, "xz: a filter chain other than LZMA2 alone");
    if (varint(h, end, &p, &id) != 0 || varint(h, end, &p, &psize) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "xz: bad block header");
    if (id != XZ_FILTER_LZMA2)
        return codec_fail(d, CODEC_ERR_UNSUPPORTED, "xz: a filter other than LZMA2");
    if (psize != 1 || p >= end || (h[p] & 0x3F) > 40)
        return codec_fail(d, CODEC_ERR_DATA, "xz: bad LZMA2 properties");
    unsigned db = h[p++] & 0x3F;
    for (; p < end; p++)
        if (h[p])
            return codec_fail(d, CODEC_ERR_DATA, "xz: block header padding is not zero");
    uint64_t dict = db == 40 ? 0xFFFFFFFFull : (uint64_t)(2 | (db & 1)) << (db / 2 + 11);
    int rc = dict_ring(d, x, dict);
    if (rc)
        return rc;
    x->block_start = x->check_at = d->ring.total;
    x->block_in = at0;
    x->block_hsize = hsize;
    x->crc32 = 0;
    x->crc64 = 0;
    x->need_dict = x->need_props = true;
    x->records++;
    x->state = X_CHUNK;
    return 0;
}

// An LZMA2 chunk's control byte and header.
static int chunk_header(codec_dec_t *d, xz_t *x) {
    codec_in_t *in = &d->in;
    int ctrl = codec_in_byte(in);
    if (ctrl < 0)
        return codec_fail(d, CODEC_ERR_TRUNCATED, "lzma2: truncated stream");
    if (ctrl == 0) {
        x->state = X_BLOCK_END;
        return 0;
    }
    uint8_t h[5];
    if (ctrl == 1 || ctrl == 2) {
        // Stored: a 16-bit size - 1, then the bytes.
        if (ctrl == 1) {
            x->dict_start = d->ring.total;
            x->need_dict = false;
        } else if (x->need_dict)
            return codec_fail(d, CODEC_ERR_DATA, "lzma2: no dictionary reset");
        if (codec_in_read(in, h, 2) != 2)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "lzma2: truncated chunk header");
        x->chunk_end = d->ring.total + codec_be16(h) + 1u;
        x->state = X_STORED;
        return 0;
    }
    if (ctrl < 0x80)
        return codec_fail(d, CODEC_ERR_DATA, "lzma2: bad control byte");
    // LZMA: sizes, maybe properties, then the range-coded bytes.
    unsigned reset = ((unsigned)ctrl >> 5) & 3; // 0 none, 1 state, 2 + props, 3 + dictionary
    if (codec_in_read(in, h, 4) != 4)
        return codec_fail(d, CODEC_ERR_TRUNCATED, "lzma2: truncated chunk header");
    uint64_t unpacked = ((uint64_t)(ctrl & 0x1F) << 16) + codec_be16(h) + 1u;
    uint64_t packed = codec_be16(h + 2) + 1u;
    if (reset == 3) {
        x->dict_start = d->ring.total;
        x->need_dict = false;
    } else if (x->need_dict)
        return codec_fail(d, CODEC_ERR_DATA, "lzma2: no dictionary reset");
    if (reset >= 2) {
        int pr = codec_in_byte(in);
        if (pr < 0)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "lzma2: truncated chunk header");
        int rc = codec_lzma_set_props(&x->m, (unsigned)pr, LZMA2_MAX_LCLP);
        if (rc)
            return codec_fail(d, rc, "lzma2: bad properties");
        x->need_props = false;
    } else if (x->need_props)
        return codec_fail(d, CODEC_ERR_DATA, "lzma2: no properties");
    if (reset >= 1)
        codec_lzma_reset_state(&x->m);
    int rc = rc_init(&x->rc, in, packed);
    if (rc)
        return codec_fail(d, rc, "lzma2: bad chunk");
    x->chunk_end = d->ring.total + unpacked;
    x->state = X_LZMA;
    return 0;
}

// Block padding and the check, against the block header's sizes.
static int block_end(codec_dec_t *d, xz_t *x) {
    codec_in_t *in = &d->in;
    check_update(d, x);
    uint64_t data = codec_in_consumed(in) - x->block_in - x->block_hsize; // the LZMA2 stream
    if (x->csize != UINT64_MAX && x->csize != data)
        return codec_fail(d, CODEC_ERR_DATA, "xz: block's compressed size differs from its header");
    if (x->usize != UINT64_MAX && x->usize != d->ring.total - x->block_start)
        return codec_fail(d, CODEC_ERR_DATA, "xz: block size differs from its header");
    while ((codec_in_consumed(in) - x->block_in) & 3) {
        int z = codec_in_byte(in);
        if (z < 0)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "xz: truncated block padding");
        if (z)
            return codec_fail(d, CODEC_ERR_DATA, "xz: block padding is not zero");
    }
    uint8_t c[64];
    size_t cs = check_size(x->check);
    if (codec_in_read(in, c, cs) != cs)
        return codec_fail(d, CODEC_ERR_TRUNCATED, "xz: truncated block check");
    if (x->check == 1 && codec_le32(c) != x->crc32)
        return codec_fail(d, CODEC_ERR_CHECKSUM, "xz: block CRC-32 mismatch");
    if (x->check == 4 && codec_le64(c) != x->crc64)
        return codec_fail(d, CODEC_ERR_CHECKSUM, "xz: block CRC-64 mismatch");
    x->state = X_BLOCK;
    return 0;
}

static int xz_fill(codec_dec_t *d) {
    xz_t *x = d->st;
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    int rc = 0;
    while (rc == 0) {
        switch (x->state) {
        case X_STREAM: {
            if (x->alone) {
                rc = 0;
                break;
            }
            // Stream padding (zeros, in fours), then another stream or the end.
            if (x->streams > 0) {
                uint64_t pad = 0;
                int b;
                while ((b = codec_in_byte(in)) == 0)
                    pad++;
                if (b < 0) {
                    if (pad & 3)
                        return codec_fail(d, CODEC_ERR_DATA, "xz: stream padding not a multiple of four");
                    x->state = X_DONE;
                    return CODEC_END;
                }
                if (pad & 3)
                    return codec_fail(d, CODEC_ERR_DATA, "xz: stream padding not a multiple of four");
                rc = stream_header(d, x, b);
            } else
                rc = stream_header(d, x, -1);
            break;
        }
        case X_BLOCK:
            // A block may bring a larger dictionary, and so a new ring:
            // only once what is in this one has been taken.
            if (r->total != r->drained)
                goto more;
            rc = block_or_index(d, x);
            break;
        case X_CHUNK:
            rc = chunk_header(d, x);
            break;
        case X_STORED: {
            size_t room = ring_room(r);
            while (r->total < x->chunk_end && room) {
                if (in->pos == in->len && !codec_in_refill(in))
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "lzma2: truncated stored chunk");
                size_t k = in->len - in->pos;
                if (k > x->chunk_end - r->total)
                    k = (size_t)(x->chunk_end - r->total);
                if (k > room)
                    k = room;
                ring_write(r, in->p + in->pos, k);
                in->pos += k;
                room -= k;
            }
            if (r->total < x->chunk_end)
                goto more;
            x->state = X_CHUNK;
            break;
        }
        case X_LZMA: {
            int s = lzma_symbols(d, x);
            if (s < 0)
                return s;
            if (s == CODEC_MORE)
                goto more;
            if (x->alone) {
                x->state = X_DONE;
                return CODEC_END;
            }
            if (x->rc.left != 0)
                return codec_fail(d, CODEC_ERR_DATA, "lzma2: a chunk's sizes and its data disagree");
            x->state = X_CHUNK;
            break;
        }
        case X_BLOCK_END:
            rc = block_end(d, x);
            break;
        case X_DONE:
            return CODEC_END;
        }
    }
    return rc;
more:
    if (!x->alone)
        check_update(d, x);
    return CODEC_MORE;
}

static void xz_free(codec_dec_t *d) {
    xz_t *x = d->st;
    free(x->m.lit);
    free(x);
    d->st = NULL;
}

static int xz_init(codec_dec_t *d) {
    xz_t *x = calloc(1, sizeof(*x));
    if (!x)
        return CODEC_ERR_NOMEM;
    d->st = x;
    x->state = X_STREAM;
    return 0; // the ring comes with the first block's dictionary
}

// ============================================================================
// .lzma
// ============================================================================

#define LZMA_ALONE_HEADER 13 // properties, dictionary size, decoded size

static int alone_fill(codec_dec_t *d) {
    xz_t *x = d->st;
    if (x->state == X_STREAM) {
        uint8_t h[LZMA_ALONE_HEADER];
        if (codec_in_read(&d->in, h, sizeof(h)) != sizeof(h))
            return codec_fail(d, CODEC_ERR_TRUNCATED, "lzma: truncated header");
        uint64_t size = codec_le64(h + 5);
        x->marker = size == UINT64_MAX;
        if (!x->marker && d->out_len != CODEC_LEN_UNKNOWN && size > d->out_len)
            return codec_fail(d, CODEC_ERR_OVERFLOW, "lzma: decodes past its size");
        int rc = codec_lzma_set_props(&x->m, h[0], 8 + 4);
        if (rc)
            return codec_fail(d, rc, "lzma: bad properties");
        codec_lzma_reset_state(&x->m);
        if ((rc = dict_ring(d, x, codec_le32(h + 1))) != 0)
            return rc;
        x->chunk_end = x->marker ? (d->out_len != CODEC_LEN_UNKNOWN ? d->out_len : UINT64_MAX) : size;
        if ((rc = rc_init(&x->rc, &d->in, UINT64_MAX)) != 0)
            return codec_fail(d, rc, "lzma: bad stream start");
        x->dict_start = 0;
        x->state = X_LZMA;
    }
    return xz_fill(d);
}

static int alone_init(codec_dec_t *d) {
    int rc = xz_init(d);
    if (rc == 0)
        ((xz_t *)d->st)->alone = true;
    return rc;
}

const codec_dec_ops_t codec_xz_dec = {xz_init, xz_fill, xz_free, false};
const codec_dec_ops_t codec_lzma_alone_dec = {alone_init, alone_fill, xz_free, false};
