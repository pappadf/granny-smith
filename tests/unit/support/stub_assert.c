// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Assert and test harness stubs for unit tests

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Custom assertion failure handler
void gs_assert_fail(const char *expr, const char *file, int line, const char *func, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[unit] assertion failed: %s:%d", file ? file : "?", line);
    if (func)
        fprintf(stderr, " (%s)", func);
    if (expr)
        fprintf(stderr, ": %s", expr);
    if (fmt) {
        fprintf(stderr, ": ");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
    abort();
}

// The unimplemented-function handler.  A unit suite reaching one has driven the
// model into a corner the emulator does not model, which is a test failure --
// so this aborts, like the assert stub above, rather than pausing a scheduler
// the suites do not have.
void gs_unimplemented_fail(const char *file, int line, const char *func, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[unit] UNIMPLEMENTED: %s:%d", file ? file : "?", line);
    if (func)
        fprintf(stderr, " (%s)", func);
    if (fmt) {
        fprintf(stderr, ": ");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
    abort();
}

// Test harness init stub (called from setup when full test suite present)
void init_tests(void) {}
