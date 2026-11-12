// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// inflate.c
// DEFLATE decompressor (RFC 1951), resumable on the output side, and the
// zlib (RFC 1950) wrapper over it.  See codec.h.
//
// The one inflate in the tree: peeler's zip and gzip members decode through
// it, and so do the emulator's UDIF zlib chunks, QCOW2 clusters and PNG
// reader.  A member of a zip is read on demand, a little at a time, so the
// decoder must stop when the caller's buffer is full and pick up exactly
// there on the next call -- mid-block, even mid-match.  Input is pulled
// through a callback as needed; running out of input is a truncated stream
// (the whole compressed range is always available locally), so only the
// output side has to be resumable.
//
// Symbols are decoded through a 9-bit lookup table, with a canonical walk
// for the rarer longer codes.

#include "codec_internal.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants
// ============================================================================

#define INF_WINDOW    32768u // RFC 1951 § 2: back-references reach 32 KiB
#define INF_FAST_BITS 9 // primary lookup table width
#define INF_IN_CHUNK  16384 // compressed bytes pulled per refill

// RFC 1951 § 3.2.5 length and distance code tables, shared with deflate.c.
const uint16_t codec_deflate_len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t codec_deflate_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t codec_deflate_dist_base[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                              33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                              1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t codec_deflate_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// ============================================================================
// Types
// ============================================================================

// A canonical Huffman code: per-length counts and symbols in canonical
// order (for the slow walk), plus the fast table: entry = sym << 4 | len,
// 0 when the code is longer than INF_FAST_BITS.
typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
    uint16_t fast[1u << INF_FAST_BITS];
} inf_huff_t;

typedef enum { ST_HEADER, ST_STORED, ST_HUFF, ST_DONE } inf_state_t;

struct codec_inflater {
    codec_pull_fn pull;
    void *pull_ctx;
    uint8_t in[INF_IN_CHUNK];
    size_t in_pos, in_len;
    bool in_eof;
    uint64_t in_total; // compressed bytes pulled so far

    uint64_t bitbuf; // LSB first
    int bitcnt; // valid bits in bitbuf

    uint8_t window[INF_WINDOW];
    uint64_t total_out; // bytes produced so far

    inf_state_t state;
    bool last; // the current block is the final one
    uint32_t stored_left; // bytes left in a stored block
    uint32_t copy_len, copy_dist; // a match not yet fully emitted
    inf_huff_t lit, dist;
};

// ============================================================================
// Bit input
// ============================================================================

// Top the bit buffer up to at least `need` bits (need <= 57) if input remains.
static int inf_refill(codec_inflater_t *z, int need) {
    while (z->bitcnt < need) {
        if (z->in_pos == z->in_len) {
            if (z->in_eof)
                return 0; // no more input: the caller checks bitcnt
            int64_t n = z->pull(z->pull_ctx, z->in, sizeof(z->in));
            if (n < 0)
                return (int)n;
            if (n == 0) {
                z->in_eof = true;
                return 0;
            }
            z->in_pos = 0;
            z->in_len = (size_t)n;
            z->in_total += (uint64_t)n;
        }
        z->bitbuf |= (uint64_t)z->in[z->in_pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    return 0;
}

// Take `n` bits (0..25).  -1 when the input ran out (a truncated stream).
static int64_t inf_bits(codec_inflater_t *z, int n) {
    if (n == 0)
        return 0;
    if (z->bitcnt < n) {
        int rc = inf_refill(z, n);
        if (rc < 0 || z->bitcnt < n)
            return -1;
    }
    uint32_t v = (uint32_t)(z->bitbuf & ((1u << n) - 1));
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return v;
}

// ============================================================================
// Huffman codes
// ============================================================================

// Reverse the low `n` bits of `v` (deflate stores codes MSB-first in an
// LSB-first bit stream).
static uint32_t inf_reverse(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

// Build a code from per-symbol lengths.  0, or -1 for an over-subscribed set.
static int inf_build(inf_huff_t *h, const uint8_t *lengths, int n) {
    memset(h->counts, 0, sizeof(h->counts));
    memset(h->fast, 0, sizeof(h->fast));
    for (int i = 0; i < n; i++)
        h->counts[lengths[i]]++;
    h->counts[0] = 0;
    // Over-subscription check (an incomplete code is legal: RFC 1951 allows
    // a single distance code, and unused codes simply never appear).
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0)
            return -1;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->counts[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbols[offs[lengths[i]]++] = (uint16_t)i;
    // Fast table: walk codes in canonical order, assigning code values.
    uint32_t code = 0;
    int idx = 0;
    for (int len = 1; len <= INF_FAST_BITS; len++) {
        for (int k = 0; k < h->counts[len]; k++, idx++, code++) {
            uint32_t rev = inf_reverse(code, len);
            uint16_t entry = (uint16_t)(h->symbols[idx] << 4 | len);
            for (uint32_t fill = rev; fill < (1u << INF_FAST_BITS); fill += 1u << len)
                h->fast[fill] = entry;
        }
        code <<= 1;
    }
    return 0;
}

// Decode one symbol.  -1 on a truncated or malformed stream.
static int inf_decode(codec_inflater_t *z, const inf_huff_t *h) {
    if (z->bitcnt < 15) {
        int rc = inf_refill(z, 15);
        if (rc < 0)
            return -1;
    }
    uint16_t e = h->fast[z->bitbuf & ((1u << INF_FAST_BITS) - 1)];
    if (e) {
        int len = e & 15;
        if (len > z->bitcnt)
            return -1;
        z->bitbuf >>= len;
        z->bitcnt -= len;
        return e >> 4;
    }
    // Canonical walk, one bit at a time (codes longer than the fast table).
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        if (len > z->bitcnt)
            return -1;
        code |= (int)((z->bitbuf >> (len - 1)) & 1);
        int count = h->counts[len];
        if (code - first < count) {
            z->bitbuf >>= len;
            z->bitcnt -= len;
            return h->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

// ============================================================================
// Block headers
// ============================================================================

// The fixed codes of RFC 1951 § 3.2.6.
static void inf_fixed(codec_inflater_t *z) {
    uint8_t ll[288], dl[30];
    for (int i = 0; i < 288; i++)
        ll[i] = (uint8_t)((i < 144) ? 8 : (i < 256) ? 9 : (i < 280) ? 7 : 8);
    for (int i = 0; i < 30; i++)
        dl[i] = 5;
    inf_build(&z->lit, ll, 288);
    inf_build(&z->dist, dl, 30);
}

// Read a dynamic block's code definitions (RFC 1951 § 3.2.7).  0 / -1.
static int inf_dynamic(codec_inflater_t *z) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int64_t hlit = inf_bits(z, 5), hdist = inf_bits(z, 5), hclen = inf_bits(z, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0)
        return -1;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30)
        return -1;
    uint8_t cl[19] = {0};
    for (int i = 0; i < hclen; i++) {
        int64_t v = inf_bits(z, 3);
        if (v < 0)
            return -1;
        cl[order[i]] = (uint8_t)v;
    }
    inf_huff_t clh;
    if (inf_build(&clh, cl, 19) != 0)
        return -1;
    uint8_t lengths[320] = {0};
    int n = 0, total = (int)(hlit + hdist);
    while (n < total) {
        int sym = inf_decode(z, &clh);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
            continue;
        }
        int64_t rep;
        uint8_t val = 0;
        if (sym == 16) {
            if (n == 0)
                return -1;
            val = lengths[n - 1];
            rep = inf_bits(z, 2);
            rep = rep < 0 ? -1 : rep + 3;
        } else if (sym == 17) {
            rep = inf_bits(z, 3);
            rep = rep < 0 ? -1 : rep + 3;
        } else {
            rep = inf_bits(z, 7);
            rep = rep < 0 ? -1 : rep + 11;
        }
        if (rep < 0 || n + rep > total)
            return -1;
        while (rep-- > 0)
            lengths[n++] = val;
    }
    if (lengths[256] == 0)
        return -1; // no end-of-block code: the block could never end
    if (inf_build(&z->lit, lengths, (int)hlit) != 0 || inf_build(&z->dist, lengths + hlit, (int)hdist) != 0)
        return -1;
    return 0;
}

// Start the next block.  0 / -1.
static int inf_header(codec_inflater_t *z) {
    int64_t bfinal = inf_bits(z, 1), btype = inf_bits(z, 2);
    if (bfinal < 0 || btype < 0)
        return -1;
    z->last = bfinal != 0;
    if (btype == 0) {
        // Stored: skip to a byte boundary, then LEN and its complement.
        (void)inf_bits(z, z->bitcnt & 7);
        int64_t len = inf_bits(z, 16), nlen = inf_bits(z, 16);
        if (len < 0 || nlen < 0 || (len ^ 0xFFFF) != nlen)
            return -1;
        z->stored_left = (uint32_t)len;
        z->state = ST_STORED;
        return 0;
    }
    if (btype == 1)
        inf_fixed(z);
    else if (btype == 2) {
        if (inf_dynamic(z) != 0)
            return -1;
    } else
        return -1; // reserved
    z->state = ST_HUFF;
    return 0;
}

// ============================================================================
// Output
// ============================================================================

// Emit one byte to both the caller's buffer and the window.
static inline void inf_put(codec_inflater_t *z, uint8_t *out, size_t *n, uint8_t b) {
    out[(*n)++] = b;
    z->window[z->total_out & (INF_WINDOW - 1)] = b;
    z->total_out++;
}

// Continue a pending match into out[*n..cap).
static void inf_copy(codec_inflater_t *z, uint8_t *out, size_t *n, size_t cap) {
    while (z->copy_len > 0 && *n < cap) {
        uint8_t b = z->window[(z->total_out - z->copy_dist) & (INF_WINDOW - 1)];
        inf_put(z, out, n, b);
        z->copy_len--;
    }
}

// ============================================================================
// Public (codec.h)
// ============================================================================

codec_inflater_t *codec_inflater_new(codec_pull_fn pull, void *ctx) {
    codec_inflater_t *z = calloc(1, sizeof(*z));
    if (!z)
        return NULL;
    z->pull = pull;
    z->pull_ctx = ctx;
    z->state = ST_HEADER;
    return z;
}

void codec_inflater_free(codec_inflater_t *z) {
    free(z);
}

uint64_t codec_inflater_total_out(const codec_inflater_t *z) {
    return z->total_out;
}

// Compressed bytes consumed: pulled, less what is still buffered.
uint64_t codec_inflater_consumed(const codec_inflater_t *z) {
    return z->in_total - (z->in_len - z->in_pos) - (uint64_t)(z->bitcnt / 8);
}

// Discard the partial byte after the final block (a gzip trailer follows on
// a byte boundary) and hand back the whole bytes still buffered, by count.
void codec_inflater_align(codec_inflater_t *z) {
    int drop = z->bitcnt & 7;
    z->bitbuf >>= drop;
    z->bitcnt -= drop;
}

int codec_inflater_run(codec_inflater_t *z, uint8_t *out, size_t cap, size_t *out_n) {
    size_t n = 0;
    *out_n = 0;
    while (n < cap) {
        if (z->copy_len) {
            inf_copy(z, out, &n, cap);
            continue;
        }
        switch (z->state) {
        case ST_DONE:
            *out_n = n;
            return 1;
        case ST_HEADER:
            if (inf_header(z) != 0)
                goto bad;
            break;
        case ST_STORED:
            if (z->stored_left == 0) {
                z->state = z->last ? ST_DONE : ST_HEADER;
                break;
            }
            {
                int64_t b = inf_bits(z, 8);
                if (b < 0)
                    goto bad;
                inf_put(z, out, &n, (uint8_t)b);
                z->stored_left--;
            }
            break;
        case ST_HUFF: {
            int sym = inf_decode(z, &z->lit);
            if (sym < 0)
                goto bad;
            if (sym < 256) {
                inf_put(z, out, &n, (uint8_t)sym);
                break;
            }
            if (sym == 256) {
                z->state = z->last ? ST_DONE : ST_HEADER;
                break;
            }
            sym -= 257;
            if (sym >= 29)
                goto bad;
            int64_t le = inf_bits(z, codec_deflate_len_extra[sym]);
            int dsym = inf_decode(z, &z->dist);
            if (le < 0 || dsym < 0 || dsym >= 30)
                goto bad;
            int64_t de = inf_bits(z, codec_deflate_dist_extra[dsym]);
            if (de < 0)
                goto bad;
            uint32_t dist = codec_deflate_dist_base[dsym] + (uint32_t)de;
            if (dist > z->total_out || dist > INF_WINDOW)
                goto bad; // reaches before the start of the stream
            z->copy_len = codec_deflate_len_base[sym] + (uint32_t)le;
            z->copy_dist = dist;
            break;
        }
        }
    }
    *out_n = n;
    return z->state == ST_DONE && z->copy_len == 0 ? 1 : 0;
bad:
    *out_n = n;
    return -1;
}

// ============================================================================
// Whole-buffer entry points (codec.h)
// ============================================================================

typedef struct {
    const uint8_t *src;
    size_t len, pos;
} mem_pull_t;

// codec_pull_fn over a memory buffer.
static int64_t mem_pull(void *ctx, uint8_t *buf, size_t cap) {
    mem_pull_t *m = ctx;
    size_t n = m->len - m->pos < cap ? m->len - m->pos : cap;
    memcpy(buf, m->src + m->pos, n);
    m->pos += n;
    return (int64_t)n;
}

// Inflate the raw stream at src[0..len) into dst; *used gets the compressed
// bytes consumed (the final block's partial byte included).
static int64_t inflate_mem(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, uint64_t *used) {
    if ((!src && len) || (!dst && cap))
        return CODEC_ERR_ARG;
    mem_pull_t m = {src, len, 0};
    codec_inflater_t *z = codec_inflater_new(mem_pull, &m);
    if (!z)
        return CODEC_ERR_NOMEM;
    size_t n = 0;
    int64_t rc = codec_inflater_run(z, dst, cap, &n);
    if (rc == 0) {
        // Output full: the stream must end right here, not overrun.
        uint8_t probe;
        size_t extra = 0;
        rc = codec_inflater_run(z, &probe, 1, &extra);
        if (extra)
            rc = CODEC_ERR_OVERFLOW;
    }
    if (used)
        *used = codec_inflater_consumed(z);
    codec_inflater_free(z);
    if (rc == 1)
        return (int64_t)n;
    return rc == CODEC_ERR_OVERFLOW ? CODEC_ERR_OVERFLOW : CODEC_ERR_DATA;
}

int64_t codec_inflate(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    return inflate_mem(src, len, dst, cap, NULL);
}

int64_t codec_zlib_inflate(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    if (!src)
        return CODEC_ERR_ARG;
    if (len < 6)
        return CODEC_ERR_TRUNCATED; // zlib header + at least one block
    // RFC 1950 § 2.2: CM 8, header a multiple of 31, no preset dictionary.
    uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8 || (((uint32_t)cmf << 8) | flg) % 31u != 0 || (flg & 0x20))
        return CODEC_ERR_DATA;
    uint64_t used = 0;
    int64_t n = inflate_mem(src + 2, len - 2, dst, cap, &used);
    if (n < 0)
        return n;
    // The Adler-32 trailer, when present, must match.  Some writers omit it
    // (UDIF chunks always carry it); a stream that ends early is accepted.
    if (2 + used + 4 <= len) {
        uint32_t want = codec_be32(src + 2 + used);
        if (want != codec_adler32(1, dst, (size_t)n))
            return CODEC_ERR_CHECKSUM;
    }
    return n;
}

uint8_t *codec_zlib_inflate_alloc(const uint8_t *src, size_t len, size_t out_max, size_t *out_len) {
    if (!src || !out_len || out_max == 0)
        return NULL;
    // Unknown size: try a buffer, and double it (up to out_max) while the
    // stream does not fit.  A PNG's stream is small; this rarely loops.
    size_t cap = out_max < (1u << 16) ? out_max : (1u << 16);
    for (;;) {
        uint8_t *buf = malloc(cap);
        if (!buf)
            return NULL;
        int64_t n = codec_zlib_inflate(src, len, buf, cap);
        if (n >= 0) {
            *out_len = (size_t)n;
            return buf;
        }
        free(buf);
        if (n != CODEC_ERR_OVERFLOW || cap >= out_max)
            return NULL; // malformed, or larger than the caller allows
        cap = cap > out_max / 2 ? out_max : cap * 2;
    }
}
