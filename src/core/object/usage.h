// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// usage.h
// Usage text for any path: `help <path>` and `shell.usage(path)`.

#ifndef GS_OBJECT_USAGE_H
#define GS_OBJECT_USAGE_H

#include "value.h"

// {signature, arg_spans, text} for `path`: a method's signature (empty for
// an attribute or node), the byte span [start, end) of each argument's
// bracketed form in it, and the full usage text.  V_ERROR when the path
// does not resolve.
value_t object_usage(const char *path);

// Just the text (what `help <path>` prints), or V_ERROR.
value_t object_usage_text(const char *path);

#endif // GS_OBJECT_USAGE_H
