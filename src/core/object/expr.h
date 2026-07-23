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

#include "object.h"
#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

struct object;

// User-function call hook (shell v2 §3.10): when a single-segment call
// form (`step_to(0x400128)`) does not resolve against the object tree,
// the evaluator hands it to this hook. The shell registers its script-
// function registry here so functions work in any expression — asserts,
// conditions, and fire-time templates included.
typedef value_t (*expr_func_hook_fn)(void *ud, const char *name, int argc, const value_t *argv, int named_n,
                                     const named_arg_t *named);
void expr_set_func_hook(expr_func_hook_fn fn, void *ud);

// Bindings callback: resolve `$name` to a value_t (shell v2 §3.4/§3.5).
// One namespace, one sigil: the callback is the single lookup path for
// `$name` in expressions, command arguments, and string interpolation.
// The shell wires this to its scoped binding store (which itself falls
// back to the built-in alias table, returning V_REF reference values);
// fire-time evaluators (logpoint templates) chain their per-fire
// bindings (`$value`/`$addr`/`$size`) in front of the shell store.
//
// Contract: return V_ERROR("no such binding ...") for unknown names —
// V_NONE is a *real* value (`let x = none` must read back as none).
// Returned value_t is OWNED by the caller of expr_eval; the binding fn
// must produce a freshly-allocated value (or a self-contained inline
// value like val_int that needs no free).
typedef value_t (*expr_binding_fn)(void *ud, const char *name);

// Context bundling the bindings a single evaluation needs.
typedef struct expr_ctx {
    struct object *root; // bare paths resolve against this; may be NULL
    expr_binding_fn binding; // $name lookup; may be NULL
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

// Decode + interpolate a raw double-quoted string *body* in one pass
// (shell v2 §3.3). `body` is the text between the quotes with escapes
// NOT yet decoded. Handles `\n \t \r \0 \\ \" \' \$ \xHH` escapes,
// `${EXPR[:FMT]}` splices, and `$name` binding splices. Returns
// V_STRING, or V_ERROR on an unterminated `${` or a failed splice.
// This is also the fire-time evaluator for template-typed arguments
// (logpoint messages): store the raw body, call this per fire.
value_t expr_interpolate_body(const char *body, const expr_ctx_t *ctx);

// Parse a double-quoted string literal at *p (cursor on the opening
// `"`), advancing past the closing quote, then decode + interpolate its
// body via expr_interpolate_body. Returns V_STRING or V_ERROR.
value_t expr_parse_dq_string(const char **p, const expr_ctx_t *ctx);

// Scan a raw dq-string body starting just past the opening quote;
// returns a pointer to the closing quote (skipping `\x` escapes and
// brace-balanced `${…}` regions, which may contain quotes) or NULL.
const char *expr_scan_dq_body(const char *q);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_EXPR_H
