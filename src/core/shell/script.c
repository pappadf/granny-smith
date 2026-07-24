// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// script.c
// Shell v2 statement parser + interpreter. See script.h and
// proposal-shell-control-flow-and-functions.md §3.
//
// Pipeline: source text → lines → statement tree (blocks resolved by
// the line-position rule; inline blocks allowed for one statement) →
// interpretation. Expressions are stored as text and evaluated with
// expr_eval where they appear, so loop conditions re-test naturally.
// Two parsing modes (§3.2): command arguments parse bare words as
// strings (argument mode); every other value slot is expression mode.

#include "script.h"

#include "alias.h"
#include "expr.h"
#include "object.h"
#include "parse.h"
#include "shell_funcs.h"
#include "shell_internal.h"
#include "shell_var.h"
#include "value.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// === Statement tree =========================================================

typedef enum {
    ST_LET = 1,
    ST_ALIAS,
    ST_ASSIGN,
    ST_COMMAND,
    ST_EXPR,
    ST_IF,
    ST_WHILE,
    ST_FOR,
    ST_BREAK,
    ST_CONTINUE,
    ST_RETURN,
    ST_DEF,
    ST_ASSERT,
} stmt_kind_t;

typedef struct stmt stmt_t;

struct script_block {
    stmt_t **stmts;
    int n;
    int cap;
};

struct stmt {
    stmt_kind_t kind;
    int line; // 1-based source line for diagnostics
    char *name; // let/for/def target, alias name
    char *text; // expression / command / iterable / assert text
    char *lvalue; // assignment left-hand side (raw text)
    // if-chain: conds[i] guards blocks[i]; else_block may be NULL.
    char **conds;
    script_block_t **blocks;
    int n_conds;
    script_block_t *else_block;
    // while / for / def body.
    script_block_t *body;
    // def parameters.
    char **params;
    int n_params;
};

struct script {
    script_block_t *top;
};

// === Small helpers ==========================================================

static bool ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}
static bool ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static const char *skip_sp(const char *p) {
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

// Duplicate [s, e) trimmed of surrounding whitespace.
static char *dup_trim(const char *s, const char *e) {
    while (s < e && isspace((unsigned char)*s))
        s++;
    while (e > s && isspace((unsigned char)e[-1]))
        e--;
    size_t n = (size_t)(e - s);
    char *r = (char *)malloc(n + 1);
    if (!r)
        return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

// Match `kw` at p with an identifier boundary after it. Returns the
// position past the keyword, or NULL.
static const char *kw_match(const char *p, const char *kw) {
    size_t n = strlen(kw);
    if (strncmp(p, kw, n) != 0)
        return NULL;
    if (ident_char(p[n]))
        return NULL;
    return p + n;
}

// Quote-aware scan state used by comment stripping and brace counting.
typedef struct qstate {
    char quote; // 0, '"' or '\''
    bool esc;
} qstate_t;

static void qstate_step(qstate_t *q, char c) {
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

// Strip an unquoted trailing `# comment` and trailing whitespace, in
// place.
static void strip_comment(char *line) {
    qstate_t q = {0};
    for (char *p = line; *p; p++) {
        if (!q.quote && !q.esc && *p == '#') {
            *p = '\0';
            break;
        }
        qstate_step(&q, *p);
    }
    size_t n = strlen(line);
    while (n > 0 && isspace((unsigned char)line[n - 1]))
        line[--n] = '\0';
}

// Net brace depth delta of a line (unquoted braces only).
static int line_brace_delta(const char *line) {
    qstate_t q = {0};
    int d = 0;
    for (const char *p = line; *p; p++) {
        if (!q.quote && !q.esc) {
            if (*p == '{')
                d++;
            else if (*p == '}')
                d--;
        }
        qstate_step(&q, *p);
    }
    return d;
}

// Find the first unquoted occurrence of `c`; NULL if none.
static const char *find_unquoted(const char *line, char c) {
    qstate_t q = {0};
    for (const char *p = line; *p; p++) {
        if (!q.quote && !q.esc && *p == c)
            return p;
        qstate_step(&q, *p);
    }
    return NULL;
}

bool script_needs_continuation(const char *buf) {
    if (!buf)
        return false;
    // Sum brace deltas across lines, ignoring comments.
    int depth = 0;
    const char *p = buf;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char *line = dup_trim(p, p + n);
        if (line) {
            strip_comment(line);
            depth += line_brace_delta(line);
            free(line);
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    return depth > 0;
}

// === Parser =================================================================

typedef struct parser {
    char **lines; // stripped statement lines (comments removed)
    int *line_nos; // original 1-based line numbers
    int n_lines;
    int i; // cursor
    char err[256];
    int err_line;
    bool err_set;
} parser_t;

static void parse_error(parser_t *ps, int line_no, const char *fmt, ...) {
    if (ps->err_set)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ps->err, sizeof(ps->err), fmt, ap);
    va_end(ap);
    ps->err_line = line_no;
    ps->err_set = true;
}

static stmt_t *stmt_new(stmt_kind_t kind, int line) {
    stmt_t *st = (stmt_t *)calloc(1, sizeof(*st));
    if (st) {
        st->kind = kind;
        st->line = line;
    }
    return st;
}

static void stmt_free(stmt_t *st);

void script_block_free(script_block_t *b) {
    if (!b)
        return;
    for (int i = 0; i < b->n; i++)
        stmt_free(b->stmts[i]);
    free(b->stmts);
    free(b);
}

static void stmt_free(stmt_t *st) {
    if (!st)
        return;
    free(st->name);
    free(st->text);
    free(st->lvalue);
    for (int i = 0; i < st->n_conds; i++) {
        free(st->conds[i]);
        script_block_free(st->blocks[i]);
    }
    free(st->conds);
    free(st->blocks);
    script_block_free(st->else_block);
    script_block_free(st->body);
    for (int i = 0; i < st->n_params; i++)
        free(st->params[i]);
    free(st->params);
    free(st);
}

static script_block_t *block_new(void) {
    return (script_block_t *)calloc(1, sizeof(script_block_t));
}

static bool block_append(script_block_t *b, stmt_t *st) {
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 8;
        stmt_t **t = (stmt_t **)realloc(b->stmts, (size_t)cap * sizeof(*t));
        if (!t)
            return false;
        b->stmts = t;
        b->cap = cap;
    }
    b->stmts[b->n++] = st;
    return true;
}

static stmt_t *parse_stmt(parser_t *ps); // consumes 1+ lines
static stmt_t *parse_stmt_text(parser_t *ps, const char *text, int line_no); // single-line statement from text

// Collect statements until a line starting with '}' (which is left for
// the caller to consume) — the multi-line block body.
static script_block_t *parse_block_lines(parser_t *ps, int opened_line) {
    script_block_t *b = block_new();
    if (!b) {
        parse_error(ps, opened_line, "out of memory");
        return NULL;
    }
    while (1) {
        if (ps->i >= ps->n_lines) {
            parse_error(ps, opened_line, "missing '}' for block opened here");
            script_block_free(b);
            return NULL;
        }
        const char *line = ps->lines[ps->i];
        if (line[0] == '}')
            break;
        stmt_t *st = parse_stmt(ps);
        if (!st) {
            script_block_free(b);
            return NULL;
        }
        if (!block_append(b, st)) {
            stmt_free(st);
            script_block_free(b);
            parse_error(ps, opened_line, "out of memory");
            return NULL;
        }
    }
    return b;
}

// A block header line either ends with `{` (multi-line form) or holds a
// full inline block `... { stmt }`. Returns the body via *out_body and
// the header text (before the '{') via *out_head; for the inline form
// the single inner statement is parsed immediately.
static bool parse_block_header(parser_t *ps, const char *line, int line_no, char **out_head,
                               script_block_t **out_body) {
    *out_head = NULL;
    *out_body = NULL;
    const char *brace = find_unquoted(line, '{');
    if (!brace) {
        parse_error(ps, line_no, "expected '{' to open a block");
        return false;
    }
    const char *after = skip_sp(brace + 1);
    if (*after == '\0') {
        // Multi-line form: body is on the following lines.
        *out_head = dup_trim(line, brace);
        script_block_t *b = parse_block_lines(ps, line_no);
        if (!b) {
            free(*out_head);
            *out_head = NULL;
            return false;
        }
        // Consume the closing line; it must be exactly "}" here (the
        // if-chain parser handles `} elif` / `} else` itself before
        // calling us again).
        const char *closer = ps->lines[ps->i];
        if (strcmp(closer, "}") != 0) {
            parse_error(ps, ps->line_nos[ps->i], "unexpected text after '}'");
            free(*out_head);
            *out_head = NULL;
            script_block_free(b);
            return false;
        }
        ps->i++;
        if (b->n == 0) {
            parse_error(ps, line_no, "empty block");
            script_block_free(b);
            free(*out_head);
            *out_head = NULL;
            return false;
        }
        *out_body = b;
        return true;
    }
    // Inline form: `HEAD { STMT }` — one statement, no nested braces.
    size_t len = strlen(line);
    const char *close = line + len - 1;
    if (*close != '}') {
        parse_error(ps, line_no, "inline block must end with '}'");
        return false;
    }
    char *inner = dup_trim(brace + 1, close);
    if (!inner) {
        parse_error(ps, line_no, "out of memory");
        return false;
    }
    if (find_unquoted(inner, '{') != NULL) {
        parse_error(ps, line_no, "inline blocks cannot nest");
        free(inner);
        return false;
    }
    if (inner[0] == '\0') {
        parse_error(ps, line_no, "empty block");
        free(inner);
        return false;
    }
    stmt_t *st = parse_stmt_text(ps, inner, line_no);
    free(inner);
    if (!st)
        return false;
    script_block_t *b = block_new();
    if (!b || !block_append(b, st)) {
        stmt_free(st);
        script_block_free(b);
        parse_error(ps, line_no, "out of memory");
        return false;
    }
    *out_head = dup_trim(line, brace);
    *out_body = b;
    return true;
}

// Parse the `if` statement starting at `line` (already consumed from
// the cursor). Handles the inline form and multi-line `} elif` / `}
// else` chains.
static stmt_t *parse_if(parser_t *ps, const char *line, int line_no) {
    stmt_t *st = stmt_new(ST_IF, line_no);
    if (!st) {
        parse_error(ps, line_no, "out of memory");
        return NULL;
    }
    const char *brace = find_unquoted(line, '{');
    bool multiline = brace && *skip_sp(brace + 1) == '\0';

    // First arm: `if COND {`.
    char *head = NULL;
    script_block_t *body = NULL;
    if (!multiline) {
        if (!parse_block_header(ps, line, line_no, &head, &body)) {
            stmt_free(st);
            return NULL;
        }
        const char *cond = skip_sp(kw_match(head, "if"));
        st->conds = (char **)calloc(1, sizeof(char *));
        st->blocks = (script_block_t **)calloc(1, sizeof(script_block_t *));
        st->conds[0] = strdup(cond);
        st->blocks[0] = body;
        st->n_conds = 1;
        free(head);
        return st; // inline if: no elif/else chain
    }

    // Multi-line chain.
    const char *cursor = line;
    while (1) {
        const char *open = find_unquoted(cursor, '{');
        if (!open || *skip_sp(open + 1) != '\0') {
            parse_error(ps, line_no, "expected '{' at end of line");
            stmt_free(st);
            return NULL;
        }
        char *cond_text;
        const char *after_if = kw_match(cursor, "if");
        const char *after_elif = NULL;
        if (!after_if) {
            after_elif = kw_match(cursor, "elif");
        }
        const char *cstart = after_if ? after_if : after_elif;
        cond_text = dup_trim(cstart, open);
        if (!cond_text || !*cond_text) {
            parse_error(ps, line_no, "missing condition");
            free(cond_text);
            stmt_free(st);
            return NULL;
        }
        script_block_t *b = parse_block_lines(ps, line_no);
        if (!b) {
            free(cond_text);
            stmt_free(st);
            return NULL;
        }
        if (b->n == 0) {
            parse_error(ps, line_no, "empty block");
            free(cond_text);
            script_block_free(b);
            stmt_free(st);
            return NULL;
        }
        // Grow the chain arrays.
        char **nc = (char **)realloc(st->conds, (size_t)(st->n_conds + 1) * sizeof(char *));
        script_block_t **nb =
            (script_block_t **)realloc(st->blocks, (size_t)(st->n_conds + 1) * sizeof(script_block_t *));
        if (!nc || !nb) {
            parse_error(ps, line_no, "out of memory");
            free(cond_text);
            script_block_free(b);
            st->conds = nc ? nc : st->conds;
            st->blocks = nb ? nb : st->blocks;
            stmt_free(st);
            return NULL;
        }
        st->conds = nc;
        st->blocks = nb;
        st->conds[st->n_conds] = cond_text;
        st->blocks[st->n_conds] = b;
        st->n_conds++;

        // Closing line: `}`, `} elif COND {`, or `} else {`.
        const char *closer = ps->lines[ps->i];
        int closer_no = ps->line_nos[ps->i];
        ps->i++;
        const char *rest = skip_sp(closer + 1); // past '}'
        if (*rest == '\0')
            return st;
        if (kw_match(rest, "elif")) {
            cursor = rest;
            line_no = closer_no;
            continue;
        }
        const char *after_else = kw_match(rest, "else");
        if (after_else) {
            const char *ob = skip_sp(after_else);
            if (*ob != '{' || *skip_sp(ob + 1) != '\0') {
                parse_error(ps, closer_no, "expected '{' after else");
                stmt_free(st);
                return NULL;
            }
            script_block_t *eb = parse_block_lines(ps, closer_no);
            if (!eb) {
                stmt_free(st);
                return NULL;
            }
            if (eb->n == 0) {
                parse_error(ps, closer_no, "empty block");
                script_block_free(eb);
                stmt_free(st);
                return NULL;
            }
            const char *final_close = ps->lines[ps->i];
            if (strcmp(final_close, "}") != 0) {
                parse_error(ps, ps->line_nos[ps->i], "unexpected text after '}'");
                script_block_free(eb);
                stmt_free(st);
                return NULL;
            }
            ps->i++;
            st->else_block = eb;
            return st;
        }
        parse_error(ps, closer_no, "expected 'elif' or 'else' after '}'");
        stmt_free(st);
        return NULL;
    }
}

// `def NAME(P1, P2, …) BLOCK` — parse the header's parameter list.
static bool parse_def_params(parser_t *ps, const char *head, int line_no, char **out_name, char ***out_params,
                             int *out_n) {
    const char *p = skip_sp(kw_match(head, "def"));
    if (!ident_start(*p)) {
        parse_error(ps, line_no, "def: expected function name");
        return false;
    }
    const char *ns = p;
    while (ident_char(*p))
        p++;
    char *name = dup_trim(ns, p);
    p = skip_sp(p);
    if (*p != '(') {
        parse_error(ps, line_no, "def: expected '(' after function name");
        free(name);
        return false;
    }
    p = skip_sp(p + 1);
    char **params = NULL;
    int n = 0;
    if (*p != ')') {
        while (1) {
            if (!ident_start(*p)) {
                parse_error(ps, line_no, "def: expected parameter name");
                goto fail;
            }
            const char *s = p;
            while (ident_char(*p))
                p++;
            char *param = dup_trim(s, p);
            char **np = (char **)realloc(params, (size_t)(n + 1) * sizeof(char *));
            if (!np) {
                free(param);
                parse_error(ps, line_no, "out of memory");
                goto fail;
            }
            params = np;
            params[n++] = param;
            p = skip_sp(p);
            if (*p == ':') {
                parse_error(ps, line_no, "def: type annotations not yet supported");
                goto fail;
            }
            if (*p == ',') {
                p = skip_sp(p + 1);
                continue;
            }
            if (*p == ')')
                break;
            parse_error(ps, line_no, "def: expected ',' or ')'");
            goto fail;
        }
    }
    p = skip_sp(p + 1);
    if (*p != '\0') {
        parse_error(ps, line_no, "def: unexpected text after ')'");
        goto fail;
    }
    *out_name = name;
    *out_params = params;
    *out_n = n;
    return true;
fail:
    for (int i = 0; i < n; i++)
        free(params[i]);
    free(params);
    free(name);
    return false;
}

// Classify + parse one single-line statement (no block forms) from raw
// text. Used for inline block bodies and by parse_stmt for the simple
// statement kinds.
static stmt_t *parse_stmt_text(parser_t *ps, const char *text, int line_no) {
    const char *p = skip_sp(text);
    const char *after;

    if ((after = kw_match(p, "break"))) {
        if (*skip_sp(after) != '\0') {
            parse_error(ps, line_no, "break takes no arguments");
            return NULL;
        }
        return stmt_new(ST_BREAK, line_no);
    }
    if ((after = kw_match(p, "continue"))) {
        if (*skip_sp(after) != '\0') {
            parse_error(ps, line_no, "continue takes no arguments");
            return NULL;
        }
        return stmt_new(ST_CONTINUE, line_no);
    }
    if ((after = kw_match(p, "return"))) {
        stmt_t *st = stmt_new(ST_RETURN, line_no);
        st->text = dup_trim(after, after + strlen(after));
        return st;
    }
    if ((after = kw_match(p, "assert"))) {
        const char *body = skip_sp(after);
        if (*body == '\0') {
            parse_error(ps, line_no, "assert: missing predicate");
            return NULL;
        }
        stmt_t *st = stmt_new(ST_ASSERT, line_no);
        st->text = strdup(body);
        return st;
    }
    if ((after = kw_match(p, "let"))) {
        const char *q = skip_sp(after);
        if (!ident_start(*q)) {
            parse_error(ps, line_no, "let: expected a name");
            return NULL;
        }
        const char *ns = q;
        while (ident_char(*q))
            q++;
        char *name = dup_trim(ns, q);
        q = skip_sp(q);
        if (*q != '=' || q[1] == '=') {
            parse_error(ps, line_no, "let: expected '=' after name");
            free(name);
            return NULL;
        }
        q = skip_sp(q + 1);
        if (*q == '\0') {
            parse_error(ps, line_no, "let: missing expression");
            free(name);
            return NULL;
        }
        stmt_t *st = stmt_new(ST_LET, line_no);
        st->name = name;
        st->text = strdup(q);
        return st;
    }
    // `alias NAME = PATH` — only the exact statement shape is the
    // keyword; anything else (e.g. a path starting with `alias.`) falls
    // through to command parsing.
    if ((after = kw_match(p, "alias"))) {
        const char *q = skip_sp(after);
        if (ident_start(*q)) {
            const char *ns = q;
            const char *scan = q;
            while (ident_char(*scan))
                scan++;
            const char *eq = skip_sp(scan);
            if (*eq == '=' && eq[1] != '=') {
                char *name = dup_trim(ns, scan);
                const char *path = skip_sp(eq + 1);
                if (*path == '\0') {
                    parse_error(ps, line_no, "alias: missing path");
                    free(name);
                    return NULL;
                }
                stmt_t *st = stmt_new(ST_ALIAS, line_no);
                st->name = name;
                st->text = strdup(path);
                return st;
            }
        }
        parse_error(ps, line_no, "alias: expected `alias NAME = PATH`");
        return NULL;
    }
    if (kw_match(p, "elif") || kw_match(p, "else")) {
        parse_error(ps, line_no, "'%s' without a preceding if-block", kw_match(p, "elif") ? "elif" : "else");
        return NULL;
    }

    // Path-or-binding statement head: decide assignment vs command vs
    // bare expression by what follows the leading path token.
    if (*p == '$' || ident_start(*p)) {
        // Scan the path token: `$`? ident ( '.'seg | '[' … ']' )* —
        // bracket contents skipped quote-aware.
        const char *q = p;
        bool is_binding = (*q == '$');
        if (is_binding)
            q++;
        if (ident_start(*q)) {
            while (ident_char(*q))
                q++;
            const char *ident_end = q;
            while (1) {
                if (*q == '.' && (ident_char(q[1]))) {
                    q++;
                    while (ident_char(*q))
                        q++;
                } else if (*q == '[') {
                    qstate_t qs = {0};
                    int depth = 0;
                    const char *r = q;
                    for (; *r; r++) {
                        if (!qs.quote && !qs.esc) {
                            if (*r == '[')
                                depth++;
                            else if (*r == ']') {
                                depth--;
                                if (depth == 0) {
                                    r++;
                                    break;
                                }
                            }
                        }
                        qstate_step(&qs, *r);
                    }
                    if (depth != 0) {
                        parse_error(ps, line_no, "unbalanced '['");
                        return NULL;
                    }
                    q = r;
                } else {
                    break;
                }
            }
            const char *rest = skip_sp(q);
            if (*rest == '=' && rest[1] != '=') {
                // Assignment: LVALUE = EXPR.
                const char *rhs = skip_sp(rest + 1);
                if (*rhs == '\0') {
                    parse_error(ps, line_no, "assignment: missing right-hand side");
                    return NULL;
                }
                stmt_t *st = stmt_new(ST_ASSIGN, line_no);
                st->lvalue = dup_trim(p, q);
                st->text = strdup(rhs);
                return st;
            }
            if (*rest == '\0') {
                if (is_binding && q == ident_end) {
                    // Plain `$name` → binding read, expression statement.
                    stmt_t *st = stmt_new(ST_EXPR, line_no);
                    st->text = strdup(p);
                    return st;
                }
                // Bare path (or `$binding.path`) → zero-argument command:
                // methods are called, attributes/objects are read.
                stmt_t *st = stmt_new(ST_COMMAND, line_no);
                st->text = strdup(p);
                return st;
            }
            if (*q == '(') {
                // `(` immediately after the path (no space): call form
                // as a statement → expression statement. (A space then
                // `(` is a command with a parenthesized argument.)
                stmt_t *st = stmt_new(ST_EXPR, line_no);
                st->text = strdup(p);
                return st;
            }
            // Command with arguments.
            stmt_t *st = stmt_new(ST_COMMAND, line_no);
            st->text = strdup(p);
            return st;
        }
        // `$` not followed by identifier — let the expression parser
        // produce its error.
    }

    // Everything else — numbers, strings, parens, operators — is an
    // expression statement.
    stmt_t *st = stmt_new(ST_EXPR, line_no);
    st->text = strdup(p);
    return st;
}

// Parse one statement starting at the parser cursor; consumes the
// header line plus any block lines.
static stmt_t *parse_stmt(parser_t *ps) {
    const char *line = ps->lines[ps->i];
    int line_no = ps->line_nos[ps->i];
    ps->i++;

    const char *p = skip_sp(line);
    if (kw_match(p, "if"))
        return parse_if(ps, p, line_no);
    if (kw_match(p, "while")) {
        char *head = NULL;
        script_block_t *body = NULL;
        if (!parse_block_header(ps, p, line_no, &head, &body))
            return NULL;
        const char *cond = skip_sp(kw_match(head, "while"));
        if (*cond == '\0') {
            parse_error(ps, line_no, "while: missing condition");
            free(head);
            script_block_free(body);
            return NULL;
        }
        stmt_t *st = stmt_new(ST_WHILE, line_no);
        st->text = strdup(cond);
        st->body = body;
        free(head);
        return st;
    }
    if (kw_match(p, "for")) {
        char *head = NULL;
        script_block_t *body = NULL;
        if (!parse_block_header(ps, p, line_no, &head, &body))
            return NULL;
        // `for NAME in EXPR`
        const char *q = skip_sp(kw_match(head, "for"));
        if (!ident_start(*q)) {
            parse_error(ps, line_no, "for: expected a loop variable name");
            free(head);
            script_block_free(body);
            return NULL;
        }
        const char *ns = q;
        while (ident_char(*q))
            q++;
        char *name = dup_trim(ns, q);
        const char *after_in = kw_match(skip_sp(q), "in");
        if (!after_in) {
            parse_error(ps, line_no, "for: expected 'in'");
            free(name);
            free(head);
            script_block_free(body);
            return NULL;
        }
        const char *iter = skip_sp(after_in);
        if (*iter == '\0') {
            parse_error(ps, line_no, "for: missing iterable expression");
            free(name);
            free(head);
            script_block_free(body);
            return NULL;
        }
        stmt_t *st = stmt_new(ST_FOR, line_no);
        st->name = name;
        st->text = strdup(iter);
        st->body = body;
        free(head);
        return st;
    }
    if (kw_match(p, "def")) {
        char *head = NULL;
        script_block_t *body = NULL;
        if (!parse_block_header(ps, p, line_no, &head, &body))
            return NULL;
        char *name = NULL;
        char **params = NULL;
        int n_params = 0;
        if (!parse_def_params(ps, head, line_no, &name, &params, &n_params)) {
            free(head);
            script_block_free(body);
            return NULL;
        }
        stmt_t *st = stmt_new(ST_DEF, line_no);
        st->name = name;
        st->params = params;
        st->n_params = n_params;
        st->body = body;
        st->text = strdup(head);
        free(head);
        return st;
    }
    return parse_stmt_text(ps, line, line_no);
}

script_t *script_parse(const char *src, char *err_buf, size_t err_size) {
    if (!src)
        src = "";
    parser_t ps = {0};

    // Split into stripped statement lines, remembering line numbers.
    int cap = 64;
    ps.lines = (char **)malloc((size_t)cap * sizeof(char *));
    ps.line_nos = (int *)malloc((size_t)cap * sizeof(int));
    if (!ps.lines || !ps.line_nos) {
        free(ps.lines);
        free(ps.line_nos);
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "out of memory");
        return NULL;
    }
    int line_no = 0;
    const char *p = src;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        line_no++;
        char *line = dup_trim(p, p + n);
        if (line) {
            strip_comment(line);
            if (line[0] != '\0') {
                if (ps.n_lines == cap) {
                    cap *= 2;
                    char **nl = (char **)realloc(ps.lines, (size_t)cap * sizeof(char *));
                    int *nn = (int *)realloc(ps.line_nos, (size_t)cap * sizeof(int));
                    if (!nl || !nn) {
                        ps.lines = nl ? nl : ps.lines;
                        ps.line_nos = nn ? nn : ps.line_nos;
                        parse_error(&ps, line_no, "out of memory");
                        free(line);
                        break;
                    }
                    ps.lines = nl;
                    ps.line_nos = nn;
                }
                ps.lines[ps.n_lines] = line;
                ps.line_nos[ps.n_lines] = line_no;
                ps.n_lines++;
            } else {
                free(line);
            }
        }
        if (!eol)
            break;
        p = eol + 1;
    }

    script_t *s = NULL;
    if (!ps.err_set) {
        script_block_t *top = block_new();
        while (!ps.err_set && top && ps.i < ps.n_lines) {
            if (ps.lines[ps.i][0] == '}') {
                parse_error(&ps, ps.line_nos[ps.i], "'}' without an open block");
                break;
            }
            stmt_t *st = parse_stmt(&ps);
            if (!st)
                break;
            if (!block_append(top, st)) {
                stmt_free(st);
                parse_error(&ps, 0, "out of memory");
                break;
            }
        }
        if (!ps.err_set && top) {
            s = (script_t *)calloc(1, sizeof(*s));
            if (s)
                s->top = top;
            else
                script_block_free(top);
        } else {
            script_block_free(top);
        }
    }

    for (int i = 0; i < ps.n_lines; i++)
        free(ps.lines[i]);
    free(ps.lines);
    free(ps.line_nos);

    if (!s && err_buf && err_size) {
        if (ps.err_set)
            snprintf(err_buf, err_size, "line %d: %s", ps.err_line, ps.err);
        else
            snprintf(err_buf, err_size, "out of memory");
    }
    return s;
}

void script_free(script_t *s) {
    if (!s)
        return;
    script_block_free(s->top);
    free(s);
}

// === Interpreter ============================================================

typedef enum {
    SIG_NONE = 0,
    SIG_BREAK,
    SIG_CONTINUE,
    SIG_RETURN,
    SIG_ERROR,
    SIG_QUIT, // pump hook requested a clean stop (quit)
} exec_sig_t;

typedef struct exec_ctx {
    bool interactive; // REPL prints non-V_NONE results
    int loop_depth;
    bool in_function;
    exec_sig_t sig;
    value_t ret; // SIG_RETURN payload
} exec_ctx_t;

static script_pump_fn g_pump_hook = NULL;
static volatile bool g_interrupt = false;

void script_set_pump_hook(script_pump_fn fn) {
    g_pump_hook = fn;
}

void script_interrupt(void) {
    g_interrupt = true;
}

static value_t script_binding_cb(void *ud, const char *name) {
    (void)ud;
    return shell_binding_get(name);
}

void script_expr_ctx(expr_ctx_t *out) {
    out->root = object_root();
    out->binding = script_binding_cb;
    out->binding_ud = NULL;
}

// Report a statement-level error and set the abort signal.
static void exec_error(exec_ctx_t *cx, int line, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "line %d: %s\n", line, buf);
    cx->sig = SIG_ERROR;
}

static void exec_block(script_block_t *b, exec_ctx_t *cx);
static void exec_stmt(stmt_t *st, exec_ctx_t *cx);

// --- path helpers -----------------------------------------------------------

// Scan `('.' seg | '[' EXPR ']')*` at *p, appending to out (bracket
// expressions evaluated to integers). Returns false with *errv set.
static bool scan_path_continuation(const char **p, const expr_ctx_t *ectx, char *out, size_t out_size, value_t *errv) {
    size_t pi = strlen(out);
    while (1) {
        if ((*p)[0] == '.' && (ident_char((*p)[1]))) {
            const char *q = *p + 1;
            const char *s = q;
            while (ident_char(*q))
                q++;
            int n = snprintf(out + pi, out_size - pi, ".%.*s", (int)(q - s), s);
            if (n < 0 || (size_t)n >= out_size - pi) {
                *errv = val_err("path too long");
                return false;
            }
            pi += (size_t)n;
            *p = q;
        } else if ((*p)[0] == '[') {
            const char *q = *p + 1;
            value_t idx = expr_eval_at(&q, ectx);
            if (val_is_error(&idx)) {
                *errv = idx;
                return false;
            }
            q = skip_sp(q);
            if (*q != ']') {
                value_free(&idx);
                *errv = val_err("expected ']'");
                return false;
            }
            q++;
            // Integer index (indexed child / list slot) or string index
            // (map key, emitted as a `["key"]` segment).
            int n;
            if (idx.kind == V_STRING) {
                const char *k = idx.s ? idx.s : "";
                if (strpbrk(k, "\"\\")) {
                    value_free(&idx);
                    *errv = val_err("map key may not contain '\"' or '\\'");
                    return false;
                }
                n = snprintf(out + pi, out_size - pi, "[\"%s\"]", k);
            } else {
                bool ok = false;
                int64_t iv = val_as_i64(&idx, &ok);
                if (!ok) {
                    value_free(&idx);
                    *errv = val_err("index must be numeric or a string key");
                    return false;
                }
                n = snprintf(out + pi, out_size - pi, "[%lld]", (long long)iv);
            }
            value_free(&idx);
            if (n < 0 || (size_t)n >= out_size - pi) {
                *errv = val_err("path too long");
                return false;
            }
            pi += (size_t)n;
            *p = q;
        } else {
            return true;
        }
    }
}

// Resolve a command/lvalue head at *p into a node. Handles both bare
// object paths and `$binding` heads (V_REF re-resolution, V_OBJECT
// relative resolution). On success advances *p past the path text.
// When out_path/out_base are non-NULL they receive the normalized path
// text and the object it resolves against even when node resolution
// fails, so exec_command can retry the head as a structured-value read
// (map/list access into an attribute or method result).
static bool resolve_path_head_ex(const char **p, const expr_ctx_t *ectx, node_t *out_node, value_t *errv,
                                 char *out_path, size_t out_path_size, struct object **out_base) {
    char path[512] = "";
    if (out_path && out_path_size)
        out_path[0] = '\0';
    if (out_base)
        *out_base = NULL;
    if (**p == '$') {
        (*p)++;
        char name[64];
        size_t i = 0;
        if (!ident_start(**p)) {
            *errv = val_err("expected binding name after '$'");
            return false;
        }
        while (ident_char(**p)) {
            if (i + 1 < sizeof(name))
                name[i++] = **p;
            (*p)++;
        }
        name[i] = '\0';
        value_t base = shell_binding_get(name);
        if (val_is_error(&base)) {
            *errv = base;
            return false;
        }
        char sub[256] = "";
        const char *q = *p;
        if (!scan_path_continuation(&q, ectx, sub, sizeof(sub), errv)) {
            value_free(&base);
            return false;
        }
        *p = q;
        if (base.kind == V_REF) {
            snprintf(path, sizeof(path), "%s%s", base.ref ? base.ref : "", sub);
            value_free(&base);
            if (out_path && out_path_size)
                snprintf(out_path, out_path_size, "%s", path);
            if (out_base)
                *out_base = object_root();
            node_t n = object_resolve(object_root(), path);
            if (!node_valid(n)) {
                *errv = val_err("'$%s' → '%s' did not resolve", name, path);
                return false;
            }
            *out_node = n;
            return true;
        }
        if (base.kind == V_OBJECT) {
            struct object *obj = base.obj;
            value_free(&base);
            if (!obj) {
                *errv = val_err("'$%s' holds a destroyed object", name);
                return false;
            }
            const char *rel = sub[0] == '.' ? sub + 1 : sub;
            if (out_path && out_path_size)
                snprintf(out_path, out_path_size, "%s", rel);
            if (out_base)
                *out_base = obj;
            node_t n = object_resolve(obj, rel);
            if (!node_valid(n)) {
                *errv = val_err("'$%s%s' did not resolve", name, sub);
                return false;
            }
            *out_node = n;
            return true;
        }
        value_free(&base);
        *errv = val_err("'$%s' is not an object or reference", name);
        return false;
    }
    // Bare path: leading identifier then continuation.
    if (!ident_start(**p)) {
        *errv = val_err("expected a path");
        return false;
    }
    size_t i = 0;
    while (ident_char(**p)) {
        if (i + 1 < sizeof(path))
            path[i++] = **p;
        (*p)++;
    }
    path[i] = '\0';
    if (!scan_path_continuation(p, ectx, path, sizeof(path), errv))
        return false;
    if (out_path && out_path_size)
        snprintf(out_path, out_path_size, "%s", path);
    if (out_base)
        *out_base = object_root();
    node_t n = object_resolve(object_root(), path);
    if (!node_valid(n)) {
        *errv = val_err("unknown command or path '%s'", path);
        return false;
    }
    *out_node = n;
    return true;
}

// Back-compat wrapper: resolve without exposing the normalized path.
static bool resolve_path_head(const char **p, const expr_ctx_t *ectx, node_t *out_node, value_t *errv) {
    return resolve_path_head_ex(p, ectx, out_node, errv, NULL, 0, NULL);
}

// --- argument mode ----------------------------------------------------------

#define CMD_MAX_ARGS_V2 16

// Extract a bare word token: maximal run up to unescaped whitespace.
// Backslash escapes the next character. Returns malloc'd text.
static char *scan_bare_word(const char **p) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    const char *q = *p;
    while (*q && !isspace((unsigned char)*q)) {
        char c = *q;
        if (c == '\\' && q[1]) {
            c = q[1];
            q += 2;
        } else {
            q++;
        }
        if (len + 2 > cap) {
            size_t nc = cap ? cap * 2 : 32;
            char *nb = (char *)realloc(buf, nc);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            cap = nc;
        }
        buf[len++] = c;
        buf[len] = '\0';
    }
    *p = q;
    return buf ? buf : strdup("");
}

// Parse one argument-mode value at *p (§3.2). Returns V_ERROR on
// failure. Advances *p. `raw_template` marks a template-typed slot
// (§6.3): a double-quoted value is captured as its raw body — no escape
// decoding, no interpolation — for the subsystem to evaluate at fire
// time.
static value_t parse_arg_value(const char **p, const expr_ctx_t *ectx, bool raw_template) {
    const char *q = *p;
    // Interpolating string (or raw capture for template slots).
    if (*q == '"') {
        if (raw_template) {
            const char *body = q + 1;
            q = expr_scan_dq_body(body);
            if (!q)
                return val_err("unterminated string literal");
            char *copy = (char *)malloc((size_t)(q - body) + 1);
            if (!copy)
                return val_err("oom");
            memcpy(copy, body, (size_t)(q - body));
            copy[q - body] = '\0';
            *p = q + 1;
            value_t v = val_str(copy);
            free(copy);
            return v;
        }
        value_t v = expr_parse_dq_string(&q, ectx);
        *p = q;
        return v;
    }
    // Curly-quote paste tolerance: “…” behaves like "…".
    if ((unsigned char)q[0] == 0xE2 && (unsigned char)q[1] == 0x80 && (unsigned char)q[2] == 0x9C) {
        q += 3;
        const char *body = q;
        while (*q && !((unsigned char)q[0] == 0xE2 && (unsigned char)q[1] == 0x80 && (unsigned char)q[2] == 0x9D))
            q++;
        if (!*q)
            return val_err("unterminated curly-quoted string");
        char *copy = dup_trim(body, q);
        q += 3;
        *p = q;
        if (!copy)
            return val_err("oom");
        value_t v = expr_interpolate_body(copy, ectx);
        free(copy);
        return v;
    }
    // Raw string.
    if (*q == '\'') {
        q++;
        char *buf = NULL;
        size_t len = 0, cap = 0;
        while (*q && *q != '\'') {
            char c = *q;
            if (c == '\\' && q[1] == '\'') {
                c = '\'';
                q += 2;
            } else {
                q++;
            }
            if (len + 2 > cap) {
                size_t nc = cap ? cap * 2 : 32;
                char *nb = (char *)realloc(buf, nc);
                if (!nb) {
                    free(buf);
                    return val_err("oom");
                }
                buf = nb;
                cap = nc;
            }
            buf[len++] = c;
            buf[len] = '\0';
        }
        if (*q != '\'') {
            free(buf);
            return val_err("unterminated raw string");
        }
        q++;
        *p = q;
        value_t v = val_str(buf ? buf : "");
        free(buf);
        return v;
    }
    // Parenthesised expression.
    if (*q == '(') {
        q++;
        value_t v = expr_eval_at(&q, ectx);
        if (val_is_error(&v)) {
            *p = q;
            return v;
        }
        q = skip_sp(q);
        if (*q != ')') {
            value_free(&v);
            return val_err("expected ')' after expression argument");
        }
        q++;
        *p = q;
        return v;
    }
    // Binding path (may continue through `.`/`[…]`).
    if (*q == '$') {
        value_t v = expr_eval_at(&q, ectx);
        *p = q;
        return v;
    }
    // Bare word: keyword literals, numbers, else string.
    char *word = scan_bare_word(&q);
    *p = q;
    if (!word)
        return val_err("oom");
    value_t v;
    if (strcmp(word, "true") == 0)
        v = val_bool(true);
    else if (strcmp(word, "false") == 0)
        v = val_bool(false);
    else if (strcmp(word, "none") == 0)
        v = val_none();
    else if (word[0] == '+' || word[0] == '-' || isdigit((unsigned char)word[0])) {
        v = parse_literal_full(word, NULL, 0);
        if (val_is_error(&v) || v.kind == V_STRING) {
            // Not a clean numeric literal — the whole word is a string.
            value_free(&v);
            v = val_str(word);
        }
    } else {
        v = val_str(word);
    }
    free(word);
    return v;
}

// Parse the argument tail of a command (positional then named), then
// dispatch. `fn` selects a user function instead of a tree node.
static value_t exec_command_tail(const char *p, const expr_ctx_t *ectx, node_t node, script_func_t *fn) {
    value_t vals[CMD_MAX_ARGS_V2];
    char named_names[CMD_MAX_ARGS_V2][64];
    named_arg_t named[CMD_MAX_ARGS_V2];
    int pos_n = 0, named_n = 0, total = 0;
    value_t result = val_none();

    while (1) {
        p = skip_sp(p);
        if (*p == '\0')
            break;
        if (total >= CMD_MAX_ARGS_V2) {
            result = val_err("too many arguments (max %d)", CMD_MAX_ARGS_V2);
            goto out;
        }
        // Named argument: IDENT '=' VALUE, no spaces at '='.
        const char *name = NULL;
        if (ident_start(*p)) {
            const char *q = p;
            while (ident_char(*q))
                q++;
            if (*q == '=' && q[1] != '=' && (size_t)(q - p) < sizeof(named_names[0])) {
                memcpy(named_names[named_n], p, (size_t)(q - p));
                named_names[named_n][q - p] = '\0';
                name = named_names[named_n];
                p = q + 1;
                if (*p == '\0' || isspace((unsigned char)*p)) {
                    result = val_err("empty value for argument '%s' (omit the argument instead)", name);
                    goto out;
                }
            }
        }
        // Template slot? Look up the declared argument this value will
        // land in; template-typed strings are captured raw (§6.3).
        bool raw_template = false;
        if (node.member && node.member->kind == M_METHOD && node.member->method.args) {
            const arg_decl_t *args = node.member->method.args;
            int nargs = node.member->method.nargs;
            const arg_decl_t *decl = NULL;
            if (name) {
                for (int i = 0; i < nargs; i++) {
                    if (args[i].name && strcmp(args[i].name, name) == 0) {
                        decl = &args[i];
                        break;
                    }
                }
            } else if (pos_n < nargs) {
                decl = &args[pos_n];
            }
            raw_template = decl && (decl->validation_flags & OBJ_ARG_TEMPLATE);
        }
        value_t v = parse_arg_value(&p, ectx, raw_template);
        if (val_is_error(&v)) {
            result = v;
            goto out;
        }
        vals[total] = v;
        if (name) {
            named[named_n].name = name;
            named[named_n].value = v;
            named_n++;
        } else {
            if (named_n > 0) {
                result = val_err("positional argument after named argument");
                total++;
                goto out;
            }
            pos_n++;
        }
        total++;
    }

    if (fn) {
        result = shell_func_call(fn, pos_n, vals, named_n, named);
    } else if (node.member && node.member->kind == M_METHOD) {
        if (named_n > 0) {
            value_t bound[OBJ_BIND_MAX_ARGS];
            int bound_n = 0;
            value_t err = node_bind_args(node, pos_n, vals, named_n, named, bound, &bound_n);
            if (val_is_error(&err))
                result = err;
            else
                result = node_call(node, bound_n, bound);
        } else {
            result = node_call(node, pos_n, vals);
        }
    } else if (total == 0) {
        // Bare non-method path: attribute / object / collection read.
        result = node_get(node);
    } else {
        result = val_err("path is not a method (cannot take arguments)");
    }

out:
    for (int i = 0; i < total; i++)
        value_free(&vals[i]);
    return result;
}

static void exec_command(stmt_t *st, exec_ctx_t *cx) {
    expr_ctx_t ectx;
    script_expr_ctx(&ectx);
    const char *p = st->text;

    node_t node = {0};
    script_func_t *fn = NULL;
    value_t errv = val_none();
    char norm_path[512] = "";
    struct object *path_base = NULL;

    // Try the object tree first; fall back to the user-function
    // registry for single-identifier heads.
    const char *head = p;
    if (!resolve_path_head_ex(&p, &ectx, &node, &errv, norm_path, sizeof(norm_path), &path_base)) {
        // Single bare identifier → user function?
        const char *q = head;
        if (ident_start(*q)) {
            const char *s = q;
            while (ident_char(*q))
                q++;
            char name[64];
            size_t n = (size_t)(q - s) < sizeof(name) - 1 ? (size_t)(q - s) : sizeof(name) - 1;
            memcpy(name, s, n);
            name[n] = '\0';
            fn = shell_func_find(name);
            if (fn) {
                value_free(&errv);
                p = q;
            }
        }
        // Bare read into a structured value: the head may address a map
        // key / list slot inside an attribute result
        // (`machine.config.vroms[0].card_id`). Only for argument-less
        // heads — values cannot take command arguments.
        if (!fn && norm_path[0] && path_base && *skip_sp(p) == '\0') {
            value_t v = expr_object_path_read(path_base, norm_path);
            if (!val_is_error(&v)) {
                value_free(&errv);
                if (cx->interactive)
                    shell_print_value(&v);
                value_free(&v);
                return;
            }
            // A specific map/list access error ("no key 'x' in map") beats
            // the generic resolver message; the generic "path ... did not
            // resolve" falls through to the original command error.
            if (v.err && strncmp(v.err, "path '", 6) != 0) {
                value_free(&errv);
                exec_error(cx, st->line, "%s", v.err);
                value_free(&v);
                return;
            }
            value_free(&v);
        }
        if (!fn) {
            exec_error(cx, st->line, "%s", errv.err ? errv.err : "unknown command");
            value_free(&errv);
            return;
        }
    }

    value_t result = exec_command_tail(p, &ectx, node, fn);
    if (val_is_error(&result)) {
        exec_error(cx, st->line, "%s", result.err ? result.err : "command failed");
        value_free(&result);
        return;
    }
    if (cx->interactive)
        shell_print_value(&result);
    value_free(&result);
}

// --- assignment -------------------------------------------------------------

static void exec_assign(stmt_t *st, exec_ctx_t *cx) {
    expr_ctx_t ectx;
    script_expr_ctx(&ectx);

    value_t rhs = expr_eval(st->text, &ectx);
    if (val_is_error(&rhs)) {
        exec_error(cx, st->line, "%s", rhs.err ? rhs.err : "bad expression");
        value_free(&rhs);
        return;
    }

    const char *p = st->lvalue;
    if (*p == '$') {
        p++;
        char name[64];
        size_t i = 0;
        while (ident_char(*p)) {
            if (i + 1 < sizeof(name))
                name[i++] = *p;
            p++;
        }
        name[i] = '\0';
        bool has_sub = (*p != '\0');
        if (!has_sub) {
            const value_t *cur = NULL;
            const char *alias_path = NULL;
            shell_binding_kind_t k = shell_binding_classify(name, &cur, &alias_path);
            if (k == SHELL_BINDING_NONE) {
                exec_error(cx, st->line, "no binding '$%s' — declare it with `let %s = …`", name, name);
                value_free(&rhs);
                return;
            }
            const char *ref_path = NULL;
            if (k == SHELL_BINDING_VALUE && cur && cur->kind == V_REF)
                ref_path = cur->ref;
            else if (k == SHELL_BINDING_ALIAS)
                ref_path = alias_path;
            if (ref_path) {
                // Write-through reference binding.
                node_t n = object_resolve(object_root(), ref_path);
                if (!node_valid(n)) {
                    exec_error(cx, st->line, "'$%s' → '%s' did not resolve", name, ref_path);
                    value_free(&rhs);
                    return;
                }
                value_t r = node_set(n, rhs);
                if (val_is_error(&r)) {
                    exec_error(cx, st->line, "set $%s: %s", name, r.err ? r.err : "failed");
                    value_free(&r);
                    return;
                }
                value_free(&r);
                return;
            }
            shell_binding_mutate(name, rhs);
            return;
        }
        // `$name.path = …` — write through the binding.
        value_t base = shell_binding_get(name);
        if (val_is_error(&base)) {
            exec_error(cx, st->line, "%s", base.err);
            value_free(&base);
            value_free(&rhs);
            return;
        }
        value_t errv = val_none();
        char sub[256] = "";
        const char *q = p;
        if (!scan_path_continuation(&q, &ectx, sub, sizeof(sub), &errv)) {
            exec_error(cx, st->line, "%s", errv.err ? errv.err : "bad path");
            value_free(&errv);
            value_free(&base);
            value_free(&rhs);
            return;
        }
        node_t n = {0};
        if (base.kind == V_REF) {
            char full[512];
            snprintf(full, sizeof(full), "%s%s", base.ref ? base.ref : "", sub);
            n = object_resolve(object_root(), full);
        } else if (base.kind == V_OBJECT && base.obj) {
            n = object_resolve(base.obj, sub[0] == '.' ? sub + 1 : sub);
        }
        value_free(&base);
        if (!node_valid(n)) {
            exec_error(cx, st->line, "'$%s%s' did not resolve", name, sub);
            value_free(&rhs);
            return;
        }
        value_t r = node_set(n, rhs);
        if (val_is_error(&r)) {
            exec_error(cx, st->line, "set $%s%s: %s", name, sub, r.err ? r.err : "failed");
            value_free(&r);
            return;
        }
        value_free(&r);
        return;
    }

    // Attribute write: PATH = EXPR.
    value_t errv = val_none();
    node_t n = {0};
    const char *q = p;
    if (!resolve_path_head(&q, &ectx, &n, &errv)) {
        exec_error(cx, st->line, "%s", errv.err ? errv.err : "bad path");
        value_free(&errv);
        value_free(&rhs);
        return;
    }
    if (!n.member || n.member->kind != M_ATTR) {
        exec_error(cx, st->line, "'%s' is not a settable attribute", st->lvalue);
        value_free(&rhs);
        return;
    }
    value_t r = node_set(n, rhs);
    if (val_is_error(&r)) {
        exec_error(cx, st->line, "set %s: %s", st->lvalue, r.err ? r.err : "failed");
        value_free(&r);
        return;
    }
    value_free(&r);
}

// --- control flow -----------------------------------------------------------

static void exec_if(stmt_t *st, exec_ctx_t *cx) {
    expr_ctx_t ectx;
    script_expr_ctx(&ectx);
    for (int i = 0; i < st->n_conds; i++) {
        value_t c = expr_eval(st->conds[i], &ectx);
        if (val_is_error(&c)) {
            exec_error(cx, st->line, "%s", c.err ? c.err : "bad condition");
            value_free(&c);
            return;
        }
        bool t = val_as_bool(&c);
        value_free(&c);
        if (t) {
            exec_block(st->blocks[i], cx);
            return;
        }
    }
    if (st->else_block)
        exec_block(st->else_block, cx);
}

static void exec_while(stmt_t *st, exec_ctx_t *cx) {
    expr_ctx_t ectx;
    script_expr_ctx(&ectx);
    while (1) {
        if (g_interrupt) {
            g_interrupt = false;
            exec_error(cx, st->line, "interrupted");
            return;
        }
        value_t c = expr_eval(st->text, &ectx);
        if (val_is_error(&c)) {
            exec_error(cx, st->line, "%s", c.err ? c.err : "bad condition");
            value_free(&c);
            return;
        }
        bool t = val_as_bool(&c);
        value_free(&c);
        if (!t)
            return;
        cx->loop_depth++;
        exec_block(st->body, cx);
        cx->loop_depth--;
        if (cx->sig == SIG_BREAK) {
            cx->sig = SIG_NONE;
            return;
        }
        if (cx->sig == SIG_CONTINUE)
            cx->sig = SIG_NONE;
        if (cx->sig != SIG_NONE)
            return;
    }
}

static void exec_for(stmt_t *st, exec_ctx_t *cx) {
    expr_ctx_t ectx;
    script_expr_ctx(&ectx);
    value_t iter = expr_eval(st->text, &ectx);
    if (val_is_error(&iter)) {
        exec_error(cx, st->line, "%s", iter.err ? iter.err : "bad iterable");
        value_free(&iter);
        return;
    }
    if (iter.kind != V_LIST && iter.kind != V_RANGE && iter.kind != V_BYTES && iter.kind != V_MAP) {
        exec_error(cx, st->line, "for: cannot iterate a %s (use a list, map, a..b range, or bytes)",
                   iter.kind == V_INT || iter.kind == V_UINT ? "plain integer" : "value of this kind");
        value_free(&iter);
        return;
    }

    // Save any shadowed same-named binding in the current scope; the
    // loop variable is removed at every exit route (§3.8).
    value_t saved = val_none();
    bool had = shell_binding_save_top(st->name, &saved);

    size_t count = 0;
    if (iter.kind == V_LIST)
        count = iter.list.len;
    else if (iter.kind == V_MAP)
        count = iter.map.len;
    else if (iter.kind == V_BYTES)
        count = iter.bytes.n;
    else
        count = iter.range.stop > iter.range.start ? (size_t)(iter.range.stop - iter.range.start) : 0;

    for (size_t i = 0; i < count; i++) {
        if (g_interrupt) {
            g_interrupt = false;
            exec_error(cx, st->line, "interrupted");
            break;
        }
        value_t item;
        if (iter.kind == V_LIST)
            item = value_dup(&iter.list.items[i]);
        else if (iter.kind == V_MAP)
            // Maps iterate their keys (dict idiom); `map[k]` fetches values.
            item = val_str(iter.map.entries[i].key);
        else if (iter.kind == V_BYTES)
            item = val_uint(1, iter.bytes.p[i]);
        else
            item = val_int(iter.range.start + (int64_t)i);
        char err[160];
        if (shell_binding_let(st->name, item, err, sizeof(err)) < 0) {
            exec_error(cx, st->line, "%s", err);
            break;
        }
        cx->loop_depth++;
        exec_block(st->body, cx);
        cx->loop_depth--;
        if (cx->sig == SIG_BREAK) {
            cx->sig = SIG_NONE;
            break;
        }
        if (cx->sig == SIG_CONTINUE)
            cx->sig = SIG_NONE;
        if (cx->sig != SIG_NONE)
            break;
    }

    shell_binding_remove_top(st->name);
    if (had) {
        char err[160];
        shell_binding_let(st->name, saved, err, sizeof(err));
    }
    value_free(&iter);
}

static void exec_assert(stmt_t *st, exec_ctx_t *cx) {
    expr_ctx_t ectx;
    script_expr_ctx(&ectx);
    const char *p = st->text;
    value_t v = expr_eval_at(&p, &ectx);
    p = skip_sp(p);
    bool is_err = val_is_error(&v);
    const char *msg_at = NULL;
    if (*p == '"')
        msg_at = p;
    else if (*p != '\0' && !is_err) {
        // (When the predicate errored the cursor may sit mid-text; the
        // error itself is the diagnosis, not the trailing text.)
        value_free(&v);
        exec_error(cx, st->line, "assert: unexpected text after predicate (message must be a \"string\")");
        return;
    }
    bool ok = !is_err && val_as_bool(&v);
    if (ok) {
        value_free(&v);
        return; // silent on success (§5)
    }
    // Failure: format the message (interpolated now, at failure time).
    char msg[512] = "";
    if (msg_at) {
        const char *mp = msg_at;
        value_t m = expr_parse_dq_string(&mp, &ectx);
        if (m.kind == V_STRING && m.s)
            snprintf(msg, sizeof(msg), "%s", m.s);
        value_free(&m);
    }
    if (is_err)
        fprintf(stderr, "line %d: %s\n", st->line, v.err ? v.err : "error in assert predicate");
    printf("ASSERT FAILED: %s\n", msg[0] ? msg : st->text);
    value_free(&v);
    cx->sig = SIG_ERROR;
}

// --- statement dispatch -----------------------------------------------------

static void exec_stmt(stmt_t *st, exec_ctx_t *cx) {
    expr_ctx_t ectx;
    switch (st->kind) {
    case ST_LET: {
        script_expr_ctx(&ectx);
        value_t v = expr_eval(st->text, &ectx);
        if (val_is_error(&v)) {
            exec_error(cx, st->line, "%s", v.err ? v.err : "bad expression");
            value_free(&v);
            return;
        }
        char err[160];
        if (shell_binding_let(st->name, v, err, sizeof(err)) < 0)
            exec_error(cx, st->line, "%s", err);
        return;
    }
    case ST_ALIAS: {
        char err[160];
        if (alias_add_user(st->name, st->text, err, sizeof(err)) < 0)
            exec_error(cx, st->line, "alias: %s", err);
        return;
    }
    case ST_ASSIGN:
        exec_assign(st, cx);
        return;
    case ST_COMMAND:
        exec_command(st, cx);
        return;
    case ST_EXPR: {
        script_expr_ctx(&ectx);
        value_t v = expr_eval(st->text, &ectx);
        if (val_is_error(&v)) {
            exec_error(cx, st->line, "%s", v.err ? v.err : "error");
            value_free(&v);
            return;
        }
        if (cx->interactive)
            shell_print_value(&v);
        value_free(&v);
        return;
    }
    case ST_IF:
        exec_if(st, cx);
        return;
    case ST_WHILE:
        exec_while(st, cx);
        return;
    case ST_FOR:
        exec_for(st, cx);
        return;
    case ST_BREAK:
        if (cx->loop_depth == 0) {
            exec_error(cx, st->line, "break outside a loop");
            return;
        }
        cx->sig = SIG_BREAK;
        return;
    case ST_CONTINUE:
        if (cx->loop_depth == 0) {
            exec_error(cx, st->line, "continue outside a loop");
            return;
        }
        cx->sig = SIG_CONTINUE;
        return;
    case ST_RETURN: {
        if (!cx->in_function) {
            exec_error(cx, st->line, "return outside a function");
            return;
        }
        value_t v = val_none();
        if (st->text && st->text[0]) {
            script_expr_ctx(&ectx);
            v = expr_eval(st->text, &ectx);
            if (val_is_error(&v)) {
                exec_error(cx, st->line, "%s", v.err ? v.err : "bad expression");
                value_free(&v);
                return;
            }
        }
        value_free(&cx->ret);
        cx->ret = v;
        cx->sig = SIG_RETURN;
        return;
    }
    case ST_DEF: {
        char err[200];
        if (shell_func_define(st->name, st->params, st->n_params, st->body, err, sizeof(err)) < 0) {
            exec_error(cx, st->line, "def: %s", err);
            return;
        }
        // Ownership of the body moved into the registry; detach it from
        // the statement so re-running the def (e.g. in a loop) is safe.
        st->body = NULL;
        // Params are duplicated by the registry; nothing to detach.
        return;
    }
    case ST_ASSERT:
        exec_assert(st, cx);
        return;
    }
}

static void exec_block(script_block_t *b, exec_ctx_t *cx) {
    for (int i = 0; i < b->n && cx->sig == SIG_NONE; i++) {
        exec_stmt(b->stmts[i], cx);
        // Give the platform a chance to drive the scheduler after each
        // statement (`scheduler.run N` schedules; the pump executes).
        if (cx->sig == SIG_NONE && g_pump_hook && g_pump_hook())
            cx->sig = SIG_QUIT;
    }
}

int script_exec(script_t *s, bool interactive) {
    if (!s)
        return -1;
    exec_ctx_t cx = {0};
    cx.interactive = interactive;
    exec_block(s->top, &cx);
    value_free(&cx.ret);
    return cx.sig == SIG_ERROR ? -1 : 0;
}

value_t script_exec_func_body(script_block_t *body) {
    exec_ctx_t cx = {0};
    cx.in_function = true;
    exec_block(body, &cx);
    if (cx.sig == SIG_ERROR) {
        value_free(&cx.ret);
        return val_err("function body failed");
    }
    value_t r = cx.ret;
    cx.ret = val_none();
    if (cx.sig != SIG_RETURN)
        value_free(&r), r = val_none();
    return r;
}

int script_run_line(const char *line) {
    char err[256];
    script_t *s = script_parse(line, err, sizeof(err));
    if (!s) {
        fprintf(stderr, "%s\n", err);
        return -1;
    }
    int rc = script_exec(s, true);
    script_free(s);
    return rc;
}

int script_run_source(const char *src) {
    char err[256];
    script_t *s = script_parse(src, err, sizeof(err));
    if (!s) {
        fprintf(stderr, "%s\n", err);
        return -1;
    }
    int rc = script_exec(s, false);
    script_free(s);
    return rc;
}
