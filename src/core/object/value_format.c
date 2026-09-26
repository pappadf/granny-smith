// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// value_format.c
// See value_format.h for why this exists and what the modes mean.

#include "value_format.h"

#include "object.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// V_BYTES is capped in the composed modes so a 1 MiB bytes attribute does not
// render 2 MiB of hex into a `name = value` row.  Top-level output is not
// capped: asking for the value IS asking for all of it.
#define VFMT_BYTES_INLINE_CAP 64

// === Buffer =================================================================

void vbuf_append(vbuf_t *b, const char *s, size_t n) {
    if (!b || !s || n == 0)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 64;
        while (want < b->len + n + 1)
            want *= 2;
        char *np = (char *)realloc(b->p, want);
        if (!np)
            return; // out of memory: the buffer stays valid, just short
        b->p = np;
        b->cap = want;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void vbuf_appendf(vbuf_t *b, const char *fmt, ...) {
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    // vsnprintf reports the length it WOULD have written.  Appending that
    // many bytes out of `tmp` is the overread this codebase has already shipped
    // once, so the short path clamps and the long path
    // formats again into an exact allocation rather than trusting the count.
    if ((size_t)n < sizeof(tmp)) {
        vbuf_append(b, tmp, (size_t)n);
        return;
    }
    char *big = (char *)malloc((size_t)n + 1);
    if (!big)
        return;
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
}

// === Helpers ================================================================

static bool mode_is_json(value_format_mode_t m) {
    return m == VFMT_JSON || m == VFMT_JSON_TAGGED;
}

// RFC 8259 string literal: quotes plus the escapes JSON requires.
static void append_json_string(vbuf_t *b, const char *s) {
    vbuf_append(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':
            vbuf_append(b, "\\\"", 2);
            break;
        case '\\':
            vbuf_append(b, "\\\\", 2);
            break;
        case '\n':
            vbuf_append(b, "\\n", 2);
            break;
        case '\r':
            vbuf_append(b, "\\r", 2);
            break;
        case '\t':
            vbuf_append(b, "\\t", 2);
            break;
        case '\b':
            vbuf_append(b, "\\b", 2);
            break;
        case '\f':
            vbuf_append(b, "\\f", 2);
            break;
        default:
            if (*p < 0x20)
                vbuf_appendf(b, "\\u%04x", (unsigned)*p);
            else
                vbuf_append(b, (const char *)p, 1);
            break;
        }
    }
    vbuf_append(b, "\"", 1);
}

// The enum's label, or NULL when the index is out of range or unnamed.
static const char *enum_label(const value_t *v) {
    if (v->enm.table && v->enm.idx >= 0 && (size_t)v->enm.idx < v->enm.n_table)
        return v->enm.table[v->enm.idx];
    return NULL;
}

static const char *object_class_name(const value_t *v) {
    const class_desc_t *c = v->obj ? object_class(v->obj) : NULL;
    return (c && c->name) ? c->name : "object";
}

// Render a value nested inside a larger one (a list element, a map value).
// Text modes compose with INLINE so strings stay quoted; JSON modes recurse
// in their own mode so the document stays uniform.
static value_format_mode_t nested_mode(value_format_mode_t m) {
    if (mode_is_json(m))
        return m;
    // TEXT recurses in TEXT: `${list}` renders [a, b], not ["a", "b"].  Every
    // other text mode composes, so its elements are quoted.
    return m == VFMT_TEXT ? VFMT_TEXT : VFMT_INLINE;
}

// === The one switch =========================================================

void value_format(const value_t *v, value_format_mode_t mode, vbuf_t *out) {
    if (!v) {
        vbuf_append(out, mode_is_json(mode) ? "null" : "", mode_is_json(mode) ? 4 : 0);
        return;
    }

    switch (v->kind) {
    case V_NONE:
        if (mode_is_json(mode))
            vbuf_append(out, "null", 4);
        else if (mode == VFMT_CELL)
            vbuf_append(out, "-", 1);
        else if (mode == VFMT_INLINE)
            vbuf_append(out, "null", 4);
        // DISPLAY: the empty string.
        return;

    case V_BOOL:
        vbuf_append(out, v->b ? "true" : "false", v->b ? 4 : 5);
        return;

    case V_INT:
        // VAL_HEX is honoured in every text mode.  format_value_print used to
        // ignore it for V_INT while format_scalar_inline honoured it, so the
        // same attribute rendered two ways depending on whether it was asked
        // for alone or inside a table.  JSON keeps V_INT numeric so the
        // document stays machine-readable.
        if (mode_is_json(mode))
            vbuf_appendf(out, "%" PRId64, v->i);
        else if (v->flags & VAL_HEX)
            vbuf_appendf(out, "0x%" PRIx64, (uint64_t)v->i);
        else
            vbuf_appendf(out, "%" PRId64, v->i);
        return;

    case V_UINT:
        // A hex-flagged unsigned becomes a JSON *string*, because "0x1f" is
        // not a JSON number and both encoders already agreed on that.
        if (mode_is_json(mode) && (v->flags & VAL_HEX))
            vbuf_appendf(out, "\"0x%" PRIx64 "\"", v->u);
        else if (mode_is_json(mode))
            vbuf_appendf(out, "%" PRIu64, v->u);
        else if (v->flags & VAL_HEX)
            vbuf_appendf(out, "0x%" PRIx64, v->u);
        else
            vbuf_appendf(out, "%" PRIu64, v->u);
        return;

    case V_FLOAT:
        vbuf_appendf(out, "%g", v->f);
        return;

    case V_STRING: {
        const char *s = v->s ? v->s : "";
        if (mode_is_json(mode))
            append_json_string(out, s);
        else if (mode == VFMT_INLINE)
            vbuf_appendf(out, "\"%s\"", s);
        else
            vbuf_append(out, s, strlen(s));
        return;
    }

    case V_BYTES: {
        size_t n = v->bytes.n;
        bool capped = (mode == VFMT_INLINE || mode == VFMT_CELL) && n > VFMT_BYTES_INLINE_CAP;
        size_t shown = capped ? VFMT_BYTES_INLINE_CAP : n;
        // TEXT is the one mode with no `0x`: `${bytes}` interpolates bare hex
        // digits, which is what a script composing a string wants.
        if (mode_is_json(mode))
            vbuf_append(out, "\"0x", 3);
        else if (mode != VFMT_TEXT)
            vbuf_append(out, "0x", 2);
        for (size_t i = 0; i < shown; i++)
            vbuf_appendf(out, "%02x", v->bytes.p[i]);
        if (capped)
            vbuf_appendf(out, " ...%zu more", n - shown);
        if (mode_is_json(mode))
            vbuf_append(out, "\"", 1);
        return;
    }

    case V_ENUM: {
        const char *label = enum_label(v);
        if (mode == VFMT_JSON_TAGGED) {
            vbuf_append(out, "{\"enum\":", 8);
            if (label)
                append_json_string(out, label);
            else
                vbuf_append(out, "null", 4);
            vbuf_appendf(out, ",\"index\":%d}", v->enm.idx);
        } else if (mode == VFMT_JSON) {
            if (label)
                append_json_string(out, label);
            else
                vbuf_appendf(out, "%d", v->enm.idx);
        } else if (label) {
            if (mode == VFMT_INLINE)
                vbuf_appendf(out, "\"%s\"", label);
            else
                vbuf_append(out, label, strlen(label));
        } else {
            bool bare = (mode == VFMT_TEXT || mode == VFMT_REPL);
            vbuf_appendf(out, bare ? "<enum:%d>" : "enum:%d", v->enm.idx);
        }
        return;
    }

    case V_LIST:
        if (mode == VFMT_CELL) {
            vbuf_appendf(out, "<list:%zu>", v->list.len);
            return;
        }
        if (mode == VFMT_INLINE) {
            // Composed into a bigger line: a size placeholder, not the items.
            vbuf_appendf(out, "<list:%zu>", v->list.len);
            return;
        }
        vbuf_append(out, "[", 1);
        for (size_t i = 0; i < v->list.len; i++) {
            if (i)
                vbuf_append(out, mode_is_json(mode) ? "," : ", ", mode_is_json(mode) ? 1 : 2);
            value_format(&v->list.items[i], nested_mode(mode), out);
        }
        vbuf_append(out, "]", 1);
        return;

    case V_MAP:
        if (mode == VFMT_CELL || mode == VFMT_INLINE) {
            vbuf_appendf(out, "<map:%zu>", v->map.len);
            return;
        }
        // DISPLAY renders a map as canonical compact JSON, because
        // `${machine.profile(m)}` has to stay machine-parseable -- schema
        // probes pipe it straight to a JSON parser.
        {
            value_format_mode_t m = mode_is_json(mode) ? mode : VFMT_JSON;
            vbuf_append(out, "{", 1);
            for (size_t i = 0; i < v->map.len; i++) {
                if (i)
                    vbuf_append(out, ",", 1);
                append_json_string(out, v->map.entries[i].key);
                vbuf_append(out, ":", 1);
                value_format(&v->map.entries[i].val, m, out);
            }
            vbuf_append(out, "}", 1);
        }
        return;

    case V_OBJECT: {
        const char *cls = object_class_name(v);
        const char *nm = v->obj ? object_name(v->obj) : NULL;
        if (mode == VFMT_JSON_TAGGED) {
            vbuf_append(out, "{\"object\":", 10);
            append_json_string(out, cls);
            vbuf_append(out, ",\"name\":", 8);
            append_json_string(out, nm ? nm : "");
            vbuf_append(out, "}", 1);
        } else if (mode == VFMT_JSON) {
            append_json_string(out, "<object>");
        } else if (mode == VFMT_INLINE) {
            vbuf_appendf(out, "<%s:%s>", cls, nm ? nm : "");
        } else if (mode == VFMT_CELL) {
            vbuf_appendf(out, "<%s>", cls);
        } else {
            vbuf_append(out, "<object>", 8);
        }
        return;
    }

    case V_ERROR: {
        const char *msg = v->err ? v->err : "";
        if (mode == VFMT_JSON_TAGGED) {
            vbuf_append(out, "{\"error\":", 9);
            append_json_string(out, *msg ? msg : "unknown");
            vbuf_append(out, "}", 1);
        } else if (mode == VFMT_JSON) {
            vbuf_t inner = {0};
            vbuf_appendf(&inner, "<error: %s>", msg);
            append_json_string(out, inner.p ? inner.p : "");
            vbuf_free(&inner);
        } else if (mode == VFMT_INLINE) {
            vbuf_append(out, "<error>", 7);
        } else {
            vbuf_appendf(out, "<error: %s>", msg);
        }
        return;
    }

    case V_REF: {
        const char *r = v->ref ? v->ref : "";
        if (mode_is_json(mode))
            append_json_string(out, r);
        else
            vbuf_append(out, r, strlen(r));
        return;
    }

    case V_RANGE: {
        // api.c's encoder handled neither V_REF nor V_RANGE and had no
        // default, so either emitted NOTHING -- a malformed document rather
        // than a wrong one.  Latent today (gs_eval takes a path, and ranges
        // come only from expression evaluation), but it is the shape that
        // makes an eighth kind break the bridge silently.
        vbuf_t inner = {0};
        if (v->range.step == 1 || v->range.step == 0)
            vbuf_appendf(&inner, "%lld..%lld", (long long)v->range.start, (long long)v->range.stop);
        else
            vbuf_appendf(&inner, "%lld..%lld step %lld", (long long)v->range.start, (long long)v->range.stop,
                         (long long)v->range.step);
        if (mode_is_json(mode))
            append_json_string(out, inner.p ? inner.p : "");
        else
            vbuf_append(out, inner.p ? inner.p : "", inner.len);
        vbuf_free(&inner);
        return;
    }
    }

    // No default above: the compiler flags a missing kind.  This is the
    // belt-and-braces path for a value whose tag is out of range entirely.
    vbuf_append(out, mode_is_json(mode) ? "null" : "<?>", mode_is_json(mode) ? 4 : 3);
}

size_t value_format_into(const value_t *v, value_format_mode_t mode, char *buf, size_t size) {
    vbuf_t b = {0};
    value_format(v, mode, &b);
    size_t want = b.len;
    if (buf && size > 0) {
        size_t take = want < size - 1 ? want : size - 1;
        if (b.p)
            memcpy(buf, b.p, take);
        buf[take] = '\0';
    }
    vbuf_free(&b);
    return want;
}
