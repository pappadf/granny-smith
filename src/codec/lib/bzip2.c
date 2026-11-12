// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// bzip2.c
// bzip2 decoder (see codec.h).
//
// A bzip2 stream is "BZh" and a level digit ('1'..'9', the block size in
// units of 100 000 bytes), then blocks, then an end-of-stream marker, all in
// one MSB-first bit stream:
//
//   block   48-bit magic 0x314159265359, 32-bit CRC of the block's output,
//           1 bit "randomised", 24-bit origin pointer, then
//           - the symbol map: 16 bits saying which 16-byte ranges occur,
//             and 16 bits for each range present, one per byte value;
//           - 3 bits: the number of Huffman tables (2..6);
//           - 15 bits: the number of selectors, then the selectors, each a
//             unary-coded move-to-front index of the table that codes the
//             next 50 symbols;
//           - each table's code lengths: a 5-bit start, then per symbol a
//             run of 1-bit "change" flags, each followed by +1 (0) or -1
//             (1), ended by a 0 flag;
//           - the symbols: RUNA/RUNB digits (bijective base-2 run lengths
//             of the move-to-front front byte), move-to-front indices + 1,
//             and the end-of-block symbol (the alphabet's last).
//   end     48-bit magic 0x177245385090, then the 32-bit combined CRC; the
//           stream then pads to a byte boundary.
//
// Undoing the stages in reverse: Huffman -> run lengths and move-to-front
// -> the inverse Burrows-Wheeler transform (from the origin pointer) -> the
// initial run-length coding, where four equal bytes are followed by a count
// of further copies (0..255).  A block is decoded whole into `tt` (no
// output yet), then emitted into the ring as far as it has room, so a block
// is resumable on the output side.
//
// `tt` holds one 32-bit word per block byte: the low byte is the symbol
// and, once the transform's links are built, the upper 24 bits the index of
// the next entry to visit (a block is at most 900 000 bytes).  The walk
// over those links is the decoder's bottleneck on large blocks: each step
// is a load from megabytes of table that depends on the one before.
// (Walking from both ends at once, to overlap the misses, needs a second
// table of reverse links, whose traffic costs more than it saves.)
//
// Huffman symbols decode through a 10-bit lookup table per table (codes up
// to 10 bits, nearly all of them), falling back to a walk over the code
// lengths for the rest.

#include "codec_internal.h"

#include <stdlib.h>

// ============================================================================
// Constants
// ============================================================================

#define BZ_BLOCK_MAGIC_HI 0x314159u // first half of the block magic
#define BZ_BLOCK_MAGIC_LO 0x265359u // second half
#define BZ_EOS_MAGIC_HI   0x177245u // first half of the end-of-stream magic
#define BZ_EOS_MAGIC_LO   0x385090u // second half

#define BZ_MAX_GROUPS    6 // Huffman tables per block
#define BZ_MIN_GROUPS    2
#define BZ_GROUP_SIZE    50 // symbols coded by one selector
#define BZ_MAX_ALPHA     258 // 256 bytes + RUNA/RUNB (EOB replaces one byte slot)
#define BZ_MAX_CODE_LEN  20 // longest code length the format allows
#define BZ_MAX_SELECTORS 18002 // enough for 900 000 symbols; more are read and ignored
#define BZ_RUNA          0
#define BZ_RUNB          1
#define BZ_FAST_BITS     10 // the lookup table's width
#define BZ_BURST         (4 + 255) // the most one step of the run-length decoding emits

// ============================================================================
// Types
// ============================================================================

// A canonical code: a lookup table on the next BZ_FAST_BITS bits (entry:
// symbol << 5 | length, 0 for a longer code), and, for those, the codes of
// length l being first[l] .. first[l]+count[l]-1 with their symbols in
// `sorted` from offset[l], in symbol order.
typedef struct {
    uint16_t fast[1 << BZ_FAST_BITS];
    uint32_t first[BZ_MAX_CODE_LEN + 1];
    uint16_t count[BZ_MAX_CODE_LEN + 1];
    uint16_t offset[BZ_MAX_CODE_LEN + 1];
    uint16_t sorted[BZ_MAX_ALPHA];
    int min_len, max_len;
} bz_huff_t;

typedef enum { B_STREAM, B_BLOCK, B_EMIT, B_DONE } bz_state_t;

typedef struct {
    bz_state_t state;
    uint32_t block_max; // the level's block size
    uint32_t *tt; // block symbols, then their links
    uint32_t combined; // the stream CRC so far
    uint32_t streams; // streams begun
    // The block being emitted.
    uint32_t want_crc; // the block's stored CRC
    uint32_t n, k; // its length and bytes visited
    uint32_t pos; // the transform's next entry
    int last, same; // the previous byte and how many in a row (run-length decoding)
    uint32_t crc; // over its emitted bytes
    uint64_t crc_at; // ring.total the block CRC covers
    uint8_t sel[BZ_MAX_SELECTORS]; // table index per 50-symbol group
    uint8_t lens[BZ_MAX_GROUPS][BZ_MAX_ALPHA];
    bz_huff_t huff[BZ_MAX_GROUPS];
} bz_t;

// ============================================================================
// Huffman tables
// ============================================================================

// Build the decoder for `n` symbols with lengths `lens` (each 1..20).
// -1 when the lengths over-subscribe the code space.
static int huff_build(bz_huff_t *h, const uint8_t *lens, int n) {
    memset(h->count, 0, sizeof(h->count));
    memset(h->fast, 0, sizeof(h->fast));
    h->min_len = BZ_MAX_CODE_LEN;
    h->max_len = 1;
    for (int i = 0; i < n; i++) {
        h->count[lens[i]]++;
        if (lens[i] < h->min_len)
            h->min_len = lens[i];
        if (lens[i] > h->max_len)
            h->max_len = lens[i];
    }
    uint32_t code = 0; // the first code of the current length
    uint16_t idx = 0; // the first sorted slot of the current length
    for (int l = 1; l <= BZ_MAX_CODE_LEN; l++) {
        h->first[l] = code;
        h->offset[l] = idx;
        code += h->count[l];
        idx = (uint16_t)(idx + h->count[l]);
        if (code > (1u << l))
            return -1; // more codes of this length than fit
        code <<= 1;
    }
    // Symbols in canonical order (by length, then value); the short codes
    // into the lookup table, each over every index it prefixes.
    uint16_t fill[BZ_MAX_CODE_LEN + 1];
    memcpy(fill, h->offset, sizeof(fill));
    for (int i = 0; i < n; i++)
        h->sorted[fill[lens[i]]++] = (uint16_t)i;
    for (int l = 1; l <= BZ_FAST_BITS; l++)
        for (int j = 0; j < h->count[l]; j++) {
            uint32_t c = h->first[l] + (uint32_t)j, sym = h->sorted[h->offset[l] + j];
            uint32_t lo = c << (BZ_FAST_BITS - l), hi = lo + (1u << (BZ_FAST_BITS - l));
            for (uint32_t f = lo; f < hi; f++)
                h->fast[f] = (uint16_t)(sym << 5 | (uint32_t)l);
        }
    return 0;
}

// Decode one symbol (the accumulator holds >= 20 bits); -1 for bits that
// match no code (an incomplete table).
static inline int huff_decode(const bz_huff_t *h, codec_in_t *in) {
    uint32_t e = h->fast[msb_peek(in, BZ_FAST_BITS)];
    if (e) {
        msb_drop(in, (int)(e & 31));
        return (int)(e >> 5);
    }
    uint32_t bits = msb_peek(in, BZ_MAX_CODE_LEN);
    for (int l = BZ_FAST_BITS + 1; l <= h->max_len; l++) {
        uint32_t c = bits >> (BZ_MAX_CODE_LEN - l); // the next l bits
        if (c - h->first[l] < h->count[l]) {
            msb_drop(in, l);
            return h->sorted[h->offset[l] + (c - h->first[l])];
        }
    }
    return -1;
}

// ============================================================================
// Blocks
// ============================================================================

// Read the symbol map, tables and symbols of one block into tt, and link
// it for the transform.  0 or CODEC_ERR_*.
static int read_block(codec_dec_t *d, bz_t *s) {
    codec_in_t *in = &d->in;
    msb_fill(in);
    s->want_crc = msb_get(in, 32);
    if (msb_get(in, 1))
        return codec_fail(d, CODEC_ERR_UNSUPPORTED, "bzip2: randomised block (bzip2 before 0.9.5)");
    uint32_t orig = msb_get(in, 24);

    // The bytes that occur, in order: the move-to-front list starts as them.
    uint8_t mtf[256];
    int n_used = 0;
    msb_fill(in);
    uint32_t ranges = msb_get(in, 16);
    for (int r = 0; r < 16; r++) {
        if (!(ranges & (0x8000u >> r)))
            continue;
        msb_fill(in);
        uint32_t bytes = msb_get(in, 16);
        for (int j = 0; j < 16; j++)
            if (bytes & (0x8000u >> j))
                mtf[n_used++] = (uint8_t)(r * 16 + j);
    }
    if (n_used == 0)
        return codec_fail(d, CODEC_ERR_DATA, "bzip2: empty symbol map");
    int alpha = n_used + 2; // RUNA, RUNB, n_used - 1 indices, end of block
    int eob = alpha - 1;

    msb_fill(in);
    int n_groups = (int)msb_get(in, 3);
    uint32_t n_sel = msb_get(in, 15);
    if (n_groups < BZ_MIN_GROUPS || n_groups > BZ_MAX_GROUPS || n_sel == 0)
        return codec_fail(d, CODEC_ERR_DATA, "bzip2: bad table counts");

    // Selectors: unary move-to-front indices over the table numbers.
    uint8_t order[BZ_MAX_GROUPS];
    for (int g = 0; g < n_groups; g++)
        order[g] = (uint8_t)g;
    for (uint32_t i = 0; i < n_sel; i++) {
        msb_fill(in);
        int j = 0;
        while (msb_get(in, 1)) {
            if (++j >= n_groups)
                return codec_fail(d, CODEC_ERR_DATA, "bzip2: bad selector");
        }
        if (codec_in_overrun(in))
            return codec_fail(d, CODEC_ERR_TRUNCATED, "bzip2: truncated stream");
        uint8_t g = order[j];
        memmove(order + 1, order, (size_t)j);
        order[0] = g;
        if (i < BZ_MAX_SELECTORS)
            s->sel[i] = g;
    }
    if (n_sel > BZ_MAX_SELECTORS)
        n_sel = BZ_MAX_SELECTORS;

    // Code lengths, delta-coded from a 5-bit start.
    for (int g = 0; g < n_groups; g++) {
        msb_fill(in);
        int cur = (int)msb_get(in, 5);
        for (int i = 0; i < alpha; i++) {
            for (;;) {
                if (cur < 1 || cur > BZ_MAX_CODE_LEN)
                    return codec_fail(d, CODEC_ERR_DATA, "bzip2: bad code length");
                if (in->nbits < 2)
                    msb_fill(in);
                if (!msb_get(in, 1))
                    break;
                cur += msb_get(in, 1) ? -1 : 1;
            }
            s->lens[g][i] = (uint8_t)cur;
        }
        if (codec_in_overrun(in))
            return codec_fail(d, CODEC_ERR_TRUNCATED, "bzip2: truncated stream");
        if (huff_build(&s->huff[g], s->lens[g], alpha) != 0)
            return codec_fail(d, CODEC_ERR_DATA, "bzip2: over-subscribed code");
    }

    // Symbols: runs of the front byte, move-to-front indices, end of block.
    uint32_t *tt = s->tt, n = 0, block_max = s->block_max;
    uint32_t group = 0, left = 0; // current selector and symbols it has left
    uint32_t run = 0, weight = 1; // pending run length and next digit's weight
    uint32_t counts[256] = {0}; // occurrences of each byte, for the transform
    const bz_huff_t *h = NULL;
    for (;;) {
        if (left == 0) {
            if (group >= n_sel)
                return codec_fail(d, CODEC_ERR_DATA, "bzip2: more symbols than selectors");
            h = &s->huff[s->sel[group++]];
            left = BZ_GROUP_SIZE;
        }
        left--;
        if (in->nbits < BZ_MAX_CODE_LEN)
            msb_fill(in);
        int sym = huff_decode(h, in);
        if (sym < 0)
            return codec_fail(d, CODEC_ERR_DATA, "bzip2: bad Huffman code");
        if (sym <= BZ_RUNB) {
            // Bijective base 2: RUNA adds the digit weight, RUNB twice it.
            run += weight << sym;
            weight <<= 1;
            if (run > block_max)
                return codec_fail(d, CODEC_ERR_DATA, "bzip2: run past the block size");
            continue;
        }
        if (run) {
            if (run > block_max - n)
                return codec_fail(d, CODEC_ERR_DATA, "bzip2: block larger than its level");
            uint8_t v = mtf[0];
            counts[v] += run;
            for (uint32_t k = 0; k < run; k++)
                tt[n++] = v;
            run = 0;
            weight = 1;
        }
        if (sym == eob)
            break;
        // Index sym - 1 moves to the front and is emitted.
        int idx = sym - 1;
        uint8_t v = mtf[idx];
        memmove(mtf + 1, mtf, (size_t)idx);
        mtf[0] = v;
        if (n >= block_max)
            return codec_fail(d, CODEC_ERR_DATA, "bzip2: block larger than its level");
        counts[v]++;
        tt[n++] = v;
    }
    if (codec_in_overrun(in))
        return codec_fail(d, CODEC_ERR_TRUNCATED, "bzip2: truncated stream");
    if (n == 0 || orig >= n)
        return codec_fail(d, CODEC_ERR_DATA, "bzip2: bad origin pointer");

    // Each byte's position in the sorted first column, by counting sort;
    // link it to its position in the block.
    uint32_t start[256], sum = 0;
    for (int c = 0; c < 256; c++) {
        start[c] = sum;
        sum += counts[c];
    }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)tt[i];
        tt[start[c]++] |= i << 8;
    }
    s->n = n;
    s->k = 0;
    s->pos = tt[orig] >> 8;
    s->last = -1;
    s->same = 0;
    s->crc = 0;
    s->crc_at = d->ring.total;
    s->state = B_EMIT;
    return 0;
}

// Fold the bytes emitted since the last call into the block CRC.
static void crc_update(codec_dec_t *d, bz_t *s) {
    codec_ring_t *r = &d->ring;
    while (s->crc_at < r->total) {
        size_t pending = (size_t)(r->total - s->crc_at);
        size_t at = r->w >= pending ? r->w - pending : r->w + r->size - pending;
        size_t k = r->size - at < pending ? r->size - at : pending;
        s->crc = codec_crc32_msb(s->crc, r->buf + at, k);
        s->crc_at += k;
    }
}

// Emit the block's bytes while the ring has room: the transform's walk and
// the run-length decoding, in locals.  True when the block is finished.
static bool emit_block(codec_dec_t *d, bz_t *s) {
    codec_ring_t *r = &d->ring;
    const uint32_t *tt = s->tt;
    uint32_t k = s->k, n = s->n, pos = s->pos;
    int last = s->last, same = s->same;
    uint8_t *buf = r->buf;
    size_t w = r->w, size = r->size;
    uint64_t total = r->total, stop = r->drained + size - BZ_BURST;
    while (k < n && total <= stop) {
        uint32_t e = tt[pos];
        uint8_t c = (uint8_t)e;
        pos = e >> 8;
        k++;
        if (same == 4) {
            // A count byte: that many more copies of the run's byte.
            for (uint32_t i = 0; i < c; i++) {
                buf[w] = (uint8_t)last;
                if (++w == size)
                    w = 0;
            }
            total += c;
            same = 0;
            last = -1;
            continue;
        }
        if (c == last)
            same++;
        else {
            last = c;
            same = 1;
        }
        buf[w] = c;
        if (++w == size)
            w = 0;
        total++;
    }
    r->w = w;
    r->total = total;
    s->k = k;
    s->pos = pos;
    s->last = last;
    s->same = same;
    return k == n;
}

// ============================================================================
// The decoder
// ============================================================================

static int bz_fill(codec_dec_t *d) {
    bz_t *s = d->st;
    codec_in_t *in = &d->in;
    for (;;) {
        switch (s->state) {
        case B_STREAM: {
            // "BZh" and the level.
            int b[4];
            for (int i = 0; i < 4; i++)
                b[i] = msb_byte(in);
            if (b[0] != 'B' || b[1] != 'Z' || b[2] != 'h' || b[3] < '1' || b[3] > '9') {
                if (s->streams > 0) {
                    s->state = B_DONE; // bytes after the last stream that start none: ignored, as bzip2 does
                    return CODEC_END;
                }
                return codec_fail(d, b[0] < 0 || b[3] < 0 ? CODEC_ERR_TRUNCATED : CODEC_ERR_DATA,
                                  "bzip2: bad stream header");
            }
            s->streams++;
            uint32_t block_max = (uint32_t)(b[3] - '0') * 100000u;
            if (block_max > s->block_max) {
                free(s->tt);
                s->tt = malloc((size_t)block_max * sizeof(uint32_t));
                if (!s->tt) {
                    s->block_max = 0;
                    return codec_fail(d, CODEC_ERR_NOMEM, "bzip2: out of memory");
                }
            }
            s->block_max = block_max;
            s->combined = 0;
            s->state = B_BLOCK;
            break;
        }
        case B_BLOCK: {
            msb_fill(in);
            uint32_t hi = msb_get(in, 24), lo = msb_get(in, 24);
            if (codec_in_overrun(in))
                return codec_fail(d, CODEC_ERR_TRUNCATED, "bzip2: truncated stream");
            if (hi == BZ_EOS_MAGIC_HI && lo == BZ_EOS_MAGIC_LO) {
                uint32_t want = msb_get(in, 32);
                if (codec_in_overrun(in))
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "bzip2: truncated stream");
                if (want != s->combined)
                    return codec_fail(d, CODEC_ERR_CHECKSUM, "bzip2: stream CRC mismatch");
                msb_align(in);
                // Another stream may follow (bzip2 -c a b > ab).
                if (in->nbits / 8 <= (int)in->over && in->pos == in->len && !codec_in_refill(in)) {
                    s->state = B_DONE;
                    return CODEC_END;
                }
                s->state = B_STREAM;
                break;
            }
            if (hi != BZ_BLOCK_MAGIC_HI || lo != BZ_BLOCK_MAGIC_LO)
                return codec_fail(d, CODEC_ERR_DATA, "bzip2: bad block magic");
            int rc = read_block(d, s);
            if (rc)
                return rc;
            break;
        }
        case B_EMIT:
            if (ring_room(&d->ring) < BZ_BURST)
                return CODEC_MORE;
            if (!emit_block(d, s)) {
                crc_update(d, s);
                return CODEC_MORE;
            }
            crc_update(d, s);
            if (s->crc != s->want_crc)
                return codec_fail(d, CODEC_ERR_CHECKSUM, "bzip2: block CRC mismatch");
            s->combined = ((s->combined << 1) | (s->combined >> 31)) ^ s->crc;
            s->state = B_BLOCK;
            break;
        case B_DONE:
            return CODEC_END;
        }
    }
}

static int bz_init(codec_dec_t *d) {
    bz_t *s = calloc(1, sizeof(*s));
    if (!s)
        return CODEC_ERR_NOMEM;
    d->st = s;
    return codec_ring_alloc(d, 0, BZ_BURST);
}

static void bz_free(codec_dec_t *d) {
    bz_t *s = d->st;
    free(s->tt);
    free(s);
    d->st = NULL;
}

const codec_dec_ops_t codec_bzip2_dec = {bz_init, bz_fill, bz_free, false};
