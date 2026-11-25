// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// gs_assert_abort.c — the GS_ASSERT handlers for the host programs that are
// not the emulator: the standalone tools (tools/disasm, tools/dump) and the
// unit tier (tests/unit).  A failed assertion there is a bug the run must
// stop on, so these print where and what, then abort.  The emulator's own
// handlers (src/core/debug/debug.c) report and return instead.  Dependency
// free, so any of those programs links it as is; not part of either
// product build (they compile src/core, src/machines and one platform
// directory).

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "gs_assert.h"

// Prints the failed assertion's location and expression (no newline)
static void print_assert_location(const char *expr, const char *file, int line, const char *func) {
    fprintf(stderr, "assertion failed: %s:%d", file ? file : "?", line);
    if (func)
        fprintf(stderr, " (%s)", func);
    if (expr)
        fprintf(stderr, ": %s", expr);
}

// Assertion failure handler (GS_ASSERT)
void gs_assert_fail(const char *expr, const char *file, int line, const char *func) {
    print_assert_location(expr, file, line, func);
    fputc('\n', stderr);
    abort();
}

// Assertion failure handler with a message (GS_ASSERTF)
void gs_assert_failf(const char *expr, const char *file, int line, const char *func, const char *fmt, ...) {
    va_list ap;
    print_assert_location(expr, file, line, func);
    if (fmt) {
        fprintf(stderr, ": ");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
    abort();
}
