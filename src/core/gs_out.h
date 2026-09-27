// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_out.h -- the core's output sink.
//
// What a leaf or the interpreter prints belongs to whoever asked for it:
// the script whose statement it is, or the request being served.  Every
// stdout site in the core goes through here; the sink routes the text to
// the job running on the calling thread (gs_out.c: its buffer, delivered
// to its client as EVT_LOG output records in order), to the request the
// emulator thread is serving (the answer carries it), or -- outside any
// request: boot messages, a breakpoint hit, a diagnostic -- to fd 1 as
// before.  A build without the sink (a unit suite linking one file) has
// no gs_out_route and prints to stdout.

#ifndef GS_OUT_H
#define GS_OUT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The router (gs_out.c).  Weak reference: NULL when not linked.
void gs_out_route(const char *text, size_t len) __attribute__((weak));

static inline void gs_out(const char *text, size_t len) {
    if (gs_out_route)
        gs_out_route(text, len);
    else
        fwrite(text, 1, len, stdout);
}

static inline void gs_outs(const char *text) {
    gs_out(text, strlen(text));
}

static inline void gs_outc(int c) {
    char ch = (char)c;
    gs_out(&ch, 1);
}

static inline void gs_voutf(const char *fmt, va_list ap) {
    char buf[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    if ((size_t)n < sizeof buf) {
        gs_out(buf, (size_t)n);
    } else {
        char *big = (char *)malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            gs_out(big, (size_t)n);
            free(big);
        }
    }
    va_end(ap2);
}

static inline void gs_outf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static inline void gs_outf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gs_voutf(fmt, ap);
    va_end(ap);
}

// Flushes fd 1 (what the sink wrote there); a no-op for captured text.
static inline void gs_out_flush(void) {
    fflush(stdout);
}

#endif // GS_OUT_H
