// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// highlight.c
// shell.highlight: syntax classes for a shell line or block, computed the
// way the script parser reads it (docs/internals/core/shell/shell.md,
// "Statements" and "Two parsing modes").  The parser's own classifier
// (syntax.h) says what each statement is and where a line's block braces,
// inline body and comment are; this file only colours the tokens:
//
//   - a keyword form colours its keyword, its declared name and its
//     expression (`let NAME = EXPR`, `for NAME in EXPR {`, …);
//   - an assignment colours its target path and its right-hand side;
//   - a command colours its head path (or command word) and its arguments,
//     which are in argument mode (bare words are strings);
//   - an expression statement is coloured as an expression.
//
// Path segments are resolved against the live tree as they are read:
// each resolved segment is an object, attribute or method; the first one
// that does not resolve, and every segment after it, is `unknown`.  In
// argument mode a bare word matching the argument's enum values is an
// `enum`; other bare words are strings and get no class.
//
// Partial or malformed input still gets spans for what lexes (an
// unterminated string runs to the end of its line); nothing here fails.

#include "highlight.h"

#include "alias.h"
#include "commands.h"
#include "object.h"
#include "syntax.h"

#include <stdlib.h>
#include <string.h>

#define PREFIX_MAX 512

// === Span list ================================================================

// The spans being built, and where scanning stops.
typedef struct {
    const char *base;
    // Scanning stops here: the end of the statement being coloured, or an
    // interpolation's format suffix.
    const char *limit;
    value_t *items;
    size_t len, cap;
    const char *last_cls;
    size_t last_end;
} hl_t;

// Emit [s, e) as `cls`; a span touching the previous one of the same class
// extends it.
static void emit(hl_t *h, const char *s, const char *e, const char *cls) {
    if (!s || !e || e <= s)
        return;
    size_t start = (size_t)(s - h->base), end = (size_t)(e - h->base);
    if (h->len && h->last_cls == cls && h->last_end == start) {
        value_t *m = &h->items[h->len - 1];
        for (size_t i = 0; i < m->map.len; i++)
            if (strcmp(m->map.entries[i].key, "end") == 0)
                m->map.entries[i].val = val_uint(4, end);
        h->last_end = end;
        return;
    }
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "start", val_uint(4, start));
    val_map_put(b, "end", val_uint(4, end));
    val_map_put(b, "class", val_str(cls));
    val_list_push(&h->items, &h->len, &h->cap, val_map_finish(b));
    h->last_cls = cls;
    h->last_end = end;
}

// True at the end of what is being scanned.
static bool at_end(const hl_t *h, const char *p) {
    return !*p || p >= h->limit;
}

// === Characters ===============================================================

static bool ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool ident_char(char c) {
    return ident_start(c) || (c >= '0' && c <= '9');
}
static bool digit(char c) {
    return c >= '0' && c <= '9';
}
static const char *skip_ws(const hl_t *h, const char *p) {
    while (!at_end(h, p) && (*p == ' ' || *p == '\t' || *p == '\r'))
        p++;
    return p;
}
static const char *ident_end(const char *p) {
    while (ident_char(*p))
        p++;
    return p;
}
static bool word_is(const char *s, const char *e, const char *w) {
    size_t n = strlen(w);
    return (size_t)(e - s) == n && strncmp(s, w, n) == 0;
}
// A curly double quote (U+201C / U+201D, UTF-8 E2 80 9C / 9D).
static bool curly_open(const char *p) {
    return (unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x9C;
}
static bool curly_close(const char *p) {
    return (unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x9D;
}

// === Literals =================================================================

// A number at p (0x…, 0b…, decimal, float with fraction / exponent); the
// end, or p when there is none.  `1..5` is a range: the number stops
// before the `..`.
static const char *number_end(const char *p) {
    const char *q = p;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
        q += 2;
        while (digit(*q) || (*q >= 'a' && *q <= 'f') || (*q >= 'A' && *q <= 'F') || *q == '_')
            q++;
        return q;
    }
    if (q[0] == '0' && (q[1] == 'b' || q[1] == 'B') && (q[2] == '0' || q[2] == '1')) {
        q += 2;
        while (*q == '0' || *q == '1' || *q == '_')
            q++;
        return q;
    }
    if (!digit(*q))
        return p;
    while (digit(*q) || *q == '_')
        q++;
    if (*q == '.' && digit(q[1])) {
        q++;
        while (digit(*q))
            q++;
    }
    if ((*q == 'e' || *q == 'E') && (digit(q[1]) || ((q[1] == '+' || q[1] == '-') && digit(q[2])))) {
        q += 2;
        while (digit(*q))
            q++;
    }
    return q;
}

static const char *expr_until(hl_t *h, const char *p, char close);
static const char *binding(hl_t *h, const char *p);

// The `}` closing an interpolation whose body starts at p (nested braces
// and strings skipped), or NULL.
static const char *interp_close(const hl_t *h, const char *p) {
    int depth = 0;
    char quote = 0;
    for (; !at_end(h, p); p++) {
        if (quote) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == quote)
                quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            if (depth == 0)
                return p;
            depth--;
        }
    }
    return NULL;
}

// A format suffix `:FMT` ending at `close` (printf-ish: %, digits, letters,
// `.`, `-`, `+`, `#`, space), at the last top-level `:`; NULL when none.
static const char *format_colon(const char *p, const char *close) {
    const char *colon = NULL;
    int depth = 0;
    for (const char *q = p; q < close; q++) {
        if (*q == '(' || *q == '[')
            depth++;
        else if (*q == ')' || *q == ']')
            depth--;
        else if (*q == ':' && depth == 0)
            colon = q;
    }
    if (!colon || colon + 1 == close)
        return NULL;
    for (const char *q = colon + 1; q < close; q++)
        if (!(ident_char(*q) || strchr("%.-+# ", *q)))
            return NULL;
    return colon;
}

// A double-quoted string at p (opening quote included): `$name` splices
// are variables, `${EXPR}` is interp around an expression.  Answers the
// end (after the closing quote, or the end of the statement).
static const char *dq_string(hl_t *h, const char *p, bool curly) {
    const char *run = p;
    const char *q = p + (curly ? 3 : 1);
    for (;;) {
        if (at_end(h, q)) {
            emit(h, run, q, "string");
            return q;
        }
        if (!curly && *q == '"') {
            emit(h, run, q + 1, "string");
            return q + 1;
        }
        if (curly && curly_close(q)) {
            emit(h, run, q + 3, "string");
            return q + 3;
        }
        if (*q == '\\' && q[1]) {
            q += 2;
            continue;
        }
        if (*q == '$' && q[1] == '{') {
            emit(h, run, q, "string");
            emit(h, q, q + 2, "interp");
            // `${EXPR:FMT}`: the expression stops at a top-level `:` whose
            // suffix up to the `}` is a format spec.
            const char *close = interp_close(h, q + 2);
            const char *fmt = close ? format_colon(q + 2, close) : NULL;
            const char *saved = h->limit;
            if (fmt)
                h->limit = fmt;
            const char *r = expr_until(h, q + 2, '}');
            h->limit = saved;
            if (fmt) {
                emit(h, fmt, close, "interp");
                r = close;
            }
            if (!at_end(h, r) && *r == '}') {
                emit(h, r, r + 1, "interp");
                r++;
            }
            q = run = r;
            continue;
        }
        if (*q == '$' && ident_start(q[1])) {
            emit(h, run, q, "string");
            const char *e = ident_end(q + 1);
            emit(h, q, e, "variable");
            q = run = e;
            continue;
        }
        q++;
    }
}

// A raw string ('…', only \' escapes).
static const char *sq_string(hl_t *h, const char *p) {
    const char *q = p + 1;
    while (!at_end(h, q) && *q != '\'') {
        if (*q == '\\' && q[1] == '\'')
            q++;
        q++;
    }
    if (!at_end(h, q) && *q == '\'')
        q++;
    emit(h, p, q, "string");
    return q;
}

// === Paths ====================================================================

// A path's resolved node, from its text.
typedef struct {
    char text[PREFIX_MAX];
    size_t len;
    bool ok; // everything so far resolved
    bool known; // resolution applies (not a plain variable's continuation)
    node_t node;
} prefix_t;

static void prefix_add(prefix_t *pf, const char *s, const char *e, bool dot) {
    size_t n = (size_t)(e - s);
    if (pf->len + n + 2 >= sizeof(pf->text)) {
        pf->ok = false;
        return;
    }
    if (dot && pf->len)
        pf->text[pf->len++] = '.';
    memcpy(pf->text + pf->len, s, n);
    pf->len += n;
    pf->text[pf->len] = '\0';
}

static const char *node_class(node_t n) {
    if (n.member) {
        if (n.member->kind == MK_ATTR)
            return "attribute";
        if (n.member->kind == MK_METHOD)
            return "method";
    }
    return "object";
}

// Resolve the prefix; answers the class for the segment just added, or
// "unknown" (and the prefix stays failed).
static const char *prefix_resolve(prefix_t *pf) {
    if (!pf->ok)
        return "unknown";
    node_t n = object_resolve(object_root(), pf->text);
    if (!node_valid(n)) {
        pf->ok = false;
        return "unknown";
    }
    pf->node = n;
    return node_class(n);
}

// The continuation of a path at p (`.seg`, `[index]`, `["key"]`), with
// its segments classified against `pf`.  Segments are the parser's
// (script_path_end): a `.` before an identifier or digit, and a bracket up
// to its matching `]`.
static const char *path_rest(hl_t *h, const char *p, prefix_t *pf) {
    for (;;) {
        if (at_end(h, p))
            return p;
        if (*p == '.' && ident_char(p[1]) && p + 1 < h->limit) {
            const char *s = p + 1, *e = ident_end(s);
            if (pf->known) {
                prefix_add(pf, s, e, true);
                emit(h, s, e, prefix_resolve(pf));
            }
            p = e;
            continue;
        }
        if (*p == '[') {
            const char *close = script_bracket_close(p, h->limit);
            emit(h, p, p + 1, "operator");
            // The index is an expression; an unclosed one runs to the end.
            const char *saved = h->limit;
            if (close)
                h->limit = close;
            const char *q = expr_until(h, p + 1, ']');
            h->limit = saved;
            if (!close)
                return q;
            emit(h, close, close + 1, "operator");
            // A failed index makes what follows unknown; the index itself
            // keeps its literal classes.
            if (pf->known && pf->ok) {
                prefix_add(pf, p, close + 1, false);
                (void)prefix_resolve(pf);
            }
            p = close + 1;
            continue;
        }
        return p;
    }
}

// A path starting with an identifier at p.  An unresolved single word
// followed by `(`, or naming a `def` function, is a call, not unknown.
// Answers the end; `out` (optional) gets the resolved node when the whole
// path resolved.
static const char *path(hl_t *h, const char *p, node_t *out) {
    const char *e = ident_end(p);
    const char *after = e;
    prefix_t pf = {.ok = true, .known = true};
    prefix_add(&pf, p, e, false);
    const char *cls = prefix_resolve(&pf);
    if (!pf.ok) {
        // An unresolved single word followed by `(` is a function call.
        if (*after == '(') {
            emit(h, p, e, "method");
            return e;
        }
        if (*after != '.' && *after != '[' &&
            shell_word_resolve(p, (size_t)(e - p), NULL, NULL, NULL, 0) == SHELL_HEAD_FUNCTION) {
            emit(h, p, e, "method");
            return e;
        }
    }
    emit(h, p, e, cls);
    const char *end = path_rest(h, e, &pf);
    if (out)
        *out = pf.ok ? pf.node : (node_t){0};
    return end;
}

// `$name` (a variable, or an alias when one of that name exists) and its
// path continuation (resolved through the alias's path).
static const char *binding(hl_t *h, const char *p) {
    if (!ident_start(p[1])) {
        emit(h, p, p + 1, "operator");
        return p + 1;
    }
    const char *s = p + 1, *e = ident_end(s);
    char name[128];
    size_t n = (size_t)(e - s) < sizeof(name) - 1 ? (size_t)(e - s) : sizeof(name) - 1;
    memcpy(name, s, n);
    name[n] = '\0';
    // The emulator thread highlights while the job thread may redefine
    // aliases: work on a copy.
    char target[PREFIX_MAX];
    bool is_alias = alias_lookup_copy(name, target, sizeof(target), NULL);
    emit(h, p, e, is_alias ? "alias" : "variable");
    prefix_t pf = {.ok = true, .known = is_alias};
    if (is_alias) {
        prefix_add(&pf, target, target + strlen(target), false);
        (void)prefix_resolve(&pf);
    }
    return path_rest(h, e, &pf);
}

// === Expression mode ============================================================

static bool op_char(char c) {
    return strchr("+-*/%<>=!&|^~,:?.", c) != NULL && c != '\0';
}

// An expression from p, up to `close` at depth 0 (`)`, `]`, `}`) or the
// scan limit.  Answers where it stopped (on the closer, not after it).
static const char *expr_until(hl_t *h, const char *p, char close) {
    int depth = 0;
    for (;;) {
        p = skip_ws(h, p);
        if (at_end(h, p))
            return p;
        char c = *p;
        if (depth == 0 && close && c == close)
            return p;
        if (c == '"') {
            p = dq_string(h, p, false);
        } else if (c == '\'') {
            p = sq_string(h, p);
        } else if (curly_open(p)) {
            p = dq_string(h, p, true);
        } else if (digit(c)) {
            const char *e = number_end(p);
            emit(h, p, e, "number");
            p = e;
        } else if (c == '$') {
            p = binding(h, p);
        } else if (ident_start(c)) {
            const char *e = ident_end(p);
            if (word_is(p, e, "true") || word_is(p, e, "false") || word_is(p, e, "none") || word_is(p, e, "in")) {
                emit(h, p, e, "keyword");
                p = e;
            } else {
                p = path(h, p, NULL);
            }
        } else if (c == '(' || c == '[' || c == '{') {
            emit(h, p, p + 1, "operator");
            depth++;
            p++;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth == 0 && close)
                return p; // a closer that is not ours
            emit(h, p, p + 1, "operator");
            if (depth > 0)
                depth--;
            p++;
        } else if (op_char(c)) {
            emit(h, p, p + 1, "operator");
            p++;
        } else {
            p++; // no class
        }
    }
}

// === Argument mode ===============================================================

// The declared argument a positional slot or a name selects.
static const arg_decl_t *arg_at(node_t m, int pos, const char *name, size_t name_len) {
    int slot = script_arg_slot(m.member, pos, name, name_len);
    return slot >= 0 ? &m.member->method.args[slot] : NULL;
}

static bool in_enum(const arg_decl_t *a, const char *s, const char *e) {
    if (!a || !a->enum_values)
        return false;
    for (const char *const *v = a->enum_values; *v; v++)
        if (word_is(s, e, *v))
            return true;
    return false;
}

// One argument value at p; a bare word ends where the parser's does.
static const char *arg_value(hl_t *h, const char *p, const arg_decl_t *decl) {
    char c = *p;
    if (c == '"')
        return dq_string(h, p, false);
    if (c == '\'')
        return sq_string(h, p);
    if (curly_open(p))
        return dq_string(h, p, true);
    if (c == '$')
        return binding(h, p);
    if (c == '(') {
        emit(h, p, p + 1, "operator");
        const char *q = expr_until(h, p + 1, ')');
        if (!at_end(h, q) && *q == ')') {
            emit(h, q, q + 1, "operator");
            q++;
        }
        return q;
    }
    const char *e = script_arg_end(p, h->limit, NULL);
    if (number_end(p) == e && e > p)
        emit(h, p, e, "number");
    else if (word_is(p, e, "true") || word_is(p, e, "false") || word_is(p, e, "none"))
        emit(h, p, e, "keyword");
    else if (in_enum(decl, p, e))
        emit(h, p, e, "enum");
    return e > p ? e : p + 1;
}

// Arguments of the command `m` from p to the end of the statement.
static void arguments(hl_t *h, const char *p, node_t m) {
    int pos = 0;
    for (;;) {
        p = skip_ws(h, p);
        if (at_end(h, p))
            return;
        // name=VALUE
        if (ident_start(*p)) {
            const char *e = ident_end(p);
            if (*e == '=' && e[1] != '=') {
                emit(h, p, e, "attribute");
                emit(h, e, e + 1, "operator");
                p = arg_value(h, e + 1, arg_at(m, 0, p, (size_t)(e - p)));
                continue;
            }
        }
        p = arg_value(h, p, arg_at(m, pos, NULL, 0));
        pos++;
    }
}

// === Statements ===================================================================

// An enum-typed attribute's values, for `PATH = word`.
static bool attr_enum(node_t n, const char *s, const char *e) {
    if (!n.member || n.member->kind != MK_ATTR || !n.member->attr.enum_values)
        return false;
    for (const char *const *v = n.member->attr.enum_values; *v; v++)
        if (word_is(s, e, *v))
            return true;
    return false;
}

// The right-hand side of an assignment: an enum attribute's bare value
// word is an enum, anything else an expression.
static void rhs(hl_t *h, const char *p, node_t target) {
    p = skip_ws(h, p);
    if (ident_start(*p)) {
        const char *e = ident_end(p);
        if (attr_enum(target, p, e) && at_end(h, skip_ws(h, e))) {
            emit(h, p, e, "enum");
            return;
        }
    }
    expr_until(h, p, '\0');
}

// A path where a declaration wants one (`alias`, `command`), else an
// expression.
static void path_or_expr(hl_t *h, const char *p) {
    if (!at_end(h, p) && ident_start(*p))
        path(h, p, NULL);
    else
        expr_until(h, p, '\0');
}

// `def NAME(P, …)`: the parameter list after the name.
static void def_params(hl_t *h, const char *q) {
    q = skip_ws(h, q);
    if (at_end(h, q) || *q != '(')
        return;
    emit(h, q, q + 1, "operator");
    q++;
    for (;;) {
        q = skip_ws(h, q);
        if (at_end(h, q))
            return;
        if (ident_start(*q)) {
            const char *pe = ident_end(q);
            emit(h, q, pe, "variable");
            q = pe;
        } else if (*q == ',') {
            emit(h, q, q + 1, "operator");
            q++;
        } else if (*q == ')') {
            emit(h, q, q + 1, "operator");
            return;
        } else {
            return;
        }
    }
}

// The head of a command statement, then its arguments.  A bare word may be
// a command, which stands for the method it runs.
static void command(hl_t *h, const script_stmt_t *c) {
    const char *s = c->head, *e = c->head_end;
    if (*s == '$') {
        binding(h, s);
        arguments(h, c->rest, (node_t){0});
        return;
    }
    if (ident_end(s) == e) {
        node_t cmd = {0};
        if (shell_head_resolve(s, (size_t)(e - s), &cmd, NULL, NULL, 0) == SHELL_HEAD_COMMAND) {
            emit(h, s, e, "method");
            arguments(h, c->rest, cmd);
            return;
        }
    }
    node_t n = {0};
    path(h, s, &n);
    arguments(h, c->rest, n);
}

// True when the statement's head is a reserved word (not a path).
static bool keyword_head(const script_stmt_t *c) {
    if (!c->head || ident_end(c->head) != c->head_end || c->head_end - c->head >= 16)
        return false;
    char w[16];
    memcpy(w, c->head, (size_t)(c->head_end - c->head));
    w[c->head_end - c->head] = '\0';
    return object_is_reserved_word(w);
}

// Colour one classified statement.
static void statement(hl_t *h, const script_stmt_t *c) {
    if (c->kind == SCRIPT_STMT_EMPTY)
        return;
    const char *saved = h->limit;
    h->limit = c->end;
    switch (c->kind) {
    case SCRIPT_STMT_LET:
    case SCRIPT_STMT_ALIAS:
    case SCRIPT_STMT_COMMAND_DEF:
        emit(h, c->head, c->head_end, "decl");
        emit(h, c->name, c->name_end,
             c->kind == SCRIPT_STMT_LET     ? "variable"
             : c->kind == SCRIPT_STMT_ALIAS ? "alias"
                                            : "method");
        emit(h, c->eq, c->eq + 1, "operator");
        if (c->kind == SCRIPT_STMT_LET)
            expr_until(h, c->rest, '\0');
        else
            path_or_expr(h, c->rest);
        break;
    case SCRIPT_STMT_ASSIGN:
        if (*c->head == '$') {
            binding(h, c->head);
            emit(h, c->eq, c->eq + 1, "operator");
            expr_until(h, c->rest, '\0');
        } else {
            node_t n = {0};
            path(h, c->head, &n);
            emit(h, c->eq, c->eq + 1, "operator");
            rhs(h, c->rest, n);
        }
        break;
    case SCRIPT_STMT_COMMAND:
        command(h, c);
        break;
    case SCRIPT_STMT_DEF:
        emit(h, c->head, c->head_end, "decl");
        emit(h, c->name, c->name_end, "method");
        def_params(h, c->name ? c->name_end : c->rest);
        break;
    case SCRIPT_STMT_FOR: {
        emit(h, c->head, c->head_end, "keyword");
        const char *q = c->rest;
        if (c->name) {
            emit(h, c->name, c->name_end, "variable");
            q = skip_ws(h, c->name_end);
        }
        expr_until(h, q, '\0'); // colours `in` as a keyword
        break;
    }
    case SCRIPT_STMT_INVALID:
        if (!keyword_head(c)) {
            // A path head the parser rejects (an unclosed `[`): an expression.
            expr_until(h, c->start, '\0');
            break;
        }
        // A keyword in a shape the parser rejects: its keyword, then what
        // lexes after it.
        {
            bool is_alias = word_is(c->head, c->head_end, "alias");
            bool decl = is_alias || word_is(c->head, c->head_end, "let");
            emit(h, c->head, c->head_end, decl ? "decl" : "keyword");
            emit(h, c->name, c->name_end, is_alias ? "alias" : "variable");
            if (c->eq)
                emit(h, c->eq, c->eq + 1, "operator");
            expr_until(h, c->eq ? c->eq + 1 : c->name ? c->name_end : c->rest, '\0');
        }
        break;
    case SCRIPT_STMT_EXPR:
        expr_until(h, c->start, '\0');
        break;
    default:
        // if, elif, else, while, break, continue, return, assert, include.
        emit(h, c->head, c->head_end, "keyword");
        expr_until(h, c->rest, '\0');
        break;
    }
    h->limit = saved;
}

// One line: a leading `}`, the statement (or a block form's header), its
// `{`, an inline block's statement and `}`, and a comment -- the parts the
// parser reads (script_line_split).
static void line(hl_t *h, const char *s, const char *e) {
    script_line_t ln;
    script_line_split(s, e, &ln);
    h->limit = e;
    if (ln.closer)
        emit(h, ln.closer, ln.closer + 1, "operator");
    statement(h, &ln.stmt);
    if (ln.open) {
        emit(h, ln.open, ln.open + 1, "operator");
        statement(h, &ln.body);
        if (ln.close)
            emit(h, ln.close, ln.close + 1, "operator");
    }
    if (ln.comment)
        emit(h, ln.comment, e, "comment");
}

value_t shell_highlight(const char *text) {
    hl_t h = {.base = text ? text : ""};
    const char *p = h.base;
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *e = eol ? eol : p + strlen(p);
        line(&h, p, e);
        if (!eol)
            break;
        p = eol + 1;
    }
    return val_list(h.items, h.len);
}
