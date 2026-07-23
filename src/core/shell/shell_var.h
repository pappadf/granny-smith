// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_var.h
// Scoped shell binding store (shell v2 §3.4/§3.7).
//
// One namespace, one sigil: `$name` resolves through a stack of scopes
// (function frame(s) → script top level → process globals) and then
// falls back to the built-in/user alias table, whose entries surface as
// V_REF reference values (path text, re-resolved per access — §3.5).
// `let` creates in the top scope, `$x =` mutates the innermost holding
// scope, and mutating an undeclared name is a loud error.

#pragma once

#ifndef SHELL_VAR_H
#define SHELL_VAR_H

#include "value.h"

#include <stdbool.h>
#include <stddef.h>

// Initialize the store (creates the global + top-level scopes and the
// default TMP_DIR binding).
void shell_var_init(void);

// `$name` lookup: walk scopes top-down, then the alias table (aliases
// come back as V_REF). Returns an OWNED value; V_ERROR("no such
// binding …") when the name is nowhere. A binding holding a destroyed
// V_OBJECT reads as V_ERROR (§3.5 snapshot semantics).
value_t shell_binding_get(const char *name);

// `let NAME = v` — create (or overwrite) in the current top scope.
// Takes ownership of v. Returns 0, or -1 with a message in err_buf
// (invalid name / table full).
int shell_binding_let(const char *name, value_t v, char *err_buf, size_t err_size);

// `$NAME = v` — mutate the innermost scope holding NAME. Takes
// ownership of v. Returns 0, or -1 if no such binding (v is freed).
int shell_binding_mutate(const char *name, value_t v);

// Lvalue classification for the script layer (`$x = …`):
//   VALUE — a scope binding exists; *value_out borrows its current value
//   ALIAS — no scope binding, but the alias table maps name → path
//   NONE  — unknown everywhere (assignment must error: "use let")
typedef enum {
    SHELL_BINDING_NONE = 0,
    SHELL_BINDING_VALUE,
    SHELL_BINDING_ALIAS,
} shell_binding_kind_t;
shell_binding_kind_t shell_binding_classify(const char *name, const value_t **value_out, const char **alias_path_out);

// Function-call scopes. push returns -1 when the frame cap (16) is hit.
int shell_binding_push_scope(void);
void shell_binding_pop_scope(void);

// Loop-variable support (§3.8): save a duplicate of the top-scope
// binding if present (returns true), and remove a name from the top
// scope only.
bool shell_binding_save_top(const char *name, value_t *saved_out);
void shell_binding_remove_top(const char *name);

// Legacy string API — process-start bindings (`--var FOO=BAR`) and
// internal init code. Stores V_STRING in the global scope.
int shell_var_set(const char *name, const char *value);
const char *shell_var_get(const char *name);

// Walk every scope binding, innermost scope first. Used by `shell.vars`.
typedef bool (*shell_var_iter_fn)(const char *name, const value_t *v, void *ud);
void shell_var_each(shell_var_iter_fn fn, void *ud);

#endif // SHELL_VAR_H
