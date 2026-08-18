// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// inflate.c
// zlib/DEFLATE decompressor — see inflate.h.  Extracted verbatim from the PNG
// reader in debug.c (where it had been private) so image_udif.c can share it;
// the zlib-header validation and the fixed-capacity entry point are new.

#include "inflate.h"

#include <stdlib.h>

const uint16_t deflate_len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t deflate_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t deflate_dist_base[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                        33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t deflate_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// LSB-first bit reader over a deflate stream.
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t acc;
    int nbits;
    int error;
} inflate_br_t;

// Growable or fixed output sink: `grow` distinguishes the malloc'ing entry
// point from the caller-supplied-buffer one, which must never reallocate.
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    int grow;
} inflate_out_t;

// Pull `need` bits (0..16), least-significant bit first.
static uint32_t inflate_bits(inflate_br_t *b, int need) {
    if (need <= 0)
        return 0;
    while (b->nbits < need) {
        if (b->pos >= b->len) {
            b->error = 1;
            return 0;
        }
        b->acc |= (uint32_t)b->data[b->pos++] << b->nbits;
        b->nbits += 8;
    }
    uint32_t v = b->acc & ((1u << need) - 1);
    b->acc >>= need;
    b->nbits -= need;
    return v;
}

// A canonical Huffman table: how many codes exist per bit length, plus the
// symbols in canonical order.
typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
} inflate_huff_t;

// Build a canonical Huffman table from an array of per-symbol code lengths.
static void inflate_build(inflate_huff_t *h, const uint8_t *lengths, int n) {
    for (int i = 0; i < 16; i++)
        h->counts[i] = 0;
    for (int i = 0; i < n; i++)
        h->counts[lengths[i]]++;
    h->counts[0] = 0;
    uint16_t offsets[16];
    offsets[0] = 0;
    offsets[1] = 0;
    for (int i = 1; i < 15; i++)
        offsets[i + 1] = (uint16_t)(offsets[i] + h->counts[i]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbols[offsets[lengths[i]]++] = (uint16_t)i;
}

// Decode one symbol, walking code lengths shortest-first.  Returns -1 on a
// malformed stream.
static int inflate_decode(inflate_br_t *b, const inflate_huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)inflate_bits(b, 1);
        if (b->error)
            return -1;
        int count = h->counts[len];
        if (code - first < count)
            return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

// Append one byte to the output sink, growing it when allowed.  Returns 0 when
// the fixed buffer is full or an allocation fails.
static int inflate_push(inflate_out_t *o, uint8_t byte) {
    if (o->pos >= o->cap) {
        // A fixed-capacity caller stated the exact expected size: overflowing
        // it means the stream disagrees, which is an error rather than a clamp.
        if (!o->grow)
            return 0;
        size_t new_cap = o->cap * 2;
        uint8_t *grown = realloc(o->buf, new_cap);
        if (!grown)
            return 0;
        o->buf = grown;
        o->cap = new_cap;
    }
    o->buf[o->pos++] = byte;
    return 1;
}

// Inflate the deflate blocks of a zlib stream into `o`.  Returns 0 on success,
// -1 if the stream is truncated, malformed, or overruns a fixed output buffer.
static int inflate_stream(const uint8_t *data, size_t data_len, inflate_out_t *o) {
    if (data_len < 6)
        return -1; // zlib header + at least one block

    // RFC 1950 §2.2: CM must be 8 (deflate), the two header bytes form a
    // multiple of 31, and a preset dictionary (FDICT) is never used here.
    uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8 || (((uint32_t)cmf << 8) | flg) % 31u != 0 || (flg & 0x20))
        return -1;

    inflate_br_t b = {data + 2, data_len - 2, 0, 0, 0, 0};

    for (;;) {
        int bfinal = (int)inflate_bits(&b, 1);
        int btype = (int)inflate_bits(&b, 2);
        if (b.error)
            return -1;

        if (btype == 0) {
            // Stored: discard the partial byte, then copy LEN raw bytes.
            b.acc = 0;
            b.nbits = 0;
            if (b.pos + 4 > b.len)
                return -1;
            uint16_t len = (uint16_t)(b.data[b.pos] | ((uint16_t)b.data[b.pos + 1] << 8));
            b.pos += 4; // LEN + NLEN
            if (b.pos + len > b.len)
                return -1;
            for (uint16_t i = 0; i < len; i++)
                if (!inflate_push(o, b.data[b.pos + i]))
                    return -1;
            b.pos += len;
        } else if (btype == 1 || btype == 2) {
            inflate_huff_t lit, dist;
            if (btype == 1) {
                // Fixed code lengths (RFC 1951 §3.2.6).
                uint8_t ll[288], dl[30];
                for (int i = 0; i < 288; i++)
                    ll[i] = (i < 144) ? 8 : (i < 256) ? 9 : (i < 280) ? 7 : 8;
                for (int i = 0; i < 30; i++)
                    dl[i] = 5;
                inflate_build(&lit, ll, 288);
                inflate_build(&dist, dl, 30);
            } else {
                // Dynamic: read the code-length code, then the two real ones.
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                int hlit = (int)inflate_bits(&b, 5) + 257;
                int hdist = (int)inflate_bits(&b, 5) + 1;
                int hclen = (int)inflate_bits(&b, 4) + 4;
                if (b.error || hlit > 286 || hdist > 30)
                    return -1;
                uint8_t cl[19] = {0};
                for (int i = 0; i < hclen; i++)
                    cl[order[i]] = (uint8_t)inflate_bits(&b, 3);
                if (b.error)
                    return -1;
                inflate_huff_t clh;
                inflate_build(&clh, cl, 19);

                uint8_t lengths[318] = {0};
                int n = 0;
                while (n < hlit + hdist) {
                    int sym = inflate_decode(&b, &clh);
                    if (sym < 0)
                        return -1;
                    if (sym < 16) {
                        lengths[n++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (n == 0)
                            return -1;
                        uint8_t prev = lengths[n - 1];
                        int repeat = 3 + (int)inflate_bits(&b, 2);
                        while (repeat-- > 0 && n < hlit + hdist)
                            lengths[n++] = prev;
                    } else if (sym == 17) {
                        int repeat = 3 + (int)inflate_bits(&b, 3);
                        while (repeat-- > 0 && n < hlit + hdist)
                            lengths[n++] = 0;
                    } else {
                        int repeat = 11 + (int)inflate_bits(&b, 7);
                        while (repeat-- > 0 && n < hlit + hdist)
                            lengths[n++] = 0;
                    }
                    if (b.error)
                        return -1;
                }
                inflate_build(&lit, lengths, hlit);
                inflate_build(&dist, lengths + hlit, hdist);
            }

            for (;;) {
                int sym = inflate_decode(&b, &lit);
                if (sym < 0)
                    return -1;
                if (sym == 256)
                    break;
                if (sym < 256) {
                    if (!inflate_push(o, (uint8_t)sym))
                        return -1;
                    continue;
                }
                sym -= 257;
                if (sym >= 29)
                    return -1;
                size_t length = deflate_len_base[sym] + inflate_bits(&b, deflate_len_extra[sym]);
                int dsym = inflate_decode(&b, &dist);
                if (dsym < 0 || dsym >= 30)
                    return -1;
                size_t distance = deflate_dist_base[dsym] + inflate_bits(&b, deflate_dist_extra[dsym]);
                if (b.error || distance == 0 || distance > o->pos)
                    return -1;
                for (size_t i = 0; i < length; i++)
                    if (!inflate_push(o, o->buf[o->pos - distance]))
                        return -1;
            }
        } else {
            return -1; // BTYPE 3 is reserved
        }

        if (bfinal)
            break;
    }
    return 0;
}

uint8_t *inflate_zlib_alloc(const uint8_t *data, size_t data_len, size_t *out_len) {
    if (!data || !out_len)
        return NULL;
    inflate_out_t o = {malloc(1 << 16), 1 << 16, 0, 1};
    if (!o.buf)
        return NULL;
    if (inflate_stream(data, data_len, &o) != 0) {
        free(o.buf);
        return NULL;
    }
    *out_len = o.pos;
    return o.buf;
}

long inflate_zlib(const uint8_t *data, size_t data_len, uint8_t *out, size_t out_cap) {
    if (!data || !out)
        return -1;
    inflate_out_t o = {out, out_cap, 0, 0};
    if (inflate_stream(data, data_len, &o) != 0)
        return -1;
    return (long)o.pos;
}
