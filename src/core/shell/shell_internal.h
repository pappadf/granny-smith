// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_internal.h
// Private surface of shell.c for the rest of src/core/shell (the script
// interpreter prints its REPL results through it). Not part of the
// public shell API — see shell.h for that.

#ifndef GS_SHELL_INTERNAL_H
#define GS_SHELL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

// The REPL value formatter: scalars, object attribute tables, and
// object-list tables. Used by the script interpreter for interactive
// statement results.
void shell_print_value(const value_t *v);

#ifdef __cplusplus
}
#endif

#endif // GS_SHELL_INTERNAL_H
