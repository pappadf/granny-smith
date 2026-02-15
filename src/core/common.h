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

// Project-wide assertions and diagnostics (folded from gs_assert.h)
#ifdef __cplusplus
extern "C" {
#endif

// Failure handler prints diagnostics (host + target backtraces, process info) then pauses the scheduler
void gs_assert_fail(const char *expr, const char *file, int line, const char *func, const char *fmt, ...);

// Basic assert macros (always enabled; use conditional build guards yourself if desired)
#define GS_ASSERT(cond) ((cond) ? (void)0 : gs_assert_fail(#cond, __FILE__, __LINE__, __func__, NULL))
#define GS_ASSERTF(cond, fmt, ...)                                                                                     \
    ((cond) ? (void)0 : gs_assert_fail(#cond, __FILE__, __LINE__, __func__, (fmt), ##__VA_ARGS__))

#ifdef __cplusplus
}
#endif

// Forward declaration for checkpoint data type used across modules
// Modules receive a pointer to this opaque struct when saving/restoring state.
struct checkpoint;
typedef struct checkpoint checkpoint_t;

#endif // COMMON_H
