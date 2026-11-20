// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// common.h
// Small project-local helpers with no better home: the unaligned byte-order
// accessors and the four-character-code converters.  Charter: only
// header-only helpers that are genuinely used across subsystems and depend on
// nothing but <stdint.h>/<stdbool.h>.  Not a place for module types, status
// codes (status.h), assertions (gs_assert.h) or libc bundles -- a TU includes
// the libc headers it uses itself.

#ifndef COMMON_H
#define COMMON_H

// What the helpers below use (uint*_t, bool, size_t); nothing else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Unaligned byte-order accessors =========================================
// For on-disk and on-wire structures, which are read at arbitrary offsets:
// HFS catalog records, AFP reply blocks, APM partition entries, DBDMA
// descriptors.  Contrast memory.h's LOAD_BE*/STORE_BE*, which cast the
// pointer and so require natural alignment — use those for register
// windows, these for buffers.
//
// Macros, not static inline: at -Og (the `make debug` build) a static
// inline helper is NOT reliably inlined — measured, it emitted a real call
// — while these expand to a single load plus a byte-swap at every -O level
// this project builds with.  __builtin_memcpy into a compound literal is
// what makes the unaligned access well-defined (no cast through a wider
// pointer, so no strict-aliasing violation); it compiles to the bare load.
// Each argument is expanded exactly once, so RD_BE32(p++) is safe.
//
// This is also the seam where a host could diverge: if a platform grows a
// cheaper unaligned byte-swapped load, it is redefined here and no call
// site changes.
#define RD_BE16(p) __builtin_bswap16(*(const uint16_t *)__builtin_memcpy(&(uint16_t){0}, (p), 2))
#define RD_BE32(p) __builtin_bswap32(*(const uint32_t *)__builtin_memcpy(&(uint32_t){0}, (p), 4))
#define RD_BE64(p) __builtin_bswap64(*(const uint64_t *)__builtin_memcpy(&(uint64_t){0}, (p), 8))
#define RD_LE16(p) (*(const uint16_t *)__builtin_memcpy(&(uint16_t){0}, (p), 2))
#define RD_LE32(p) (*(const uint32_t *)__builtin_memcpy(&(uint32_t){0}, (p), 4))

#define WR_BE16(p, v) ((void)__builtin_memcpy((p), &(uint16_t){__builtin_bswap16((uint16_t)(v))}, 2))
#define WR_BE32(p, v) ((void)__builtin_memcpy((p), &(uint32_t){__builtin_bswap32((uint32_t)(v))}, 4))
#define WR_BE64(p, v) ((void)__builtin_memcpy((p), &(uint64_t){__builtin_bswap64((uint64_t)(v))}, 8))
#define WR_LE16(p, v) ((void)__builtin_memcpy((p), &(uint16_t){(uint16_t)(v)}, 2))
#define WR_LE32(p, v) ((void)__builtin_memcpy((p), &(uint32_t){(uint32_t)(v)}, 4))

// The number of elements of an array -- an array, not a pointer: a
// pointer passed by mistake is refused at compile time (the GNU builtins
// tell an array from a pointer to its first element).
#define GS_ARRAY_LEN(a)                                                                                                \
    (sizeof(a) / sizeof((a)[0]) +                                                                                      \
     0 * sizeof(char[1 - 2 * __builtin_types_compatible_p(__typeof__(a), __typeof__(&(a)[0]))]))

// Four-character codes (OSType, ResType, an Apple event's class and ID): four
// bytes on the wire, read as a big-endian uint32; as text, those characters and
// a NUL.  A code shorter than four is padded with spaces, as the Mac pads one.
// The Apple-event layer and its codec had a copy each of both directions.
static inline void fourcc_text(uint32_t v, char out[5]) {
    out[0] = (char)(v >> 24);
    out[1] = (char)(v >> 16);
    out[2] = (char)(v >> 8);
    out[3] = (char)v;
    out[4] = '\0';
}
static inline uint32_t fourcc_value(const char *s) {
    uint32_t v = 0;
    bool ended = !s;
    for (int i = 0; i < 4; i++) {
        if (!ended && !s[i])
            ended = true;
        v = (v << 8) | (ended ? (uint8_t)' ' : (uint8_t)s[i]);
    }
    return v;
}

// The value of hex digit `c` (either case), or -1 when it is not one.
static inline int hex_digit(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// `n` bytes as 2n hex digits (upper or lower case) into `out`, which must
// hold 2n + 1: NUL-terminated.
static inline void hex_encode(char *out, const uint8_t *in, size_t n, bool upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xF];
    }
    out[2 * n] = '\0';
}

// The Mac epoch: seconds from 1904-01-01 (the Mac's clock, HFS and AFP
// dates) to 1970-01-01 (Unix).  The conversions do not clamp: a caller keeps
// its own policy for dates outside the other side's range.
#define MAC_EPOCH_UNIX_OFFSET 2082844800u

static inline int64_t mac_to_unix64(uint32_t mac) {
    return (int64_t)mac - MAC_EPOCH_UNIX_OFFSET;
}

static inline int64_t unix_to_mac64(int64_t unix_secs) {
    return unix_secs + MAC_EPOCH_UNIX_OFFSET;
}

// FNV-1a, 32- and 64-bit: a hash `h` (start from FNV1A32_INIT /
// FNV1A64_INIT) continued over `n` bytes, or over a C string.  Standard
// FNV-1a, so values that reach a file (checkpoint block tags, the AFP remote
// cache names) are the published algorithm's.  Callers hashing whole words
// rather than bytes (the GPU texture keys, the PPC JIT signatures) keep their
// own loops with these constants.
#define FNV1A32_INIT  2166136261u
#define FNV1A32_PRIME 16777619u
#define FNV1A64_INIT  14695981039346656037ull
#define FNV1A64_PRIME 1099511628211ull

static inline uint32_t fnv1a32(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * FNV1A32_PRIME;
    return h;
}

static inline uint32_t fnv1a32_str(uint32_t h, const char *s) {
    for (const uint8_t *b = (const uint8_t *)s; *b; b++)
        h = (h ^ *b) * FNV1A32_PRIME;
    return h;
}

static inline uint64_t fnv1a64(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * FNV1A64_PRIME;
    return h;
}

static inline uint64_t fnv1a64_str(uint64_t h, const char *s) {
    for (const uint8_t *b = (const uint8_t *)s; *b; b++)
        h = (h ^ *b) * FNV1A64_PRIME;
    return h;
}

#endif // COMMON_H
