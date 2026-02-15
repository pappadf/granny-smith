// Assert and test harness stubs for unit tests

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Custom assertion failure handler
void gs_assert_fail(const char *expr,
                    const char *file,
                    int line,
                    const char *func,
                    const char *fmt,
                    ...) {
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

// Test harness init stub (called from setup when full test suite present)
void init_tests(void) {}
