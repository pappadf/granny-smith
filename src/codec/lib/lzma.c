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
// The whole output buffer is the dictionary: a one-shot decode never needs
// a window of its own, so a match may reach anywhere back to the last
// dictionary reset.
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
    const uint8_t *p;
    size_t len, pos; // the coded bytes and the next one to read
    uint32_t range, code;
    bool over; // a byte past the end was needed
} lzma_rc_t;

// The next coded byte; zero, and the overrun noted, past the end.
static inline uint8_t rc_byte(lzma_rc_t *rc) {
    if (rc->pos < rc->len)
        return rc->p[rc->pos++];
    rc->over = true;
    return 0;
}

// Start decoding src[0..len): a zero byte, then the 32-bit code.
static int rc_init(lzma_rc_t *rc, const uint8_t *src, size_t len) {
    *rc = (lzma_rc_t){.p = src, .len = len, .range = 0xFFFFFFFFu};
    if (len < 5)
        return CODEC_ERR_TRUNCATED;
    if (rc_byte(rc) != 0)
        return CODEC_ERR_DATA;
    for (int i = 0; i < 4; i++)
        rc->code = (rc->code << 8) | rc_byte(rc);
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

// Decode until the output reaches `limit`, or -- with `marker` -- until the
// end marker (then reaching `limit` first is an overflow).  Returns 0, 1
// when the end marker was seen, or a negative code.
static int lzma_run(lzma_t *z, lzma_rc_t *rc, size_t limit, bool marker) {
    unsigned pb_mask = (1u << z->pb) - 1, lp_mask = (1u << z->lp) - 1;
    for (;;) {
        if (rc->over)
            return CODEC_ERR_TRUNCATED;
        if (z->pos == limit && !marker)
            return 0;
        size_t rel = z->pos - z->dict_start; // position since the dictionary reset
        unsigned ps = (unsigned)rel & pb_mask;
        unsigned st = z->state;

        if (!rc_bit(rc, &z->is_match[st][ps])) {
            // A literal, coded under the previous byte's top lc bits and
            // the position's low lp bits.
            if (z->pos >= limit)
                return CODEC_ERR_OVERFLOW;
            unsigned prev = rel ? z->dst[z->pos - 1] : 0;
            lzma_prob_t *probs =
                z->lit + 0x300 * (size_t)((((unsigned)rel & lp_mask) << z->lc) + (prev >> (8 - z->lc)));
            uint32_t sym = 1;
            if (st >= LZMA_LIT_STATES) {
                // After a match: follow the byte at rep0 while it agrees.
                if (z->rep[0] >= rel)
                    return CODEC_ERR_DATA;
                unsigned mb = z->dst[z->pos - z->rep[0] - 1];
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
            z->dst[z->pos++] = (uint8_t)sym;
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
                    return CODEC_ERR_TRUNCATED;
                return marker ? 1 : CODEC_ERR_DATA; // the end marker
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
                        return CODEC_ERR_DATA;
                    if (z->pos >= limit)
                        return CODEC_ERR_OVERFLOW;
                    z->dst[z->pos] = z->dst[z->pos - z->rep[0] - 1];
                    z->pos++;
                    z->state = st < LZMA_LIT_STATES ? 9 : 11;
                    continue;
                }
            } else {
                // rep1..rep3 moves to the front.
                uint32_t d;
                if (!rc_bit(rc, &z->is_rep_g1[st]))
                    d = z->rep[1];
                else {
                    if (!rc_bit(rc, &z->is_rep_g2[st]))
                        d = z->rep[2];
                    else {
                        d = z->rep[3];
                        z->rep[3] = z->rep[2];
                    }
                    z->rep[2] = z->rep[1];
                }
                z->rep[1] = z->rep[0];
                z->rep[0] = d;
            }
            len = lzma_len(rc, &z->rep_len, ps);
            z->state = st < LZMA_LIT_STATES ? 8 : 11;
        }
        if (rc->over)
            return CODEC_ERR_TRUNCATED;
        if (z->rep[0] >= rel)
            return CODEC_ERR_DATA; // reaches before the dictionary
        if (len > limit - z->pos)
            return marker ? CODEC_ERR_OVERFLOW : CODEC_ERR_DATA;
        // Byte by byte: the source may overlap what is being written.
        const uint8_t *from = z->dst + z->pos - z->rep[0] - 1;
        for (uint32_t i = 0; i < len; i++)
            z->dst[z->pos + i] = from[i];
        z->pos += len;
    }
}

// ============================================================================
// LZMA2
// ============================================================================

// Decode an LZMA2 stream from src[0..len) into the model's output; *used
// gets the bytes it occupied, its end byte included.
static int64_t lzma2_run(lzma_t *z, const uint8_t *src, size_t len, size_t *used) {
    size_t at = 0;
    bool need_dict = true, need_props = true; // the first chunk sets both
    for (;;) {
        if (at >= len)
            return CODEC_ERR_TRUNCATED;
        unsigned ctrl = src[at++];
        if (ctrl == 0) {
            *used = at;
            return 0;
        }
        if (ctrl == 1 || ctrl == 2) {
            // Stored: a 16-bit size - 1, then the bytes.
            if (ctrl == 1) {
                z->dict_start = z->pos;
                need_dict = false;
            } else if (need_dict)
                return CODEC_ERR_DATA;
            if (len - at < 2)
                return CODEC_ERR_TRUNCATED;
            size_t n = codec_be16(src + at) + 1u;
            at += 2;
            if (n > len - at)
                return CODEC_ERR_TRUNCATED;
            if (n > z->cap - z->pos)
                return CODEC_ERR_OVERFLOW;
            memcpy(z->dst + z->pos, src + at, n);
            z->pos += n;
            at += n;
            continue;
        }
        if (ctrl < 0x80)
            return CODEC_ERR_DATA;
        // LZMA: sizes, maybe properties, then the range-coded bytes.
        unsigned reset = (ctrl >> 5) & 3; // 0 none, 1 state, 2 + props, 3 + dictionary
        if (len - at < 4)
            return CODEC_ERR_TRUNCATED;
        size_t unpacked = ((size_t)(ctrl & 0x1F) << 16) + codec_be16(src + at) + 1u;
        size_t packed = codec_be16(src + at + 2) + 1u;
        at += 4;
        if (reset == 3) {
            z->dict_start = z->pos;
            need_dict = false;
        } else if (need_dict)
            return CODEC_ERR_DATA;
        if (reset >= 2) {
            if (at >= len)
                return CODEC_ERR_TRUNCATED;
            int rc = codec_lzma_set_props(z, src[at++], LZMA2_MAX_LCLP);
            if (rc)
                return rc;
            need_props = false;
        } else if (need_props)
            return CODEC_ERR_DATA;
        if (reset >= 1)
            codec_lzma_reset_state(z);
        if (packed > len - at)
            return CODEC_ERR_TRUNCATED;
        if (unpacked > z->cap - z->pos)
            return CODEC_ERR_OVERFLOW;
        lzma_rc_t rc;
        int r = rc_init(&rc, src + at, packed);
        if (r == 0)
            r = lzma_run(z, &rc, z->pos + unpacked, false);
        if (r < 0)
            return r;
        if (rc.pos != packed)
            return CODEC_ERR_DATA; // the chunk's sizes and its data disagree
        at += packed;
    }
}

// ============================================================================
// .xz
// ============================================================================

const uint8_t codec_xz_magic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};

// A variable-length integer (7 bits a byte, low first) at src[*at..end).
static int xz_varint(const uint8_t *src, size_t end, size_t *at, uint64_t *v) {
    *v = 0;
    for (int i = 0; i < 9; i++) {
        if (*at >= end)
            return CODEC_ERR_TRUNCATED;
        uint8_t b = src[(*at)++];
        *v |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80))
            return (b == 0 && i > 0) ? CODEC_ERR_DATA : 0; // no redundant zero bytes
    }
    return CODEC_ERR_DATA;
}

// Bytes in the check field for check type `t`.
static size_t xz_check_size(unsigned t) {
    return t == 0 ? 0 : (size_t)4 << ((t - 1) / 3);
}

// Decode one block at src[*at..len); `check` is the stream's check type.
static int64_t xz_block(lzma_t *z, const uint8_t *src, size_t len, size_t *at, unsigned check) {
    size_t start = *at;
    size_t hsize = ((size_t)src[start] + 1) * 4;
    if (hsize > len - start)
        return CODEC_ERR_TRUNCATED;
    if (codec_crc32(0, src + start, hsize - 4) != codec_le32(src + start + hsize - 4))
        return CODEC_ERR_CHECKSUM;
    size_t hend = start + hsize - 4; // the header's fields end before its CRC
    size_t p = start + 1;
    unsigned flags = src[p++];
    if (flags & 0x3C)
        return CODEC_ERR_UNSUPPORTED; // reserved flags
    uint64_t csize = UINT64_MAX, usize = UINT64_MAX;
    int rc;
    if ((flags & 0x40) && (rc = xz_varint(src, hend, &p, &csize)) != 0)
        return rc;
    if ((flags & 0x80) && (rc = xz_varint(src, hend, &p, &usize)) != 0)
        return rc;
    // One filter, LZMA2; anything else (BCJ, delta) is not implemented.
    if ((flags & 3) != 0)
        return CODEC_ERR_UNSUPPORTED;
    uint64_t id = 0, psize = 0;
    if ((rc = xz_varint(src, hend, &p, &id)) != 0 || (rc = xz_varint(src, hend, &p, &psize)) != 0)
        return rc;
    if (id != XZ_FILTER_LZMA2)
        return CODEC_ERR_UNSUPPORTED;
    if (psize != 1 || p >= hend || (src[p] & 0x3F) > 40)
        return CODEC_ERR_DATA; // one byte: the dictionary size code
    p++;
    for (; p < hend; p++)
        if (src[p])
            return CODEC_ERR_DATA; // header padding must be zero

    size_t data = start + hsize, out0 = z->pos, used = 0;
    int64_t r = lzma2_run(z, src + data, len - data, &used);
    if (r < 0)
        return r;
    if ((csize != UINT64_MAX && csize != used) || (usize != UINT64_MAX && usize != z->pos - out0))
        return CODEC_ERR_DATA;
    p = data + used;
    while (p & 3) { // block padding, counted from the stream start (offset 0 here)
        if (p >= len)
            return CODEC_ERR_TRUNCATED;
        if (src[p++])
            return CODEC_ERR_DATA;
    }
    size_t csz = xz_check_size(check);
    if (csz > len - p)
        return CODEC_ERR_TRUNCATED;
    if (check == 1 && codec_crc32(0, z->dst + out0, z->pos - out0) != codec_le32(src + p))
        return CODEC_ERR_CHECKSUM;
    if (check == 4 && codec_crc64(0, z->dst + out0, z->pos - out0) != codec_le64(src + p))
        return CODEC_ERR_CHECKSUM;
    *at = p + csz;
    return 0;
}

// Skip and check the index at src[*at..len) and the stream footer after it.
static int xz_index_footer(const uint8_t *src, size_t len, size_t *at, const uint8_t *flags) {
    size_t start = *at, p = start + 1; // past the zero indicator
    uint64_t n = 0, v = 0;
    int rc = xz_varint(src, len, &p, &n);
    if (rc)
        return rc;
    for (uint64_t i = 0; i < n; i++) {
        if ((rc = xz_varint(src, len, &p, &v)) != 0 || (rc = xz_varint(src, len, &p, &v)) != 0)
            return rc;
    }
    while ((p - start) & 3) {
        if (p >= len)
            return CODEC_ERR_TRUNCATED;
        if (src[p++])
            return CODEC_ERR_DATA;
    }
    if (len - p < 4 + XZ_HEADER_SIZE)
        return CODEC_ERR_TRUNCATED;
    if (codec_crc32(0, src + start, p - start) != codec_le32(src + p))
        return CODEC_ERR_CHECKSUM;
    p += 4;
    // Footer: CRC-32 of (backward size, flags), backward size, flags, "YZ".
    const uint8_t *f = src + p;
    if (f[10] != 'Y' || f[11] != 'Z' || f[8] != flags[0] || f[9] != flags[1])
        return CODEC_ERR_DATA;
    if (codec_crc32(0, f + 4, 6) != codec_le32(f))
        return CODEC_ERR_CHECKSUM;
    if (((uint64_t)codec_le32(f + 4) + 1) * 4 != p - start)
        return CODEC_ERR_DATA; // backward size names the index
    *at = p + XZ_HEADER_SIZE;
    return 0;
}

// Decode one stream at src[*at..len).
static int64_t xz_stream(lzma_t *z, const uint8_t *src, size_t len, size_t *at) {
    size_t p = *at;
    if (len - p < XZ_HEADER_SIZE)
        return CODEC_ERR_TRUNCATED;
    if (memcmp(src + p, codec_xz_magic, sizeof(codec_xz_magic)) != 0)
        return CODEC_ERR_DATA;
    const uint8_t *flags = src + p + 6;
    if (codec_crc32(0, flags, 2) != codec_le32(src + p + 8))
        return CODEC_ERR_CHECKSUM;
    if (flags[0] != 0 || (flags[1] & 0xF0))
        return CODEC_ERR_UNSUPPORTED;
    unsigned check = flags[1] & 0x0F;
    p += XZ_HEADER_SIZE;
    // Blocks are counted from the stream's start for their padding, so
    // decode each with src rebased on it.
    const uint8_t *base = src + *at;
    size_t blen = len - *at, q = p - *at;
    for (;;) {
        if (q >= blen)
            return CODEC_ERR_TRUNCATED;
        if (base[q] == 0)
            break; // the index
        int64_t r = xz_block(z, base, blen, &q, check);
        if (r < 0)
            return r;
    }
    int rc = xz_index_footer(base, blen, &q, flags);
    if (rc)
        return rc;
    *at += q;
    return 0;
}

int64_t codec_xz_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    if (!src || (!dst && cap))
        return CODEC_ERR_ARG;
    lzma_t *z = calloc(1, sizeof(*z));
    if (!z)
        return CODEC_ERR_NOMEM;
    z->dst = dst;
    z->cap = cap;
    size_t at = 0;
    int64_t rc = 0;
    for (;;) {
        rc = xz_stream(z, src, len, &at);
        if (rc < 0)
            break;
        // Stream padding: zeros in fours, then perhaps another stream.
        size_t pad = at;
        while (pad < len && src[pad] == 0)
            pad++;
        if (pad == len && ((pad - at) & 3) == 0)
            break;
        if (((pad - at) & 3) != 0 || len - pad < sizeof(codec_xz_magic) || memcmp(src + pad, codec_xz_magic, 6) != 0) {
            rc = CODEC_ERR_DATA; // something other than a stream follows
            break;
        }
        at = pad;
    }
    int64_t out = (int64_t)z->pos;
    free(z->lit);
    free(z);
    return rc < 0 ? rc : out;
}

// ============================================================================
// .lzma
// ============================================================================

#define LZMA_ALONE_HEADER 13 // properties, dictionary size, decoded size

int64_t codec_lzma_alone_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    if (!src || (!dst && cap))
        return CODEC_ERR_ARG;
    if (len < LZMA_ALONE_HEADER + 5)
        return CODEC_ERR_TRUNCATED;
    uint64_t size = codec_le64(src + 5);
    bool unknown = size == UINT64_MAX;
    if (!unknown && size > cap)
        return CODEC_ERR_OVERFLOW;
    lzma_t *z = calloc(1, sizeof(*z));
    if (!z)
        return CODEC_ERR_NOMEM;
    z->dst = dst;
    z->cap = cap;
    int rc = codec_lzma_set_props(z, src[0], 8 + 4);
    if (rc == 0) {
        codec_lzma_reset_state(z);
        lzma_rc_t r;
        rc = rc_init(&r, src + LZMA_ALONE_HEADER, len - LZMA_ALONE_HEADER);
        if (rc == 0)
            rc = lzma_run(z, &r, unknown ? cap : (size_t)size, unknown);
        if (rc == 0 && unknown)
            rc = CODEC_ERR_DATA; // unreachable: an unknown size ends at the marker
    }
    int64_t out = (int64_t)z->pos;
    free(z->lit);
    free(z);
    return rc < 0 ? rc : out;
}
