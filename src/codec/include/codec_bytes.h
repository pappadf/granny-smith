// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// codec_bytes.h
// Byte-order readers and writers for the libraries (codec and peeler).
//
// The libraries are kept free of src/core, so they cannot use common.h's
// RD_*/WR_*; this is their one family instead.  Codec's framework and
// peeler's formats both read their headers and trailers through these --
// peeler already depends on codec, so it includes this rather than keep a
// copy (it had rd16be/rd32be/wr16be/wr32be in internal.h and per-format
// le16/le32/le64 in zip.c and gz.c).  The 16-bit readers return uint32_t,
// as codec's always have.  Decoder inner loops use their own bit readers,
// not these, so nothing here is on a hot path.

#ifndef CODEC_BYTES_H
#define CODEC_BYTES_H

#include <stdint.h>

// Little-endian 16-bit read.
static inline uint32_t codec_le16(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

// Little-endian 32-bit read.
static inline uint32_t codec_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Little-endian 64-bit read.
static inline uint64_t codec_le64(const uint8_t *p) {
    return (uint64_t)codec_le32(p) | (uint64_t)codec_le32(p + 4) << 32;
}

// Big-endian 16-bit read.
static inline uint32_t codec_be16(const uint8_t *p) {
    return (uint32_t)p[0] << 8 | (uint32_t)p[1];
}

// Big-endian 32-bit read.
static inline uint32_t codec_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

// Big-endian 64-bit read.
static inline uint64_t codec_be64(const uint8_t *p) {
    return (uint64_t)codec_be32(p) << 32 | codec_be32(p + 4);
}

// Little-endian 32-bit write.
static inline void codec_wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

// Little-endian 64-bit write.
static inline void codec_wr_le64(uint8_t *p, uint64_t v) {
    codec_wr_le32(p, (uint32_t)v);
    codec_wr_le32(p + 4, (uint32_t)(v >> 32));
}

// Big-endian 16-bit write.
static inline void codec_wr_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

// Big-endian 32-bit write.
static inline void codec_wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

// Big-endian 64-bit write.
static inline void codec_wr_be64(uint8_t *p, uint64_t v) {
    codec_wr_be32(p, (uint32_t)(v >> 32));
    codec_wr_be32(p + 4, (uint32_t)v);
}

#endif // CODEC_BYTES_H
