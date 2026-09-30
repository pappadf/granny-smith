// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// highlight.h
// Syntax classes for a shell line or block (shell.highlight): what a
// console colours as the user types.  See highlight.c.

#ifndef SHELL_HIGHLIGHT_H
#define SHELL_HIGHLIGHT_H

#include "object.h"
#include "value.h"

#include <stdbool.h>

// Spans of `text` as a V_LIST of {start, end, class} maps: UTF-8 byte
// offsets, half-open, ordered and non-overlapping.  Classes: keyword,
// decl, variable, alias, number, string, interp, operator, comment,
// object, method, attribute, enum, unknown.  Text that does not lex gets no
// span; the call never fails.
value_t shell_highlight(const char *text);

// Whether `name` is a shell function (def): such a name at the head of a
// statement is a call, not an unknown path.  Installed by the shell; NULL
// (the default) knows no functions.
void highlight_set_function_probe(bool (*probe)(const char *name));

// Whether `word` is a command (commands.h) whose target is a method now;
// fills `out` with that method's node.  A command word at the head of a
// statement is that method, its arguments checked against its declaration.
// Installed by the shell; NULL (the default) knows no commands.
void highlight_set_command_probe(bool (*probe)(const char *word, node_t *out));

#endif // SHELL_HIGHLIGHT_H
