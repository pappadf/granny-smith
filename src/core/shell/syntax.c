// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// syntax.c
// Command-line classification and the shared lexers.  See syntax.h and
// docs/internals/core/lua/lua.md ("The terminal").

#include "syntax.h"

#include <string.h>

// === Characters ===============================================================

static bool blank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// Skip blanks from p, not past end.
static const char *skip_ws(const char *p, const char *end) {
    while (p < end && blank(*p))
        p++;
    return p;
}

// The end of the identifier at p.
static const char *ident_end(const char *p, const char *end) {
    while (p < end && script_ident_char(*p))
        p++;
    return p;
}

// A single `=` at p (not `==`).
static bool single_eq(const char *p, const char *end) {
    return p < end && *p == '=' && !(p + 1 < end && p[1] == '=');
}

const char *script_find_unquoted(const char *p, const char *end, char c) {
    script_quote_t q = {0};
    for (; p < end; p++) {
        if (!q.quote && !q.esc && *p == c)
            return p;
        script_quote_step(&q, *p);
    }
    return NULL;
}

// === Lexers ===================================================================

const char *script_bracket_close(const char *open, const char *end) {
    script_quote_t q = {0};
    int depth = 0;
    for (const char *r = open; r < end; r++) {
        if (!q.quote && !q.esc) {
            if (*r == '[')
                depth++;
            else if (*r == ']' && --depth == 0)
                return r;
        }
        script_quote_step(&q, *r);
    }
    return NULL;
}

const char *script_path_end(const char *p, const char *end) {
    const char *q = p;
    if (q < end && *q == '$')
        q++;
    if (q >= end || !script_ident_start(*q))
        return p;
    q = ident_end(q, end);
    for (;;) {
        // `.seg` (a digit is a segment too: `drives.0`).
        if (q + 1 < end && q[0] == '.' && script_ident_char(q[1])) {
            q = ident_end(q + 1, end);
            continue;
        }
        if (q < end && *q == '[') {
            const char *close = script_bracket_close(q, end);
            if (!close)
                return NULL;
            q = close + 1;
            continue;
        }
        return q;
    }
}

// A curly double quote (U+201C / U+201D, UTF-8 E2 80 9C / 9D) at p.
static bool curly(const char *p, const char *end, unsigned char last) {
    return end - p >= 3 && (unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == last;
}

const char *script_arg_end(const char *p, const char *end, bool *open) {
    if (open)
        *open = false;
    // `name=` prefix.
    if (p < end && script_ident_start(*p)) {
        const char *e = ident_end(p, end);
        if (single_eq(e, end))
            p = e + 1;
    }
    if (p >= end)
        return p;
    char c = *p;
    if (c == '"' || c == '\'') {
        // A double-quoted string takes every escape; a raw one only `\'`.
        const char *q = p + 1;
        while (q < end && *q != c) {
            if (*q == '\\' && q + 1 < end && (c == '"' || q[1] == '\''))
                q++;
            q++;
        }
        if (q >= end) {
            if (open)
                *open = true;
            return end;
        }
        return q + 1;
    }
    if (curly(p, end, 0x9C)) {
        const char *q = p + 3;
        while (q < end && !curly(q, end, 0x9D))
            q++;
        if (q >= end) {
            if (open)
                *open = true;
            return end;
        }
        return q + 3;
    }
    if (c == '(') {
        // A parenthesised expression: to the matching `)`, quote-aware.
        script_quote_t q = {0};
        int depth = 0;
        for (const char *r = p; r < end; r++) {
            if (!q.quote && !q.esc) {
                if (*r == '(')
                    depth++;
                else if (*r == ')' && --depth == 0)
                    return r + 1;
            }
            script_quote_step(&q, *r);
        }
        if (open)
            *open = true;
        return end;
    }
    if (c == '$') {
        // An alias path; its brackets may hold blanks.
        const char *e = script_path_end(p, end);
        if (!e) {
            if (open)
                *open = true;
            return end;
        }
        p = e;
    }
    // A bare word (or what follows a `$` path).
    while (p < end && !blank(*p)) {
        if (*p == '\\' && p + 1 < end)
            p++;
        p++;
    }
    return p;
}

const char *script_comment_start(const char *line, const char *end) {
    script_quote_t q = {0};
    for (const char *p = line; p < end; p++) {
        if (!q.quote && !q.esc && (*p == '#' || (*p == '-' && p + 1 < end && p[1] == '-')) &&
            (p == line || blank(p[-1])))
            return p;
        script_quote_step(&q, *p);
    }
    return NULL;
}

// === Classifier ===============================================================

void script_classify(const char *text, const char *end, script_stmt_t *out) {
    memset(out, 0, sizeof(*out));
    while (end > text && blank(end[-1]))
        end--;
    const char *p = skip_ws(text, end);
    out->start = p;
    out->end = end;
    out->rest = p;
    if (p >= end)
        return; // SCRIPT_STMT_EMPTY
    // A path or alias head: an assignment, a command (argument mode), or a
    // Lua call when `(` follows the path directly.
    if ((*p == '$' && p + 1 < end && script_ident_start(p[1])) || script_ident_start(*p)) {
        const char *q = script_path_end(p, end);
        out->head = p;
        if (!q) {
            out->head_end = end;
            out->kind = SCRIPT_STMT_INVALID;
            return;
        }
        out->head_end = q;
        const char *rest = skip_ws(q, end);
        if (single_eq(rest, end)) {
            out->kind = SCRIPT_STMT_ASSIGN;
            out->eq = rest;
            out->rest = skip_ws(rest + 1, end);
            return;
        }
        if (q < end && *q == '(') {
            out->kind = SCRIPT_STMT_EXPR;
            return;
        }
        out->kind = SCRIPT_STMT_COMMAND;
        out->rest = rest;
        return;
    }
    out->kind = SCRIPT_STMT_EXPR;
}

// === Lines ====================================================================

void script_line_split(const char *line, const char *end, script_line_t *out) {
    memset(out, 0, sizeof(*out));
    out->comment = script_comment_start(line, end);
    script_classify(line, out->comment ? out->comment : end, &out->stmt);
}

// === Arguments ================================================================

int script_arg_slot(const member_t *m, int pos, const char *name, size_t name_len) {
    if (!m || m->kind != MK_METHOD || !m->method.args)
        return -1;
    int n = m->method.nargs;
    const arg_decl_t *a = m->method.args;
    if (name) {
        for (int i = 0; i < n; i++)
            if (a[i].name && strlen(a[i].name) == name_len && strncmp(a[i].name, name, name_len) == 0)
                return i;
        return -1;
    }
    if (pos < 0)
        return -1;
    if (pos < n)
        return pos;
    if (n > 0 && (a[n - 1].validation_flags & OBJ_ARG_REST))
        return n - 1;
    return -1;
}
