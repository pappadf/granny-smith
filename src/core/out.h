// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// out.h -- the core's output sink.
//
// What a leaf or the interpreter prints belongs to whoever asked for it:
// the script whose statement it is, or the request being served.  Every
// stdout site in the core goes through here; the sink routes the text to
// the job running on the calling thread (out.c: its buffer, delivered
// to its client as EVT_LOG output records in order), to the request the
// emulator thread is serving (the answer carries it), or -- outside any
// request: boot messages, a breakpoint hit, a diagnostic -- to fd 1 as
// before.  A build without the sink (a unit suite linking one file) has
// no out_route and prints to stdout.

#ifndef GS_OUT_H
#define GS_OUT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The router (out.c).  Weak reference: NULL when not linked.
void out_route(const char *text, size_t len) __attribute__((weak));

static inline void out_write(const char *text, size_t len) {
    if (out_route)
        out_route(text, len);
    else
        fwrite(text, 1, len, stdout);
}

static inline void out_puts(const char *text) {
    out_write(text, strlen(text));
}

static inline void out_putc(int c) {
    char ch = (char)c;
    out_write(&ch, 1);
}

static inline void out_vprintf(const char *fmt, va_list ap) {
    char buf[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    if ((size_t)n < sizeof buf) {
        out_write(buf, (size_t)n);
    } else {
        char *big = (char *)malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            out_write(big, (size_t)n);
            free(big);
        }
    }
    va_end(ap2);
}

static inline void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static inline void out_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    out_vprintf(fmt, ap);
    va_end(ap);
}

// Flushes fd 1 (what the sink wrote there); a no-op for captured text.
static inline void out_flush(void) {
    fflush(stdout);
}

#endif // GS_OUT_H
