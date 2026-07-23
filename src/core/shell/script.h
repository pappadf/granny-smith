// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// script.h
// Shell v2 statement parser + interpreter (proposal-shell-control-flow-
// and-functions.md §3). A script is parsed into a statement tree
// (blocks resolved by the line-position rule), then interpreted.
// Expressions are stored as text and evaluated where they appear, at
// statement-execution time — which is what makes `while` conditions
// re-test per iteration.

#pragma once

#ifndef GS_SHELL_SCRIPT_H
#define GS_SHELL_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>

#include "expr.h"
#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct script script_t;
typedef struct script_block script_block_t;

// Parse a complete (possibly multi-line) source into a statement tree.
// Returns NULL on parse error with a one-line message in err_buf.
script_t *script_parse(const char *src, char *err_buf, size_t err_size);
void script_free(script_t *s);

// Execute a parsed script at top level. `interactive` selects REPL
// semantics (non-V_NONE statement results print); scripts print
// nothing implicitly (§5). Returns 0 on success, -1 if the script
// aborted on an error.
int script_exec(script_t *s, bool interactive);

// Parse + execute one submitted REPL chunk (a single statement, or a
// brace-balanced multi-line buffer). Interactive semantics. Returns 0
// on success, -1 on parse/execution error.
int script_run_line(const char *line);

// Parse + execute a whole script source non-interactively (shell.eval,
// shell.script_run, the headless script= runner). Returns 0 / -1.
int script_run_source(const char *src);

// True when `buf` has an unbalanced multi-line `{` (quote-aware depth
// counter) — the REPL should show a continuation prompt and accumulate
// more lines before submitting.
bool script_needs_continuation(const char *buf);

// Platform pump hook: called after every executed statement so the
// platform can drive the scheduler to completion (`scheduler.run N`
// merely schedules a stop event; the platform loop executes it).
// Return true to abort the script cleanly (quit requested).
typedef bool (*script_pump_fn)(void);
void script_set_pump_hook(script_pump_fn fn);

// Ctrl-C for loops: the interpreter checks this once per iteration and
// unwinds with an error (§3.8). Wired to `shell.interrupt`.
void script_interrupt(void);

// Fill an expr_ctx bound to the object root and the shell binding
// store — the standard evaluation context for shell expressions.
void script_expr_ctx(expr_ctx_t *out);

// Execute a function body (owned by the shell_funcs registry) inside an
// already-pushed scope. Returns the function's return value (V_NONE
// when the body falls off the end) or V_ERROR.
value_t script_exec_func_body(script_block_t *body);
void script_block_free(script_block_t *b);

#ifdef __cplusplus
}
#endif

#endif // GS_SHELL_SCRIPT_H
