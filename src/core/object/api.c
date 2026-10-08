// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// api.c
// Public entry point: gs_eval. The former gs_inspect and gs_complete
// entry points were folded into the object model itself — schema is now
// reached via `<path>.meta.*` and tab-completion via
// `gs_eval("meta.complete", [...])`.

#include "api.h"

#include "value_format.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "value.h"
#include "worker_thread.h"

// === JSON formatting ========================================================
//
// The document is value_format's VFMT_JSON_TAGGED rendering (value_format.h):
//   numeric / bool      → bare number / true / false (VAL_HEX → "0x…" string)
//   strings             → quoted string with the standard escapes
//   bytes               → "0x..." hex string
//   enum                → {"enum": "<name>", "index": <idx>}
//   list / map          → JSON array / object, recursing
//   object              → {"object": "<class>", "name": "<name>", "path": …}
//   error               → {"error": "<message>"}
//   none                → null
// Every result, failures included, is a value_t rendered through that one
// encoder into a growable buffer; gs_eval then copies it out whole or, when
// it does not fit, replaces it with an explicit {"error": ...} so no
// consumer parses a truncated document.

// Render `v` and copy it into out_buf. Returns false (out_buf then holding an
// error document naming both sizes) when the document exceeds out_size - 1.
static bool emit_json(const value_t *v, const char *path, char *out_buf, size_t out_size) {
    vbuf_t b = {0};
    value_format(v, VFMT_JSON_TAGGED, &b);
    const char *doc = b.p ? b.p : "null";
    size_t len = b.p ? b.len : 4;
    bool fits = len <= out_size - 1;
    if (fits) {
        memcpy(out_buf, doc, len + 1);
    } else {
        // The result is larger than the limit. A silently truncated payload
        // is worse than a failure -- the consumer would parse garbage (or,
        // for a string result, a shorter valid-looking document).
        value_t e = val_err("result of '%.64s' is %zu bytes, over the %zu-byte result limit", path, len, out_size - 1);
        vbuf_t eb = {0};
        value_format(&e, VFMT_JSON_TAGGED, &eb);
        size_t n = eb.len < out_size - 1 ? eb.len : out_size - 1;
        if (eb.p)
            memcpy(out_buf, eb.p, n);
        out_buf[n] = '\0';
        vbuf_free(&eb);
        value_free(&e);
    }
    vbuf_free(&b);
    return fits;
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

// Append one byte to the heap string json_parse_string builds, keeping room
// for the NUL. False (buffer freed) on OOM.
static bool jstr_push(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 1 >= *cap) {
        size_t nc = *cap * 2;
        char *nb = (char *)realloc(*buf, nc);
        if (!nb) {
            free(*buf);
            *buf = NULL;
            return false;
        }
        *buf = nb;
        *cap = nc;
    }
    (*buf)[(*len)++] = c;
    return true;
}

// Decode the 4 hex digits of a \uXXXX escape at p. -1 if malformed.
static int jstr_hex4(const char *p) {
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
        else
            return -1; // also stops at the NUL of a short escape
        code = (code << 4) | (unsigned)d;
    }
    return (int)code;
}

// Parse a JSON string literal at *pp into a heap copy. 0 on success.
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
            // An escape needs a character after the backslash; a document
            // ending here is unterminated (checked explicitly, not left to
            // the default arm below).
            if (!*p) {
                free(buf);
                return -1;
            }
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
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case '"':
            case '\\':
            case '/':
                c = e;
                break;
            case 'u': {
                // BMP-only Unicode escape — emit UTF-8.
                int code = jstr_hex4(p);
                if (code < 0) {
                    free(buf);
                    return -1;
                }
                p += 4;
                bool ok;
                if (code < 0x80) {
                    ok = jstr_push(&buf, &len, &cap, (char)code);
                } else if (code < 0x800) {
                    ok = jstr_push(&buf, &len, &cap, (char)(0xC0 | (code >> 6))) &&
                         jstr_push(&buf, &len, &cap, (char)(0x80 | (code & 0x3F)));
                } else {
                    ok = jstr_push(&buf, &len, &cap, (char)(0xE0 | (code >> 12))) &&
                         jstr_push(&buf, &len, &cap, (char)(0x80 | ((code >> 6) & 0x3F))) &&
                         jstr_push(&buf, &len, &cap, (char)(0x80 | (code & 0x3F)));
                }
                if (!ok)
                    return -1;
                continue;
            }
            default:
                free(buf);
                return -1;
            }
        }
        if (!jstr_push(&buf, &len, &cap, c))
            return -1;
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
        // Out-of-range numbers are refused rather than saturated: strtoll
        // would turn 1e20-as-integer into INT64_MAX and strtod 1e999 into
        // inf, both plausible-looking wrong arguments.
        errno = 0;
        if (is_float) {
            double d = strtod(p, &endp);
            if (!endp || endp == p || errno == ERANGE || !isfinite(d))
                return -1;
            *out = val_float(d);
            *pp = endp;
        } else {
            long long ll = strtoll(p, &endp, 10);
            if (!endp || endp == p || errno == ERANGE)
                return -1;
            *out = val_int((int64_t)ll);
            *pp = endp;
        }
        return 0;
    }
    return -1;
}

// True when only whitespace remains from `p` to the end of the document.
static bool json_at_end(const char *p) {
    return *json_skip_ws(p) == '\0';
}

// Free a parallel names array produced by the object form.
static void free_arg_names(char **names, int argc) {
    if (!names)
        return;
    for (int i = 0; i < argc; i++)
        free(names[i]);
    free(names);
}

// Parse `{"name": value, ...}` — the named-argument object form. Values
// are the same primitives the array form accepts; nested containers are
// rejected. Writes parallel names/argv arrays.
static int json_parse_object_args(const char *p, value_t **out_argv, int *out_argc, char ***out_names) {
    p = json_skip_ws(p + 1);
    if (*p == '}')
        return json_at_end(p + 1) ? 0 : -1;
    bool closed = false; // saw the closing '}'
    int cap = 4, n = 0;
    value_t *argv = (value_t *)calloc(cap, sizeof(value_t));
    char **names = (char **)calloc(cap, sizeof(char *));
    if (!argv || !names) {
        free(argv);
        free(names);
        return -1;
    }
    while (*p) {
        if (n >= cap) {
            cap *= 2;
            value_t *nb = (value_t *)realloc(argv, cap * sizeof(value_t));
            char **nn = (char **)realloc(names, cap * sizeof(char *));
            if (!nb || !nn) {
                free_args(nb ? nb : argv, n);
                free_arg_names(nn ? nn : names, n);
                return -1;
            }
            argv = nb;
            names = nn;
        }
        p = json_skip_ws(p);
        if (json_parse_string(&p, &names[n]) < 0) {
            free_args(argv, n);
            free_arg_names(names, n);
            return -1;
        }
        p = json_skip_ws(p);
        if (*p != ':') {
            free(names[n]);
            free_args(argv, n);
            free_arg_names(names, n);
            return -1;
        }
        p++;
        if (json_parse_value(&p, &argv[n]) < 0) {
            free(names[n]);
            free_args(argv, n);
            free_arg_names(names, n);
            return -1;
        }
        n++;
        p = json_skip_ws(p);
        if (*p == ',') {
            p = json_skip_ws(p + 1);
            continue;
        }
        if (*p == '}') {
            p++;
            closed = true;
            break;
        }
        free_args(argv, n);
        free_arg_names(names, n);
        return -1;
    }
    // A document that ends before its '}' was cut short (a request truncated
    // in transit ends right after a ','): refuse it rather than run the call
    // with its trailing arguments silently dropped.  Nothing may follow '}'.
    if (!closed || !json_at_end(p)) {
        free_args(argv, n);
        free_arg_names(names, n);
        return -1;
    }
    *out_argv = argv;
    *out_argc = n;
    *out_names = names;
    return 0;
}

static int json_parse_args(const char *json, value_t **out_argv, int *out_argc, char ***out_names) {
    *out_argv = NULL;
    *out_argc = 0;
    *out_names = NULL;
    if (!json || !*json)
        return 0;
    const char *p = json_skip_ws(json);
    if (*p == '{')
        return json_parse_object_args(p, out_argv, out_argc, out_names);
    if (*p != '[')
        return -1;
    p = json_skip_ws(p + 1);
    if (*p == ']')
        return json_at_end(p + 1) ? 0 : -1;
    bool closed = false; // saw the closing ']'
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
            closed = true;
            break;
        }
        free_args(argv, n);
        return -1;
    }
    // Same rule as the object form: unterminated, or anything after ']',
    // is not a document this parser accepts.
    if (!closed || !json_at_end(p)) {
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

    value_t *argv = NULL;
    int argc = 0;
    char **arg_names = NULL;
    value_t v;
    node_t n = {0};

    // Every failure is a V_ERROR rendered by the same encoder as a result,
    // so a JS caller always sees the {"error": ...} shape (object-model.md),
    // never a bare string.
    if (!path || !*path) {
        v = val_err("empty path");
        path = "";
    } else if (!node_valid(n = object_resolve(object_root(), path))) {
        v = val_err("path '%s' did not resolve", path);
    } else if (args_json && *args_json && json_parse_args(args_json, &argv, &argc, &arg_names) < 0) {
        v = val_err("args_json must be a JSON array of primitives or an object of named arguments");
    } else if (arg_names && (!n.member || n.member->kind != M_METHOD)) {
        // Method paths always dispatch via node_call. Attribute paths route
        // to node_set when args carry exactly one value, otherwise node_get.
        // Bare object/child nodes go through node_get (a V_OBJECT reference).
        v = val_err("path '%s' is not a method — named arguments require one", path);
    } else if (n.member && n.member->kind == M_METHOD && arg_names) {
        // Object form: bind every entry by name, no positionals.
        named_arg_t named[OBJ_BIND_MAX_ARGS];
        if (argc > OBJ_BIND_MAX_ARGS) {
            v = val_err("too many named arguments (limit %d)", OBJ_BIND_MAX_ARGS);
        } else {
            for (int i = 0; i < argc; i++) {
                named[i].name = arg_names[i];
                named[i].value = argv[i];
            }
            value_t bound[OBJ_BIND_MAX_ARGS];
            int bound_n = 0;
            value_t err = node_bind_args(n, 0, NULL, argc, named, bound, &bound_n);
            v = val_is_error(&err) ? err : node_call(n, bound_n, bound);
        }
    } else if (n.member && n.member->kind == M_METHOD) {
        v = node_call(n, argc, argv);
    } else if (n.member && n.member->kind == M_ATTR && argc == 1) {
        // node_set takes ownership of its value; pass a copy so the
        // outer free_args() can still walk argv.
        v = node_set(n, value_dup(&argv[0]));
    } else if (argc > 0) {
        v = val_err("path '%s' does not accept %d arg(s)", path, argc);
    } else {
        v = node_get(n);
    }

    int rc = val_is_error(&v) ? -1 : 0;
    if (!emit_json(&v, path, out_buf, out_size))
        rc = -1;
    value_free(&v);
    free_args(argv, argc);
    free_arg_names(arg_names, argc);
    return rc;
}
