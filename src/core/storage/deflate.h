// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// deflate.h
// DEFLATE / zlib compressor (RFC 1950 + RFC 1951), encode-only: LZ77 over a
// 32 KB window with hash chains, coded with the fixed Huffman tables.
//
// The counterpart of inflate.h.  Two writers use it: the PNG encoder in
// debug.c (screenshots and reference images) and the UDIF writer
// (udif_writer.h), which deflates every non-zero 64 KB chunk of a disk image
// into a complete zlib stream -- what a UDZO reader expects.
//
// Fixed Huffman costs about 9 % against dynamic Huffman on disk images; a
// dynamic-Huffman block is an isolated later improvement that would serve
// both callers.

#ifndef GS_DEFLATE_H
#define GS_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

// Bytes of output buffer that always suffice for `len` bytes of input, at
// any level (fixed Huffman codes a byte in at most 9 bits; stored blocks add
// 5 bytes per 65535).
size_t deflate_bound(size_t len);

// Reusable encoder state for inputs of at most `max_len` bytes, so a caller
// compressing many chunks allocates the hash chains once.
typedef struct deflate_state deflate_state_t;
deflate_state_t *deflate_state_new(size_t max_len);
void deflate_state_free(deflate_state_t *st);

// Compress `in` into a complete zlib stream in `out` (`cap` bytes, at least
// deflate_bound(len) to be sure).  `level` 0 emits stored blocks; 1..9 pick
// the hash-chain depth (6 is what the PNG writer has always used).  `st` may
// be NULL (allocated for the call) and must otherwise have been made for at
// least `len` bytes.  Returns the bytes written, or -1 when `cap` is too
// small or memory runs out.
long deflate_zlib(deflate_state_t *st, const uint8_t *in, size_t len, uint8_t *out, size_t cap, int level);

// Adler-32 of `len` bytes, continuing from `adler` (start from 1).
uint32_t gs_adler32(uint32_t adler, const uint8_t *data, size_t len);

#endif // GS_DEFLATE_H
