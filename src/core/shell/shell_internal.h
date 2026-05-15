// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_internal.h
// Private bridge between shell.c and shell_class.c. Not part of the
// public shell API — see shell.h for that. The two files live in the
// same compilation unit boundary (src/core/shell) and share a few
// helpers that shouldn't escape into the wider tree.

#ifndef GS_SHELL_INTERNAL_H
#define GS_SHELL_INTERNAL_H

#include "cmd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Run a free-form shell line through the typed path-form dispatcher
// (the static `dispatch_command()` in shell.c). Used by the Shell
// class's `run` method; not otherwise reachable. `line` is mutated by
// the tokeniser; `res` is filled with the dispatch outcome.
void shell_internal_dispatch_command(char *line, struct cmd_result *res);

#ifdef __cplusplus
}
#endif

#endif // GS_SHELL_INTERNAL_H
