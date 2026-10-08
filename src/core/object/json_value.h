// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// json_value.h
// Parse a JSON text into a value_t tree (VK_MAP / VK_LIST / VK_STRING / VK_INT /
// VK_FLOAT / VK_BOOL / VK_NONE for null).  The one reader of whole JSON
// documents: machine.boot's config= and the headless config=<file>.

#ifndef GS_OBJECT_JSON_VALUE_H
#define GS_OBJECT_JSON_VALUE_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

// Parse `text` (the whole of it: trailing non-blank text is an error) into
// *out, which the caller owns.  On false *out is untouched and `err` names
// the byte offset and what was expected there.
bool json_value_parse(const char *text, value_t *out, char *err, size_t errlen);

#endif // GS_OBJECT_JSON_VALUE_H
