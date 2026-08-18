// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// inflate.h
// DEFLATE / zlib decompressor (RFC 1950 + RFC 1951), decode-only.
//
// Shared by the two places the emulator meets zlib streams: the PNG reader in
// debug.c (screenshots and `screen.match` reference images) and the UDIF disk
// image decoder (image_udif.c), whose 0x80000005 chunks are each a complete
// zlib stream.  Handles all three DEFLATE block types (stored, fixed Huffman,
// dynamic Huffman).  Compression lives in debug.c — only the PNG writer needs
// it, and it shares the RFC 1951 length/distance tables exported below.
//
// The emulator core links no third-party C libraries, so this is a
// first-party implementation rather than a zlib dependency.

#ifndef GS_INFLATE_H
#define GS_INFLATE_H

#include <stddef.h>
#include <stdint.h>

// RFC 1951 §3.2.5 length/distance code tables, shared by the decoder here and
// the deflate writer in debug.c.
extern const uint16_t deflate_len_base[29];
extern const uint8_t deflate_len_extra[29];
extern const uint16_t deflate_dist_base[30];
extern const uint8_t deflate_dist_extra[30];

// Inflate a zlib stream into a freshly allocated buffer, for callers that do
// not know the decompressed size up front (the PNG reader).  Returns malloc'd
// output and sets *out_len, or NULL if the stream is truncated or malformed.
uint8_t *inflate_zlib_alloc(const uint8_t *data, size_t data_len, size_t *out_len);

// Inflate a zlib stream into a caller-supplied buffer, for callers that know
// the exact expected size (a UDIF chunk covers a fixed sector run).  Returns
// the number of bytes written, or -1 if the stream is truncated, malformed, or
// would expand past `out_cap` — overflow is an error, never a silent clamp.
long inflate_zlib(const uint8_t *data, size_t data_len, uint8_t *out, size_t out_cap);

#endif // GS_INFLATE_H
