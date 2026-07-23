// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_internal.h
// Private bridge between shell.c and shell_class.c. Not part of the
// public shell API — see shell.h for that. The two files live in the
// same compilation unit boundary (src/core/shell) and share a few
// helpers that shouldn't escape into the wider tree.

#ifndef GS_SHELL_INTERNAL_H
#define GS_SHELL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

// Run a free-form shell line through the v2 script interpreter with
// REPL semantics. Used by the Shell class's `run` method; not
// otherwise reachable. Returns true on success; on failure returns
// false and writes a brief reason into `err_buf` (if non-NULL).
bool shell_internal_dispatch_command(char *line, char *err_buf, size_t err_size);

// The REPL value formatter (§5): scalars, object attribute tables, and
// object-list tables. Used by the script interpreter for interactive
// statement results.
void shell_print_value(const value_t *v);

#ifdef __cplusplus
}
#endif

#endif // GS_SHELL_INTERNAL_H
