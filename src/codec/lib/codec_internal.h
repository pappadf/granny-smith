// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// codec_internal.h
// What the codec sources share and nothing outside the library sees: the
// RFC 1951 code tables both halves of DEFLATE use, and byte-order helpers.

#ifndef CODEC_INTERNAL_H
#define CODEC_INTERNAL_H

#include "codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// RFC 1951 §3.2.5 length and distance code tables (defined in inflate.c,
// shared with the encoder in deflate.c).
extern const uint16_t codec_deflate_len_base[29];
extern const uint8_t codec_deflate_len_extra[29];
extern const uint16_t codec_deflate_dist_base[30];
extern const uint8_t codec_deflate_dist_extra[30];

// Little-endian 16/32/64-bit loads.
static inline uint32_t codec_le16(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}
static inline uint32_t codec_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t codec_le64(const uint8_t *p) {
    return (uint64_t)codec_le32(p) | (uint64_t)codec_le32(p + 4) << 32;
}

// Big-endian 16/32-bit loads.
static inline uint32_t codec_be16(const uint8_t *p) {
    return (uint32_t)p[0] << 8 | (uint32_t)p[1];
}
static inline uint32_t codec_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

#endif // CODEC_INTERNAL_H
