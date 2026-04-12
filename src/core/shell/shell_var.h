// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_var.h
// Shell variable store and ${NAME} expansion.

#pragma once

#ifndef SHELL_VAR_H
#define SHELL_VAR_H

#include <stddef.h>

// Set a shell variable (overwrites if it exists)
int shell_var_set(const char *name, const char *value);

// Get a shell variable value (returns NULL if undefined)
const char *shell_var_get(const char *name);

// Delete a shell variable (returns 0 on success, -1 if not found)
int shell_var_unset(const char *name);

// Expand ${NAME} references in a string.
// Returns a malloc'd string with all ${NAME} replaced by their values.
// Undefined variables expand to empty string.
// Caller must free the result.
char *shell_var_expand(const char *input);

// Register the "var" command and seed built-in defaults
void shell_var_init(void);

#endif // SHELL_VAR_H
