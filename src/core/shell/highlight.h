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

#endif // SHELL_HIGHLIGHT_H
