// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// expr.c
// Recursive-descent expression parser + evaluator. See expr.h.
//
// Grammar (tightest first; matches proposal-shell-expressions.md §2.3):
//
//   primary    := literal | path-or-call | '(' expr ')'
//   postfix    := primary ( '.' IDENT | '[' expr ']' )*
//   unary      := ('+' | '-' | '!' | '~') unary | postfix
//   mul        := unary  (('*'|'/'|'%') unary)*
//   add        := mul    (('+'|'-')     mul)*
//   shift      := add    (('<<'|'>>')   add)*
//   bitand     := shift  ('&'           shift)*
//   bitxor     := bitand ('^'           bitand)*
//   bitor      := bitxor ('|'           bitxor)*
//   relational := bitor  (('<'|'<='|'>'|'>=') bitor)*
//   equality   := relational (('=='|'!=') relational)*
//   logand     := equality   ('&&' equality)*
//   logor      := logand     ('||' logand)*
//   ternary    := logor      ('?' expr ':' ternary)?
//   expr       := ternary

#include "expr.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "parse.h"

// === Lexer state ============================================================

typedef struct lex {
    const char *src;
    const char *p;
    char err[256]; // last error message; non-empty means a syntax error
    bool err_set;
} lex_t;

static void lex_skip_ws(lex_t *L) {
    while (*L->p && isspace((unsigned char)*L->p))
        L->p++;
}

static void lex_error(lex_t *L, const char *fmt, ...) {
    if (L->err_set)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(L->err, sizeof(L->err), fmt, ap);
    va_end(ap);
    L->err_set = true;
}

// Two-char token consumption, e.g. "==", "<<", "&&".
static bool lex_eat2(lex_t *L, char a, char b) {
    lex_skip_ws(L);
    if (L->p[0] == a && L->p[1] == b) {
        L->p += 2;
        return true;
    }
    return false;
}

// === Forward declarations ===================================================

static value_t parse_expr(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_ternary(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_logor(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_logand(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_equality(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_relational(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_bitor(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_bitxor(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_bitand(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_shift(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_add(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_mul(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_unary(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_postfix(lex_t *L, const expr_ctx_t *ctx);
static value_t parse_primary(lex_t *L, const expr_ctx_t *ctx);

// === Numeric promotion helpers ==============================================
//
// Two-operand promotion ladder per proposal §3.1:
//   bool          → uint
//   int + uint    → int (if either is int)
//   int|uint + f  → float
//
// Returns the kind both operands should be promoted to, or V_ERROR if
// the combination is unsupported here. Caller is responsible for
// applying the conversion.

typedef enum {
    NK_NONE = 0,
    NK_INT,
    NK_UINT,
    NK_FLOAT,
} num_kind_t;

static num_kind_t classify_numeric(const value_t *v) {
    if (!v)
        return NK_NONE;
    switch (v->kind) {
    case V_BOOL:
    case V_UINT:
        return NK_UINT;
    case V_INT:
        return NK_INT;
    case V_FLOAT:
        return NK_FLOAT;
    case V_ENUM:
        return NK_UINT;
    default:
        return NK_NONE;
    }
}

static num_kind_t promote_pair(num_kind_t a, num_kind_t b) {
    if (a == NK_NONE || b == NK_NONE)
        return NK_NONE;
    if (a == NK_FLOAT || b == NK_FLOAT)
        return NK_FLOAT;
    if (a == NK_INT || b == NK_INT)
        return NK_INT;
    return NK_UINT;
}

// Coerce a value into the requested numeric kind. Returns V_ERROR on
// non-numeric inputs. Does NOT free the input.
static value_t coerce_to(num_kind_t k, const value_t *v) {
    bool ok = false;
    switch (k) {
    case NK_FLOAT: {
        double d = val_as_f64(v, &ok);
        return ok ? val_float(d) : val_err("non-numeric");
    }
    case NK_INT: {
        int64_t i = val_as_i64(v, &ok);
        return ok ? val_int(i) : val_err("non-numeric");
    }
    case NK_UINT: {
        uint64_t u = val_as_u64(v, &ok);
        return ok ? val_uint(0, u) : val_err("non-numeric");
    }
    default:
        return val_err("non-numeric");
    }
}

// Carry the wider width and HEX flag from operands to result.
static void carry_flags(value_t *r, const value_t *a, const value_t *b) {
    uint8_t wa = a ? a->width : 0;
    uint8_t wb = b ? b->width : 0;
    r->width = wa > wb ? wa : wb;
    if ((a && (a->flags & VAL_HEX)) || (b && (b->flags & VAL_HEX)))
        r->flags |= VAL_HEX;
}

// === Equality across kinds ==================================================

static bool same_kind_equal(const value_t *a, const value_t *b) {
    if (a->kind != b->kind)
        return false;
    switch (a->kind) {
    case V_BOOL:
        return a->b == b->b;
    case V_INT:
        return a->i == b->i;
    case V_UINT:
        return a->u == b->u;
    case V_FLOAT:
        return a->f == b->f;
    case V_STRING:
        return strcmp(a->s ? a->s : "", b->s ? b->s : "") == 0;
    case V_BYTES:
        return a->bytes.n == b->bytes.n && (a->bytes.n == 0 || memcmp(a->bytes.p, b->bytes.p, a->bytes.n) == 0);
    case V_ENUM:
        return a->enm.idx == b->enm.idx;
    case V_NONE:
        return true;
    case V_OBJECT:
        return a->obj == b->obj;
    default:
        return false;
    }
}

// True if a == b under cross-kind numeric promotion.
static value_t value_equal(const value_t *a, const value_t *b) {
    num_kind_t ka = classify_numeric(a);
    num_kind_t kb = classify_numeric(b);
    if (ka != NK_NONE && kb != NK_NONE) {
        num_kind_t k = promote_pair(ka, kb);
        value_t pa = coerce_to(k, a);
        value_t pb = coerce_to(k, b);
        bool eq = same_kind_equal(&pa, &pb);
        value_free(&pa);
        value_free(&pb);
        return val_bool(eq);
    }
    return val_bool(same_kind_equal(a, b));
}

// === Path parsing inside expressions ========================================
//
// Inside `$(...)` a bare identifier is a path head; further segments
// follow via `.`/`[...]`. The path is read into a buffer, then handed
// to `object_resolve` against the context's root. Method calls use
// call form: `path(arg, arg, ...)`.

// Read an identifier starting at L->p, copying into buf. Returns true
// on success, advancing L->p.
static bool lex_read_ident(lex_t *L, char *buf, size_t buf_size) {
    if (!isalpha((unsigned char)*L->p) && *L->p != '_')
        return false;
    size_t i = 0;
    while (*L->p && (isalnum((unsigned char)*L->p) || *L->p == '_')) {
        if (i + 1 < buf_size)
            buf[i++] = *L->p;
        L->p++;
    }
    buf[i] = '\0';
    return true;
}

// Read a path expression: ident ( '.' ident | '[' expr ']' )* up to a
// terminating non-path character. Builds a textual path that can be
// fed to object_resolve, or detects a call form.
//
// On call form: returns a path-only string and sets *call_open to true.
// In that case, the cursor is left immediately after the opening `(`,
// so the caller can parse the comma-separated arguments.
//
// path_buf must be at least 256 bytes.
static bool read_path_segments(lex_t *L, const expr_ctx_t *ctx, char *path_buf, size_t path_size, bool *call_open) {
    *call_open = false;
    size_t pi = 0;
    char ident[64];
    if (!lex_read_ident(L, ident, sizeof(ident))) {
        lex_error(L, "expected identifier");
        return false;
    }
    int n = snprintf(path_buf + pi, path_size - pi, "%s", ident);
    if (n < 0 || (size_t)n >= path_size - pi) {
        lex_error(L, "path too long");
        return false;
    }
    pi += (size_t)n;

    while (*L->p) {
        if (*L->p == '.') {
            // peek: only consume if followed by an ident character (a
            // bare `.` followed by space/operator is not a path '.')
            if (!(isalpha((unsigned char)L->p[1]) || L->p[1] == '_'))
                break;
            L->p++; // consume '.'
            if (!lex_read_ident(L, ident, sizeof(ident))) {
                lex_error(L, "expected identifier after '.'");
                return false;
            }
            n = snprintf(path_buf + pi, path_size - pi, ".%s", ident);
            if (n < 0 || (size_t)n >= path_size - pi) {
                lex_error(L, "path too long");
                return false;
            }
            pi += (size_t)n;
        } else if (*L->p == '[') {
            // Index: evaluate inner expression and stringify as integer.
            L->p++;
            value_t idx = parse_expr(L, ctx);
            if (L->err_set) {
                value_free(&idx);
                return false;
            }
            if (val_is_error(&idx)) {
                lex_error(L, "%s", idx.err ? idx.err : "bad index");
                value_free(&idx);
                return false;
            }
            bool ok = false;
            int64_t iv = val_as_i64(&idx, &ok);
            value_free(&idx);
            if (!ok) {
                lex_error(L, "index must be numeric");
                return false;
            }
            lex_skip_ws(L);
            if (*L->p != ']') {
                lex_error(L, "expected ']'");
                return false;
            }
            L->p++;
            n = snprintf(path_buf + pi, path_size - pi, "[%lld]", (long long)iv);
            if (n < 0 || (size_t)n >= path_size - pi) {
                lex_error(L, "path too long");
                return false;
            }
            pi += (size_t)n;
        } else if (*L->p == '(') {
            L->p++;
            *call_open = true;
            return true;
        } else {
            break;
        }
    }
    return true;
}

// Parse a comma-separated argument list inside a method call. The
// opening `(` has already been consumed. On success, consumes the
// closing `)` and writes argv/argc; caller frees argv items and array.
static bool parse_call_args(lex_t *L, const expr_ctx_t *ctx, value_t **out_argv, int *out_argc) {
    *out_argv = NULL;
    *out_argc = 0;
    lex_skip_ws(L);
    if (*L->p == ')') {
        L->p++;
        return true;
    }
    size_t cap = 4;
    value_t *argv = (value_t *)calloc(cap, sizeof(value_t));
    int argc = 0;
    while (1) {
        value_t a = parse_expr(L, ctx);
        if (L->err_set || val_is_error(&a)) {
            if (val_is_error(&a) && !L->err_set)
                lex_error(L, "%s", a.err ? a.err : "bad argument");
            value_free(&a);
            for (int i = 0; i < argc; i++)
                value_free(&argv[i]);
            free(argv);
            return false;
        }
        if ((size_t)argc == cap) {
            cap *= 2;
            value_t *na = (value_t *)realloc(argv, cap * sizeof(value_t));
            if (!na) {
                lex_error(L, "out of memory");
                value_free(&a);
                for (int i = 0; i < argc; i++)
                    value_free(&argv[i]);
                free(argv);
                return false;
            }
            argv = na;
        }
        argv[argc++] = a;
        lex_skip_ws(L);
        if (*L->p == ',') {
            L->p++;
            continue;
        }
        if (*L->p == ')') {
            L->p++;
            break;
        }
        lex_error(L, "expected ',' or ')'");
        for (int i = 0; i < argc; i++)
            value_free(&argv[i]);
        free(argv);
        return false;
    }
    *out_argv = argv;
    *out_argc = argc;
    return true;
}

// === Primary ================================================================

static value_t parse_primary(lex_t *L, const expr_ctx_t *ctx) {
    lex_skip_ws(L);
    char c = *L->p;
    if (!c) {
        lex_error(L, "unexpected end of expression");
        return val_err("eof");
    }

    // Parenthesised sub-expression.
    if (c == '(') {
        L->p++;
        value_t v = parse_expr(L, ctx);
        if (L->err_set)
            return v;
        lex_skip_ws(L);
        if (*L->p != ')') {
            lex_error(L, "expected ')'");
            value_free(&v);
            return val_err("expected ')'");
        }
        L->p++;
        return v;
    }

    // String literal — decode the body and run interpolation against
    // ctx so `${expr}` works inside expression strings too.
    if (c == '"') {
        value_t s = parse_string_literal(&L->p);
        if (val_is_error(&s)) {
            lex_error(L, "%s", s.err ? s.err : "bad string");
            return s;
        }
        // Run interpolation on the decoded body.
        value_t out = expr_interpolate_string(s.s, ctx);
        value_free(&s);
        return out;
    }

    // `$name` alias substitution.
    if (c == '$') {
        L->p++;
        char ident[64];
        if (!lex_read_ident(L, ident, sizeof(ident))) {
            lex_error(L, "expected alias name after '$'");
            return val_err("bad alias");
        }
        const char *path = NULL;
        if (ctx && ctx->alias)
            path = ctx->alias(ctx->alias_ud, ident);
        if (!path)
            return val_err("no such alias '$%s'", ident);
        if (!ctx || !ctx->root)
            return val_err("alias '$%s' has no root", ident);
        node_t n = object_resolve(ctx->root, path);
        if (!node_valid(n))
            return val_err("alias '$%s' → '%s' did not resolve", ident, path);
        return node_get(n);
    }

    // Boolean keywords (treated as primary literals, not paths).
    if (isalpha((unsigned char)c) || c == '_') {
        // Snapshot the position so we can rewind and treat as path if
        // not a literal.
        const char *save = L->p;
        // Try literals (true/false/on/off/yes/no).
        value_t lit = parse_literal(&L->p, NULL, 0);
        if (lit.kind == V_BOOL)
            return lit;
        // Other literal kinds returned by parse_literal: V_STRING (bare
        // ident — handled below as path) or V_ERROR. Discard and parse
        // as a path-or-call.
        value_free(&lit);
        L->p = save;

        // Path-or-call.
        char path_buf[256];
        bool call_open = false;
        if (!read_path_segments(L, ctx, path_buf, sizeof(path_buf), &call_open))
            return val_err("bad path");
        if (!ctx || !ctx->root) {
            // No root bound — path resolution is a runtime error (not a
            // syntax error), so it propagates as V_ERROR through operators
            // and is short-circuited by `&&` / `||`.
            if (call_open) {
                // Drain any args to keep the cursor advancing.
                value_t *argv = NULL;
                int argc = 0;
                if (parse_call_args(L, ctx, &argv, &argc)) {
                    for (int i = 0; i < argc; i++)
                        value_free(&argv[i]);
                    free(argv);
                }
            }
            return val_err("path '%s' has no root", path_buf);
        }
        node_t node = object_resolve(ctx->root, path_buf);
        if (!node_valid(node)) {
            if (call_open) {
                value_t *argv = NULL;
                int argc = 0;
                if (parse_call_args(L, ctx, &argv, &argc)) {
                    for (int i = 0; i < argc; i++)
                        value_free(&argv[i]);
                    free(argv);
                }
            }
            return val_err("path '%s' did not resolve", path_buf);
        }
        if (call_open) {
            value_t *argv = NULL;
            int argc = 0;
            if (!parse_call_args(L, ctx, &argv, &argc))
                return val_err("bad call");
            value_t r = node_call(node, argc, argv);
            for (int i = 0; i < argc; i++)
                value_free(&argv[i]);
            free(argv);
            return r;
        }
        return node_get(node);
    }

    // Numeric / string literal via the unified literal parser.
    if (c == '+' || c == '-' || c == '.' || c == '$' || isdigit((unsigned char)c)) {
        value_t v = parse_literal(&L->p, NULL, 0);
        if (val_is_error(&v)) {
            lex_error(L, "%s", v.err ? v.err : "bad literal");
        }
        return v;
    }

    lex_error(L, "unexpected character '%c'", c);
    return val_err("unexpected '%c'", c);
}

// === Postfix ================================================================
//
// Postfix is folded into the path parser inside parse_primary (paths
// own their own `.` / `[...]`). For non-path primaries (literals,
// parenthesised expressions, calls), no postfix is permitted in M1.

static value_t parse_postfix(lex_t *L, const expr_ctx_t *ctx) {
    return parse_primary(L, ctx);
}

// === Unary ==================================================================

static value_t parse_unary(lex_t *L, const expr_ctx_t *ctx) {
    lex_skip_ws(L);
    char c = *L->p;
    if (c == '!') {
        L->p++;
        value_t v = parse_unary(L, ctx);
        if (L->err_set)
            return v;
        // Per proposal §3.2: V_ERROR is falsy, so `!error` is true.
        // This is what makes `assert $(!cpu.broken)` clean for "either
        // the attribute does not exist, or it is false".
        bool t = val_as_bool(&v);
        value_free(&v);
        return val_bool(!t);
    }
    if (c == '~') {
        L->p++;
        value_t v = parse_unary(L, ctx);
        if (L->err_set || val_is_error(&v))
            return v;
        bool ok = false;
        uint64_t u = val_as_u64(&v, &ok);
        uint8_t w = v.width;
        value_free(&v);
        if (!ok)
            return val_err("'~' requires integer");
        return val_uint(w, ~u);
    }
    if (c == '-') {
        L->p++;
        value_t v = parse_unary(L, ctx);
        if (L->err_set || val_is_error(&v))
            return v;
        if (v.kind == V_FLOAT) {
            double d = -v.f;
            value_free(&v);
            return val_float(d);
        }
        if (v.kind == V_INT) {
            int64_t i = -v.i;
            value_free(&v);
            return val_int(i);
        }
        if (v.kind == V_UINT) {
            // -uint produces signed (mirrors C semantics enough for our purposes).
            int64_t i = -(int64_t)v.u;
            value_free(&v);
            return val_int(i);
        }
        if (v.kind == V_BOOL) {
            int64_t i = v.b ? -1 : 0;
            value_free(&v);
            return val_int(i);
        }
        value_free(&v);
        return val_err("unary '-' requires numeric");
    }
    if (c == '+') {
        L->p++;
        return parse_unary(L, ctx);
    }
    return parse_postfix(L, ctx);
}

// === Mul / Add / Shift / Bitwise ============================================

static value_t numeric_op(const value_t *a, const value_t *b, char op, char op2, char *err) {
    num_kind_t k = promote_pair(classify_numeric(a), classify_numeric(b));
    if (k == NK_NONE) {
        snprintf(err, 64, "non-numeric operand to '%c%s'", op, op2 ? (char[2]){op2, 0} : (char[1]){0});
        return val_err("non-numeric");
    }
    value_t pa = coerce_to(k, a);
    value_t pb = coerce_to(k, b);
    value_t r = val_none();
    if (k == NK_FLOAT) {
        double x = pa.f, y = pb.f, z = 0;
        switch (op) {
        case '+':
            z = x + y;
            break;
        case '-':
            z = x - y;
            break;
        case '*':
            z = x * y;
            break;
        case '/':
            if (y == 0) {
                value_free(&pa);
                value_free(&pb);
                return val_err("division by zero");
            }
            z = x / y;
            break;
        case '%':
            z = fmod(x, y);
            break;
        default:
            value_free(&pa);
            value_free(&pb);
            return val_err("bad op for float");
        }
        r = val_float(z);
    } else if (k == NK_INT) {
        int64_t x = pa.i, y = pb.i, z = 0;
        switch (op) {
        case '+':
            z = x + y;
            break;
        case '-':
            z = x - y;
            break;
        case '*':
            z = x * y;
            break;
        case '/':
            if (y == 0) {
                value_free(&pa);
                value_free(&pb);
                return val_err("division by zero");
            }
            z = x / y;
            break;
        case '%':
            if (y == 0) {
                value_free(&pa);
                value_free(&pb);
                return val_err("division by zero");
            }
            z = x % y;
            break;
        case '&':
            z = x & y;
            break;
        case '|':
            z = x | y;
            break;
        case '^':
            z = x ^ y;
            break;
        case '<':
            z = x << y;
            break;
        case '>':
            z = x >> y;
            break; // arithmetic shift on signed (proposal §2.3)
        default:
            value_free(&pa);
            value_free(&pb);
            return val_err("bad op for int");
        }
        r = val_int(z);
    } else { // NK_UINT
        uint64_t x = pa.u, y = pb.u, z = 0;
        switch (op) {
        case '+':
            z = x + y;
            break;
        case '-':
            z = x - y;
            break;
        case '*':
            z = x * y;
            break;
        case '/':
            if (y == 0) {
                value_free(&pa);
                value_free(&pb);
                return val_err("division by zero");
            }
            z = x / y;
            break;
        case '%':
            if (y == 0) {
                value_free(&pa);
                value_free(&pb);
                return val_err("division by zero");
            }
            z = x % y;
            break;
        case '&':
            z = x & y;
            break;
        case '|':
            z = x | y;
            break;
        case '^':
            z = x ^ y;
            break;
        case '<':
            z = x << y;
            break;
        case '>':
            z = x >> y;
            break;
        default:
            value_free(&pa);
            value_free(&pb);
            return val_err("bad op for uint");
        }
        r = val_uint(0, z);
    }
    carry_flags(&r, a, b);
    value_free(&pa);
    value_free(&pb);
    err[0] = '\0';
    return r;
}

static value_t parse_mul(lex_t *L, const expr_ctx_t *ctx) {
    value_t a = parse_unary(L, ctx);
    if (L->err_set || val_is_error(&a))
        return a;
    while (1) {
        lex_skip_ws(L);
        char op = *L->p;
        if (op != '*' && op != '/' && op != '%')
            break;
        L->p++;
        value_t b = parse_unary(L, ctx);
        if (L->err_set || val_is_error(&b)) {
            value_free(&a);
            return b;
        }
        char err[64];
        value_t r = numeric_op(&a, &b, op, 0, err);
        value_free(&a);
        value_free(&b);
        if (val_is_error(&r) && err[0])
            lex_error(L, "%s", err);
        a = r;
        if (val_is_error(&a))
            return a;
    }
    return a;
}

// String / list / bytes concat helper for '+'.
static value_t plus_concat(const value_t *a, const value_t *b) {
    if (a->kind == V_STRING && b->kind == V_STRING) {
        size_t la = a->s ? strlen(a->s) : 0;
        size_t lb = b->s ? strlen(b->s) : 0;
        char *r = (char *)malloc(la + lb + 1);
        if (!r)
            return val_err("oom");
        if (la)
            memcpy(r, a->s, la);
        if (lb)
            memcpy(r + la, b->s, lb);
        r[la + lb] = '\0';
        value_t v = val_str(r);
        free(r);
        return v;
    }
    if (a->kind == V_BYTES && b->kind == V_BYTES) {
        size_t n = a->bytes.n + b->bytes.n;
        uint8_t *r = (uint8_t *)malloc(n ? n : 1);
        if (!r)
            return val_err("oom");
        if (a->bytes.n)
            memcpy(r, a->bytes.p, a->bytes.n);
        if (b->bytes.n)
            memcpy(r + a->bytes.n, b->bytes.p, b->bytes.n);
        value_t v = val_bytes(r, n);
        free(r);
        return v;
    }
    return val_err("type error");
}

static value_t parse_add(lex_t *L, const expr_ctx_t *ctx) {
    value_t a = parse_mul(L, ctx);
    if (L->err_set || val_is_error(&a))
        return a;
    while (1) {
        lex_skip_ws(L);
        char op = *L->p;
        if (op != '+' && op != '-')
            break;
        L->p++;
        value_t b = parse_mul(L, ctx);
        if (L->err_set || val_is_error(&b)) {
            value_free(&a);
            return b;
        }
        // String/bytes concat with '+'.
        if (op == '+' && (a.kind == V_STRING || a.kind == V_BYTES)) {
            value_t r = plus_concat(&a, &b);
            value_free(&a);
            value_free(&b);
            if (val_is_error(&r)) {
                lex_error(L, "%s", r.err ? r.err : "type error");
                return r;
            }
            a = r;
            continue;
        }
        char err[64];
        value_t r = numeric_op(&a, &b, op, 0, err);
        value_free(&a);
        value_free(&b);
        if (val_is_error(&r) && err[0])
            lex_error(L, "%s", err);
        a = r;
        if (val_is_error(&a))
            return a;
    }
    return a;
}

static value_t parse_shift(lex_t *L, const expr_ctx_t *ctx) {
    value_t a = parse_add(L, ctx);
    if (L->err_set || val_is_error(&a))
        return a;
    while (1) {
        lex_skip_ws(L);
        char op = 0;
        if (lex_eat2(L, '<', '<'))
            op = '<';
        else if (lex_eat2(L, '>', '>'))
            op = '>';
        else
            break;
        value_t b = parse_add(L, ctx);
        if (L->err_set || val_is_error(&b)) {
            value_free(&a);
            return b;
        }
        char err[64];
        value_t r = numeric_op(&a, &b, op, op, err);
        value_free(&a);
        value_free(&b);
        if (val_is_error(&r) && err[0])
            lex_error(L, "%s", err);
        a = r;
        if (val_is_error(&a))
            return a;
    }
    return a;
}

static value_t parse_bitand(lex_t *L, const expr_ctx_t *ctx) {
    value_t a = parse_shift(L, ctx);
    if (L->err_set || val_is_error(&a))
        return a;
    while (1) {
        lex_skip_ws(L);
        // '&' but not '&&'
        if (L->p[0] != '&' || L->p[1] == '&')
            break;
        L->p++;
        value_t b = parse_shift(L, ctx);
        if (L->err_set || val_is_error(&b)) {
            value_free(&a);
            return b;
        }
        char err[64];
        value_t r = numeric_op(&a, &b, '&', 0, err);
        value_free(&a);
        value_free(&b);
        if (val_is_error(&r) && err[0])
            lex_error(L, "%s", err);
        a = r;
        if (val_is_error(&a))
            return a;
    }
    return a;
}

static value_t parse_bitxor(lex_t *L, const expr_ctx_t *ctx) {
    value_t a = parse_bitand(L, ctx);
    if (L->err_set || val_is_error(&a))
        return a;
    while (1) {
        lex_skip_ws(L);
        if (*L->p != '^')
            break;
        L->p++;
        value_t b = parse_bitand(L, ctx);
        if (L->err_set || val_is_error(&b)) {
            value_free(&a);
            return b;
        }
        char err[64];
        value_t r = numeric_op(&a, &b, '^', 0, err);
        value_free(&a);
        value_free(&b);
        if (val_is_error(&r) && err[0])
            lex_error(L, "%s", err);
        a = r;
        if (val_is_error(&a))
            return a;
    }
    return a;
}

static value_t parse_bitor(lex_t *L, const expr_ctx_t *ctx) {
    value_t a = parse_bitxor(L, ctx);
    if (L->err_set || val_is_error(&a))
        return a;
    while (1) {
        lex_skip_ws(L);
        // '|' but not '||'
        if (L->p[0] != '|' || L->p[1] == '|')
            break;
        L->p++;
        value_t b = parse_bitxor(L, ctx);
        if (L->err_set || val_is_error(&b)) {
            value_free(&a);
            return b;
        }
        char err[64];
        value_t r = numeric_op(&a, &b, '|', 0, err);
        value_free(&a);
        value_free(&b);
        if (val_is_error(&r) && err[0])
            lex_error(L, "%s", err);
        a = r;
        if (val_is_error(&a))
            return a;
    }
    return a;
}

// === Relational / Equality ==================================================

static int compare_numeric(const value_t *a, const value_t *b, bool *ok) {
    *ok = true;
    num_kind_t k = promote_pair(classify_numeric(a), classify_numeric(b));
    if (k == NK_NONE) {
        // Strings compare lexicographically.
        if (a->kind == V_STRING && b->kind == V_STRING)
            return strcmp(a->s ? a->s : "", b->s ? b->s : "");
        *ok = false;
        return 0;
    }
    value_t pa = coerce_to(k, a);
    value_t pb = coerce_to(k, b);
    int r = 0;
    if (k == NK_FLOAT)
        r = (pa.f < pb.f) ? -1 : (pa.f > pb.f) ? 1 : 0;
    else if (k == NK_INT)
        r = (pa.i < pb.i) ? -1 : (pa.i > pb.i) ? 1 : 0;
    else
        r = (pa.u < pb.u) ? -1 : (pa.u > pb.u) ? 1 : 0;
    value_free(&pa);
    value_free(&pb);
    return r;
}

static value_t parse_relational(lex_t *L, const expr_ctx_t *ctx) {
    value_t a = parse_bitor(L, ctx);
    if (L->err_set || val_is_error(&a))
        return a;
    while (1) {
        lex_skip_ws(L);
        int op = 0; // -1=<, -2=<=, +1=>, +2=>=
        if (lex_eat2(L, '<', '='))
            op = -2;
        else if (lex_eat2(L, '>', '='))
            op = +2;
        else if (*L->p == '<' && L->p[1] != '<') {
            L->p++;
            op = -1;
        } else if (*L->p == '>' && L->p[1] != '>') {
            L->p++;
            op = +1;
        } else
            break;
        value_t b = parse_bitor(L, ctx);
        if (L->err_set || val_is_error(&b)) {
            value_free(&a);
            return b;
        }
        bool ok = false;
        int c = compare_numeric(&a, &b, &ok);
        value_free(&a);
        value_free(&b);
        if (!ok) {
            lex_error(L, "non-comparable operands");
            return val_err("non-comparable");
        }
        bool res = (op == -1) ? c < 0 : (op == -2) ? c <= 0 : (op == +1) ? c > 0 : c >= 0;
        a = val_bool(res);
    }
    return a;
}

static value_t parse_equality(lex_t *L, const expr_ctx_t *ctx) {
    value_t a = parse_relational(L, ctx);
    if (L->err_set || val_is_error(&a))
        return a;
    while (1) {
        lex_skip_ws(L);
        bool eq;
        if (lex_eat2(L, '=', '='))
            eq = true;
        else if (lex_eat2(L, '!', '='))
            eq = false;
        else
            break;
        value_t b = parse_relational(L, ctx);
        if (L->err_set || val_is_error(&b)) {
            value_free(&a);
            return b;
        }
        value_t r = value_equal(&a, &b);
        value_free(&a);
        value_free(&b);
        if (!eq)
            r.b = !r.b;
        a = r;
    }
    return a;
}

// === Logical short-circuit ==================================================
//
// Per proposal §3.2: errors propagate. A V_ERROR encountered anywhere
// in the chain becomes the result, with the original message preserved,
// **except** that short-circuit short-cuts skip evaluation entirely.
// `false && X` returns false even if evaluating X would have errored;
// `true || X` returns true similarly.

// Skip one equality-level expression for short-circuit purposes,
// suppressing any lex errors and discarding the value.
static void skip_equality(lex_t *L, const expr_ctx_t *ctx) {
    bool saved = L->err_set;
    char saved_msg[sizeof(L->err)];
    memcpy(saved_msg, L->err, sizeof(L->err));
    L->err_set = false;
    L->err[0] = '\0';
    value_t v = parse_equality(L, ctx);
    value_free(&v);
    L->err_set = saved;
    memcpy(L->err, saved_msg, sizeof(L->err));
}

static void skip_logand(lex_t *L, const expr_ctx_t *ctx) {
    bool saved = L->err_set;
    char saved_msg[sizeof(L->err)];
    memcpy(saved_msg, L->err, sizeof(L->err));
    L->err_set = false;
    L->err[0] = '\0';
    value_t v = parse_logand(L, ctx);
    value_free(&v);
    L->err_set = saved;
    memcpy(L->err, saved_msg, sizeof(L->err));
}

static value_t parse_logand(lex_t *L, const expr_ctx_t *ctx) {
    value_t a = parse_equality(L, ctx);
    if (L->err_set)
        return a;
    while (1) {
        lex_skip_ws(L);
        if (!lex_eat2(L, '&', '&'))
            break;
        if (val_is_error(&a)) {
            skip_equality(L, ctx);
            continue;
        }
        bool truthy = val_as_bool(&a);
        if (!truthy) {
            skip_equality(L, ctx);
            value_free(&a);
            a = val_bool(false);
            continue;
        }
        value_free(&a);
        a = parse_equality(L, ctx);
        if (L->err_set || val_is_error(&a))
            return a;
        a = val_bool(val_as_bool(&a));
    }
    return a;
}

static value_t parse_logor(lex_t *L, const expr_ctx_t *ctx) {
    value_t a = parse_logand(L, ctx);
    if (L->err_set)
        return a;
    while (1) {
        lex_skip_ws(L);
        if (!lex_eat2(L, '|', '|'))
            break;
        if (val_is_error(&a)) {
            skip_logand(L, ctx);
            continue;
        }
        bool truthy = val_as_bool(&a);
        if (truthy) {
            skip_logand(L, ctx);
            value_free(&a);
            a = val_bool(true);
            continue;
        }
        value_free(&a);
        a = parse_logand(L, ctx);
        if (L->err_set || val_is_error(&a))
            return a;
        a = val_bool(val_as_bool(&a));
    }
    return a;
}

// === Ternary ================================================================

static value_t parse_ternary(lex_t *L, const expr_ctx_t *ctx) {
    value_t c = parse_logor(L, ctx);
    if (L->err_set)
        return c;
    lex_skip_ws(L);
    if (*L->p != '?')
        return c;
    L->p++;
    bool truthy = !val_is_error(&c) && val_as_bool(&c);
    bool err = val_is_error(&c);
    value_t t = parse_expr(L, ctx);
    lex_skip_ws(L);
    if (*L->p != ':') {
        value_free(&c);
        value_free(&t);
        lex_error(L, "expected ':' in ternary");
        return val_err("ternary");
    }
    L->p++;
    value_t f = parse_ternary(L, ctx);
    if (err) {
        value_free(&t);
        value_free(&f);
        return c; // error propagates
    }
    if (truthy) {
        value_free(&c);
        value_free(&f);
        return t;
    }
    value_free(&c);
    value_free(&t);
    return f;
}

static value_t parse_expr(lex_t *L, const expr_ctx_t *ctx) {
    return parse_ternary(L, ctx);
}

// === Public entry points ====================================================

value_t expr_eval_at(const char **p, const expr_ctx_t *ctx) {
    if (!p || !*p)
        return val_err("null expression");
    lex_t L = (lex_t){.src = *p, .p = *p};
    value_t v = parse_expr(&L, ctx);
    *p = L.p;
    if (L.err_set) {
        value_free(&v);
        return val_err("%s", L.err);
    }
    return v;
}

value_t expr_eval(const char *src, const expr_ctx_t *ctx) {
    if (!src)
        return val_err("null expression");
    const char *p = src;
    value_t v = expr_eval_at(&p, ctx);
    if (val_is_error(&v))
        return v;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p) {
        value_free(&v);
        return val_err("trailing garbage in expression: '%s'", p);
    }
    return v;
}

// === String interpolation ===================================================
//
// Replaces ${expr} regions with their default-formatted values. The
// formatter is intentionally minimal in M1 — we render integers as
// hex when VAL_HEX is set, decimal otherwise; strings copy as-is. The
// proposal's full format-spec table lands in M5.

static void buf_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 64;
        while (nc < *len + n + 1)
            nc *= 2;
        char *nb = (char *)realloc(*buf, nc);
        if (!nb)
            return;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static void format_value_default(const value_t *v, char **buf, size_t *len, size_t *cap) {
    char tmp[64];
    int n = 0;
    switch (v->kind) {
    case V_NONE:
        buf_append(buf, len, cap, "", 0);
        return;
    case V_BOOL:
        n = snprintf(tmp, sizeof(tmp), "%s", v->b ? "true" : "false");
        break;
    case V_INT:
        n = snprintf(tmp, sizeof(tmp), (v->flags & VAL_HEX) ? "0x%llx" : "%lld",
                     (v->flags & VAL_HEX) ? (long long)(uint64_t)v->i : (long long)v->i);
        break;
    case V_UINT:
        n = snprintf(tmp, sizeof(tmp), (v->flags & VAL_HEX) ? "0x%llx" : "%llu", (unsigned long long)v->u);
        break;
    case V_FLOAT:
        n = snprintf(tmp, sizeof(tmp), "%g", v->f);
        break;
    case V_STRING:
        if (v->s)
            buf_append(buf, len, cap, v->s, strlen(v->s));
        return;
    case V_BYTES:
        for (size_t i = 0; i < v->bytes.n; i++) {
            int k = snprintf(tmp, sizeof(tmp), "%02x", v->bytes.p[i]);
            if (k > 0)
                buf_append(buf, len, cap, tmp, (size_t)k);
        }
        return;
    case V_ENUM:
        if (v->enm.table && (size_t)v->enm.idx < v->enm.n_table && v->enm.table[v->enm.idx])
            buf_append(buf, len, cap, v->enm.table[v->enm.idx], strlen(v->enm.table[v->enm.idx]));
        else
            n = snprintf(tmp, sizeof(tmp), "<enum:%d>", v->enm.idx);
        break;
    case V_OBJECT:
        n = snprintf(tmp, sizeof(tmp), "<object>");
        break;
    case V_ERROR:
        n = snprintf(tmp, sizeof(tmp), "<error: %s>", v->err ? v->err : "");
        break;
    case V_LIST:
        buf_append(buf, len, cap, "[", 1);
        for (size_t i = 0; i < v->list.len; i++) {
            if (i)
                buf_append(buf, len, cap, ", ", 2);
            format_value_default(&v->list.items[i], buf, len, cap);
        }
        buf_append(buf, len, cap, "]", 1);
        return;
    }
    if (n > 0)
        buf_append(buf, len, cap, tmp, (size_t)n);
}

value_t expr_interpolate_string(const char *src, const expr_ctx_t *ctx) {
    if (!src)
        return val_str("");
    const char *p = src;
    char *out = NULL;
    size_t len = 0, cap = 0;
    while (*p) {
        if (p[0] == '$' && p[1] == '{') {
            const char *q = p + 2;
            int depth = 1;
            // Find matching '}', honouring nested braces and quoted strings.
            const char *body_start = q;
            while (*q && depth > 0) {
                if (*q == '"') {
                    q++;
                    while (*q && *q != '"') {
                        if (*q == '\\' && q[1])
                            q += 2;
                        else
                            q++;
                    }
                    if (*q == '"')
                        q++;
                    continue;
                }
                if (*q == '{')
                    depth++;
                else if (*q == '}')
                    depth--;
                if (depth > 0)
                    q++;
            }
            if (depth != 0 || *q != '}') {
                free(out);
                return val_err("unterminated ${ in string");
            }
            // Copy body to a NUL-terminated temporary.
            size_t blen = (size_t)(q - body_start);
            char *body = (char *)malloc(blen + 1);
            if (!body) {
                free(out);
                return val_err("oom");
            }
            memcpy(body, body_start, blen);
            body[blen] = '\0';
            value_t v = expr_eval(body, ctx);
            free(body);
            if (val_is_error(&v)) {
                // Splice <error: ...> into the string for visibility.
                format_value_default(&v, &out, &len, &cap);
                value_free(&v);
            } else {
                format_value_default(&v, &out, &len, &cap);
                value_free(&v);
            }
            p = q + 1;
        } else {
            buf_append(&out, &len, &cap, p, 1);
            p++;
        }
    }
    value_t r = val_str(out ? out : "");
    free(out);
    return r;
}
