// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// deflate.c
// LZ77 + Huffman DEFLATE in a zlib wrapper.  See deflate.h.
//
// One LZ77 pass (hash chains over a 32 KB window, greedy matches) turns the
// input into tokens -- literals and (length, distance) pairs -- and the
// tokens are cut into blocks.  Each block is written in whichever of the
// three RFC 1951 forms is smallest: a dynamic-Huffman block (codes built from
// the block's own symbol counts, RFC 1951 §3.2.7), a fixed-Huffman block
// (the built-in codes, §3.2.6), or a stored block (§3.2.4).  Dynamic codes
// are length-limited (15 bits, 7 for the code-length code) and always
// complete, so strict decoders (zlib's) accept them.

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

// Tokens per block: enough for the codes to pay for their header, few
// enough that they follow a change in the data.
#define DEFLATE_BLOCK_TOKENS 32768u

#define N_LITLEN 286 // literal/length symbols in use (286, 287 never occur)
#define N_DIST   30
#define N_CLEN   19
#define MAX_BITS 15
#define MAX_CLEN 7

// One LZ77 token: a literal (dist 0, len the byte) or a match.
typedef struct {
    uint16_t len; // literal byte, or match length 3..258
    uint16_t dist; // 0 for a literal, else 1..32768 (32768 stored as 0x8000)
} token_t;

struct deflate_state {
    size_t max_len;
    int32_t *head; // DEFLATE_HASH_SIZE: newest position per hash
    int32_t *prev; // max_len: the previous position with the same hash
    token_t *tok; // max_len: tokens of the current input
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
    st->tok = malloc((max_len ? max_len : 1) * sizeof(token_t));
    if (!st->head || !st->prev || !st->tok) {
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
    free(st->tok);
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

// ============================================================================
// Bit output
// ============================================================================

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

static void align_byte(deflate_bw_t *w) {
    if (w->nbits > 0)
        put_bits(w, 0, 8 - w->nbits);
}

// ============================================================================
// LZ77
// ============================================================================

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

// Tokenise all of `src` into st->tok; the token count.
static size_t tokenise(deflate_state_t *st, const uint8_t *src, size_t len, int max_chain) {
    int32_t *head = st->head, *prev = st->prev;
    token_t *tok = st->tok;
    size_t nt = 0;
    for (size_t i = 0; i < DEFLATE_HASH_SIZE; i++)
        head[i] = -1;
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
            tok[nt++] = (token_t){(uint16_t)best_len, (uint16_t)best_dist};
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
            tok[nt++] = (token_t){src[pos], 0};
            pos++;
        }
    }
    return nt;
}

// The length symbol (257..285) and extra bits of a match length.
static int len_code(unsigned len) {
    int lc = 28;
    while (lc > 0 && len < deflate_len_base[lc])
        lc--;
    return lc;
}

// The distance code (0..29) of a match distance.
static int dist_code(unsigned dist) {
    int dc = 29;
    while (dc > 0 && dist < deflate_dist_base[dc])
        dc--;
    return dc;
}

// ============================================================================
// Huffman codes
// ============================================================================

// Fixed literal/length code lengths (RFC 1951 §3.2.6) and distance lengths.
static uint8_t fixed_lit_len(unsigned sym) {
    return sym < 144 ? 8 : sym < 256 ? 9 : sym < 280 ? 7 : 8;
}

// Code lengths for `n` symbols with frequencies `freq`, none longer than
// `limit`, forming a complete prefix code.  Symbols with frequency 0 get
// length 0.  The caller guarantees at least two symbols are used.
static void build_lengths(const uint32_t *freq, int n, int limit, uint8_t *len) {
    // Huffman by repeated merging of the two lightest nodes (n <= 286, so a
    // simple O(n^2) selection is cheap next to the LZ77 pass).
    int parent[2 * N_LITLEN];
    uint64_t weight[2 * N_LITLEN];
    bool live[2 * N_LITLEN];
    int nodes = 0, leaf_of[N_LITLEN];
    for (int i = 0; i < n; i++) {
        len[i] = 0;
        leaf_of[i] = -1;
        if (freq[i]) {
            leaf_of[i] = nodes;
            weight[nodes] = freq[i];
            live[nodes] = true;
            parent[nodes] = -1;
            nodes++;
        }
    }
    int alive = nodes;
    while (alive > 1) {
        int a = -1, b = -1;
        for (int i = 0; i < nodes; i++) {
            if (!live[i])
                continue;
            if (a < 0 || weight[i] < weight[a]) {
                b = a;
                a = i;
            } else if (b < 0 || weight[i] < weight[b]) {
                b = i;
            }
        }
        live[a] = live[b] = false;
        weight[nodes] = weight[a] + weight[b];
        live[nodes] = true;
        parent[nodes] = -1;
        parent[a] = parent[b] = nodes;
        nodes++;
        alive--;
    }
    // Depth of each leaf, clamped to the limit.
    for (int i = 0; i < n; i++) {
        if (leaf_of[i] < 0)
            continue;
        int d = 0;
        for (int p = leaf_of[i]; parent[p] >= 0; p = parent[p])
            d++;
        len[i] = (uint8_t)(d > limit ? limit : d);
    }
    // Clamping can over-subscribe the code (Kraft sum > 1): lengthen the
    // longest codes still under the limit until it fits, then shorten the
    // longest ones while there is room, so the code ends exactly complete.
    uint64_t full = 1ull << limit, kraft = 0;
    for (int i = 0; i < n; i++)
        if (len[i])
            kraft += 1ull << (limit - len[i]);
    while (kraft > full) {
        int pick = -1;
        for (int i = 0; i < n; i++)
            if (len[i] && len[i] < limit &&
                (pick < 0 || len[i] > len[pick] || (len[i] == len[pick] && freq[i] < freq[pick])))
                pick = i;
        kraft -= 1ull << (limit - len[pick] - 1);
        len[pick]++;
    }
    while (kraft < full) {
        int pick = -1;
        for (int i = 0; i < n; i++)
            if (len[i] > 1 && kraft + (1ull << (limit - len[i])) <= full &&
                (pick < 0 || len[i] > len[pick] || (len[i] == len[pick] && freq[i] > freq[pick])))
                pick = i;
        if (pick < 0)
            break; // cannot happen for >= 2 symbols; leave as is
        kraft += 1ull << (limit - len[pick]);
        len[pick]--;
    }
}

// Canonical codes from lengths (RFC 1951 §3.2.2).
static void build_codes(const uint8_t *len, int n, uint16_t *code) {
    uint16_t bl_count[MAX_BITS + 1] = {0}, next[MAX_BITS + 1] = {0};
    for (int i = 0; i < n; i++)
        bl_count[len[i]]++;
    bl_count[0] = 0;
    uint16_t c = 0;
    for (int bits = 1; bits <= MAX_BITS; bits++) {
        c = (uint16_t)((c + bl_count[bits - 1]) << 1);
        next[bits] = c;
    }
    for (int i = 0; i < n; i++)
        code[i] = len[i] ? next[len[i]]++ : 0;
}

// Make sure at least two symbols of a code are used (a one-symbol code is
// incomplete, which strict decoders refuse).
static void ensure_two(uint32_t *freq, int n) {
    int used = 0;
    for (int i = 0; i < n; i++)
        used += freq[i] != 0;
    for (int i = 0; i < n && used < 2; i++)
        if (!freq[i]) {
            freq[i] = 1;
            used++;
        }
}

// The code-length sequence for the literal and distance lengths, run-length
// coded with symbols 16 (repeat previous 3-6), 17 (zeros 3-10) and 18
// (zeros 11-138).  Each entry is symbol | extra << 8.
static int rle_lengths(const uint8_t *lens, int n, uint16_t *out) {
    int k = 0;
    for (int i = 0; i < n;) {
        uint8_t v = lens[i];
        int run = 1;
        while (i + run < n && lens[i + run] == v)
            run++;
        if (v == 0) {
            int left = run;
            while (left >= 11) {
                int r = left > 138 ? 138 : left;
                out[k++] = (uint16_t)(18 | (r - 11) << 8);
                left -= r;
            }
            if (left >= 3) {
                out[k++] = (uint16_t)(17 | (left - 3) << 8);
                left = 0;
            }
            while (left-- > 0)
                out[k++] = 0;
        } else {
            out[k++] = v;
            int left = run - 1;
            while (left >= 3) {
                int r = left > 6 ? 6 : left;
                out[k++] = (uint16_t)(16 | (r - 3) << 8);
                left -= r;
            }
            while (left-- > 0)
                out[k++] = v;
        }
        i += run;
    }
    return k;
}

static const uint8_t clen_order[N_CLEN] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// ============================================================================
// Blocks
// ============================================================================

// Bits the tokens cost under code lengths `ll` / `dl` (data only).
static uint64_t data_bits(const token_t *t, size_t n, const uint8_t *ll, const uint8_t *dl) {
    uint64_t bits = ll[256];
    for (size_t i = 0; i < n; i++) {
        if (!t[i].dist) {
            bits += ll[t[i].len];
        } else {
            int lc = len_code(t[i].len), dc = dist_code(t[i].dist);
            bits += ll[257 + lc] + deflate_len_extra[lc] + dl[dc] + deflate_dist_extra[dc];
        }
    }
    return bits;
}

static void write_tokens(deflate_bw_t *w, const token_t *t, size_t n, const uint8_t *ll, const uint16_t *lcode,
                         const uint8_t *dl, const uint16_t *dcode) {
    for (size_t i = 0; i < n; i++) {
        if (!t[i].dist) {
            put_code(w, lcode[t[i].len], ll[t[i].len]);
            continue;
        }
        int lc = len_code(t[i].len), dc = dist_code(t[i].dist);
        put_code(w, lcode[257 + lc], ll[257 + lc]);
        put_bits(w, (uint32_t)(t[i].len - deflate_len_base[lc]), deflate_len_extra[lc]);
        put_code(w, dcode[dc], dl[dc]);
        put_bits(w, (uint32_t)(t[i].dist - deflate_dist_base[dc]), deflate_dist_extra[dc]);
    }
    put_code(w, lcode[256], ll[256]);
}

// Stored blocks for [src, src+len); `last` marks the final one.
static void write_stored(deflate_bw_t *w, const uint8_t *src, size_t len, bool last) {
    size_t pos = 0;
    do {
        size_t n = len - pos < DEFLATE_STORED ? len - pos : DEFLATE_STORED;
        bool fin = last && pos + n == len;
        put_bits(w, fin ? 1 : 0, 1);
        put_bits(w, 0, 2);
        align_byte(w); // LEN starts on a byte boundary
        put_bits(w, (uint32_t)n, 16);
        put_bits(w, (uint32_t)~n & 0xffff, 16);
        if (w->pos + n <= w->cap)
            memcpy(w->buf + w->pos, src + pos, n);
        w->pos += n;
        pos += n;
    } while (pos < len);
}

// One block of tokens `t` covering input [src, src+len), in its smallest form.
static void write_block(deflate_bw_t *w, const token_t *t, size_t n, const uint8_t *src, size_t len, bool last) {
    // Fixed codes.
    uint8_t fll[288], fdl[N_DIST];
    uint16_t flc[288], fdc[N_DIST];
    for (int i = 0; i < 288; i++)
        fll[i] = fixed_lit_len((unsigned)i);
    for (int i = 0; i < N_DIST; i++)
        fdl[i] = 5;
    build_codes(fll, 288, flc);
    build_codes(fdl, N_DIST, fdc);
    uint64_t fixed_cost = 3 + data_bits(t, n, fll, fdl);

    // Dynamic codes from this block's counts.
    uint32_t lf[N_LITLEN] = {0}, df[N_DIST] = {0};
    for (size_t i = 0; i < n; i++) {
        if (!t[i].dist) {
            lf[t[i].len]++;
        } else {
            lf[257 + len_code(t[i].len)]++;
            df[dist_code(t[i].dist)]++;
        }
    }
    lf[256] = 1;
    ensure_two(lf, N_LITLEN);
    ensure_two(df, N_DIST);
    uint8_t dll[N_LITLEN], ddl[N_DIST];
    build_lengths(lf, N_LITLEN, MAX_BITS, dll);
    build_lengths(df, N_DIST, MAX_BITS, ddl);
    int hlit = N_LITLEN, hdist = N_DIST;
    while (hlit > 257 && !dll[hlit - 1])
        hlit--;
    while (hdist > 1 && !ddl[hdist - 1])
        hdist--;
    uint8_t all[N_LITLEN + N_DIST];
    memcpy(all, dll, (size_t)hlit);
    memcpy(all + hlit, ddl, (size_t)hdist);
    uint16_t rle[N_LITLEN + N_DIST];
    int nrle = rle_lengths(all, hlit + hdist, rle);
    uint32_t cf[N_CLEN] = {0};
    for (int i = 0; i < nrle; i++)
        cf[rle[i] & 0xff]++;
    ensure_two(cf, N_CLEN);
    uint8_t cl[N_CLEN];
    uint16_t cc[N_CLEN];
    build_lengths(cf, N_CLEN, MAX_CLEN, cl);
    build_codes(cl, N_CLEN, cc);
    int hclen = N_CLEN;
    while (hclen > 4 && !cl[clen_order[hclen - 1]])
        hclen--;
    uint64_t header = 3 + 5 + 5 + 4 + 3ull * (uint64_t)hclen;
    for (int i = 0; i < nrle; i++) {
        int s = rle[i] & 0xff;
        header += cl[s] + (s == 16 ? 2 : s == 17 ? 3 : s == 18 ? 7 : 0);
    }
    uint64_t dyn_cost = header + data_bits(t, n, dll, ddl);

    // Stored: the header bits, the alignment, then 4 + len bytes per 64 KB.
    uint64_t stored_cost = (uint64_t)(len / DEFLATE_STORED + 1) * (3 + 7 + 32) + 8ull * len;

    if (stored_cost < dyn_cost && stored_cost < fixed_cost) {
        write_stored(w, src, len, last);
    } else if (fixed_cost <= dyn_cost) {
        put_bits(w, last ? 1 : 0, 1);
        put_bits(w, 1, 2); // BTYPE 01: fixed Huffman
        write_tokens(w, t, n, fll, flc, fdl, fdc);
    } else {
        uint16_t dlc[N_LITLEN], ddc[N_DIST];
        build_codes(dll, N_LITLEN, dlc);
        build_codes(ddl, N_DIST, ddc);
        put_bits(w, last ? 1 : 0, 1);
        put_bits(w, 2, 2); // BTYPE 10: dynamic Huffman
        put_bits(w, (uint32_t)(hlit - 257), 5);
        put_bits(w, (uint32_t)(hdist - 1), 5);
        put_bits(w, (uint32_t)(hclen - 4), 4);
        for (int i = 0; i < hclen; i++)
            put_bits(w, cl[clen_order[i]], 3);
        for (int i = 0; i < nrle; i++) {
            int s = rle[i] & 0xff, extra = rle[i] >> 8;
            put_code(w, cc[s], cl[s]);
            if (s == 16)
                put_bits(w, (uint32_t)extra, 2);
            else if (s == 17)
                put_bits(w, (uint32_t)extra, 3);
            else if (s == 18)
                put_bits(w, (uint32_t)extra, 7);
        }
        write_tokens(w, t, n, dll, dlc, ddl, ddc);
    }
}

// Bytes of input token `t` covers.
static size_t token_bytes(const token_t *t) {
    return t->dist ? t->len : 1;
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
    if (level <= 0 || len == 0) {
        write_stored(&w, in, len, true);
    } else {
        size_t nt = tokenise(st, in, len, chain_for(level));
        size_t at = 0, from = 0;
        while (at < nt || from == 0) {
            size_t n = nt - at < DEFLATE_BLOCK_TOKENS ? nt - at : DEFLATE_BLOCK_TOKENS;
            size_t bytes = 0;
            for (size_t i = 0; i < n; i++)
                bytes += token_bytes(&st->tok[at + i]);
            write_block(&w, st->tok + at, n, in + from, bytes, at + n == nt);
            at += n;
            from += bytes;
            if (at >= nt)
                break;
        }
        align_byte(&w);
    }
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
