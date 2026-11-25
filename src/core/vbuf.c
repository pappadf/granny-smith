// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vbuf.c
// The growable text buffer; see vbuf.h.

#include "vbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vbuf_append(vbuf_t *b, const char *s, size_t n) {
    if (!b || !s || n == 0 || b->oom)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 64;
        while (want < b->len + n + 1)
            want *= 2;
        char *np = (char *)realloc(b->p, want);
        if (!np) {
            b->oom = true; // out of memory: the buffer stays valid, just short, and says so
            return;
        }
        b->p = np;
        b->cap = want;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void vbuf_append_str(vbuf_t *b, const char *s) {
    if (s)
        vbuf_append(b, s, strlen(s));
}

void vbuf_appendf(vbuf_t *b, const char *fmt, ...) {
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->oom = true; // nothing can be appended for this format: the text is incomplete
        return;
    }
    // vsnprintf reports the length it WOULD have written.  Appending that
    // many bytes out of `tmp` is the overread this codebase has already shipped
    // once, so the short path clamps and the long path
    // formats again into an exact allocation rather than trusting the count.
    if ((size_t)n < sizeof(tmp)) {
        vbuf_append(b, tmp, (size_t)n);
        return;
    }
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) {
        b->oom = true;
        return;
    }
    va_start(ap, fmt);
    int m = vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    if (m > 0)
        vbuf_append(b, big, (size_t)m < (size_t)n ? (size_t)m : (size_t)n);
    free(big);
}

void vbuf_free(vbuf_t *b) {
    if (!b)
        return;
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
    b->oom = false;
}

char *vbuf_take(vbuf_t *b) {
    char *p = b->p ? b->p : (char *)calloc(1, 1);
    b->p = NULL;
    b->len = b->cap = 0;
    b->oom = false;
    return p;
}
