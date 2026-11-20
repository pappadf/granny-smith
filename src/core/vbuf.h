// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vbuf.h
// The growable text buffer (vbuf_t).  It started in object/value_format as
// the renderers' output and is used well beyond them (jobs, the expression
// interpolator, scripts, the Apple-event codec), so it lives in core with
// no dependency on the object model; value_format.h re-exports it.

#ifndef GS_VBUF_H
#define GS_VBUF_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A growable text buffer.  Zero-initialise, append, then free `p` (or take
// it with vbuf_take).  `oom` is sticky: the first append that cannot grow
// the buffer sets it and every later append is a no-op, so the text stops
// at the failure instead of continuing with a hole in it -- a truncated
// JSON escape reads as well-formed but is not.  Check it once at the end.
typedef struct {
    char *p;
    size_t len;
    size_t cap;
    bool oom; // an append failed; the text is incomplete
} vbuf_t;

void vbuf_append(vbuf_t *b, const char *s, size_t n);
void vbuf_append_str(vbuf_t *b, const char *s); // NUL-terminated; NULL appends nothing
void vbuf_appendf(vbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void vbuf_free(vbuf_t *b);

// Hands over the text (NUL-terminated; "" when nothing was appended) and
// resets `b`; NULL when even that allocation fails.  Callers that care
// check `oom` first.
char *vbuf_take(vbuf_t *b);

// Appends one character; inline because the expression interpolator and
// the parsers push a character at a time.
static inline void vbuf_putc(vbuf_t *b, char c) {
    if (!b->oom && b->len + 2 <= b->cap) {
        b->p[b->len++] = c;
        b->p[b->len] = '\0';
    } else {
        vbuf_append(b, &c, 1);
    }
}

#ifdef __cplusplus
}
#endif

#endif // GS_VBUF_H
