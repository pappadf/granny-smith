// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// value_format.h
// One renderer for the tagged union, in place of seven.
//
// Before this existed, `value_t` was re-rendered by seven per-kind switch
// statements -- three in shell.c, three in expr.c, one in api.c -- and they
// had drifted in ways users could see: VAL_HEX honoured for V_INT in one and
// ignored in another, V_BYTES capped at 64 in one and uncapped in the next,
// and two JSON encoders whose comment promised they agreed "byte-for-byte"
// while disagreeing on V_ENUM, V_OBJECT and V_ERROR.  Every new value_kind_t
// meant seven edits, and the seventh was always the one that got missed.
//
// The differences that remain are MODES, declared below.  A difference that
// is not a mode is a bug.

#ifndef GS_OBJECT_VALUE_FORMAT_H
#define GS_OBJECT_VALUE_FORMAT_H

#include "value.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A growable text buffer.  Zero-initialise, append, then free `p`.
typedef struct {
    char *p;
    size_t len;
    size_t cap;
} vbuf_t;

void vbuf_append(vbuf_t *b, const char *s, size_t n);
void vbuf_appendf(vbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void vbuf_free(vbuf_t *b);

typedef enum {
    // Script interpolation, `${x}`: the value's plain text, unquoted, with
    // V_BYTES as bare hex digits and a list's elements recursing in this same
    // mode so they are unquoted too.  `echo "${methods("find")}"` in
    // tests/integration/debug pins that.
    VFMT_TEXT,

    // Top-level REPL output, a bare `x` at the prompt.  As TEXT, except that
    // V_BYTES carries its `0x` prefix and is never capped -- asking for the
    // value alone is asking for all of it.  Containers do not reach this mode:
    // shell.c renders a list of same-class objects as an attribute table and a
    // map as aligned rows, and calls value_format(elem, VFMT_INLINE) for the
    // cells.
    VFMT_REPL,

    // Composed into a larger line -- a `name = value` row, or a list element.
    // Strings and enum labels are QUOTED so the composition stays readable,
    // and V_BYTES is capped so a 1 MiB attribute does not print 2 MiB of hex.
    VFMT_INLINE,

    // One cell of a fixed-width table.  Unquoted, and structured kinds render
    // as compact placeholders rather than expanding.
    VFMT_CELL,

    // JSON for script text: `${map}` has to stay machine-parseable because
    // schema probes pipe it to a JSON parser.  Enum labels and object/error
    // values render as plain JSON strings -- readable, and what a script
    // comparing against a literal expects.
    VFMT_JSON,

    // JSON for the JS bridge.  Same document shape, except that the kinds a
    // caller must DISCRIMINATE carry their kind: {"enum":…,"index":N},
    // {"object":…,"name":…}, {"error":…}.  `gsEval` consumers branch on those.
    //
    // This is the one deliberate divergence from VFMT_JSON, and it is a
    // divergence because the two consumers genuinely differ -- not, as the
    // old comment claimed, an agreement that happened not to hold.
    VFMT_JSON_TAGGED,
} value_format_mode_t;

// Render `v` into `out` in the given mode.  Never fails; an unrenderable
// value produces a placeholder rather than nothing, because in the JSON modes
// emitting nothing yields a malformed document.
void value_format(const value_t *v, value_format_mode_t mode, vbuf_t *out);

// Convenience: render into a caller-owned fixed buffer, always
// NUL-terminated.  Returns the length that WOULD have been written, so a
// caller can detect truncation; it is never used as a copy length.
size_t value_format_into(const value_t *v, value_format_mode_t mode, char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_VALUE_FORMAT_H
