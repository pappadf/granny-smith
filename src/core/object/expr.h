// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// expr.h
// Recursive-descent expression parser/evaluator for the shell's
// `${...}` form. See proposal-shell-expressions.md.
//
// The parser is pure (no parse-time side effects). Evaluation may
// invoke methods on the object tree, which can have side effects;
// purity is convention, not enforced.
//
// Bindings: callers may supply an alias-resolver, a root object, and a
// per-eval bindings table so the parser can resolve `$alias`, bare
// object paths, and bare names (shell variables, deferred-eval scope)
// against the tree. All three are optional; tests that don't need them
// can pass NULLs and stay within pure literal/operator territory.

#ifndef GS_OBJECT_EXPR_H
#define GS_OBJECT_EXPR_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

struct object;

// Resolver callback: turn an alias name into a path string. Caller
// returns a pointer into its own storage; the expression evaluator does
// not take ownership. Return NULL if `name` is not a known alias.
typedef const char *(*expr_alias_fn)(void *ud, const char *name);

// Bindings callback: resolve a bare identifier (no leading `$`, no
// dots) to a value_t. Used for shell variables (`let X=42` → `${X}`)
// at shell time and for per-fire context (`${value}`/`${addr}`/`${size}`
// inside a logpoint message) at fire time. The evaluator queries this
// after path/alias lookup fails. Return val_none() / V_NONE if unknown
// — anything else (including V_ERROR) is treated as the resolved value.
// Returned value_t is OWNED by the caller of expr_eval; the binding fn
// must produce a freshly-allocated value (or a self-contained inline
// value like val_int that needs no free).
typedef value_t (*expr_binding_fn)(void *ud, const char *name);

// Context bundling the bindings a single evaluation needs.
typedef struct expr_ctx {
    struct object *root; // bare paths resolve against this; may be NULL
    expr_alias_fn alias; // $name lookup; may be NULL
    void *alias_ud; // user-data for alias callback
    expr_binding_fn binding; // bare-name lookup fallback; may be NULL
    void *binding_ud; // user-data for binding callback
} expr_ctx_t;

// Evaluate the expression in `src` (the body of a `$(...)` form, with
// no surrounding parens). Returns the resulting value_t (or V_ERROR on
// syntax/type/resolution errors). Caller frees with value_free.
//
// `src` is consumed entirely; trailing garbage is an error.
value_t expr_eval(const char *src, const expr_ctx_t *ctx);

// Same as expr_eval, but advances *p past the consumed expression. The
// caller is responsible for passing a position that is the start of the
// expression body. Used by `$(...)` and `${...}` recognition in the
// shell tokenizer.
value_t expr_eval_at(const char **p, const expr_ctx_t *ctx);

// Interpolate ${...} regions inside a string literal body. `src` is the
// raw body of a "..." string with escapes already decoded. Returns a
// V_STRING with all ${expr} regions replaced by their default-formatted
// values. ${ that is not closed is an error.
value_t expr_interpolate_string(const char *src, const expr_ctx_t *ctx);

// Evaluate the body of a single `${...}` substitution and return the
// result as a freshly malloc'd, NUL-terminated string. The body may
// carry an optional trailing `:fmt` format spec at the top level (see
// proposal-shell-expressions §4.2.1); in that case the spec governs the
// to-string conversion. On error the returned string is NULL and *err
// is set to a freshly malloc'd error message (caller frees both).
char *expr_substitute(const char *body, const expr_ctx_t *ctx, char **err);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_EXPR_H
