// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sit13.c — StuffIt method 13 (LZSS + Huffman) decompressor.
//
// Format spec: sit13.md
//
// This is an internal helper called by sit.c for entries compressed with
// method 13.  It is not part of the public API.
//
// Method 13 combines a 64 KiB LZSS sliding window (sit13.md § 8
// "Sliding Window") with three canonical Huffman codes — two alternating
// literal/length codes and one distance code (sit13.md § 9 "Decompression
// Procedure").  Trees are either selected from five built-in sets
// (sit13.md § 7 "Predefined Trees (Sets 1–5)") or dynamically serialized
// via a fixed 37-symbol meta-code (sit13.md § 6 "Tree Serialization
// (Dynamic Mode)").

#include "internal.h"

// ============================================================================
// Constants and Macros
// ============================================================================

// sit13.md § 5.1 "Literal/Length Symbol Alphabet" — 321 symbols per tree.
// sit13.md § 8 "Sliding Window" — 64 KiB window.

// Number of symbols in each literal/length tree.
// sit13.md § 5.1 — symbols 0..255 are literals, 256..319 encode match
// lengths, and 320 is a reserved/invalid sentinel.
#define M13_SYM_COUNT   321

// Sliding window size and mask for circular indexing.
// sit13.md § 8 "Sliding Window" — 64 KiB.
#define M13_WIN_SIZE    65536
#define M13_WIN_MASK    (M13_WIN_SIZE - 1)

// Predefined code-length tables for the 5 built-in Huffman code sets.
// sit13.md § 7 "Predefined Trees (Sets 1–5)" and § 7.3 "Code-Length Tables"
// — these tables are part of the format specification; every conformant
// encoder/decoder uses them verbatim.

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-braces"
#endif

static const int8_t predefined_first[5][M13_SYM_COUNT] = {
    4,  5,  7,  8,  8,  9,  9,  9,  9,  7,  9,  9,  9,  8,  9,  9,  9,  9,  9,  9,  9,  9,  9,  10, 9,  9,  10, 10, 9,
    10, 9,  9,  5,  9,  9,  9,  9,  10, 9,  9,  9,  9,  9,  9,  9,  9,  7,  9,  9,  8,  9,  9,  9,  9,  9,  9,  9,  9,
    9,  9,  9,  9,  9,  9,  9,  8,  9,  9,  8,  8,  9,  9,  9,  9,  9,  9,  9,  7,  8,  9,  7,  9,  9,  7,  7,  9,  9,
    9,  9,  10, 9,  10, 10, 10, 9,  9,  9,  5,  9,  8,  7,  5,  9,  8,  8,  7,  9,  9,  8,  8,  5,  5,  7,  10, 5,  8,
    5,  8,  9,  9,  9,  9,  9,  10, 9,  9,  10, 9,  9,  10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 9,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 9,  9,  10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 9,  9,  10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 9,  10, 9,  5,  6,  5,  5,  8,  9,
    9,  9,  9,  9,  9,  10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 9,  10, 9,  9,  9,  10, 9,  10, 9,  10, 9,  10, 9,  10, 10, 10, 9,  10, 9,  10, 10, 9,  9,  9,  6,  9,  9,  10,
    9,  5,  4,  7,  7,  8,  7,  8,  8,  8,  8,  7,  8,  7,  8,  7,  9,  8,  8,  8,  9,  9,  9,  9,  10, 10, 9,  10, 10,
    10, 10, 10, 9,  9,  5,  9,  8,  9,  9,  11, 10, 9,  8,  9,  9,  9,  8,  9,  7,  8,  8,  8,  9,  9,  9,  9,  9,  10,
    9,  9,  9,  10, 9,  9,  10, 9,  8,  8,  7,  7,  7,  8,  8,  9,  8,  8,  9,  9,  8,  8,  7,  8,  7,  10, 8,  7,  7,
    9,  9,  9,  9,  10, 10, 11, 11, 11, 10, 9,  8,  6,  8,  7,  7,  5,  7,  7,  7,  6,  9,  8,  6,  7,  6,  6,  7,  9,
    6,  6,  6,  7,  8,  8,  8,  8,  9,  10, 9,  10, 9,  9,  8,  9,  10, 10, 9,  10, 10, 9,  9,  10, 10, 10, 10, 10, 10,
    10, 9,  10, 10, 11, 10, 10, 10, 10, 10, 10, 10, 11, 10, 11, 10, 10, 9,  11, 10, 10, 10, 10, 10, 10, 9,  9,  10, 11,
    10, 11, 10, 11, 10, 12, 10, 11, 10, 12, 11, 12, 10, 12, 10, 11, 10, 11, 11, 11, 9,  10, 11, 11, 11, 12, 12, 10, 10,
    10, 11, 11, 10, 11, 10, 10, 9,  11, 10, 11, 10, 11, 11, 11, 10, 11, 11, 12, 11, 11, 10, 10, 10, 11, 10, 10, 11, 11,
    12, 10, 10, 11, 11, 12, 11, 11, 10, 11, 9,  12, 10, 11, 11, 11, 10, 11, 10, 11, 10, 11, 9,  10, 9,  7,  3,  5,  6,
    6,  7,  7,  8,  8,  8,  9,  9,  9,  11, 10, 10, 10, 12, 13, 11, 12, 12, 11, 13, 12, 12, 11, 12, 12, 13, 12, 14, 13,
    14, 13, 15, 13, 14, 15, 15, 14, 13, 15, 15, 14, 15, 14, 15, 15, 14, 15, 13, 13, 14, 15, 15, 14, 14, 16, 16, 15, 15,
    15, 12, 15, 10, 6,  6,  6,  6,  6,  9,  8,  8,  4,  9,  8,  9,  8,  9,  9,  9,  8,  9,  9,  10, 8,  10, 10, 10, 9,
    10, 10, 10, 9,  10, 10, 9,  9,  9,  8,  10, 9,  10, 9,  10, 9,  10, 9,  10, 9,  9,  8,  9,  8,  9,  9,  9,  10, 10,
    10, 10, 9,  9,  9,  10, 9,  10, 9,  9,  7,  8,  8,  9,  8,  9,  9,  9,  8,  9,  9,  10, 9,  9,  8,  9,  8,  9,  8,
    8,  8,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10, 9,  8,  8,  9,  8,  9,  7,  8,  8,  9,  8,  10, 10, 8,  9,  8,  8,
    8,  10, 8,  8,  8,  8,  9,  9,  9,  9,  10, 10, 10, 10, 10, 9,  7,  9,  9,  10, 10, 10, 10, 10, 9,  10, 10, 10, 10,
    10, 10, 9,  9,  10, 10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 9,  9,
    9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 9,  8,  9,  10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 9,  10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9,  9,  10, 10, 10,
    10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 9,  9,  9,  10, 10, 10, 10, 10, 10, 9,  9,  10, 9,  9,  8,  9,  8,  9,  4,
    6,  6,  6,  7,  8,  8,  9,  9,  10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    7,  10, 10, 10, 7,  10, 10, 7,  7,  7,  7,  7,  6,  7,  10, 7,  7,  10, 7,  7,  7,  6,  7,  6,  6,  7,  7,  6,  6,
    9,  6,  9,  10, 6,  10, 2,  6,  6,  7,  7,  8,  7,  8,  7,  8,  8,  9,  8,  9,  9,  9,  8,  8,  9,  9,  9,  10, 10,
    9,  8,  10, 9,  10, 9,  10, 9,  9,  6,  9,  8,  9,  9,  10, 9,  9,  9,  10, 9,  9,  9,  9,  8,  8,  8,  8,  8,  9,
    9,  9,  9,  9,  9,  9,  9,  9,  9,  10, 10, 9,  7,  7,  8,  8,  8,  8,  9,  9,  7,  8,  9,  10, 8,  8,  7,  8,  8,
    10, 8,  8,  8,  9,  8,  9,  9,  10, 9,  11, 10, 11, 9,  9,  8,  7,  9,  8,  8,  6,  8,  8,  8,  7,  10, 9,  7,  8,
    7,  7,  8,  10, 7,  7,  7,  8,  9,  9,  9,  9,  10, 11, 9,  11, 10, 9,  7,  9,  10, 10, 10, 11, 11, 10, 10, 11, 10,
    10, 10, 11, 11, 10, 9,  10, 10, 11, 10, 11, 10, 11, 10, 10, 10, 11, 10, 11, 10, 10, 9,  10, 10, 11, 10, 10, 10, 10,
    9,  10, 10, 10, 10, 11, 10, 11, 10, 11, 10, 11, 11, 11, 10, 12, 10, 11, 10, 11, 10, 11, 11, 10, 8,  10, 10, 11, 10,
    11, 11, 11, 10, 11, 10, 11, 10, 11, 11, 11, 9,  10, 11, 11, 10, 11, 11, 11, 10, 11, 11, 11, 10, 10, 10, 10, 10, 11,
    10, 10, 11, 11, 10, 10, 9,  11, 10, 10, 11, 11, 10, 10, 10, 11, 10, 10, 10, 10, 10, 10, 9,  11, 10, 10, 8,  10, 8,
    6,  5,  6,  6,  7,  7,  8,  8,  8,  9,  10, 11, 10, 10, 11, 11, 12, 12, 10, 11, 12, 12, 12, 12, 13, 13, 13, 13, 13,
    12, 13, 13, 15, 14, 12, 14, 15, 16, 12, 12, 13, 15, 14, 16, 15, 17, 18, 15, 17, 16, 15, 15, 15, 15, 13, 13, 10, 14,
    12, 13, 17, 17, 18, 10, 17, 4,  7,  9,  9,  9,  9,  9,  9,  9,  9,  8,  9,  9,  9,  7,  9,  9,  9,  9,  9,  9,  9,
    9,  9,  10, 9,  10, 9,  10, 9,  10, 9,  9,  5,  9,  7,  9,  9,  9,  9,  9,  7,  7,  7,  9,  7,  7,  8,  7,  8,  8,
    7,  7,  9,  9,  9,  9,  7,  7,  7,  9,  9,  9,  9,  9,  9,  7,  9,  7,  7,  7,  7,  9,  9,  7,  9,  9,  7,  7,  7,
    7,  7,  9,  7,  8,  7,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  7,  8,  7,  7,  7,  8,  8,  6,  7,  9,  7,
    7,  8,  7,  5,  6,  9,  5,  7,  5,  6,  7,  7,  9,  8,  9,  9,  9,  9,  9,  9,  9,  9,  10, 9,  10, 10, 10, 9,  9,
    10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 9,  10, 10, 10, 9,  9,
    10, 9,  9,  9,  9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 9,  10, 10,
    10, 9,  10, 10, 10, 9,  9,  9,  10, 10, 10, 10, 10, 9,  10, 9,  10, 10, 9,  10, 10, 9,  10, 10, 10, 10, 10, 10, 10,
    9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9,  10, 10, 10, 10, 10, 10, 10, 9,  10, 9,  10, 9,
    10, 10, 9,  5,  6,  8,  8,  7,  7,  7,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,
    9,  9,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 9,  10, 10, 5,  10, 8,  9,  8,  9,
};

static const int8_t predefined_second[5][321] = {
    4,  5,  6,  6,  7,  7,  6,  7,  7,  7,  6,  8,  7,  8,  8,  8,  8,  9,  6,  9,  8,  9,  8,  9,  9,  9,  8,  10, 5,
    9,  7,  9,  6,  9,  8,  10, 9,  10, 8,  8,  9,  9,  7,  9,  8,  9,  8,  9,  8,  8,  6,  9,  9,  8,  8,  9,  9,  10,
    8,  9,  9,  10, 8,  10, 8,  8,  8,  8,  8,  9,  7,  10, 6,  9,  9,  11, 7,  8,  8,  9,  8,  10, 7,  8,  6,  9,  10,
    9,  9,  10, 8,  11, 9,  11, 9,  10, 9,  8,  9,  8,  8,  8,  8,  10, 9,  9,  10, 10, 8,  9,  8,  8,  8,  11, 9,  8,
    8,  9,  9,  10, 8,  11, 10, 10, 8,  10, 9,  10, 8,  9,  9,  11, 9,  11, 9,  10, 10, 11, 10, 12, 9,  12, 10, 11, 10,
    11, 9,  10, 10, 11, 10, 11, 10, 11, 10, 11, 10, 10, 10, 9,  9,  9,  8,  7,  6,  8,  11, 11, 9,  12, 10, 12, 9,  11,
    11, 11, 10, 12, 11, 11, 10, 12, 10, 11, 10, 10, 10, 11, 10, 11, 11, 11, 9,  12, 10, 12, 11, 12, 10, 11, 10, 12, 11,
    12, 11, 12, 11, 12, 10, 12, 11, 12, 11, 11, 10, 12, 10, 11, 10, 12, 10, 12, 10, 12, 10, 11, 11, 11, 10, 11, 11, 11,
    10, 12, 11, 12, 10, 10, 11, 11, 9,  12, 11, 12, 10, 11, 10, 12, 10, 11, 10, 12, 10, 11, 10, 7,  5,  4,  6,  6,  7,
    7,  7,  8,  8,  7,  7,  6,  8,  6,  7,  7,  9,  8,  9,  9,  10, 11, 11, 11, 12, 11, 10, 11, 12, 11, 12, 11, 12, 12,
    12, 12, 11, 12, 12, 11, 12, 11, 12, 11, 13, 11, 12, 10, 13, 10, 14, 14, 13, 14, 15, 14, 16, 15, 15, 18, 18, 18, 9,
    18, 8,  5,  6,  6,  6,  6,  7,  7,  7,  7,  7,  7,  8,  7,  8,  7,  7,  7,  8,  8,  8,  8,  9,  8,  9,  8,  9,  9,
    9,  7,  9,  8,  8,  6,  9,  8,  9,  8,  9,  8,  9,  8,  9,  8,  9,  8,  9,  8,  8,  8,  8,  8,  9,  8,  9,  8,  9,
    9,  10, 8,  10, 8,  9,  9,  8,  8,  8,  7,  8,  8,  9,  8,  9,  7,  9,  8,  10, 8,  9,  8,  9,  8,  9,  8,  8,  8,
    9,  9,  9,  9,  10, 9,  11, 9,  10, 9,  10, 8,  8,  8,  9,  8,  8,  8,  9,  9,  8,  9,  10, 8,  9,  8,  8,  8,  11,
    8,  7,  8,  9,  9,  9,  9,  10, 9,  10, 9,  10, 9,  8,  8,  9,  9,  10, 9,  10, 9,  10, 8,  10, 9,  10, 9,  11, 10,
    11, 9,  11, 10, 10, 10, 11, 9,  11, 9,  10, 9,  11, 9,  11, 10, 10, 9,  10, 9,  9,  8,  10, 9,  11, 9,  9,  9,  11,
    10, 11, 9,  11, 9,  11, 9,  11, 10, 11, 10, 11, 10, 11, 9,  10, 10, 11, 10, 10, 8,  10, 9,  10, 10, 11, 9,  11, 9,
    10, 10, 11, 9,  10, 10, 9,  9,  10, 9,  10, 9,  10, 9,  10, 9,  11, 9,  11, 10, 10, 9,  10, 9,  11, 9,  11, 9,  11,
    9,  10, 9,  11, 9,  11, 9,  11, 9,  10, 8,  11, 9,  10, 9,  10, 9,  10, 8,  10, 8,  9,  8,  9,  8,  7,  4,  4,  5,
    6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  7,  8,  8,  9,  9,  10, 10, 10, 10, 10, 10, 11, 11, 10, 10, 12, 11, 11, 12,
    12, 11, 12, 12, 11, 12, 12, 12, 12, 12, 12, 11, 12, 11, 13, 12, 13, 12, 13, 14, 14, 14, 15, 13, 14, 13, 14, 18, 18,
    17, 7,  16, 9,  5,  6,  6,  6,  6,  7,  7,  7,  6,  8,  7,  8,  7,  9,  8,  8,  7,  7,  8,  9,  9,  9,  9,  10, 8,
    9,  9,  10, 8,  10, 9,  8,  6,  10, 8,  10, 8,  10, 9,  9,  9,  9,  9,  10, 9,  9,  8,  9,  8,  9,  8,  9,  9,  10,
    9,  10, 9,  9,  8,  10, 9,  11, 10, 8,  8,  8,  8,  9,  7,  9,  9,  10, 8,  9,  8,  11, 9,  10, 9,  10, 8,  9,  9,
    9,  9,  8,  9,  9,  10, 10, 10, 12, 10, 11, 10, 10, 8,  9,  9,  9,  8,  9,  8,  8,  10, 9,  10, 11, 8,  10, 9,  9,
    8,  12, 8,  9,  9,  9,  9,  8,  9,  10, 9,  12, 10, 10, 10, 8,  7,  11, 10, 9,  10, 11, 9,  11, 7,  11, 10, 12, 10,
    12, 10, 11, 9,  11, 9,  12, 10, 12, 10, 12, 10, 9,  11, 12, 10, 12, 10, 11, 9,  10, 9,  10, 9,  11, 11, 12, 9,  10,
    8,  12, 11, 12, 9,  12, 10, 12, 10, 13, 10, 12, 10, 12, 10, 12, 10, 9,  10, 12, 10, 9,  8,  11, 10, 12, 10, 12, 10,
    12, 10, 11, 10, 12, 8,  12, 10, 11, 10, 10, 10, 12, 9,  11, 10, 12, 10, 12, 11, 12, 10, 9,  10, 12, 9,  10, 10, 12,
    10, 11, 10, 11, 10, 12, 8,  12, 9,  12, 8,  12, 8,  11, 10, 11, 10, 11, 9,  10, 8,  10, 9,  9,  8,  9,  8,  7,  4,
    3,  5,  5,  6,  5,  6,  6,  7,  7,  8,  8,  8,  7,  7,  7,  9,  8,  9,  9,  11, 9,  11, 9,  8,  9,  9,  11, 12, 11,
    12, 12, 13, 13, 12, 13, 14, 13, 14, 13, 14, 13, 13, 13, 12, 13, 13, 12, 13, 13, 14, 14, 13, 13, 14, 14, 14, 14, 15,
    18, 17, 18, 8,  16, 10, 4,  5,  6,  6,  6,  6,  7,  7,  6,  7,  7,  9,  6,  8,  8,  7,  7,  8,  8,  8,  6,  9,  8,
    8,  7,  9,  8,  9,  8,  9,  8,  9,  6,  9,  8,  9,  8,  10, 9,  9,  8,  10, 8,  10, 8,  9,  8,  9,  8,  8,  7,  9,
    9,  9,  9,  9,  8,  10, 9,  10, 9,  10, 9,  8,  7,  8,  9,  9,  8,  9,  9,  9,  7,  10, 9,  10, 9,  9,  8,  9,  8,
    9,  8,  8,  8,  9,  9,  10, 9,  9,  8,  11, 9,  11, 10, 10, 8,  8,  10, 8,  8,  9,  9,  9,  10, 9,  10, 11, 9,  9,
    9,  9,  8,  9,  8,  8,  8,  10, 10, 9,  9,  8,  10, 11, 10, 11, 11, 9,  8,  9,  10, 11, 9,  10, 11, 11, 9,  12, 10,
    10, 10, 12, 11, 11, 9,  11, 11, 12, 9,  11, 9,  10, 10, 10, 10, 12, 9,  11, 10, 11, 9,  11, 11, 11, 10, 11, 11, 12,
    9,  10, 10, 12, 11, 11, 10, 11, 9,  11, 10, 11, 10, 11, 9,  11, 11, 9,  8,  11, 10, 11, 11, 10, 7,  12, 11, 11, 11,
    11, 11, 12, 10, 12, 11, 13, 11, 10, 12, 11, 10, 11, 10, 11, 10, 11, 11, 11, 10, 12, 11, 11, 10, 11, 10, 10, 10, 11,
    10, 12, 11, 12, 10, 11, 9,  11, 10, 11, 10, 11, 10, 12, 9,  11, 11, 11, 9,  11, 10, 10, 9,  11, 10, 10, 9,  10, 9,
    7,  4,  5,  5,  5,  6,  6,  7,  6,  8,  7,  8,  9,  9,  7,  8,  8,  10, 9,  10, 10, 12, 10, 11, 11, 11, 11, 10, 11,
    12, 11, 11, 11, 11, 11, 13, 12, 11, 12, 13, 12, 12, 12, 13, 11, 9,  12, 13, 7,  13, 11, 13, 11, 10, 11, 13, 15, 15,
    12, 14, 15, 15, 15, 6,  15, 5,  8,  10, 11, 11, 11, 12, 11, 11, 12, 6,  11, 12, 10, 5,  12, 12, 12, 12, 12, 12, 12,
    13, 13, 14, 13, 13, 12, 13, 12, 13, 12, 15, 4,  10, 7,  9,  11, 11, 10, 9,  6,  7,  8,  9,  6,  7,  6,  7,  8,  7,
    7,  8,  8,  8,  8,  8,  8,  9,  8,  7,  10, 9,  10, 10, 11, 7,  8,  6,  7,  8,  8,  9,  8,  7,  10, 10, 8,  7,  8,
    8,  7,  10, 7,  6,  7,  9,  9,  8,  11, 11, 11, 10, 11, 11, 11, 8,  11, 6,  7,  6,  6,  6,  6,  8,  7,  6,  10, 9,
    6,  7,  6,  6,  7,  10, 6,  5,  6,  7,  7,  7,  10, 8,  11, 9,  13, 7,  14, 16, 12, 14, 14, 15, 15, 16, 16, 14, 15,
    15, 15, 15, 15, 15, 15, 15, 14, 15, 13, 14, 14, 16, 15, 17, 14, 17, 15, 17, 12, 14, 13, 16, 12, 17, 13, 17, 14, 13,
    13, 14, 14, 12, 13, 15, 15, 14, 15, 17, 14, 17, 15, 14, 15, 16, 12, 16, 15, 14, 15, 16, 15, 16, 17, 17, 15, 15, 17,
    17, 13, 14, 15, 15, 13, 12, 16, 16, 17, 14, 15, 16, 15, 15, 13, 13, 15, 13, 16, 17, 15, 17, 17, 17, 16, 17, 14, 17,
    14, 16, 15, 17, 15, 15, 14, 17, 15, 17, 15, 16, 15, 15, 16, 16, 14, 17, 17, 15, 15, 16, 15, 17, 15, 14, 16, 16, 16,
    16, 16, 12, 4,  4,  5,  5,  6,  6,  6,  7,  7,  7,  8,  8,  8,  8,  9,  9,  9,  9,  9,  10, 10, 10, 11, 10, 11, 11,
    11, 11, 11, 12, 12, 12, 13, 13, 12, 13, 12, 14, 14, 12, 13, 13, 13, 13, 14, 12, 13, 13, 14, 14, 14, 13, 14, 14, 15,
    15, 13, 15, 13, 17, 17, 17, 9,  17, 7};

static const int8_t predefined_dist[5][14] = {
    {5, 6, 3, 3, 3, 3, 3, 3, 3, 4, 6},
    {5, 6, 4, 4, 3, 3, 3, 3, 3, 4, 4, 4, 6},
    {6, 7, 4, 4, 3, 3, 3, 3, 3, 4, 4, 4, 5, 7},
    {3, 6, 5, 4, 2, 3, 3, 3, 4, 4, 6},
    {6, 7, 7, 6, 4, 3, 2, 2, 3, 3, 6}};

// Number of distance symbols per predefined set.
static const int predefined_dist_nsym[5] = {11, 13, 14, 11, 11};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ============================================================================
// Bitstream Reader — sit13.md § 3 "Bit-Level Conventions"
// ============================================================================

// sit13.md § 3.1 "Bit Order" — bits are consumed LSB-first within each
// byte: peeler's shared peel_lsb_t (internal.h).  Bits past the end of the
// stream read as zeros, as this file's own reader did; the decode loops are
// bounded by the output size.

// ============================================================================
// Pool-Allocated Huffman Decoding Tree
// ============================================================================

// sit13.md § 5.3 "Canonical Huffman Code Construction" — codes are assigned
// in canonical order (ascending code-length, then ascending symbol value
// within each length).
//
// All nodes for a given tree are allocated from a single contiguous array
// (the "pool").  Child references are int16_t indices into the pool rather
// than pointers, so the entire tree is freed in one shot with the enclosing
// state struct.  sit13.md § 12.3 "Pool-Based Node Allocation".

// All trees of a block (meta + first + second + dist) share one of peeler's
// canonical-Huffman pools (internal.h), which bounds it: this file's own
// allocator did not.

// Walk the tree from root, reading one bit at a time until a leaf is
// reached.  Returns the leaf's symbol value, or -1 on error.
// sit13.md § 5.4 "Single-Symbol Tree Edge Case" — if the root IS a
// leaf, return its symbol without consuming any bits (handled by caller).
static int m13_huff_decode(const peel_hpool_t *pool, int root, peel_lsb_t *br) {
    int cur = root;
    while (peel_huff_sym(pool, cur) == PEEL_HUFF_NOSYM) {
        cur = peel_huff_child(pool, cur, (int)peel_lsb_get(br, 1));
        // Navigate to child; abort if tree is malformed
        if (cur < 0)
            return -1;
    }
    return peel_huff_sym(pool, cur);
}

// ============================================================================
// Dynamic Tree Decoding (Meta-Code Based)
// ============================================================================

// sit13.md § 6 "Tree Serialization (Dynamic Mode)" — when the header
// byte's SET field is 0, all three trees are serialized in the bitstream
// using a fixed 37-symbol meta-Huffman code.

// Fixed 37-symbol meta-Huffman code used to encode dynamic tree lengths.
// sit13.md § 6.2 "The Meta-Code".
#define M13_META_SIZE 37

static const uint16_t m13_meta_words[M13_META_SIZE] = {
    0x00dd, 0x001a, 0x0002, 0x0003, 0x0000, 0x000f, 0x0035, 0x0005,
    0x0006, 0x0007, 0x001b, 0x0034, 0x0001, 0x0001, 0x000e, 0x000c,
    0x0036, 0x01bd, 0x0006, 0x000b, 0x000e, 0x001f, 0x001e, 0x0009,
    0x0008, 0x000a, 0x01bc, 0x01bf, 0x01be, 0x01b9, 0x01b8, 0x0004,
    0x0002, 0x0001, 0x0007, 0x000c, 0x0002};

static const int m13_meta_lens[M13_META_SIZE] = {
    0xB, 0x8, 0x8, 0x8, 0x8, 0x7, 0x6, 0x5, 0x5, 0x5, 0x5, 0x6, 0x5,
    0x6, 0x7, 0x7, 0x9, 0xC, 0xA, 0xB, 0xB, 0xC, 0xC, 0xB, 0xB, 0xB,
    0xC, 0xC, 0xC, 0xC, 0xC, 0x5, 0x2, 0x2, 0x3, 0x4, 0x5};

// Build the meta-code tree from the fixed word/length pairs.
// Returns the root index in the pool, or -1 if the pool is full.
// sit13.md § 6.2 "The Meta-Code" — 37 symbols with explicit (word, length)
// pairs.  The meta-code tree uses direct codeword insertion, NOT the
// canonical code construction procedure.
static int m13_build_meta_tree(peel_hpool_t *pool) {
    int root = peel_huff_root(pool);
    for (int i = 0; root >= 0 && i < M13_META_SIZE; i++)
        if (peel_huff_insert(pool, root, m13_meta_words[i], m13_meta_lens[i], i) < 0)
            root = -1;
    return root;
}

// The range of code lengths the canonical builder can place.  31 is the
// longest a direct set produces (command 30).  -1 is legitimate: it is what a
// decrement from the reset value 0 gives, m13_build_canonical starts at -1 so
// that it counts as "absent" like 0, and real DropStuff 6 streams open a list
// with exactly that.  Below -1 the builder never matches (see
// m13_decode_lengths).
#define M13_MIN_CODE_LEN (-1)
#define M13_MAX_CODE_LEN 31

// Build a canonical tree from lengths in M13_MIN_CODE_LEN..M13_MAX_CODE_LEN.
// sit13.md § 5.3: codes go in ascending length, then ascending symbol, and
// absent symbols (-1, 0) still consume a code value -- which the shared
// builder does for any min_len below 1.  Returns the root, or -1.
static int m13_build_canonical(peel_hpool_t *pool, const int8_t *lengths, int nsym) {
    return peel_huff_build(pool, lengths, nsym, M13_MIN_CODE_LEN, M13_MAX_CODE_LEN);
}

// Decode a list of code lengths from the bitstream using the meta-code.
// sit13.md § 6.3 "Meta-Code Symbols and Code-Length RLE" — commands
// 0..30 set the length directly, 31 resets to 0, 32/33 increment/
// decrement, and 34..36 are various repeat encodings.
//
// Returns 0, or -1 on a malformed list.  Two bounds that were missing:
//
//   * Every command's entry count is checked against the space left.  The
//     loop was bounded by nsym but the repeats inside it were not: command
//     36 emits up to 74 entries, so from index 320 of a 321-entry list it
//     wrote 73 bytes past the end of the caller's stack array.  The spec
//     requires the list to end exactly at nsym, so an overshoot is an error,
//     not something to clamp.
//   * Every emitted length is M13_MIN_CODE_LEN..M13_MAX_CODE_LEN.
//     Decrementing past -1 produced a length m13_build_canonical never
//     matches, so its outer loop never finished.
static int m13_decode_lengths(const peel_hpool_t *pool, int meta_root,
                              peel_lsb_t *br, int8_t *out, int nsym) {
    int len = 0;
    int i = 0;
    while (i < nsym) {
        // The meta-code is complete (its Kraft sum is exactly 1), so this
        // walk always reaches a leaf and cmd is 0..36.
        int cmd = m13_huff_decode(pool, meta_root, br);
        int emit = 1; // entries this command produces, per the spec's table

        if (cmd <= 30) {
            len = cmd + 1;            // set the length directly
        } else if (cmd == 31) {
            len = 0;                  // symbol absent
        } else if (cmd == 32) {
            len++;
        } else if (cmd == 33) {
            len--;
        } else if (cmd == 34) {
            emit = 1 + (int)peel_lsb_get(br, 1);   // optional extra copy
        } else if (cmd == 35) {
            emit = (int)peel_lsb_get(br, 3) + 3;   // (n + 2) + the normal emit
        } else {
            emit = (int)peel_lsb_get(br, 6) + 11;  // (n + 10) + the normal emit
        }

        if (len < M13_MIN_CODE_LEN || len > M13_MAX_CODE_LEN)
            return -1;
        if (emit > nsym - i)
            return -1;
        while (emit-- > 0)
            out[i++] = (int8_t)len;
    }
    return 0;
}

// ============================================================================
// Core Decoder State
// ============================================================================

// sit13.md § 9.1 "State" — state includes the active tree pointer
// (alternates first/second), 64 KiB sliding window, and pending
// match copy for streaming.
// sit13.md § 12.3 "Pool-Based Node Allocation" — pool of 2048
// shared across all trees.

// Full decoder context for one method-13 stream.
typedef struct {
    peel_lsb_t br;

    // Node pool shared by all Huffman trees
    peel_hpool_t pool;

    // Root indices into pool for the three trees
    int root_first;
    int root_second;
    int root_dist;
    int root_active;   // Currently selected lit/len tree root

    // Sliding window
    uint8_t window[M13_WIN_SIZE];
    int     wpos;

    // Pending match state for streaming
    int match_left;
    int match_from;

    bool ready;
} m13_state_t;

// ============================================================================
// Static Helpers
// ============================================================================

// One-time initialization: read header, build trees, reset window.
// sit13.md § 4 "Block Header" — the first 8 bits encode the code-set
// selector (SET, bits 7..4), tree-sharing flag (S, bit 3), and
// distance tree symbol count (K, bits 2..0 → K+10 symbols).
static int m13_setup(m13_state_t *st) {
    // Zero-fill sliding window (sit13.md § 8.1 "Initialization")
    memset(st->window, 0, sizeof(st->window));
    st->wpos       = 0;
    st->match_left = 0;
    st->match_from = 0;
    peel_hpool_reset(&st->pool);

    // Read the single header byte.
    // sit13.md § 4.1: SET = bits 7..4, S = bit 3, K = bits 2..0.
    uint32_t hdr = peel_lsb_get(&st->br, 8);
    int set      = (int)(hdr >> 4);       // code set selector (0 = dynamic)
    bool shared  = (hdr >> 3) & 1;        // second tree == first tree?
    int dist_n   = (int)(hdr & 7) + 10;   // distance tree symbol count

    if (set == 0) {
        // Dynamic mode: build meta-code tree, then decode all three trees.
        // sit13.md § 6 "Tree Serialization (Dynamic Mode)".
        int meta_root = m13_build_meta_tree(&st->pool);
        if (meta_root < 0)
            return -1;

        int8_t lengths[M13_SYM_COUNT];

        // First literal/length tree.
        if (m13_decode_lengths(&st->pool, meta_root, &st->br,
                               lengths, M13_SYM_COUNT) < 0)
            return -1;
        st->root_first = m13_build_canonical(&st->pool, lengths, M13_SYM_COUNT);

        // Second literal/length tree (or shared).
        // sit13.md § 6.1 "Tree Sharing".
        if (shared) {
            st->root_second = st->root_first;
        } else {
            if (m13_decode_lengths(&st->pool, meta_root, &st->br,
                                   lengths, M13_SYM_COUNT) < 0)
                return -1;
            st->root_second = m13_build_canonical(&st->pool, lengths, M13_SYM_COUNT);
        }

        // Distance tree.
        if (m13_decode_lengths(&st->pool, meta_root, &st->br,
                               lengths, dist_n) < 0)
            return -1;
        st->root_dist = m13_build_canonical(&st->pool, lengths, dist_n);
    } else if (set >= 1 && set <= 5) {
        // Predefined mode: build trees from static tables.
        // sit13.md § 7 "Predefined Trees (Sets 1–5)".
        int idx = set - 1;
        st->root_first  = m13_build_canonical(&st->pool, predefined_first[idx],
                                              M13_SYM_COUNT);
        st->root_second = m13_build_canonical(&st->pool, predefined_second[idx],
                                              M13_SYM_COUNT);
        st->root_dist   = m13_build_canonical(&st->pool, predefined_dist[idx],
                                              predefined_dist_nsym[idx]);
    } else {
        // sit13.md § 11 "Error Conditions" — invalid SET value.
        return -1;
    }

    // A tree that did not fit the pool, or had a length out of range.
    if (st->root_first < 0 || st->root_second < 0 || st->root_dist < 0)
        return -1;

    // Start with the first literal/length tree active.
    // sit13.md § 9.1 "State".
    st->root_active = st->root_first;
    st->ready = true;
    return 0;
}

// Produce up to cap decoded bytes into dst.  Returns bytes produced, or -1.
// sit13.md § 9.2 "Main Loop" — symbols are decoded from the active
// literal/length tree; the active tree alternates between first and
// second trees after literals vs. after matches.
static int m13_output(m13_state_t *st, uint8_t *dst, size_t cap) {
    size_t n = 0;

    while (n < cap) {
        // Resume any pending match copy first
        if (st->match_left > 0) {
            uint8_t b = st->window[st->match_from++ & M13_WIN_MASK];
            dst[n++] = b;
            st->window[st->wpos & M13_WIN_MASK] = b;
            st->wpos++;
            if (--st->match_left == 0)
                st->root_active = st->root_second;
            continue;
        }

        // Decode next symbol from the active literal/length tree.
        // sit13.md § 5.4 "Single-Symbol Tree Edge Case" — a root that is a
        // leaf yields its symbol without reading bits, which
        // m13_huff_decode does as it stands.
        int sym = m13_huff_decode(&st->pool, st->root_active, &st->br);

        if (sym < 0)
            return -1;

        // Literal byte: emit, store in window, switch to first tree.
        // sit13.md § 9.2 — after emitting a literal, the active tree
        // reverts to the first tree.
        if (sym < 256) {
            dst[n++] = (uint8_t)sym;
            st->window[st->wpos & M13_WIN_MASK] = (uint8_t)sym;
            st->wpos++;
            st->root_active = st->root_first;
            continue;
        }

        // Match length decode.
        // sit13.md § 5.1 "Literal/Length Symbol Alphabet" — symbols
        // 256..317 encode lengths 3..64 directly; 318/319 use 10-/15-bit
        // extra fields for lengths 65+.  Symbol 320 is invalid.
        int mlen;
        if (sym <= 317)
            mlen = sym - 253;
        else if (sym == 318)
            mlen = (int)peel_lsb_get(&st->br, 10) + 65;
        else if (sym == 319)
            mlen = (int)peel_lsb_get(&st->br, 15) + 65;
        else
            return -1;   // symbol 320 or higher is invalid

        // Distance decode via the distance tree.
        // sit13.md § 5.2 "Distance Symbol Alphabet" — distance symbol
        // 0 means distance 1; other symbols d encode distance
        // 2^(d-1) + read_bits(d-1) + 1.
        int dsym = m13_huff_decode(&st->pool, st->root_dist, &st->br);
        if (dsym < 0)
            return -1;
        int dist;
        if (dsym == 0)
            dist = 1;
        else
            dist = (1 << (dsym - 1)) + (int)peel_lsb_get(&st->br, dsym - 1) + 1;

        // Stage the match for copying (may span multiple read calls)
        st->match_left = mlen;
        st->match_from = st->wpos - dist;
        // Loop will resume and copy bytes from the match
    }

    return (int)n;
}

// ============================================================================
// Entry Point (Internal)
// ============================================================================

// Decompress method-13 (LZSS + Huffman) compressed data into a freshly
// allocated buffer.  Called by sit.c for entries using compression method 13.
//
// sit13.md § "Appendix A: Complete Decompression Walkthrough"
//   1. Read header, build (or select) Huffman trees.
//   2. Main decode loop: literals + matches into sliding window.
//   3. Return the output buffer.
peel_buf_t peel_sit13(const uint8_t *src, size_t len, size_t uncomp_len, peel_err_t **err) {
    *err = NULL;

    // Handle degenerate case: zero-length output
    if (uncomp_len == 0) {
        return (peel_buf_t){.data = NULL, .size = 0, .owned = false};
    }

    // Allocate the output buffer up front (known size from container metadata)
    uint8_t *out = malloc(uncomp_len);
    if (!out) {
        *err = make_err("sit13: out of memory allocating %zu-byte output buffer", uncomp_len);
        return (peel_buf_t){0};
    }

    // The decoder state is large (~70 KiB), so heap-allocate to avoid stack overflow
    m13_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        free(out);
        *err = make_err("sit13: out of memory allocating decoder state");
        return (peel_buf_t){0};
    }

    // Initialise bit reader over the compressed input
    peel_lsb_init(&st->br, src, len);

    // Parse header and build Huffman trees
    if (m13_setup(st) < 0) {
        free(out);
        free(st);
        *err = make_err("sit13: invalid header or tree construction failed");
        return (peel_buf_t){0};
    }

    // Decode uncomp_len bytes through the main loop
    int produced = m13_output(st, out, uncomp_len);
    free(st);

    if (produced < 0 || (size_t)produced != uncomp_len) {
        free(out);
        *err = make_err("sit13: decompression failed (produced %d of %zu bytes)",
                        produced, uncomp_len);
        return (peel_buf_t){0};
    }

    return (peel_buf_t){.data = out, .size = uncomp_len, .owned = true};
}
