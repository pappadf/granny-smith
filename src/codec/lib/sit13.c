// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sit13.c
// StuffIt method 13: LZSS + Huffman (see codec.h).  Format notes:
// src/peeler/docs/sit13.md.
//
// A 64 KiB LZSS window (it starts zero-filled: a match may reach before the
// stream's start and read zeros) with three canonical Huffman codes over an
// LSB-first bit stream: two literal/length codes, used alternately -- the
// first after a literal, the second after a match -- and a distance code.
// The codes are one of five predefined sets, or serialized at the start
// with a fixed 37-symbol meta-code (sit13.md § 6).  Literal/length symbols
// 0..255 are bytes, 256..317 lengths 3..64, 318 and 319 a 10- or 15-bit
// length past 64; distance symbol 0 is 1, d is 2^(d-1) + (d - 1 bits) + 1.
// The container states the decoded size.
//
// The canonical codes are walked as trees (codec_htree_t): absent symbols
// take code values and codes reach 31 bits, which a lookup table would not
// reproduce exactly.

#include "codec_internal.h"

#include <stdlib.h>

// ============================================================================
// Constants
// ============================================================================

// sit13.md § 5.1 — symbols 0..255 are literals, 256..319 lengths, and 320
// is a reserved, invalid sentinel.
#define M13_SYM_COUNT 321
#define M13_WIN_SIZE  65536 // sit13.md § 8 "Sliding Window"
#define M13_BURST     (65 + 32767) // the longest match (symbol 319)

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
    {6, 7, 7, 6, 4, 3, 2, 2, 3, 3, 6}
};

// Number of distance symbols per predefined set.
static const int predefined_dist_nsym[5] = {11, 13, 14, 11, 11};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ============================================================================
// Huffman trees
// ============================================================================

// Walk a tree from `root`, a bit at a time, to a leaf: its symbol, or -1
// for a malformed tree.  A root that is a leaf reads no bits.
static int m13_huff_decode(const codec_htree_t *pool, int root, codec_in_t *in) {
    int cur = root;
    while (codec_htree_sym(pool, cur) == CODEC_HTREE_NOSYM) {
        cur = codec_htree_child(pool, cur, (int)lsb_get(in, 1));
        if (cur < 0)
            return -1;
    }
    return codec_htree_sym(pool, cur);
}

// Fixed 37-symbol meta-Huffman code used to encode dynamic tree lengths.
// sit13.md § 6.2 "The Meta-Code".
#define M13_META_SIZE 37

static const uint16_t m13_meta_words[M13_META_SIZE] = {
    0x00dd, 0x001a, 0x0002, 0x0003, 0x0000, 0x000f, 0x0035, 0x0005, 0x0006, 0x0007, 0x001b, 0x0034, 0x0001,
    0x0001, 0x000e, 0x000c, 0x0036, 0x01bd, 0x0006, 0x000b, 0x000e, 0x001f, 0x001e, 0x0009, 0x0008, 0x000a,
    0x01bc, 0x01bf, 0x01be, 0x01b9, 0x01b8, 0x0004, 0x0002, 0x0001, 0x0007, 0x000c, 0x0002};

static const int m13_meta_lens[M13_META_SIZE] = {0xB, 0x8, 0x8, 0x8, 0x8, 0x7, 0x6, 0x5, 0x5, 0x5, 0x5, 0x6, 0x5,
                                                 0x6, 0x7, 0x7, 0x9, 0xC, 0xA, 0xB, 0xB, 0xC, 0xC, 0xB, 0xB, 0xB,
                                                 0xC, 0xC, 0xC, 0xC, 0xC, 0x5, 0x2, 0x2, 0x3, 0x4, 0x5};

// Build the meta-code tree from the fixed word/length pairs.
// Returns the root index in the pool, or -1 if the pool is full.
// sit13.md § 6.2 "The Meta-Code" — 37 symbols with explicit (word, length)
// pairs.  The meta-code tree uses direct codeword insertion, NOT the
// canonical code construction procedure.
static int m13_build_meta_tree(codec_htree_t *pool) {
    int root = codec_htree_node(pool);
    for (int i = 0; root >= 0 && i < M13_META_SIZE; i++)
        if (codec_htree_insert(pool, root, m13_meta_words[i], m13_meta_lens[i], i) < 0)
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
static int m13_build_canonical(codec_htree_t *pool, const int8_t *lengths, int nsym) {
    return codec_htree_build(pool, lengths, nsym, M13_MIN_CODE_LEN, M13_MAX_CODE_LEN);
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
static int m13_decode_lengths(const codec_htree_t *pool, int meta_root, codec_in_t *br, int8_t *out, int nsym) {
    int len = 0;
    int i = 0;
    while (i < nsym) {
        // The meta-code is complete (its Kraft sum is exactly 1), so this
        // walk always reaches a leaf and cmd is 0..36.
        int cmd = m13_huff_decode(pool, meta_root, br);
        int emit = 1; // entries this command produces, per the spec's table

        if (cmd <= 30) {
            len = cmd + 1; // set the length directly
        } else if (cmd == 31) {
            len = 0; // symbol absent
        } else if (cmd == 32) {
            len++;
        } else if (cmd == 33) {
            len--;
        } else if (cmd == 34) {
            emit = 1 + (int)lsb_get(br, 1); // optional extra copy
        } else if (cmd == 35) {
            emit = (int)lsb_get(br, 3) + 3; // (n + 2) + the normal emit
        } else {
            emit = (int)lsb_get(br, 6) + 11; // (n + 10) + the normal emit
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
// The decoder
// ============================================================================

typedef struct {
    codec_htree_t pool; // every tree of the stream
    int root_first, root_second, root_dist;
    int root_active; // the literal/length tree in use
    bool ready; // the header has been read
} m13_t;

// Read the header byte and build (or select) the trees.  sit13.md § 4: the
// code set (bits 7..4; 0 = serialized), whether the second tree is the
// first (bit 3), and the distance tree's symbol count - 10 (bits 2..0).
static int m13_setup(codec_dec_t *d, m13_t *st) {
    codec_in_t *in = &d->in;
    codec_htree_reset(&st->pool);
    uint32_t hdr = lsb_get(in, 8);
    int set = (int)(hdr >> 4);
    bool shared = (hdr >> 3) & 1;
    int dist_n = (int)(hdr & 7) + 10;
    if (set == 0) {
        int meta_root = m13_build_meta_tree(&st->pool);
        if (meta_root < 0)
            return codec_fail(d, CODEC_ERR_DATA, "sit13: meta-code does not fit");
        int8_t lengths[M13_SYM_COUNT];
        if (m13_decode_lengths(&st->pool, meta_root, in, lengths, M13_SYM_COUNT) < 0)
            return codec_fail(d, CODEC_ERR_DATA, "sit13: bad code lengths");
        st->root_first = m13_build_canonical(&st->pool, lengths, M13_SYM_COUNT);
        if (shared) {
            st->root_second = st->root_first;
        } else {
            if (m13_decode_lengths(&st->pool, meta_root, in, lengths, M13_SYM_COUNT) < 0)
                return codec_fail(d, CODEC_ERR_DATA, "sit13: bad code lengths");
            st->root_second = m13_build_canonical(&st->pool, lengths, M13_SYM_COUNT);
        }
        if (m13_decode_lengths(&st->pool, meta_root, in, lengths, dist_n) < 0)
            return codec_fail(d, CODEC_ERR_DATA, "sit13: bad code lengths");
        st->root_dist = m13_build_canonical(&st->pool, lengths, dist_n);
    } else if (set >= 1 && set <= 5) {
        int idx = set - 1;
        st->root_first = m13_build_canonical(&st->pool, predefined_first[idx], M13_SYM_COUNT);
        st->root_second = m13_build_canonical(&st->pool, predefined_second[idx], M13_SYM_COUNT);
        st->root_dist = m13_build_canonical(&st->pool, predefined_dist[idx], predefined_dist_nsym[idx]);
    } else
        return codec_fail(d, CODEC_ERR_DATA, "sit13: invalid code set");
    if (st->root_first < 0 || st->root_second < 0 || st->root_dist < 0)
        return codec_fail(d, CODEC_ERR_DATA, "sit13: a tree does not fit, or has a length out of range");
    st->root_active = st->root_first;
    st->ready = true;
    return 0;
}

static int m13_fill(codec_dec_t *d) {
    m13_t *st = d->st;
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    if (codec_left(d) == 0)
        return CODEC_END;
    if (!st->ready) {
        int rc = m13_setup(d, st);
        if (rc)
            return rc;
    }
    // Bits past the end of the input read as zeros: the output size bounds
    // the loop, and the fork CRC catches a short stream.
    while (codec_left(d) > 0) {
        if (ring_room(r) < M13_BURST)
            return CODEC_MORE;
        int sym = m13_huff_decode(&st->pool, st->root_active, in);
        if (sym < 0)
            return codec_fail(d, CODEC_ERR_DATA, "sit13: bad literal/length code");
        if (sym < 256) {
            ring_put(r, (uint8_t)sym);
            st->root_active = st->root_first;
            continue;
        }
        int mlen;
        if (sym <= 317)
            mlen = sym - 253;
        else if (sym == 318)
            mlen = (int)lsb_get(in, 10) + 65;
        else if (sym == 319)
            mlen = (int)lsb_get(in, 15) + 65;
        else
            return codec_fail(d, CODEC_ERR_DATA, "sit13: invalid symbol 320");
        int dsym = m13_huff_decode(&st->pool, st->root_dist, in);
        if (dsym < 0)
            return codec_fail(d, CODEC_ERR_DATA, "sit13: bad distance code");
        size_t dist = dsym == 0 ? 1 : ((size_t)1 << (dsym - 1)) + lsb_get(in, dsym - 1) + 1;
        if (dist > M13_WIN_SIZE)
            return codec_fail(d, CODEC_ERR_DATA, "sit13: distance past the window");
        ring_copy_zero(r, dist, (size_t)mlen);
        st->root_active = st->root_second;
    }
    return CODEC_END;
}

static int m13_init(codec_dec_t *d) {
    m13_t *st = calloc(1, sizeof(*st));
    if (!st)
        return CODEC_ERR_NOMEM;
    d->st = st;
    return codec_ring_alloc(d, M13_WIN_SIZE, M13_BURST);
}

static void m13_free(codec_dec_t *d) {
    free(d->st);
    d->st = NULL;
}

const codec_dec_ops_t codec_sit13_dec = {m13_init, m13_fill, m13_free, true};
