// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// api.c
// Public entry point: gs_eval. The former gs_inspect and gs_complete
// entry points were folded into the object model itself — schema is now
// reached via `<path>.meta.*` and tab-completion via
// `gs_eval("meta.complete", [...])`. See
// proposal-introspection-via-meta-attribute.md.

#include "api.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "value.h"
#include "worker_thread.h"

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

// === Minimal JSON-array parser for `args_json` ==============================
//
// Accepts a single top-level JSON array of primitive values: numbers,
// strings, booleans, null. JSON objects and nested arrays are not
// argument shapes the methods declare; reject them rather than guess a
// mapping. Returns 0 on success and writes `*out_argv` (heap-allocated,
// caller frees with free_args) and `*out_argc`.

static void free_args(value_t *argv, int argc) {
    if (!argv)
        return;
    for (int i = 0; i < argc; i++)
        value_free(&argv[i]);
    free(argv);
}

static const char *json_skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static int json_parse_string(const char **pp, char **out) {
    const char *p = *pp;
    if (*p != '"')
        return -1;
    p++;
    size_t cap = 32, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return -1;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char e = *p++;
            switch (e) {
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            case 'r':
                c = '\r';
                break;
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'u': {
                // BMP-only Unicode escape — emit UTF-8.
                if (!p[0] || !p[1] || !p[2] || !p[3]) {
                    free(buf);
                    return -1;
                }
                unsigned code = 0;
                for (int i = 0; i < 4; i++) {
                    char h = p[i];
                    int d;
                    if (h >= '0' && h <= '9')
                        d = h - '0';
                    else if (h >= 'a' && h <= 'f')
                        d = 10 + h - 'a';
                    else if (h >= 'A' && h <= 'F')
                        d = 10 + h - 'A';
                    else {
                        free(buf);
                        return -1;
                    }
                    code = (code << 4) | d;
                }
                p += 4;
                if (len + 4 >= cap) {
                    cap *= 2;
                    char *nb = (char *)realloc(buf, cap);
                    if (!nb) {
                        free(buf);
                        return -1;
                    }
                    buf = nb;
                }
                if (code < 0x80)
                    buf[len++] = (char)code;
                else if (code < 0x800) {
                    buf[len++] = (char)(0xC0 | (code >> 6));
                    buf[len++] = (char)(0x80 | (code & 0x3F));
                } else {
                    buf[len++] = (char)(0xE0 | (code >> 12));
                    buf[len++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    buf[len++] = (char)(0x80 | (code & 0x3F));
                }
                continue;
            }
            default:
                free(buf);
                return -1;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
        }
        buf[len++] = c;
    }
    if (*p != '"') {
        free(buf);
        return -1;
    }
    p++;
    buf[len] = '\0';
    *pp = p;
    *out = buf;
    return 0;
}

static int json_parse_value(const char **pp, value_t *out) {
    const char *p = json_skip_ws(*pp);
    if (*p == '"') {
        char *s = NULL;
        if (json_parse_string(&p, &s) < 0)
            return -1;
        *out = val_str(s);
        free(s);
        *pp = p;
        return 0;
    }
    if (*p == 't' && strncmp(p, "true", 4) == 0) {
        *out = val_bool(true);
        *pp = p + 4;
        return 0;
    }
    if (*p == 'f' && strncmp(p, "false", 5) == 0) {
        *out = val_bool(false);
        *pp = p + 5;
        return 0;
    }
    if (*p == 'n' && strncmp(p, "null", 4) == 0) {
        *out = val_none();
        *pp = p + 4;
        return 0;
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        char *endp = NULL;
        const char *q = p;
        bool is_float = false;
        if (*q == '-')
            q++;
        while (*q && ((*q >= '0' && *q <= '9') || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-')) {
            if (*q == '.' || *q == 'e' || *q == 'E')
                is_float = true;
            q++;
        }
        if (is_float) {
            double d = strtod(p, &endp);
            if (!endp || endp == p)
                return -1;
            *out = val_float(d);
            *pp = endp;
        } else {
            long long ll = strtoll(p, &endp, 10);
            if (!endp || endp == p)
                return -1;
            *out = val_int((int64_t)ll);
            *pp = endp;
        }
        return 0;
    }
    return -1;
}

static int json_parse_args(const char *json, value_t **out_argv, int *out_argc) {
    *out_argv = NULL;
    *out_argc = 0;
    if (!json || !*json)
        return 0;
    const char *p = json_skip_ws(json);
    if (*p != '[')
        return -1;
    p = json_skip_ws(p + 1);
    if (*p == ']')
        return 0;
    int cap = 4, n = 0;
    value_t *argv = (value_t *)calloc(cap, sizeof(value_t));
    if (!argv)
        return -1;
    while (*p) {
        if (n >= cap) {
            cap *= 2;
            value_t *nb = (value_t *)realloc(argv, cap * sizeof(value_t));
            if (!nb) {
                free_args(argv, n);
                return -1;
            }
            argv = nb;
        }
        if (json_parse_value(&p, &argv[n]) < 0) {
            free_args(argv, n);
            return -1;
        }
        n++;
        p = json_skip_ws(p);
        if (*p == ',') {
            p = json_skip_ws(p + 1);
            continue;
        }
        if (*p == ']') {
            p++;
            break;
        }
        free_args(argv, n);
        return -1;
    }
    *out_argv = argv;
    *out_argc = n;
    return 0;
}

// === Public entry points ====================================================

int gs_eval(const char *path, const char *args_json, char *out_buf, size_t out_size) {
    // Thread-affinity guard (compiled out in release). See worker_thread.h.
    worker_thread_assert("gs_eval");

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

    value_t *argv = NULL;
    int argc = 0;
    if (args_json && *args_json) {
        if (json_parse_args(args_json, &argv, &argc) < 0) {
            size_t p = 0;
            out_buf[0] = '\0';
            buf_append(out_buf, out_size, &p, "{\"error\":", 9);
            buf_append_jstring(out_buf, out_size, &p, "args_json must be a JSON array of primitives");
            buf_append(out_buf, out_size, &p, "}", 1);
            return -1;
        }
    }

    value_t v;
    // Method paths always dispatch via node_call. Attribute paths route to
    // node_set when args carry exactly one value, otherwise node_get. Bare
    // object/child nodes go through node_get (returns a V_OBJECT reference).
    if (n.member && n.member->kind == M_METHOD) {
        v = node_call(n, argc, argv);
    } else if (n.member && n.member->kind == M_ATTR && argc == 1) {
        // node_set takes ownership of its value; pass a copy so the
        // outer free_args() can still walk argv.
        v = node_set(n, value_copy(&argv[0]));
    } else if (argc > 0) {
        v = val_err("path '%s' does not accept %d arg(s)", path, argc);
    } else {
        v = node_get(n);
    }

    format_value_json(&v, out_buf, out_size, &pos);
    int rc = val_is_error(&v) ? -1 : 0;
    value_free(&v);
    free_args(argv, argc);
    return rc;
}
