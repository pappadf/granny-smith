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
// of further copies (0..255).
//
// The block's decoded symbols are held as 32-bit words: the low byte is the
// symbol and, once the transform's links are built, the upper 24 bits the
// index of the next entry to visit (a block is at most 900 000 bytes, so an
// index fits).

#include "codec_internal.h"

#include <stdlib.h>
#include <string.h>

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

// ============================================================================
// Bit input (MSB first)
// ============================================================================

typedef struct {
    const uint8_t *p;
    size_t len, pos; // input and the next byte to load
    uint64_t acc; // loaded bits, the oldest highest
    int n; // valid bits in acc
    int fake; // zero bytes loaded past the end (for peeking near the end)
} bz_bits_t;

// Load bytes until at least `need` bits are buffered; past the end of the
// input zeros are loaded and counted, so a peek never fails.
static void bz_fill(bz_bits_t *b, int need) {
    while (b->n < need) {
        uint8_t byte = 0;
        if (b->pos < b->len)
            byte = b->p[b->pos++];
        else
            b->fake++;
        b->acc = (b->acc << 8) | byte;
        b->n += 8;
    }
}

// True when more bits were consumed than the input holds.
static bool bz_overrun(const bz_bits_t *b) {
    return b->n < b->fake * 8;
}

// The next `k` (1..32) bits, consumed.  Bits past the end read as zero and
// mark the stream overrun.
static uint32_t bz_get(bz_bits_t *b, int k) {
    bz_fill(b, k);
    b->n -= k;
    return (uint32_t)(b->acc >> b->n) & (uint32_t)((1ull << k) - 1);
}

// The next `k` (1..32) bits, not consumed.
static uint32_t bz_peek(bz_bits_t *b, int k) {
    bz_fill(b, k);
    return (uint32_t)(b->acc >> (b->n - k)) & (uint32_t)((1ull << k) - 1);
}

// Skip to the next byte boundary and report the byte offset reached.
static size_t bz_byte_pos(bz_bits_t *b) {
    b->n -= b->n & 7;
    return b->pos + (size_t)b->fake - (size_t)(b->n / 8); // loaded less unconsumed
}

// ============================================================================
// Huffman tables
// ============================================================================

// A canonical code, decoded by comparing the next bits against each
// length's code range: codes of length l are first[l] .. first[l]+count[l]-1,
// and their symbols sit in `sorted` from index offset[l], in symbol order.
typedef struct {
    uint32_t first[BZ_MAX_CODE_LEN + 1];
    uint16_t count[BZ_MAX_CODE_LEN + 1];
    uint16_t offset[BZ_MAX_CODE_LEN + 1];
    uint16_t sorted[BZ_MAX_ALPHA];
    int min_len, max_len;
} bz_huff_t;

// Build the decoder for `n` symbols with lengths `lens` (each 1..20).
// CODEC_ERR_DATA when the lengths over-subscribe the code space.
static int bz_huff_build(bz_huff_t *h, const uint8_t *lens, int n) {
    memset(h->count, 0, sizeof(h->count));
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
            return CODEC_ERR_DATA; // more codes of this length than fit
        code <<= 1;
    }
    // Symbols in canonical order: by length, then by symbol value.
    uint16_t fill[BZ_MAX_CODE_LEN + 1];
    memcpy(fill, h->offset, sizeof(fill));
    for (int i = 0; i < n; i++)
        h->sorted[fill[lens[i]]++] = (uint16_t)i;
    return 0;
}

// Decode one symbol; -1 when the bits match no code (an incomplete table).
static int bz_huff_decode(const bz_huff_t *h, bz_bits_t *b) {
    uint32_t bits = bz_peek(b, h->max_len);
    for (int l = h->min_len; l <= h->max_len; l++) {
        uint32_t c = bits >> (h->max_len - l); // the next l bits
        if (c - h->first[l] < h->count[l]) {
            bz_get(b, l);
            return h->sorted[h->offset[l] + (c - h->first[l])];
        }
    }
    return -1;
}

// ============================================================================
// Blocks
// ============================================================================

// Per-stream working state (the block buffer is sized by the level).
typedef struct {
    bz_bits_t bits;
    uint32_t *tt; // block symbols, then their transform links
    uint32_t block_max; // the level's block size
    uint8_t *dst;
    size_t cap, out; // the output buffer and bytes written to it
    uint8_t sel[BZ_MAX_SELECTORS]; // table index per 50-symbol group
    uint8_t lens[BZ_MAX_GROUPS][BZ_MAX_ALPHA];
    bz_huff_t huff[BZ_MAX_GROUPS];
} bz_t;

// Read the symbol map, tables and symbols of one block into s->tt.  Returns
// the block's length or a negative code; *orig gets the origin pointer.
static int64_t bz_read_block(bz_t *s, uint32_t *orig) {
    bz_bits_t *b = &s->bits;
    if (bz_get(b, 1))
        return CODEC_ERR_UNSUPPORTED; // randomised block (bzip2 < 0.9.5)
    *orig = bz_get(b, 24);

    // The bytes that occur, in order: the move-to-front list starts as them.
    uint8_t mtf[256];
    int n_used = 0;
    uint32_t ranges = bz_get(b, 16);
    for (int r = 0; r < 16; r++) {
        if (!(ranges & (0x8000u >> r)))
            continue;
        uint32_t bytes = bz_get(b, 16);
        for (int j = 0; j < 16; j++)
            if (bytes & (0x8000u >> j))
                mtf[n_used++] = (uint8_t)(r * 16 + j);
    }
    if (n_used == 0)
        return CODEC_ERR_DATA;
    int alpha = n_used + 2; // RUNA, RUNB, n_used - 1 indices, end of block
    int eob = alpha - 1;

    int n_groups = (int)bz_get(b, 3);
    if (n_groups < BZ_MIN_GROUPS || n_groups > BZ_MAX_GROUPS)
        return CODEC_ERR_DATA;
    uint32_t n_sel = bz_get(b, 15);
    if (n_sel == 0)
        return CODEC_ERR_DATA;

    // Selectors: unary move-to-front indices over the table numbers.
    uint8_t order[BZ_MAX_GROUPS];
    for (int g = 0; g < n_groups; g++)
        order[g] = (uint8_t)g;
    for (uint32_t i = 0; i < n_sel; i++) {
        int j = 0;
        while (bz_get(b, 1)) {
            if (++j >= n_groups)
                return CODEC_ERR_DATA;
        }
        if (bz_overrun(b))
            return CODEC_ERR_TRUNCATED;
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
        int cur = (int)bz_get(b, 5);
        for (int i = 0; i < alpha; i++) {
            for (;;) {
                if (cur < 1 || cur > BZ_MAX_CODE_LEN)
                    return CODEC_ERR_DATA;
                if (!bz_get(b, 1))
                    break;
                cur += bz_get(b, 1) ? -1 : 1;
            }
            s->lens[g][i] = (uint8_t)cur;
        }
        if (bz_overrun(b))
            return CODEC_ERR_TRUNCATED;
        int rc = bz_huff_build(&s->huff[g], s->lens[g], alpha);
        if (rc)
            return rc;
    }

    // Symbols: runs of the front byte, move-to-front indices, end of block.
    uint32_t n = 0; // block bytes so far
    uint32_t group = 0, left = 0; // current selector and symbols it has left
    uint32_t run = 0, weight = 1; // pending run length and next digit's weight
    for (;;) {
        if (left == 0) {
            if (group >= n_sel)
                return CODEC_ERR_DATA; // more symbols than selectors cover
            left = BZ_GROUP_SIZE;
            group++;
        }
        left--;
        int sym = bz_huff_decode(&s->huff[s->sel[group - 1]], b);
        if (sym < 0)
            return CODEC_ERR_DATA;
        if (bz_overrun(b))
            return CODEC_ERR_TRUNCATED;
        if (sym == BZ_RUNA || sym == BZ_RUNB) {
            // Bijective base 2: RUNA adds the digit weight, RUNB twice it.
            run += weight << (sym == BZ_RUNB);
            weight <<= 1;
            if (run > s->block_max)
                return CODEC_ERR_DATA;
            continue;
        }
        if (run) {
            if (run > s->block_max - n)
                return CODEC_ERR_DATA;
            for (uint32_t k = 0; k < run; k++)
                s->tt[n++] = mtf[0];
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
        if (n >= s->block_max)
            return CODEC_ERR_DATA;
        s->tt[n++] = v;
    }
    if (n == 0 || *orig >= n)
        return CODEC_ERR_DATA;
    return n;
}

// Undo the transform and the initial run-length coding of an n-byte block,
// appending to the output.  Returns the block's CRC or a negative code (as
// int64_t, so every 32-bit CRC is representable).
static int64_t bz_emit_block(bz_t *s, uint32_t n, uint32_t orig) {
    // Each byte's position in the sorted first column, by counting sort;
    // link it to its position in the block.
    uint32_t start[256] = {0};
    for (uint32_t i = 0; i < n; i++)
        start[s->tt[i] & 0xff]++;
    uint32_t sum = 0;
    for (int c = 0; c < 256; c++) {
        uint32_t k = start[c];
        start[c] = sum;
        sum += k;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)(s->tt[i] & 0xff);
        s->tt[start[c]++] |= i << 8;
    }

    size_t at = s->out; // where this block's output begins
    uint32_t pos = s->tt[orig] >> 8;
    int last = -1, same = 0; // the previous byte and how many in a row
    for (uint32_t k = 0; k < n; k++) {
        uint32_t e = s->tt[pos];
        uint8_t c = (uint8_t)(e & 0xff);
        pos = e >> 8;
        if (same == 4) {
            // A count byte: that many more copies of the run's byte.
            if (c > s->cap - s->out)
                return CODEC_ERR_OVERFLOW;
            memset(s->dst + s->out, last, c);
            s->out += c;
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
        if (s->out >= s->cap)
            return CODEC_ERR_OVERFLOW;
        s->dst[s->out++] = c;
    }
    return codec_crc32_msb(0, s->dst + at, s->out - at);
}

// Decode one stream starting at src[0]; *used gets the bytes it occupied.
static int64_t bz_stream(bz_t *s, const uint8_t *src, size_t len, size_t *used) {
    if (len < 4 || src[0] != 'B' || src[1] != 'Z' || src[2] != 'h' || src[3] < '1' || src[3] > '9')
        return CODEC_ERR_DATA;
    uint32_t block_max = (uint32_t)(src[3] - '0') * 100000u;
    if (block_max > s->block_max) {
        free(s->tt);
        s->tt = malloc((size_t)block_max * sizeof(uint32_t));
        if (!s->tt) {
            s->block_max = 0;
            return CODEC_ERR_NOMEM;
        }
    }
    s->block_max = block_max;
    s->bits = (bz_bits_t){.p = src, .len = len, .pos = 4};
    bz_bits_t *b = &s->bits;

    uint32_t combined = 0; // the stream CRC: rotate, then fold in each block's
    for (;;) {
        uint32_t hi = bz_get(b, 24), lo = bz_get(b, 24);
        if (bz_overrun(b))
            return CODEC_ERR_TRUNCATED;
        if (hi == BZ_EOS_MAGIC_HI && lo == BZ_EOS_MAGIC_LO) {
            uint32_t want = bz_get(b, 32);
            if (bz_overrun(b))
                return CODEC_ERR_TRUNCATED;
            if (want != combined)
                return CODEC_ERR_CHECKSUM;
            *used = bz_byte_pos(b);
            return 0;
        }
        if (hi != BZ_BLOCK_MAGIC_HI || lo != BZ_BLOCK_MAGIC_LO)
            return CODEC_ERR_DATA;
        uint32_t want = bz_get(b, 32);
        uint32_t orig = 0;
        int64_t n = bz_read_block(s, &orig);
        if (n < 0)
            return n;
        int64_t crc = bz_emit_block(s, (uint32_t)n, orig);
        if (crc < 0)
            return crc;
        if ((uint32_t)crc != want)
            return CODEC_ERR_CHECKSUM;
        combined = ((combined << 1) | (combined >> 31)) ^ (uint32_t)crc;
    }
}

int64_t codec_bzip2_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    if (!src || (!dst && cap))
        return CODEC_ERR_ARG;
    bz_t *s = calloc(1, sizeof(*s));
    if (!s)
        return CODEC_ERR_NOMEM;
    s->dst = dst;
    s->cap = cap;
    int64_t rc = 0;
    size_t at = 0;
    // One stream, then any concatenated after it (bzip2 -c a b > ab).
    do {
        size_t used = 0;
        rc = bz_stream(s, src + at, len - at, &used);
        at += used;
    } while (rc == 0 && len - at >= 4 && memcmp(src + at, "BZh", 3) == 0);
    free(s->tt);
    int64_t out = (int64_t)s->out;
    free(s);
    return rc < 0 ? rc : out;
}
