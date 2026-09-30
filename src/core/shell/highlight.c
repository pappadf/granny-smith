// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// highlight.c
// shell.highlight: syntax classes for a shell line or block, computed the
// way the script parser reads it (docs/internals/core/shell/shell.md,
// "Statements" and "Two parsing modes"):
//
//   - a statement starting with a reserved word is that keyword's form
//     (`let NAME = EXPR`, `for NAME in EXPR {`, `def NAME(P, …) {`, …);
//   - a statement starting with a path is an assignment (`PATH = EXPR`),
//     a call form (`PATH(`, an expression), a bare path, or a command
//     whose arguments are in argument mode (bare words are strings);
//   - anything else is an expression.
//
// Path segments are resolved against the live tree as they are read:
// each resolved segment is an object, attribute or method; the first one
// that does not resolve, and every segment after it, is `unknown`.  In
// argument mode a bare word matching the argument's enum values is an
// `enum`; other bare words are strings and get no class.
//
// Partial or malformed input still gets spans for what lexes (an
// unterminated string runs to the end); nothing here fails.

#include "highlight.h"

#include "alias.h"
#include "commands.h"
#include "object.h"

#include <stdlib.h>
#include <string.h>

#define PREFIX_MAX 512

// === Span list ================================================================

typedef struct {
    const char *base;
    // Expression scanning stops here (an interpolation's format suffix);
    // NULL means the end of the text.
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
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r')
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
static const char *interp_close(const char *p) {
    int depth = 0;
    char quote = 0;
    for (; *p; p++) {
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
// end (after the closing quote, or the end of the text).
static const char *dq_string(hl_t *h, const char *p, bool curly) {
    const char *run = p;
    const char *q = p + (curly ? 3 : 1);
    for (;;) {
        if (!*q) {
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
            const char *close = interp_close(q + 2);
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
            if (*r == '}') {
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
    while (*q && *q != '\'') {
        if (*q == '\\' && q[1] == '\'')
            q++;
        q++;
    }
    if (*q == '\'')
        q++;
    emit(h, p, q, "string");
    return q;
}

static const char *comment(hl_t *h, const char *p) {
    const char *q = p;
    while (*q && *q != '\n')
        q++;
    emit(h, p, q, "comment");
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
        if (n.member->kind == M_ATTR)
            return "attribute";
        if (n.member->kind == M_METHOD)
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
// its segments classified against `pf`.  Stops at anything else.
static const char *path_rest(hl_t *h, const char *p, prefix_t *pf) {
    for (;;) {
        if (*p == '.' && ident_start(p[1])) {
            const char *s = p + 1, *e = ident_end(s);
            if (pf->known) {
                prefix_add(pf, s, e, true);
                emit(h, s, e, prefix_resolve(pf));
            }
            p = e;
            continue;
        }
        if (*p == '[') {
            const char *open = p;
            emit(h, p, p + 1, "operator");
            const char *q = skip_ws(p + 1);
            if (*q == '"')
                q = dq_string(h, q, false);
            else if (*q == '\'')
                q = sq_string(h, q);
            else if (*q == '$')
                q = binding(h, q);
            else if (digit(*q)) {
                const char *e = number_end(q);
                emit(h, q, e, "number");
                q = e;
            } else
                q = expr_until(h, q, ']');
            q = skip_ws(q);
            if (*q != ']')
                return q; // unterminated index: stop here
            emit(h, q, q + 1, "operator");
            // A failed index makes what follows unknown; the index itself
            // keeps its literal classes.
            if (pf->known && pf->ok) {
                prefix_add(pf, open, q + 1, false);
                (void)prefix_resolve(pf);
            }
            p = q + 1;
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
    // Where the whole path ends decides whether it is a call.
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

// An expression from p, up to `close` at depth 0 (`)`, `]`, `}`), a newline
// or `;` at depth 0, or a `{` at depth 0 when close is '{'.  Answers where
// it stopped (on the closer, not after it).
static const char *expr_until(hl_t *h, const char *p, char close) {
    int depth = 0;
    for (;;) {
        p = skip_ws(p);
        char c = *p;
        if (!c || (h->limit && p >= h->limit))
            return p;
        if (depth == 0 && (c == close || c == '\n' || c == ';'))
            return p;
        if (depth == 0 && close != '}' && close != ')' && close != ']' && c == '}')
            return p;
        if (c == '#')
            return comment(h, p);
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
            if (depth == 0)
                return p; // a closer that is not ours
            emit(h, p, p + 1, "operator");
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
    if (!m.member || m.member->kind != M_METHOD || !m.member->method.args)
        return NULL;
    int n = m.member->method.nargs;
    const arg_decl_t *a = m.member->method.args;
    if (name) {
        for (int i = 0; i < n; i++)
            if (a[i].name && strlen(a[i].name) == name_len && strncmp(a[i].name, name, name_len) == 0)
                return &a[i];
        return NULL;
    }
    if (pos < n)
        return &a[pos];
    if (n > 0 && (a[n - 1].validation_flags & OBJ_ARG_REST))
        return &a[n - 1];
    return NULL;
}

static bool in_enum(const arg_decl_t *a, const char *s, const char *e) {
    if (!a || !a->enum_values)
        return false;
    for (const char *const *v = a->enum_values; *v; v++)
        if (word_is(s, e, *v))
            return true;
    return false;
}

// One argument value at p (a bare word ends at whitespace).
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
        if (*q == ')') {
            emit(h, q, q + 1, "operator");
            q++;
        }
        return q;
    }
    const char *e = p;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' && *e != ';')
        e++;
    if (number_end(p) == e && e > p)
        emit(h, p, e, "number");
    else if (word_is(p, e, "true") || word_is(p, e, "false") || word_is(p, e, "none"))
        emit(h, p, e, "keyword");
    else if (in_enum(decl, p, e))
        emit(h, p, e, "enum");
    return e;
}

// Arguments of the command `m` from p to the end of the statement.
static const char *arguments(hl_t *h, const char *p, node_t m) {
    int pos = 0;
    for (;;) {
        p = skip_ws(p);
        if (!*p || *p == '\n' || *p == ';')
            return p;
        if (*p == '#')
            return comment(h, p);
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
    if (!n.member || n.member->kind != M_ATTR || !n.member->attr.enum_values)
        return false;
    for (const char *const *v = n.member->attr.enum_values; *v; v++)
        if (word_is(s, e, *v))
            return true;
    return false;
}

// The right-hand side of an assignment: an enum attribute's bare value
// word is an enum, anything else an expression.
static const char *rhs(hl_t *h, const char *p, node_t target) {
    p = skip_ws(p);
    if (ident_start(*p)) {
        const char *e = ident_end(p);
        const char *after = skip_ws(e);
        if (attr_enum(target, p, e) && (!*after || *after == '\n' || *after == ';' || *after == '#')) {
            emit(h, p, e, "enum");
            return after;
        }
    }
    return expr_until(h, p, '\0');
}

// One statement from p; answers where it ended (a newline, `;`, `{`, `}`
// or the end).
static const char *statement(hl_t *h, const char *p) {
    p = skip_ws(p);
    if (*p == '#')
        return comment(h, p);
    if (ident_start(*p)) {
        const char *s = p, *e = ident_end(p);
        const char *q = skip_ws(e);
        if (word_is(s, e, "let") || word_is(s, e, "alias")) {
            bool is_alias = word_is(s, e, "alias");
            if (ident_start(*q)) {
                const char *ne = ident_end(q);
                const char *eq = skip_ws(ne);
                if (*eq == '=' && eq[1] != '=') {
                    emit(h, s, e, "decl");
                    emit(h, q, ne, is_alias ? "alias" : "variable");
                    emit(h, eq, eq + 1, "operator");
                    const char *r = skip_ws(eq + 1);
                    if (is_alias && ident_start(*r))
                        return path(h, r, NULL);
                    return expr_until(h, r, '\0');
                }
            }
            if (!is_alias) {
                emit(h, s, e, "decl");
                if (ident_start(*q))
                    emit(h, q, ident_end(q), "variable");
                return ident_start(*q) ? expr_until(h, ident_end(q), '\0') : q;
            }
            // `alias` not in its statement shape: a path (e.g. alias.list).
        } else if (word_is(s, e, "def")) {
            emit(h, s, e, "decl");
            if (ident_start(*q)) {
                const char *ne = ident_end(q);
                emit(h, q, ne, "method");
                q = skip_ws(ne);
            }
            if (*q == '(') {
                emit(h, q, q + 1, "operator");
                q++;
                for (;;) {
                    q = skip_ws(q);
                    if (ident_start(*q)) {
                        const char *pe = ident_end(q);
                        emit(h, q, pe, "variable");
                        q = pe;
                    } else if (*q == ',') {
                        emit(h, q, q + 1, "operator");
                        q++;
                    } else if (*q == ')') {
                        emit(h, q, q + 1, "operator");
                        q++;
                        break;
                    } else {
                        break;
                    }
                }
            }
            return skip_ws(q);
        } else if (word_is(s, e, "if") || word_is(s, e, "elif") || word_is(s, e, "while")) {
            emit(h, s, e, "keyword");
            return expr_until(h, q, '{');
        } else if (word_is(s, e, "for")) {
            emit(h, s, e, "keyword");
            if (ident_start(*q)) {
                const char *ne = ident_end(q);
                emit(h, q, ne, "variable");
                q = skip_ws(ne);
            }
            if (ident_start(*q) && word_is(q, ident_end(q), "in")) {
                emit(h, q, ident_end(q), "keyword");
                q = ident_end(q);
            }
            return expr_until(h, q, '{');
        } else if (word_is(s, e, "return") || word_is(s, e, "assert") || word_is(s, e, "include")) {
            emit(h, s, e, "keyword");
            return expr_until(h, q, '\0');
        } else if (word_is(s, e, "else") || word_is(s, e, "break") || word_is(s, e, "continue") ||
                   word_is(s, e, "do")) {
            emit(h, s, e, "keyword");
            return q;
        } else if (word_is(s, e, "true") || word_is(s, e, "false") || word_is(s, e, "none")) {
            return expr_until(h, p, '\0');
        } else if (word_is(s, e, "command") && ident_start(*q)) {
            // `command NAME = PATH` (a contextual keyword: any other shape
            // is a path statement).
            const char *ne = ident_end(q);
            const char *eq = skip_ws(ne);
            if (*eq == '=' && eq[1] != '=') {
                emit(h, s, e, "decl");
                emit(h, q, ne, "method");
                emit(h, eq, eq + 1, "operator");
                const char *r = skip_ws(eq + 1);
                return ident_start(*r) ? path(h, r, NULL) : r;
            }
        }
        // A command word: the method it names, with its arguments.
        if (*e != '.' && *e != '[' && *e != '(') {
            node_t cmd = {0};
            if (shell_head_resolve(s, (size_t)(e - s), &cmd, NULL, NULL, 0) == SHELL_HEAD_COMMAND) {
                emit(h, s, e, "method");
                const char *r = skip_ws(e);
                if (!*r || *r == '\n' || *r == ';' || *r == '#' || *r == '{' || *r == '}')
                    return *r == '#' ? comment(h, r) : r;
                return arguments(h, r, cmd);
            }
        }
        // A path statement.
        node_t n = {0};
        const char *pe = path(h, p, &n);
        if (*pe == '(')
            return expr_until(h, pe, '\0'); // call form: an expression
        const char *r = skip_ws(pe);
        if (*r == '=' && r[1] != '=') {
            emit(h, r, r + 1, "operator");
            return rhs(h, r + 1, n);
        }
        if (!*r || *r == '\n' || *r == ';' || *r == '#' || *r == '{' || *r == '}')
            return *r == '#' ? comment(h, r) : r;
        return arguments(h, r, n);
    }
    if (*p == '$' && ident_start(p[1])) {
        const char *pe = binding(h, p);
        const char *r = skip_ws(pe);
        if (*r == '=' && r[1] != '=') {
            emit(h, r, r + 1, "operator");
            return expr_until(h, r + 1, '\0');
        }
        if (!*r || *r == '\n' || *r == ';')
            return r;
        if (*pe == '(' || op_char(*r))
            return expr_until(h, pe, '\0');
        return arguments(h, r, (node_t){0});
    }
    return expr_until(h, p, '\0');
}

value_t shell_highlight(const char *text) {
    hl_t h = {.base = text ? text : ""};
    const char *p = h.base;
    while (*p) {
        const char *q = statement(&h, p);
        // Between statements: block braces and separators.
        q = skip_ws(q);
        if (*q == '{' || *q == '}') {
            emit(&h, q, q + 1, "operator");
            q++;
        } else if (*q == ';' || *q == '\n') {
            q++;
        } else if (q == p) {
            q++; // no progress: skip a byte that does not lex
        }
        p = q;
    }
    return val_list(h.items, h.len);
}
