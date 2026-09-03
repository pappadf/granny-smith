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
// measured ~4.5% of steady-state gameplay host time (perf proposal §5.3).
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

// Forward declaration for checkpoint data type used across modules
// Modules receive a pointer to this opaque struct when saving/restoring state.
struct checkpoint;
typedef struct checkpoint checkpoint_t;

#endif // COMMON_H
