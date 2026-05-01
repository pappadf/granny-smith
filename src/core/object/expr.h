// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// expr.h
// Recursive-descent expression parser/evaluator for the shell's
// `$(...)` and `${...}` forms. See proposal-shell-expressions.md.
//
// The parser is pure (no parse-time side effects). Evaluation may
// invoke methods on the object tree, which can have side effects;
// purity is convention, not enforced.
//
// Bindings: callers may supply an alias-resolver and a root object so
// the parser can resolve `$alias` and bare paths against the tree. M1
// keeps these optional; tests that don't need them can pass NULLs and
// stay within pure literal/operator territory.

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

// Context bundling the bindings a single evaluation needs.
typedef struct expr_ctx {
    struct object *root; // bare paths resolve against this; may be NULL
    expr_alias_fn alias; // $name lookup; may be NULL
    void *alias_ud; // user-data for alias callback
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

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_EXPR_H
