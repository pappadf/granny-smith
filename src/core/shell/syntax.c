// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// syntax.c
// Statement classification and the shared lexers.  See syntax.h and
// docs/internals/core/shell/shell.md ("Statements").

#include "syntax.h"

#include <string.h>

// === Characters ===============================================================

static bool ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool ident_char(char c) {
    return ident_start(c) || (c >= '0' && c <= '9');
}

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
    while (p < end && ident_char(*p))
        p++;
    return p;
}

// Keyword `kw` at p with an identifier boundary after it: the position past
// it, or NULL.
static const char *kw_at(const char *p, const char *end, const char *kw) {
    size_t n = strlen(kw);
    if ((size_t)(end - p) < n || strncmp(p, kw, n) != 0)
        return NULL;
    if (p + n < end && ident_char(p[n]))
        return NULL;
    return p + n;
}

// A single `=` at p (not `==`).
static bool single_eq(const char *p, const char *end) {
    return p < end && *p == '=' && !(p + 1 < end && p[1] == '=');
}

// Quote state for the bracket and comment scans: the parser's rule, where a
// backslash escapes the next byte inside and outside quotes.
typedef struct {
    char quote; // 0, '"' or '\''
    bool esc;
} quote_state_t;

// Advance the quote state over c.
static void quote_step(quote_state_t *q, char c) {
    if (q->esc) {
        q->esc = false;
        return;
    }
    if (c == '\\') {
        q->esc = true;
        return;
    }
    if (q->quote) {
        if (c == q->quote)
            q->quote = 0;
        return;
    }
    if (c == '"' || c == '\'')
        q->quote = c;
}

// The first unquoted `c` in [p, end), or NULL.
static const char *find_unquoted(const char *p, const char *end, char c) {
    quote_state_t q = {0};
    for (; p < end; p++) {
        if (!q.quote && !q.esc && *p == c)
            return p;
        quote_step(&q, *p);
    }
    return NULL;
}

// === Lexers ===================================================================

const char *script_bracket_close(const char *open, const char *end) {
    quote_state_t q = {0};
    int depth = 0;
    for (const char *r = open; r < end; r++) {
        if (!q.quote && !q.esc) {
            if (*r == '[')
                depth++;
            else if (*r == ']' && --depth == 0)
                return r;
        }
        quote_step(&q, *r);
    }
    return NULL;
}

const char *script_path_end(const char *p, const char *end) {
    const char *q = p;
    if (q < end && *q == '$')
        q++;
    if (q >= end || !ident_start(*q))
        return p;
    q = ident_end(q, end);
    for (;;) {
        // `.seg` (a digit is a segment too: `drives.0`).
        if (q + 1 < end && q[0] == '.' && ident_char(q[1])) {
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
    if (p < end && ident_start(*p)) {
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
        quote_state_t q = {0};
        int depth = 0;
        for (const char *r = p; r < end; r++) {
            if (!q.quote && !q.esc) {
                if (*r == '(')
                    depth++;
                else if (*r == ')' && --depth == 0)
                    return r + 1;
            }
            quote_step(&q, *r);
        }
        if (open)
            *open = true;
        return end;
    }
    if (c == '$') {
        // A binding path; its brackets may hold blanks.
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
    return find_unquoted(line, end, '#');
}

// === Classifier ===============================================================

// A keyword statement: the keyword and what follows it.
static void keyword_stmt(script_stmt_t *out, script_stmt_kind_t kind, const char *p, const char *after,
                         const char *end) {
    out->kind = kind;
    out->head = p;
    out->head_end = after;
    out->rest = skip_ws(after, end);
}

// Mark the statement invalid with the parser's message.
static void invalid(script_stmt_t *out, const char *msg) {
    out->kind = SCRIPT_STMT_INVALID;
    out->error = msg;
}

// `KW NAME = VALUE` (let, alias, command): fill name / eq / rest when the
// shape holds.  True when it does.
static bool decl_shape(script_stmt_t *out, const char *after, const char *end) {
    const char *q = skip_ws(after, end);
    if (q >= end || !ident_start(*q))
        return false;
    out->name = q;
    out->name_end = ident_end(q, end);
    const char *eq = skip_ws(out->name_end, end);
    if (!single_eq(eq, end))
        return false;
    out->eq = eq;
    out->rest = skip_ws(eq + 1, end);
    return true;
}

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
    const char *after;

    // Keywords with nothing after them.
    if ((after = kw_at(p, end, "break")) || (after = kw_at(p, end, "continue"))) {
        bool brk = p[0] == 'b';
        keyword_stmt(out, brk ? SCRIPT_STMT_BREAK : SCRIPT_STMT_CONTINUE, p, after, end);
        if (out->rest < end)
            invalid(out, brk ? "break takes no arguments" : "continue takes no arguments");
        return;
    }
    // Keywords taking an expression.
    if ((after = kw_at(p, end, "return"))) {
        keyword_stmt(out, SCRIPT_STMT_RETURN, p, after, end);
        return;
    }
    if ((after = kw_at(p, end, "include"))) {
        keyword_stmt(out, SCRIPT_STMT_INCLUDE, p, after, end);
        if (out->rest >= end)
            invalid(out, "include: missing path");
        return;
    }
    if ((after = kw_at(p, end, "assert"))) {
        keyword_stmt(out, SCRIPT_STMT_ASSERT, p, after, end);
        if (out->rest >= end)
            invalid(out, "assert: missing predicate");
        return;
    }
    // Block forms: the keyword decides; the parser checks the header.
    static const struct {
        const char *kw;
        script_stmt_kind_t kind;
    } blocks[] = {
        {"if",    SCRIPT_STMT_IF   },
        {"elif",  SCRIPT_STMT_ELIF },
        {"else",  SCRIPT_STMT_ELSE },
        {"while", SCRIPT_STMT_WHILE},
        {"for",   SCRIPT_STMT_FOR  },
        {"def",   SCRIPT_STMT_DEF  },
    };
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        if ((after = kw_at(p, end, blocks[i].kw))) {
            keyword_stmt(out, blocks[i].kind, p, after, end);
            // `for NAME` and `def NAME`: the name being bound.
            if ((blocks[i].kind == SCRIPT_STMT_FOR || blocks[i].kind == SCRIPT_STMT_DEF) && out->rest < end &&
                ident_start(*out->rest)) {
                out->name = out->rest;
                out->name_end = ident_end(out->rest, end);
            }
            return;
        }
    }
    // Declarations.  `let` and `alias` are reserved: any other shape after
    // them is an error, never a path.
    if ((after = kw_at(p, end, "let"))) {
        keyword_stmt(out, SCRIPT_STMT_LET, p, after, end);
        const char *q = out->rest;
        if (q >= end || !ident_start(*q))
            invalid(out, "let: expected a name");
        else if (!decl_shape(out, after, end))
            invalid(out, "let: expected '=' after name");
        else if (out->rest >= end)
            invalid(out, "let: missing expression");
        return;
    }
    if ((after = kw_at(p, end, "alias"))) {
        keyword_stmt(out, SCRIPT_STMT_ALIAS, p, after, end);
        if (!decl_shape(out, after, end))
            invalid(out, "alias: expected `alias NAME = PATH`");
        else if (out->rest >= end)
            invalid(out, "alias: missing path");
        return;
    }
    // `command NAME = PATH`: a contextual keyword, not a reserved word (a
    // member may be called `command`).  Only this exact shape with a path
    // declares a command; anything else is read as a path statement.
    if ((after = kw_at(p, end, "command"))) {
        script_stmt_t d = {0};
        if (decl_shape(&d, after, end) && d.rest < end) {
            keyword_stmt(out, SCRIPT_STMT_COMMAND_DEF, p, after, end);
            out->name = d.name;
            out->name_end = d.name_end;
            out->eq = d.eq;
            out->rest = d.rest;
            return;
        }
    }
    // Reserved words that head nothing.
    if ((after = kw_at(p, end, "in")) || (after = kw_at(p, end, "do"))) {
        keyword_stmt(out, SCRIPT_STMT_INVALID, p, after, end);
        invalid(out, p[0] == 'i' ? "'in' outside a for header" : "'do' is a reserved word");
        return;
    }
    // Literals read as expressions, not as paths.
    if (kw_at(p, end, "true") || kw_at(p, end, "false") || kw_at(p, end, "none")) {
        out->kind = SCRIPT_STMT_EXPR;
        return;
    }

    // A path or binding head: assignment, call form, bare path or command.
    if ((*p == '$' && p + 1 < end && ident_start(p[1])) || ident_start(*p)) {
        const char *q = script_path_end(p, end);
        out->head = p;
        if (!q) {
            out->head_end = end;
            invalid(out, "unbalanced '['");
            return;
        }
        out->head_end = q;
        const char *rest = skip_ws(q, end);
        if (single_eq(rest, end)) {
            out->kind = SCRIPT_STMT_ASSIGN;
            out->eq = rest;
            out->rest = skip_ws(rest + 1, end);
            if (out->rest >= end)
                invalid(out, "assignment: missing right-hand side");
            return;
        }
        // A plain `$name` is a binding read; `PATH(` with no blank between
        // is a call form.  Both are expressions.  (`PATH (x)` is a command
        // with a parenthesised argument.)
        if ((rest >= end && *p == '$' && q == ident_end(p + 1, end)) || (q < end && *q == '(')) {
            out->kind = SCRIPT_STMT_EXPR;
            return;
        }
        out->kind = SCRIPT_STMT_COMMAND;
        out->rest = rest;
        return;
    }
    // Numbers, strings, parentheses, operators: an expression.
    out->kind = SCRIPT_STMT_EXPR;
}

// === Lines ====================================================================

// True for the kinds whose statement opens a block.
static bool block_kind(script_stmt_kind_t k) {
    return k == SCRIPT_STMT_IF || k == SCRIPT_STMT_ELIF || k == SCRIPT_STMT_ELSE || k == SCRIPT_STMT_WHILE ||
           k == SCRIPT_STMT_FOR || k == SCRIPT_STMT_DEF;
}

void script_line_split(const char *line, const char *end, script_line_t *out) {
    memset(out, 0, sizeof(*out));
    out->comment = script_comment_start(line, end);
    // The statement text: the line without its comment and outer blanks.
    const char *e = out->comment ? out->comment : end;
    while (e > line && blank(e[-1]))
        e--;
    const char *p = skip_ws(line, e);
    if (p < e && *p == '}') {
        out->closer = p;
        p = skip_ws(p + 1, e);
    }
    script_classify(p, e, &out->stmt);
    script_classify(e, e, &out->body);
    if (!block_kind(out->stmt.kind))
        return;
    const char *brace = find_unquoted(p, e, '{');
    if (!brace)
        return;
    // The header ends at the brace.
    script_classify(p, brace, &out->stmt);
    out->open = brace;
    const char *after = skip_ws(brace + 1, e);
    if (after >= e)
        return; // multi-line form: the body is on the lines that follow
    // Inline form: one statement, then the line's last `}`.
    const char *body_end = e;
    if (e[-1] == '}' && e - 1 > brace) {
        out->close = e - 1;
        body_end = e - 1;
    }
    script_classify(after, body_end, &out->body);
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
