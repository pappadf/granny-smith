// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_api.c
// Public entry points: gs_eval / gs_inspect / gs_complete.

#include "gs_api.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "value.h"

// === JSON formatting ========================================================
//
// Tiny JSON emitter — values become a single JSON-encodable shape:
//   numeric / bool      → bare number / true / false
//   strings, errors     → quoted string with the standard escapes
//   bytes               → "0x..." hex string (proposal default formatter)
//   enum                → {"enum": "<name>", "index": <idx>}
//   list                → JSON array, recurse
//   object              → {"object": "<class>", "name": "<name>"}
//   none                → null
// Caller passes a buffer; we truncate (with a trailing "..." marker) on
// overflow rather than failing — easier for shell consumption.

static void buf_append(char *buf, size_t size, size_t *pos, const char *src, size_t n) {
    if (!buf || !size || *pos >= size - 1)
        return;
    size_t room = size - 1 - *pos;
    size_t k = n < room ? n : room;
    memcpy(buf + *pos, src, k);
    *pos += k;
    buf[*pos] = '\0';
}

static void buf_appendf(char *buf, size_t size, size_t *pos, const char *fmt, ...) {
    if (!buf || !size || *pos >= size - 1)
        return;
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    buf_append(buf, size, pos, tmp, (size_t)n);
}

// Append `s` as a JSON string literal (with quotes and escapes).
static void buf_append_jstring(char *buf, size_t size, size_t *pos, const char *s) {
    buf_append(buf, size, pos, "\"", 1);
    if (s) {
        for (const char *p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            switch (c) {
            case '"':
                buf_append(buf, size, pos, "\\\"", 2);
                break;
            case '\\':
                buf_append(buf, size, pos, "\\\\", 2);
                break;
            case '\n':
                buf_append(buf, size, pos, "\\n", 2);
                break;
            case '\r':
                buf_append(buf, size, pos, "\\r", 2);
                break;
            case '\t':
                buf_append(buf, size, pos, "\\t", 2);
                break;
            default:
                if (c < 0x20)
                    buf_appendf(buf, size, pos, "\\u%04x", c);
                else
                    buf_append(buf, size, pos, (char *)&c, 1);
            }
        }
    }
    buf_append(buf, size, pos, "\"", 1);
}

static void format_value_json(const value_t *v, char *buf, size_t size, size_t *pos) {
    if (!v) {
        buf_append(buf, size, pos, "null", 4);
        return;
    }
    switch (v->kind) {
    case V_NONE:
        buf_append(buf, size, pos, "null", 4);
        break;
    case V_BOOL:
        buf_append(buf, size, pos, v->b ? "true" : "false", v->b ? 4 : 5);
        break;
    case V_INT:
        buf_appendf(buf, size, pos, "%" PRId64, v->i);
        break;
    case V_UINT:
        if (v->flags & VAL_HEX)
            buf_appendf(buf, size, pos, "\"0x%" PRIx64 "\"", v->u);
        else
            buf_appendf(buf, size, pos, "%" PRIu64, v->u);
        break;
    case V_FLOAT:
        buf_appendf(buf, size, pos, "%g", v->f);
        break;
    case V_STRING:
        buf_append_jstring(buf, size, pos, v->s);
        break;
    case V_BYTES:
        buf_append(buf, size, pos, "\"0x", 3);
        for (size_t i = 0; i < v->bytes.n; i++)
            buf_appendf(buf, size, pos, "%02x", v->bytes.p[i]);
        buf_append(buf, size, pos, "\"", 1);
        break;
    case V_ENUM:
        buf_append(buf, size, pos, "{\"enum\":", 8);
        if (v->enm.table && (size_t)v->enm.idx < v->enm.n_table && v->enm.table[v->enm.idx])
            buf_append_jstring(buf, size, pos, v->enm.table[v->enm.idx]);
        else
            buf_append(buf, size, pos, "null", 4);
        buf_appendf(buf, size, pos, ",\"index\":%d}", v->enm.idx);
        break;
    case V_LIST:
        buf_append(buf, size, pos, "[", 1);
        for (size_t i = 0; i < v->list.len; i++) {
            if (i)
                buf_append(buf, size, pos, ",", 1);
            format_value_json(&v->list.items[i], buf, size, pos);
        }
        buf_append(buf, size, pos, "]", 1);
        break;
    case V_OBJECT: {
        const class_desc_t *cls = v->obj ? object_class(v->obj) : NULL;
        const char *cls_name = (cls && cls->name) ? cls->name : "object";
        const char *o_name = v->obj ? object_name(v->obj) : NULL;
        buf_append(buf, size, pos, "{\"object\":", 10);
        buf_append_jstring(buf, size, pos, cls_name);
        buf_append(buf, size, pos, ",\"name\":", 8);
        buf_append_jstring(buf, size, pos, o_name ? o_name : "");
        buf_append(buf, size, pos, "}", 1);
        break;
    }
    case V_ERROR:
        buf_append(buf, size, pos, "{\"error\":", 9);
        buf_append_jstring(buf, size, pos, v->err ? v->err : "unknown");
        buf_append(buf, size, pos, "}", 1);
        break;
    }
}

// === Public entry points ====================================================

int gs_eval(const char *path, const char *args_json, char *out_buf, size_t out_size) {
    (void)args_json; // method arguments arrive in a later milestone
    if (!out_buf || out_size == 0)
        return -1;
    out_buf[0] = '\0';
    size_t pos = 0;

    if (!path || !*path) {
        buf_append_jstring(out_buf, out_size, &pos, "empty path");
        return -1;
    }

    node_t n = object_resolve(object_root(), path);
    if (!node_valid(n)) {
        size_t p = 0;
        out_buf[0] = '\0';
        buf_append(out_buf, out_size, &p, "{\"error\":", 9);
        char msg[256];
        snprintf(msg, sizeof(msg), "path '%s' did not resolve", path);
        buf_append_jstring(out_buf, out_size, &p, msg);
        buf_append(out_buf, out_size, &p, "}", 1);
        return -1;
    }

    value_t v = node_get(n);
    format_value_json(&v, out_buf, out_size, &pos);
    int rc = val_is_error(&v) ? -1 : 0;
    value_free(&v);
    return rc;
}

int gs_inspect(const char *path, char *out_buf, size_t out_size) {
    // M2: same shape as gs_eval. Subtree expansion arrives with M11.
    return gs_eval(path, NULL, out_buf, out_size);
}

int gs_complete(const char *partial, char *out_buf, size_t out_size) {
    (void)partial;
    if (!out_buf || out_size == 0)
        return -1;
    // M2 placeholder: empty completion array. Tab completion stays on
    // the legacy engine until M9.
    snprintf(out_buf, out_size, "[]");
    return 0;
}
