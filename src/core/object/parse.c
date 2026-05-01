// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// parse.c
// Unified literal parser. See parse.h for the grammar.

#include "parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h" // for object_is_reserved_word

// Skip ASCII whitespace at *p in place.
static void skip_ws(const char **p) {
    while (**p && isspace((unsigned char)**p))
        (*p)++;
}

// Append c to a heap buffer (*buf, *len, *cap), growing as needed.
static bool buf_append_char(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 1 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 32;
        char *nb = (char *)realloc(*buf, nc);
        if (!nb)
            return false;
        *buf = nb;
        *cap = nc;
    }
    (*buf)[(*len)++] = c;
    (*buf)[*len] = '\0';
    return true;
}

// Decode a single \-escape starting at *p (which points at the '\').
// Advances *p past the escape and writes the resulting byte into *out.
// Returns false on a malformed escape.
static bool decode_escape(const char **p, char *out) {
    if ((*p)[0] != '\\')
        return false;
    char c = (*p)[1];
    switch (c) {
    case 'n':
        *out = '\n';
        *p += 2;
        return true;
    case 't':
        *out = '\t';
        *p += 2;
        return true;
    case 'r':
        *out = '\r';
        *p += 2;
        return true;
    case '0':
        *out = '\0';
        *p += 2;
        return true;
    case '\\':
        *out = '\\';
        *p += 2;
        return true;
    case '"':
        *out = '"';
        *p += 2;
        return true;
    case '\'':
        *out = '\'';
        *p += 2;
        return true;
    case 'x': {
        // \xHH
        if (!isxdigit((unsigned char)(*p)[2]) || !isxdigit((unsigned char)(*p)[3]))
            return false;
        char hex[3] = {(*p)[2], (*p)[3], 0};
        *out = (char)strtoul(hex, NULL, 16);
        *p += 4;
        return true;
    }
    default:
        return false;
    }
}

value_t parse_string_literal(const char **p) {
    if (!p || !*p || **p != '"')
        return val_err("expected '\"' to start string literal");
    const char *q = *p + 1;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    while (*q && *q != '"') {
        if (*q == '\\') {
            char c = 0;
            if (!decode_escape(&q, &c)) {
                free(buf);
                return val_err("bad string escape");
            }
            if (!buf_append_char(&buf, &len, &cap, c)) {
                free(buf);
                return val_err("out of memory in string literal");
            }
        } else {
            if (!buf_append_char(&buf, &len, &cap, *q)) {
                free(buf);
                return val_err("out of memory in string literal");
            }
            q++;
        }
    }
    if (*q != '"') {
        free(buf);
        return val_err("unterminated string literal");
    }
    *p = q + 1;
    // Wrap the heap buffer into a V_STRING. val_str strdups, so we then
    // free our own buffer — slightly wasteful, but keeps the "constructors
    // copy" rule consistent (proposal §2.6).
    value_t v = val_str(buf ? buf : "");
    free(buf);
    return v;
}

// Parse an integer literal: optional sign, optional base prefix, digits
// (with `_` as a separator), optional `u`/`i` suffix. On success
// advances *p and returns V_UINT (default) or V_INT (if signed/`i`).
value_t parse_integer_literal(const char **p) {
    if (!p || !*p)
        return val_err("expected integer literal");
    const char *q = *p;
    bool negative = false;
    if (*q == '-') {
        negative = true;
        q++;
    } else if (*q == '+') {
        q++;
    }

    int base = 10;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
        base = 16;
        q += 2;
    } else if (q[0] == '0' && (q[1] == 'b' || q[1] == 'B')) {
        base = 2;
        q += 2;
    } else if (q[0] == '0' && (q[1] == 'o' || q[1] == 'O')) {
        base = 8;
        q += 2;
    } else if (q[0] == '0' && (q[1] == 'd' || q[1] == 'D')) {
        base = 10;
        q += 2;
    } else if (q[0] == '$') {
        base = 16;
        q += 1;
    }

    // Collect digits + underscores into a temporary buffer to ignore '_'.
    char tmp[80];
    size_t ti = 0;
    bool any = false;
    while (*q && (isalnum((unsigned char)*q) || *q == '_')) {
        if (*q == '_') {
            q++;
            continue;
        }
        // Validate against base.
        char c = (char)tolower((unsigned char)*q);
        bool ok = (base == 16 && (isdigit((unsigned char)c) || (c >= 'a' && c <= 'f'))) ||
                  (base == 10 && isdigit((unsigned char)c)) || (base == 8 && c >= '0' && c <= '7') ||
                  (base == 2 && (c == '0' || c == '1'));
        if (!ok)
            break;
        if (ti + 1 >= sizeof(tmp))
            return val_err("integer literal too long");
        tmp[ti++] = *q;
        q++;
        any = true;
    }
    if (!any)
        return val_err("expected integer literal");
    tmp[ti] = '\0';

    // Suffix: u|i, optional bit-width number (8/16/32/64) is not yet honoured.
    bool force_signed = negative;
    bool force_unsigned = false;
    if (*q == 'u' || *q == 'U') {
        force_unsigned = true;
        q++;
    } else if (*q == 'i' || *q == 'I') {
        force_signed = true;
        q++;
    }
    while (isdigit((unsigned char)*q))
        q++; // swallow optional bit-width

    char *endp = NULL;
    unsigned long long uv = strtoull(tmp, &endp, base);
    if (endp == tmp)
        return val_err("malformed integer literal");
    *p = q;

    if (force_unsigned || (!force_signed && !negative)) {
        return val_uint(8, (uint64_t)uv);
    }
    int64_t iv = negative ? -(int64_t)uv : (int64_t)uv;
    return val_int(iv);
}

// Parse a float literal at *p. Returns V_ERROR if not a float; on
// success advances *p. A literal qualifies as a float if it has a '.',
// or an 'e'/'E' exponent, or starts with `0x` and contains a 'p'/'P'.
static value_t parse_float_literal(const char **p) {
    const char *q = *p;
    if (*q == '+' || *q == '-')
        q++;
    bool has_dot = false, has_exp = false, has_digits = false;
    bool hex = false;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
        hex = true;
        q += 2;
        while (isxdigit((unsigned char)*q) || *q == '_') {
            if (*q != '_')
                has_digits = true;
            q++;
        }
        if (*q == '.') {
            has_dot = true;
            q++;
            while (isxdigit((unsigned char)*q) || *q == '_') {
                if (*q != '_')
                    has_digits = true;
                q++;
            }
        }
        if (*q == 'p' || *q == 'P') {
            has_exp = true;
            q++;
            if (*q == '+' || *q == '-')
                q++;
            while (isdigit((unsigned char)*q))
                q++;
        }
        if (!has_exp)
            return val_err("not a float");
    } else {
        while (isdigit((unsigned char)*q) || *q == '_') {
            if (*q != '_')
                has_digits = true;
            q++;
        }
        if (*q == '.') {
            has_dot = true;
            q++;
            while (isdigit((unsigned char)*q) || *q == '_') {
                if (*q != '_')
                    has_digits = true;
                q++;
            }
        }
        if (*q == 'e' || *q == 'E') {
            has_exp = true;
            q++;
            if (*q == '+' || *q == '-')
                q++;
            while (isdigit((unsigned char)*q))
                q++;
        }
        if (!has_dot && !has_exp)
            return val_err("not a float");
        if (!has_digits)
            return val_err("not a float");
    }
    (void)hex;

    // Defer to strtod on a copy that strips underscores.
    size_t span = (size_t)(q - *p);
    if (span >= 80)
        return val_err("float literal too long");
    char tmp[80];
    size_t ti = 0;
    for (size_t i = 0; i < span; i++) {
        if ((*p)[i] != '_')
            tmp[ti++] = (*p)[i];
    }
    tmp[ti] = '\0';
    char *endp = NULL;
    double d = strtod(tmp, &endp);
    if (!endp || endp == tmp)
        return val_err("malformed float literal");
    *p = q;
    return val_float(d);
}

// Match keyword `kw` at *q only if it is followed by a non-identifier
// character. Returns the position past the keyword on match, NULL on
// no-match.
static const char *match_keyword(const char *q, const char *kw) {
    size_t n = strlen(kw);
    if (strncmp(q, kw, n) != 0)
        return NULL;
    char c = q[n];
    if (c == '\0' || !(isalnum((unsigned char)c) || c == '_'))
        return q + n;
    return NULL;
}

value_t parse_literal(const char **p, const char *const *enum_table, size_t n_enum) {
    if (!p || !*p)
        return val_err("null parser input");
    skip_ws(p);
    const char *q = *p;
    if (!*q)
        return val_err("empty literal");

    // String "..."
    if (*q == '"')
        return parse_string_literal(p);

    // Boolean keywords (reserved words; cannot be shadowed).
    {
        const char *r;
        if ((r = match_keyword(q, "true"))) {
            *p = r;
            return val_bool(true);
        }
        if ((r = match_keyword(q, "false"))) {
            *p = r;
            return val_bool(false);
        }
        if ((r = match_keyword(q, "on"))) {
            *p = r;
            return val_bool(true);
        }
        if ((r = match_keyword(q, "off"))) {
            *p = r;
            return val_bool(false);
        }
        if ((r = match_keyword(q, "yes"))) {
            *p = r;
            return val_bool(true);
        }
        if ((r = match_keyword(q, "no"))) {
            *p = r;
            return val_bool(false);
        }
    }

    // Try float first when the prefix matches a float pattern; otherwise
    // try integer; otherwise identifier-as-enum.
    bool starts_numeric = (*q == '+' || *q == '-' || *q == '.' || *q == '$' || isdigit((unsigned char)*q));
    if (starts_numeric) {
        // Float requires either '.' anywhere up to the next non-numeric, or
        // an exponent letter. Probe by scanning ahead briefly.
        const char *scan = q;
        if (*scan == '+' || *scan == '-')
            scan++;
        bool maybe_float = false;
        if (*scan == '.')
            maybe_float = true;
        else if (scan[0] == '0' && (scan[1] == 'x' || scan[1] == 'X')) {
            const char *r = scan + 2;
            while (isxdigit((unsigned char)*r) || *r == '_' || *r == '.') {
                if (*r == '.') {
                    maybe_float = true;
                    break;
                }
                r++;
            }
            if (!maybe_float) {
                while (isxdigit((unsigned char)*r) || *r == '_')
                    r++;
                if (*r == 'p' || *r == 'P')
                    maybe_float = true;
            }
        } else {
            const char *r = scan;
            while (isdigit((unsigned char)*r) || *r == '_')
                r++;
            if (*r == '.')
                maybe_float = true;
            else if (*r == 'e' || *r == 'E')
                maybe_float = true;
        }
        if (maybe_float) {
            const char *save = *p;
            value_t fv = parse_float_literal(p);
            if (fv.kind == V_FLOAT) {
                // No :bytes suffix on floats.
                return fv;
            }
            value_free(&fv);
            *p = save;
        }
        // Integer.
        const char *save = *p;
        value_t iv = parse_integer_literal(p);
        if (iv.kind == V_INT || iv.kind == V_UINT) {
            // Optional bytes suffix: NUMBER:N → V_BYTES of N bytes
            // big-endian little-endian? Proposal §4.2 spells
            // "0xDEAD_BEEF:4" — N is the byte width. We emit big-endian.
            if (**p == ':') {
                const char *r = *p + 1;
                long long n = 0;
                const char *after = NULL;
                {
                    char *endp = NULL;
                    long long v = strtoll(r, &endp, 0);
                    if (endp && endp != r) {
                        n = v;
                        after = endp;
                    }
                }
                if (after && n > 0 && n <= 16) {
                    *p = after;
                    uint64_t u = (iv.kind == V_INT) ? (uint64_t)iv.i : iv.u;
                    uint8_t buf[16] = {0};
                    for (int i = (int)n - 1; i >= 0; i--) {
                        buf[i] = (uint8_t)(u & 0xFF);
                        u >>= 8;
                    }
                    value_free(&iv);
                    return val_bytes(buf, (size_t)n);
                }
            }
            return iv;
        }
        value_free(&iv);
        *p = save;
        return val_err("expected literal");
    }

    // Identifier — match against enum_table if provided.
    if (isalpha((unsigned char)*q) || *q == '_') {
        char ident[64];
        size_t i = 0;
        while (*q && (isalnum((unsigned char)*q) || *q == '_')) {
            if (i + 1 < sizeof(ident))
                ident[i++] = *q;
            q++;
        }
        ident[i] = '\0';
        if (object_is_reserved_word(ident)) {
            // Reserved words other than the boolean keywords above aren't
            // valid as bare-identifier literals.
            return val_err("'%s' is a reserved word", ident);
        }
        if (enum_table) {
            for (size_t k = 0; k < n_enum; k++) {
                if (enum_table[k] && strcmp(enum_table[k], ident) == 0) {
                    *p = q;
                    return val_enum((int)k, enum_table, n_enum);
                }
            }
            return val_err("'%s' is not in the enum table", ident);
        }
        // No enum context — return as a string. The caller decides
        // whether bare identifiers are allowed in its position.
        *p = q;
        return val_str(ident);
    }

    return val_err("unexpected character '%c'", *q);
}

value_t parse_literal_full(const char *s, const char *const *enum_table, size_t n_enum) {
    if (!s)
        return val_err("null literal string");
    const char *p = s;
    value_t v = parse_literal(&p, enum_table, n_enum);
    if (val_is_error(&v))
        return v;
    skip_ws(&p);
    if (*p != '\0') {
        value_free(&v);
        return val_err("trailing garbage after literal: '%s'", p);
    }
    return v;
}
