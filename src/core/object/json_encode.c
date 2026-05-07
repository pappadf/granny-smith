// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// json_encode.c
// Minimal JSON builder.  See json_encode.h for the contract.

#include "json_encode.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Builder state: a growable byte buffer plus a small stack tracking whether
// each open container is an object (true) or array (false), and whether the
// next emit needs a leading comma.  err is sticky: any allocation failure or
// imbalanced close flips it, and json_finish surfaces it as V_ERROR.
struct json_builder {
    char *buf;
    size_t len;
    size_t cap;
    bool *is_obj;
    bool *first;
    int depth;
    int stack_cap;
    bool key_pending; // true after json_key, until the next value call
    bool err;
};

// Grow the buffer to fit at least `extra` more bytes (plus terminator slack).
static bool ensure_cap(json_builder_t *b, size_t extra) {
    if (b->err)
        return false;
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap * 2 : 64;
    while (cap < need)
        cap *= 2;
    char *p = (char *)realloc(b->buf, cap);
    if (!p) {
        b->err = true;
        return false;
    }
    b->buf = p;
    b->cap = cap;
    return true;
}

// Grow the open-container stack to fit one more level.
static bool ensure_stack(json_builder_t *b) {
    if (b->depth + 1 <= b->stack_cap)
        return true;
    int cap = b->stack_cap ? b->stack_cap * 2 : 8;
    bool *nis = (bool *)realloc(b->is_obj, (size_t)cap * sizeof(bool));
    bool *nfirst = (bool *)realloc(b->first, (size_t)cap * sizeof(bool));
    if (!nis || !nfirst) {
        free(nis);
        free(nfirst);
        b->err = true;
        return false;
    }
    b->is_obj = nis;
    b->first = nfirst;
    b->stack_cap = cap;
    return true;
}

// Append raw bytes verbatim.
static void append_raw(json_builder_t *b, const char *s, size_t n) {
    if (!ensure_cap(b, n))
        return;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void append_cstr(json_builder_t *b, const char *s) {
    append_raw(b, s, strlen(s));
}

// Emit a separator before the next value: "" or "," for top-level / arrays;
// "":" trailer for object values is handled by json_key already.
static void emit_separator(json_builder_t *b) {
    if (b->key_pending) {
        b->key_pending = false;
        return;
    }
    if (b->depth > 0) {
        if (!b->first[b->depth - 1])
            append_cstr(b, ",");
        b->first[b->depth - 1] = false;
    }
}

// Write a JSON-escaped string body (no surrounding quotes).
static void write_escaped(json_builder_t *b, const char *s) {
    if (!s) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':
            append_cstr(b, "\\\"");
            break;
        case '\\':
            append_cstr(b, "\\\\");
            break;
        case '\n':
            append_cstr(b, "\\n");
            break;
        case '\r':
            append_cstr(b, "\\r");
            break;
        case '\t':
            append_cstr(b, "\\t");
            break;
        case '\b':
            append_cstr(b, "\\b");
            break;
        case '\f':
            append_cstr(b, "\\f");
            break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                append_cstr(b, esc);
            } else {
                char ch = (char)c;
                append_raw(b, &ch, 1);
            }
            break;
        }
    }
}

json_builder_t *json_builder_new(void) {
    json_builder_t *b = (json_builder_t *)calloc(1, sizeof(*b));
    return b;
}

void json_open_obj(json_builder_t *b) {
    if (!b || b->err)
        return;
    emit_separator(b);
    if (!ensure_stack(b))
        return;
    append_cstr(b, "{");
    b->is_obj[b->depth] = true;
    b->first[b->depth] = true;
    b->depth++;
}

void json_close_obj(json_builder_t *b) {
    if (!b || b->err)
        return;
    if (b->depth <= 0 || !b->is_obj[b->depth - 1]) {
        assert(false && "json_close_obj: imbalanced");
        b->err = true;
        return;
    }
    append_cstr(b, "}");
    b->depth--;
}

void json_open_arr(json_builder_t *b) {
    if (!b || b->err)
        return;
    emit_separator(b);
    if (!ensure_stack(b))
        return;
    append_cstr(b, "[");
    b->is_obj[b->depth] = false;
    b->first[b->depth] = true;
    b->depth++;
}

void json_close_arr(json_builder_t *b) {
    if (!b || b->err)
        return;
    if (b->depth <= 0 || b->is_obj[b->depth - 1]) {
        assert(false && "json_close_arr: imbalanced");
        b->err = true;
        return;
    }
    append_cstr(b, "]");
    b->depth--;
}

void json_key(json_builder_t *b, const char *key) {
    if (!b || b->err)
        return;
    if (b->depth <= 0 || !b->is_obj[b->depth - 1]) {
        assert(false && "json_key: not inside an object");
        b->err = true;
        return;
    }
    if (!b->first[b->depth - 1])
        append_cstr(b, ",");
    b->first[b->depth - 1] = false;
    append_cstr(b, "\"");
    write_escaped(b, key);
    append_cstr(b, "\":");
    b->key_pending = true;
}

void json_str(json_builder_t *b, const char *value) {
    if (!b || b->err)
        return;
    emit_separator(b);
    append_cstr(b, "\"");
    write_escaped(b, value);
    append_cstr(b, "\"");
}

void json_int(json_builder_t *b, int64_t value) {
    if (!b || b->err)
        return;
    emit_separator(b);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", (long long)value);
    append_cstr(b, tmp);
}

void json_bool(json_builder_t *b, bool value) {
    if (!b || b->err)
        return;
    emit_separator(b);
    append_cstr(b, value ? "true" : "false");
}

void json_null(json_builder_t *b) {
    if (!b || b->err)
        return;
    emit_separator(b);
    append_cstr(b, "null");
}

value_t json_finish(json_builder_t *b) {
    if (!b)
        return val_err("json_finish: null builder");
    bool ok = !b->err && b->depth == 0 && !b->key_pending;
    value_t result;
    if (ok && b->buf)
        result = val_str(b->buf);
    else if (ok)
        result = val_str("");
    else
        result = val_err("json_finish: builder in error or unbalanced state");
    free(b->buf);
    free(b->is_obj);
    free(b->first);
    free(b);
    return result;
}
