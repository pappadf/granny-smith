// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// inflate.c
// zlib/DEFLATE decompression for the emulator — see inflate.h — over the
// decoder peeler owns, plus the RFC 1951 tables the PNG writer in debug.c
// shares.

#include "inflate.h"

#include "peeler.h"

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

// The decoder itself is peeler's (peel_inflate): one inflate in the tree,
// shared with zip and gzip.  These keep the emulator's two call shapes and
// its contract: the zlib header is checked, the Adler-32 trailer is not
// (it never was, and UDIF chunks carry their own table checksums).

// Inflate the deflate stream inside a zlib stream.  -1 on a bad header.
static int64_t zlib_body(const uint8_t *data, size_t data_len, uint8_t *out, size_t out_cap) {
    if (data_len < 6)
        return -1; // zlib header + at least one block
    // RFC 1950 §2.2: CM must be 8 (deflate), the two header bytes form a
    // multiple of 31, and a preset dictionary (FDICT) is never used here.
    uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8 || (((uint32_t)cmf << 8) | flg) % 31u != 0 || (flg & 0x20))
        return -1;
    return peel_inflate(data + 2, data_len - 2, out, out_cap);
}

uint8_t *inflate_zlib_alloc(const uint8_t *data, size_t data_len, size_t out_max, size_t *out_len) {
    if (!data || !out_len || out_max == 0)
        return NULL;
    // Unknown size: try a buffer, and double it (up to out_max) while the
    // stream does not fit.  A PNG's stream is small; this rarely loops.
    size_t cap = out_max < (1u << 16) ? out_max : (1u << 16);
    for (;;) {
        uint8_t *buf = malloc(cap);
        if (!buf)
            return NULL;
        int64_t n = zlib_body(data, data_len, buf, cap);
        if (n >= 0) {
            *out_len = (size_t)n;
            return buf;
        }
        free(buf);
        if (cap >= out_max)
            return NULL; // malformed, or larger than the caller allows
        cap = cap > out_max / 2 ? out_max : cap * 2;
    }
}

long inflate_zlib(const uint8_t *data, size_t data_len, uint8_t *out, size_t out_cap) {
    if (!data || !out)
        return -1;
    int64_t n = zlib_body(data, data_len, out, out_cap);
    return n < 0 ? -1 : (long)n;
}
