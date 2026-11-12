// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vec_gen.h
// The data sets the codec vectors (vectors.h) decode to, regenerated rather
// than stored.  Shared by the codec and udif suites; must match gen() in
// make_vectors.py byte for byte.

#ifndef VEC_GEN_H
#define VEC_GEN_H

#include "test_assert.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// `n` bytes of data set `kind` -- 0 text-like, 1 runs of one byte (1..700
// long), 2 incompressible, 3 zeros -- from xorshift32 seeded with `seed`.
// malloc'd; the caller frees.
static inline uint8_t *vec_gen(int kind, size_t n, uint32_t seed) {
    static const char *words[] = {"System", "Folder",   "Finder", "the",  "of",   "Macintosh", "disk",
                                  "file",   "resource", "fork",   "INIT", "cdev", "{",         "}\r"};
    uint32_t x = seed;
    uint8_t *b = (uint8_t *)malloc(n ? n : 1);
    ASSERT_TRUE(b != NULL);
    size_t at = 0;
    while (at < n) {
        if (kind == 3) {
            b[at++] = 0;
            continue;
        }
        // xorshift32
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        if (kind == 0) {
            const char *w = words[x % (sizeof(words) / sizeof(words[0]))];
            for (size_t i = 0; w[i] && at < n; i++)
                b[at++] = (uint8_t)w[i];
            if (at < n)
                b[at++] = ' ';
        } else if (kind == 1) {
            size_t run = 1 + (x >> 8) % 700;
            for (size_t i = 0; i < run && at < n; i++)
                b[at++] = (uint8_t)(x & 0xFF);
        } else {
            b[at++] = (uint8_t)(x & 0xFF);
        }
    }
    return b;
}

#endif // VEC_GEN_H
