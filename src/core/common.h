// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// common.h
// Small project-local helpers with no better home: the unaligned byte-order
// accessors and the four-character-code converters.  Charter: only
// header-only helpers that are genuinely used across subsystems and depend on
// nothing but <stdint.h>/<stdbool.h>.  Not a place for module types, status
// codes (status.h), assertions (gs_assert.h) or libc bundles -- a TU includes
// the libc headers it uses itself.
//
// Transitional: the libc includes and status.h below are still pulled in here
// because many TUs rely on getting them transitively; they go once those TUs
// include what they use.

#ifndef COMMON_H
#define COMMON_H

// Standard headers (transitional -- see above)
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Status codes (status_t) live in status.h; common.h still includes it so
// existing users keep building while includes are pushed down to the TUs
// that need them.
#include "status.h"

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

#endif // COMMON_H
