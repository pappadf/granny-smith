// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Test harness stubs for unit tests.  The GS_ASSERT handlers are
// src/platform/gs_assert_abort.c, shared with the standalone tools.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// The unimplemented-function handler.  A unit suite reaching one has driven the
// model into a corner the emulator does not model, which is a test failure --
// so this aborts, like the assert handlers, rather than pausing a scheduler
// the suites do not have.
void unimplemented_fail(const char *file, int line, const char *func, const char *fmt, ...) {
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
