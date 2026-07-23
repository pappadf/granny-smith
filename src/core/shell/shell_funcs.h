// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_funcs.h
// User-defined script functions (`def`, shell v2 §3.10). The registry
// owns each function's parsed body; functions are callable from command
// form (script.c), call form in any expression (via the expr function
// hook), and are surfaced for introspection/removal as attached entry
// objects under `shell.functions`.

#pragma once

#ifndef SHELL_FUNCS_H
#define SHELL_FUNCS_H

#include <stddef.h>

#include "object.h"
#include "script.h"
#include "value.h"

typedef struct script_func script_func_t;

// Define (or redefine) a function. Takes ownership of `body`; `params`
// strings are duplicated. Returns 0, or -1 with a message in err_buf.
int shell_func_define(const char *name, char **params, int n_params, script_block_t *body, char *err_buf,
                      size_t err_size);

// Look up a function by flat name. NULL if absent.
script_func_t *shell_func_find(const char *name);

// Call: bind positional + named arguments to the declared parameters,
// push a scope (16-frame recursion cap), run the body, pop, and return
// the function's value (V_NONE when the body falls off the end).
value_t shell_func_call(script_func_t *f, int argc, const value_t *argv, int named_n, const named_arg_t *named);

// Remove a function by name. Returns 0, -1 if absent.
int shell_func_remove(const char *name);

// Attach the `shell.functions` container under the shell object and
// install the expression-layer call hook. Called from root_install.
void shell_funcs_install(struct object *shell_obj);
void shell_funcs_uninstall(void);

#endif // SHELL_FUNCS_H
