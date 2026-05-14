// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_var.h
// Typed shell-variable store and ${...} substitution.
//
// Variables are created by `name = expr` in scripts (where `name` is a
// bare identifier that doesn't resolve to any object-tree path) or by
// the legacy --var NAME=VALUE command-line flag (which always stores a
// string).  Values are typed `value_t` — assigning the result of an
// integer expression preserves that as a numeric type, so subsequent
// arithmetic in `${...}` works without round-trip-through-string
// coercion.

#pragma once

#ifndef SHELL_VAR_H
#define SHELL_VAR_H

#include "value.h"

#include <stddef.h>

// Set a shell variable as a string (legacy / --var path).  Stores
// V_STRING internally.  Returns 0 on success.
int shell_var_set(const char *name, const char *value);

// Set a shell variable to a typed value.  Takes ownership of `v` —
// caller must not free it afterwards.  Returns 0 on success.
int shell_var_set_value(const char *name, value_t v);

// Get a shell variable's string repr (returns NULL if undefined or if
// the stored value isn't a string).  Legacy callers that expect a C
// string only see V_STRING entries; numeric variables look "undefined"
// through this API.  New code should use shell_var_get_value.
const char *shell_var_get(const char *name);

// Get a shell variable's typed value.  Returns V_NONE if undefined.
// Caller does not own the returned value; treat it as a borrowed copy
// (use value_dup if you need to keep it past the next set/unset).
value_t shell_var_get_value(const char *name);

// Delete a shell variable (returns 0 on success, -1 if not found).
int shell_var_unset(const char *name);

// Expand ${...} references in a string.  Returns a malloc'd string;
// caller must free.
char *shell_var_expand(const char *input);

// Register the "var" command and seed built-in defaults.
void shell_var_init(void);

#endif // SHELL_VAR_H
