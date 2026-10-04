// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// json_value.c
// A small recursive-descent JSON reader producing value_t trees.

#include "json_value.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Nesting limit: a configuration document is a few levels deep.
#define JSON_MAX_DEPTH 32

// Parser state: the text, the cursor and the first error.
typedef struct {
    const char *start;
    const char *p;
    char *err;
    size_t errlen;
    bool failed;
} json_reader_t;

static bool parse_value(json_reader_t *r, value_t *out, int depth);

// Record the first error, with the offset it happened at.
static bool fail(json_reader_t *r, const char *what) {
    if (!r->failed && r->err && r->errlen)
        snprintf(r->err, r->errlen, "JSON: %s at offset %zu", what, (size_t)(r->p - r->start));
    r->failed = true;
    return false;
}

// Skip blanks.
static void skip_ws(json_reader_t *r) {
    while (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')
        r->p++;
}

// Append one byte to a growable buffer.
static bool buf_put(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 1 >= *cap) {
        size_t ncap = *cap ? *cap * 2 : 32;
        char *nb = (char *)realloc(*buf, ncap);
        if (!nb)
            return false;
        *buf = nb;
        *cap = ncap;
    }
    (*buf)[(*len)++] = c;
    return true;
}

// Append a code point as UTF-8.
static bool buf_put_utf8(char **buf, size_t *len, size_t *cap, uint32_t cp) {
    if (cp < 0x80)
        return buf_put(buf, len, cap, (char)cp);
    if (cp < 0x800)
        return buf_put(buf, len, cap, (char)(0xC0 | (cp >> 6))) && buf_put(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    if (cp < 0x10000)
        return buf_put(buf, len, cap, (char)(0xE0 | (cp >> 12))) &&
               buf_put(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
               buf_put(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    return buf_put(buf, len, cap, (char)(0xF0 | (cp >> 18))) &&
           buf_put(buf, len, cap, (char)(0x80 | ((cp >> 12) & 0x3F))) &&
           buf_put(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
           buf_put(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
}

// Four hex digits of a \u escape; -1 on garbage.
static int32_t hex4(const char *p) {
    int32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char h = p[i];
        int d = (h >= '0' && h <= '9')   ? h - '0'
                : (h >= 'a' && h <= 'f') ? 10 + h - 'a'
                : (h >= 'A' && h <= 'F') ? 10 + h - 'A'
                                         : -1;
        if (d < 0)
            return -1;
        v = (v << 4) | d;
    }
    return v;
}

// Parse a string literal into a heap buffer the caller frees.
static bool parse_string(json_reader_t *r, char **out) {
    if (*r->p != '"')
        return fail(r, "expected a string");
    r->p++;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    while (*r->p != '"') {
        unsigned char c = (unsigned char)*r->p;
        if (c == '\0' || c < 0x20) {
            free(buf);
            return fail(r, c ? "control character in string" : "unterminated string");
        }
        r->p++;
        if (c != '\\') {
            if (!buf_put(&buf, &len, &cap, (char)c))
                goto oom;
            continue;
        }
        char e = *r->p++;
        char lit = 0;
        switch (e) {
        case '"':
        case '\\':
        case '/':
            lit = e;
            break;
        case 'b':
            lit = '\b';
            break;
        case 'f':
            lit = '\f';
            break;
        case 'n':
            lit = '\n';
            break;
        case 'r':
            lit = '\r';
            break;
        case 't':
            lit = '\t';
            break;
        case 'u': {
            int32_t cp = hex4(r->p);
            if (cp < 0) {
                free(buf);
                return fail(r, "bad \\u escape");
            }
            r->p += 4;
            // A surrogate pair joins into one code point.
            if (cp >= 0xD800 && cp <= 0xDBFF && r->p[0] == '\\' && r->p[1] == 'u') {
                int32_t lo = hex4(r->p + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    r->p += 6;
                }
            }
            if (!buf_put_utf8(&buf, &len, &cap, (uint32_t)cp))
                goto oom;
            continue;
        }
        default:
            free(buf);
            r->p--;
            return fail(r, "bad escape");
        }
        if (!buf_put(&buf, &len, &cap, lit))
            goto oom;
    }
    r->p++;
    if (!buf_put(&buf, &len, &cap, '\0'))
        goto oom;
    *out = buf;
    return true;
oom:
    free(buf);
    return fail(r, "out of memory");
}

// Parse a number: an integer when it has no fraction or exponent.
static bool parse_number(json_reader_t *r, value_t *out) {
    const char *q = r->p;
    bool is_float = false;
    if (*q == '-')
        q++;
    if (!(*q >= '0' && *q <= '9'))
        return fail(r, "expected a value");
    while ((*q >= '0' && *q <= '9') || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-') {
        if (*q == '.' || *q == 'e' || *q == 'E')
            is_float = true;
        q++;
    }
    char *end = NULL;
    if (is_float) {
        double d = strtod(r->p, &end);
        if (end != q)
            return fail(r, "bad number");
        *out = val_float(d);
    } else {
        long long ll = strtoll(r->p, &end, 10);
        if (end != q)
            return fail(r, "bad number");
        *out = val_int((int64_t)ll);
    }
    r->p = q;
    return true;
}

// Parse an array.
static bool parse_array(json_reader_t *r, value_t *out, int depth) {
    r->p++; // '['
    value_t *items = NULL;
    size_t n = 0, cap = 0;
    skip_ws(r);
    if (*r->p == ']') {
        r->p++;
        *out = val_list(NULL, 0);
        return true;
    }
    for (;;) {
        value_t v;
        if (!parse_value(r, &v, depth + 1))
            goto bad;
        if (!val_list_push(&items, &n, &cap, v)) {
            value_free(&v);
            fail(r, "out of memory");
            goto bad;
        }
        skip_ws(r);
        if (*r->p == ',') {
            r->p++;
            continue;
        }
        if (*r->p == ']') {
            r->p++;
            break;
        }
        fail(r, "expected ',' or ']'");
        goto bad;
    }
    *out = val_list(items, n);
    return true;
bad: {
    value_t tmp = val_list(items, n);
    value_free(&tmp);
}
    return false;
}

// Parse an object.  A key given twice is an error.
static bool parse_object(json_reader_t *r, value_t *out, int depth) {
    r->p++; // '{'
    value_map_builder_t *b = val_map_new();
    if (!b)
        return fail(r, "out of memory");
    char **keys = NULL;
    size_t nkeys = 0;
    bool ok = false;
    skip_ws(r);
    if (*r->p == '}') {
        r->p++;
        ok = true;
        goto done;
    }
    for (;;) {
        skip_ws(r);
        char *key = NULL;
        if (!parse_string(r, &key))
            goto done;
        for (size_t i = 0; i < nkeys; i++) {
            if (strcmp(keys[i], key) == 0) {
                free(key);
                fail(r, "key given twice");
                goto done;
            }
        }
        char **nk = (char **)realloc(keys, (nkeys + 1) * sizeof(char *));
        if (!nk) {
            free(key);
            fail(r, "out of memory");
            goto done;
        }
        keys = nk;
        keys[nkeys++] = key;
        skip_ws(r);
        if (*r->p != ':') {
            fail(r, "expected ':'");
            goto done;
        }
        r->p++;
        value_t v;
        if (!parse_value(r, &v, depth + 1))
            goto done;
        val_map_put(b, key, v);
        skip_ws(r);
        if (*r->p == ',') {
            r->p++;
            continue;
        }
        if (*r->p == '}') {
            r->p++;
            ok = true;
            break;
        }
        fail(r, "expected ',' or '}'");
        goto done;
    }
done:
    for (size_t i = 0; i < nkeys; i++)
        free(keys[i]);
    free(keys);
    value_t m = val_map_finish(b);
    if (!ok) {
        value_free(&m);
        return false;
    }
    *out = m;
    return true;
}

static bool parse_value(json_reader_t *r, value_t *out, int depth) {
    if (depth > JSON_MAX_DEPTH)
        return fail(r, "nested too deeply");
    skip_ws(r);
    switch (*r->p) {
    case '{':
        return parse_object(r, out, depth);
    case '[':
        return parse_array(r, out, depth);
    case '"': {
        char *s = NULL;
        if (!parse_string(r, &s))
            return false;
        *out = val_str(s);
        free(s);
        return true;
    }
    case 't':
        if (strncmp(r->p, "true", 4) == 0) {
            r->p += 4;
            *out = val_bool(true);
            return true;
        }
        break;
    case 'f':
        if (strncmp(r->p, "false", 5) == 0) {
            r->p += 5;
            *out = val_bool(false);
            return true;
        }
        break;
    case 'n':
        if (strncmp(r->p, "null", 4) == 0) {
            r->p += 4;
            *out = val_none();
            return true;
        }
        break;
    default:
        return parse_number(r, out);
    }
    return fail(r, "expected a value");
}

bool json_value_parse(const char *text, value_t *out, char *err, size_t errlen) {
    if (err && errlen)
        err[0] = '\0';
    json_reader_t r = {.start = text ? text : "", .p = text ? text : "", .err = err, .errlen = errlen};
    value_t v;
    if (!parse_value(&r, &v, 0))
        return false;
    skip_ws(&r);
    if (*r.p) {
        value_free(&v);
        return fail(&r, "trailing text after the document");
    }
    *out = v;
    return true;
}
