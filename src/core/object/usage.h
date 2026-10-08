// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// usage.h
// Usage text for any path: `help <path>` and `shell.usage(path)`.

#ifndef GS_OBJECT_USAGE_H
#define GS_OBJECT_USAGE_H

#include "value.h"

#include <stdbool.h>
#include <stddef.h>

// {signature, arg_spans, text} for `path`: a method's signature (empty for
// an attribute or node), the byte span [start, end) of each argument's
// bracketed form in it, and the full usage text.  VK_ERROR when the path
// does not resolve.
value_t object_usage(const char *path);

// Just the text (what `help <path>` prints), or VK_ERROR.
value_t object_usage_text(const char *path);

// How the shell reads a word that is no path (the object layer cannot ask
// the shell itself): a `def` function, or a command running the method
// whose path it writes to `target`.  Usage of a function is a note; of a
// command, the method's text with a note.  Installed by the shell; NULL
// (the default) knows neither.
typedef enum { USAGE_WORD_NONE, USAGE_WORD_FUNCTION, USAGE_WORD_COMMAND } usage_word_t;
void object_usage_set_word_resolver(usage_word_t (*resolve)(const char *word, char *target, size_t target_size));

#endif // GS_OBJECT_USAGE_H
