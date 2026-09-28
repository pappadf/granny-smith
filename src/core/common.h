// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

#ifndef COMMON_H
#define COMMON_H

// Commonly used standard headers across modules
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Shared status codes used across modules
#define GS_SUCCESS 0
#define GS_ERROR   -1

// Project-wide assertions and diagnostics
#ifdef __cplusplus
extern "C" {
#endif

// Failure handler prints diagnostics (host + target backtraces, process info) then pauses the scheduler
void gs_assert_fail(const char *expr, const char *file, int line, const char *func, const char *fmt, ...);

// Basic assert macros.  Enabled in every build except the GS_FAST production
// profile (wasm release / headless MODE=fast), where they compile to nothing —
// measured ~4.5% of steady-state gameplay host time.
// The default headless build keeps them: it is the debugging tool, and CI
// runs it so the checks retain their value.
#ifdef GS_FAST
#define GS_ASSERT(cond)            ((void)0)
#define GS_ASSERTF(cond, fmt, ...) ((void)0)
#else
#define GS_ASSERT(cond) ((cond) ? (void)0 : gs_assert_fail(#cond, __FILE__, __LINE__, __func__, NULL))
#define GS_ASSERTF(cond, fmt, ...)                                                                                     \
    ((cond) ? (void)0 : gs_assert_fail(#cond, __FILE__, __LINE__, __func__, (fmt), ##__VA_ARGS__))
#endif

// The guest asked for something the hardware being emulated really does, and
// this emulator has not implemented it.
//
// NOT an assert, and deliberately NOT compiled out by GS_FAST.  An assert says
// "this cannot happen" and earns its removal from the shipping build because a
// correct program never trips one.  This says "this can happen, it is legal,
// and we cannot do it" -- a statement that is just as true in the release
// build, and more useful there, because that is the build a user is running
// when they find the gap.
//
// It is also not a guest-facing error.  Answering the guest -- a SCSI CHECK
// CONDITION, a bus error, a NAK -- claims the request was wrong when it was
// not, and sends whoever is debugging it to look at the driver.  Fault at the
// host, name the missing function, and stop.
//
// Handled rather than fatal: gs_unimplemented_fail prints the banner and the
// same diagnostics an assertion does, then stops the scheduler and returns
// control to the shell.  A dead browser tab tells a user less than a stopped
// machine with a message does.
void gs_unimplemented_fail(const char *file, int line, const char *func, const char *fmt, ...);

#define GS_UNIMPLEMENTED(fmt, ...) gs_unimplemented_fail(__FILE__, __LINE__, __func__, (fmt), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

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

// Forward declaration for checkpoint data type used across modules
// Modules receive a pointer to this opaque struct when saving/restoring state.
struct checkpoint;
typedef struct checkpoint checkpoint_t;

#endif // COMMON_H
