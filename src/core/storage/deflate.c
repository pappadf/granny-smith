// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// deflate.c
// LZ77 + fixed-Huffman DEFLATE in a zlib wrapper.  See deflate.h.
//
// Moved here from the PNG writer in debug.c, unchanged at its level (6: a
// 64-deep hash chain), so screenshots encode byte-for-byte as before.

#include "deflate.h"

#include "inflate.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define DEFLATE_WINDOW    32768
#define DEFLATE_MIN_MATCH 3
#define DEFLATE_MAX_MATCH 258
#define DEFLATE_HASH_BITS 15
#define DEFLATE_HASH_SIZE (1 << DEFLATE_HASH_BITS)
#define DEFLATE_STORED    65535u // largest stored block

struct deflate_state {
    size_t max_len;
    int32_t *head; // DEFLATE_HASH_SIZE: newest position per hash
    int32_t *prev; // max_len: the previous position with the same hash
};

size_t deflate_bound(size_t len) {
    return len + len / 4 + 5 * (len / DEFLATE_STORED + 1) + 64;
}

deflate_state_t *deflate_state_new(size_t max_len) {
    deflate_state_t *st = calloc(1, sizeof(*st));
    if (!st)
        return NULL;
    st->max_len = max_len;
    st->head = malloc(DEFLATE_HASH_SIZE * sizeof(int32_t));
    st->prev = malloc((max_len ? max_len : 1) * sizeof(int32_t));
    if (!st->head || !st->prev) {
        deflate_state_free(st);
        return NULL;
    }
    return st;
}

void deflate_state_free(deflate_state_t *st) {
    if (!st)
        return;
    free(st->head);
    free(st->prev);
    free(st);
}

uint32_t gs_adler32(uint32_t adler, const uint8_t *data, size_t len) {
    uint32_t a = adler & 0xffff, b = adler >> 16;
    while (len > 0) {
        // 5552 is the most bytes that can be summed before b can overflow.
        size_t n = len < 5552 ? len : 5552;
        len -= n;
        while (n--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

// LSB-first bit writer over a caller-sized buffer.  Writes past `cap` are
// counted but dropped, so an overflow is seen at the end.
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    uint32_t acc; // pending bits, lowest bit written first
    int nbits;
} deflate_bw_t;

// Append the low `n` bits of `v`, least-significant bit first.
static void put_bits(deflate_bw_t *w, uint32_t v, int n) {
    if (n <= 0)
        return;
    w->acc |= (v & ((1u << n) - 1)) << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        if (w->pos < w->cap)
            w->buf[w->pos] = (uint8_t)(w->acc & 0xff);
        w->pos++;
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

// Append a Huffman code: the code's most-significant bit goes out first.
static void put_code(deflate_bw_t *w, uint32_t code, int n) {
    uint32_t reversed = 0;
    for (int i = 0; i < n; i++)
        reversed |= ((code >> i) & 1u) << (n - 1 - i);
    put_bits(w, reversed, n);
}

// Emit one literal/length symbol in the fixed Huffman code (RFC 1951 §3.2.6).
static void put_symbol(deflate_bw_t *w, unsigned sym) {
    if (sym < 144)
        put_code(w, 0x30 + sym, 8);
    else if (sym < 256)
        put_code(w, 0x190 + (sym - 144), 9);
    else if (sym < 280)
        put_code(w, sym - 256, 7);
    else
        put_code(w, 0xc0 + (sym - 280), 8);
}

// Hash three bytes into the match-chain head table.
static uint32_t hash3(const uint8_t *p) {
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2]) & (DEFLATE_HASH_SIZE - 1);
}

// Hash-chain depth per level.
static int chain_for(int level) {
    static const int depth[10] = {0, 4, 8, 16, 32, 48, 64, 128, 256, 1024};
    if (level < 1)
        return 0;
    return depth[level > 9 ? 9 : level];
}

// One fixed-Huffman block covering all of `src`.
static void encode_fixed(deflate_state_t *st, deflate_bw_t *w, const uint8_t *src, size_t len, int max_chain) {
    int32_t *head = st->head, *prev = st->prev;
    for (size_t i = 0; i < DEFLATE_HASH_SIZE; i++)
        head[i] = -1;
    put_bits(w, 1, 1); // BFINAL — one block covers the whole stream
    put_bits(w, 1, 2); // BTYPE = 01, fixed Huffman

    size_t pos = 0;
    while (pos < len) {
        size_t best_len = 0, best_dist = 0;
        if (pos + DEFLATE_MIN_MATCH <= len) {
            uint32_t h = hash3(src + pos);
            // Walk the chain of earlier positions with the same hash, newest
            // first, keeping the longest match inside the 32K window.
            int32_t cand = head[h];
            for (int chain = max_chain; cand >= 0 && chain > 0; chain--) {
                size_t dist = pos - (size_t)cand;
                if (dist == 0 || dist > DEFLATE_WINDOW)
                    break;
                size_t max_len = len - pos;
                if (max_len > DEFLATE_MAX_MATCH)
                    max_len = DEFLATE_MAX_MATCH;
                size_t l = 0;
                while (l < max_len && src[(size_t)cand + l] == src[pos + l])
                    l++;
                if (l > best_len) {
                    best_len = l;
                    best_dist = dist;
                    if (l >= DEFLATE_MAX_MATCH)
                        break;
                }
                cand = prev[cand];
            }
            prev[pos] = head[h];
            head[h] = (int32_t)pos;
        }

        if (best_len >= DEFLATE_MIN_MATCH) {
            int lc = 28;
            while (lc > 0 && best_len < deflate_len_base[lc])
                lc--;
            put_symbol(w, 257 + (unsigned)lc);
            put_bits(w, (uint32_t)(best_len - deflate_len_base[lc]), deflate_len_extra[lc]);
            int dc = 29;
            while (dc > 0 && best_dist < deflate_dist_base[dc])
                dc--;
            put_code(w, (uint32_t)dc, 5);
            put_bits(w, (uint32_t)(best_dist - deflate_dist_base[dc]), deflate_dist_extra[dc]);
            // Index the bytes the match covered so later matches can find them.
            for (size_t k = 1; k < best_len; k++) {
                if (pos + k + DEFLATE_MIN_MATCH > len)
                    break;
                uint32_t h2 = hash3(src + pos + k);
                prev[pos + k] = head[h2];
                head[h2] = (int32_t)(pos + k);
            }
            pos += best_len;
        } else {
            put_symbol(w, src[pos]);
            pos++;
        }
    }
    put_symbol(w, 256); // end of block
    if (w->nbits > 0)
        put_bits(w, 0, 8 - w->nbits); // pad the final byte
}

// Stored blocks (BTYPE 00) of at most 65535 bytes each.
static void encode_stored(deflate_bw_t *w, const uint8_t *src, size_t len) {
    size_t pos = 0;
    do {
        size_t n = len - pos < DEFLATE_STORED ? len - pos : DEFLATE_STORED;
        bool last = pos + n == len;
        put_bits(w, last ? 1 : 0, 1);
        put_bits(w, 0, 2);
        if (w->nbits > 0)
            put_bits(w, 0, 8 - w->nbits); // LEN starts on a byte boundary
        put_bits(w, (uint32_t)n, 16);
        put_bits(w, (uint32_t)~n & 0xffff, 16);
        if (w->pos + n <= w->cap)
            memcpy(w->buf + w->pos, src + pos, n);
        w->pos += n;
        pos += n;
    } while (pos < len);
}

long deflate_zlib(deflate_state_t *st, const uint8_t *in, size_t len, uint8_t *out, size_t cap, int level) {
    if (cap < 6)
        return -1;
    deflate_state_t *own = NULL;
    if (level > 0 && (!st || st->max_len < len)) {
        own = deflate_state_new(len);
        if (!own)
            return -1;
        st = own;
    }
    deflate_bw_t w = {out, cap, 0, 0, 0};
    // zlib header: CM=8 / CINFO=7 (32K window), FLEVEL=2, FCHECK making the
    // 16-bit value a multiple of 31.
    out[w.pos++] = 0x78;
    out[w.pos++] = 0x9c;
    if (level <= 0)
        encode_stored(&w, in, len);
    else
        encode_fixed(st, &w, in, len, chain_for(level));
    deflate_state_free(own);
    if (w.pos + 4 > cap)
        return -1;
    uint32_t adler = gs_adler32(1, in, len);
    out[w.pos++] = (uint8_t)(adler >> 24);
    out[w.pos++] = (uint8_t)(adler >> 16);
    out[w.pos++] = (uint8_t)(adler >> 8);
    out[w.pos++] = (uint8_t)adler;
    return (long)w.pos;
}
