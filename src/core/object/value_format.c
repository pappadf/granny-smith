// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// value_format.c
// See value_format.h for why this exists and what the modes mean.

#include "value_format.h"

#include "json_escape.h"
#include "object.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// VK_BYTES is capped in the composed modes so a 1 MiB bytes attribute does not
// render 2 MiB of hex into a `name = value` row.  Top-level output is not
// capped: asking for the value IS asking for all of it.
#define VFMT_BYTES_INLINE_CAP 64

// === JSON into a vbuf ======================================================

void vbuf_append_json_escaped(vbuf_t *b, const char *s, size_t n) {
    char esc[JSON_ESCAPE_MAX];
    size_t run = 0; // start of the pending run of bytes that stand for themselves
    for (size_t i = 0; s && i < n; i++) {
        size_t k = json_escape_char((unsigned char)s[i], esc);
        if (!k)
            continue;
        // Runs of ordinary characters go in one call, not byte by byte
        vbuf_append(b, s + run, i - run);
        vbuf_append(b, esc, k);
        run = i + 1;
    }
    if (s)
        vbuf_append(b, s + run, n - run);
}

void vbuf_append_json_string(vbuf_t *b, const char *s) {
    vbuf_append(b, "\"", 1);
    if (s)
        vbuf_append_json_escaped(b, s, strlen(s));
    vbuf_append(b, "\"", 1);
}

// === Helpers ================================================================

static bool mode_is_json(value_format_mode_t m) {
    return m == VFMT_JSON || m == VFMT_JSON_TAGGED;
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
    case VK_NONE:
        if (mode_is_json(mode))
            vbuf_append(out, "null", 4);
        else if (mode == VFMT_CELL)
            vbuf_append(out, "-", 1);
        else if (mode == VFMT_INLINE)
            vbuf_append(out, "null", 4);
        // DISPLAY: the empty string.
        return;

    case VK_BOOL:
        vbuf_append_str(out, v->b ? "true" : "false");
        return;

    case VK_INT:
        // VFLAG_HEX is honoured in every text mode: format_value_print used to
        // ignore it for VK_INT while format_scalar_inline honoured it, so the
        // same attribute rendered two ways.  Hex shows the two's-complement
        // bit pattern at the value's width (-1 at width 4 is 0xffffffff).
        // JSON keeps VK_INT a bare number whatever its presentation, so web
        // consumers of a signed attribute (a NuBus slot number) read a number.
        if (!mode_is_json(mode) && (v->flags & VFLAG_HEX)) {
            uint64_t bits = (uint64_t)v->i;
            // mask to the value's width so a negative shows its own bit pattern
            if (v->width > 0 && v->width < 8)
                bits &= ((uint64_t)1 << (8 * v->width)) - 1;
            vbuf_appendf(out, "0x%" PRIx64, bits);
        } else {
            vbuf_appendf(out, "%" PRId64, v->i);
        }
        return;

    case VK_UINT:
        // A hex-flagged unsigned becomes a JSON *string*, because "0x1f" is
        // not a JSON number and both encoders already agreed on that.
        if (mode_is_json(mode) && (v->flags & VFLAG_HEX))
            vbuf_appendf(out, "\"0x%" PRIx64 "\"", v->u);
        else if (mode_is_json(mode))
            vbuf_appendf(out, "%" PRIu64, v->u);
        else if (v->flags & VFLAG_HEX)
            vbuf_appendf(out, "0x%" PRIx64, v->u);
        else
            vbuf_appendf(out, "%" PRIu64, v->u);
        return;

    case VK_FLOAT:
        // JSON has no inf/nan: a non-finite float is null there
        if (mode_is_json(mode) && !isfinite(v->f))
            vbuf_append(out, "null", 4);
        else
            vbuf_appendf(out, "%g", v->f);
        return;

    case VK_STRING: {
        const char *s = v->s ? v->s : "";
        if (mode_is_json(mode))
            vbuf_append_json_string(out, s);
        else if (mode == VFMT_INLINE)
            vbuf_appendf(out, "\"%s\"", s);
        else
            vbuf_append_str(out, s);
        return;
    }

    case VK_BYTES: {
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

    case VK_ENUM: {
        const char *label = enum_label(v);
        if (mode == VFMT_JSON_TAGGED) {
            vbuf_append(out, "{\"enum\":", 8);
            if (label)
                vbuf_append_json_string(out, label);
            else
                vbuf_append(out, "null", 4);
            vbuf_appendf(out, ",\"index\":%d}", v->enm.idx);
        } else if (mode == VFMT_JSON) {
            if (label)
                vbuf_append_json_string(out, label);
            else
                vbuf_appendf(out, "%d", v->enm.idx);
        } else if (label) {
            if (mode == VFMT_INLINE)
                vbuf_appendf(out, "\"%s\"", label);
            else
                vbuf_append_str(out, label);
        } else {
            bool bare = (mode == VFMT_TEXT || mode == VFMT_REPL);
            vbuf_appendf(out, bare ? "<enum:%d>" : "enum:%d", v->enm.idx);
        }
        return;
    }

    case VK_LIST:
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

    case VK_MAP:
        if (mode == VFMT_CELL || mode == VFMT_INLINE) {
            vbuf_appendf(out, "<map:%zu>", v->map.len);
            return;
        }
        // DISPLAY renders a map as canonical compact JSON, because
        // `${catalog.profile(m)}` has to stay machine-parseable -- schema
        // probes pipe it straight to a JSON parser.
        {
            value_format_mode_t m = mode_is_json(mode) ? mode : VFMT_JSON;
            vbuf_append(out, "{", 1);
            for (size_t i = 0; i < v->map.len; i++) {
                if (i)
                    vbuf_append(out, ",", 1);
                vbuf_append_json_string(out, v->map.entries[i].key);
                vbuf_append(out, ":", 1);
                value_format(&v->map.entries[i].val, m, out);
            }
            vbuf_append(out, "}", 1);
        }
        return;

    case VK_OBJECT: {
        const char *cls = object_class_name(v);
        const char *nm = v->obj ? object_name(v->obj) : NULL;
        if (mode == VFMT_JSON_TAGGED) {
            vbuf_append(out, "{\"object\":", 10);
            vbuf_append_json_string(out, cls);
            vbuf_append(out, ",\"name\":", 8);
            vbuf_append_json_string(out, nm ? nm : "");
            // Its path, so a client can link to the node (select it in the
            // command browser, reveal it in SYSTEM).
            char path[512];
            object_compute_path(v->obj, path, sizeof(path));
            vbuf_append(out, ",\"path\":", 8);
            vbuf_append_json_string(out, path);
            vbuf_append(out, "}", 1);
        } else if (mode == VFMT_JSON) {
            vbuf_append_json_string(out, "<object>");
        } else if (mode == VFMT_INLINE) {
            vbuf_appendf(out, "<%s:%s>", cls, nm ? nm : "");
        } else if (mode == VFMT_CELL) {
            vbuf_appendf(out, "<%s>", cls);
        } else {
            vbuf_append(out, "<object>", 8);
        }
        return;
    }

    case VK_ERROR: {
        const char *msg = v->err ? v->err : "";
        if (mode == VFMT_JSON_TAGGED) {
            vbuf_append(out, "{\"error\":", 9);
            vbuf_append_json_string(out, *msg ? msg : "unknown");
            vbuf_append(out, "}", 1);
        } else if (mode == VFMT_JSON) {
            vbuf_t inner = {0};
            vbuf_appendf(&inner, "<error: %s>", msg);
            vbuf_append_json_string(out, inner.p ? inner.p : "");
            vbuf_free(&inner);
        } else if (mode == VFMT_INLINE) {
            vbuf_append(out, "<error>", 7);
        } else {
            vbuf_appendf(out, "<error: %s>", msg);
        }
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
