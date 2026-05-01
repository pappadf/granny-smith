// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// parse.h
// Unified literal parser. See proposal-module-object-model.md §4.2 and
// §2.4 for the literal grammar.
//
// Accepted forms:
//   integers:  42, 0x1234, 0b1010, 0o17, 0d100, $1234, with optional
//              underscore digit separators (1_000_000) and `u`/`i`
//              suffix (`100u` → V_UINT, `100i` → V_INT).
//   floats:    1.0, 1e6, .5, 1.5e-3 (also 0x1.8p+1 hex floats per
//              strtod). A bare `42` without `.` or `e` parses to int.
//   booleans:  true, false, on, off, yes, no
//   strings:   "..." with backslash escapes (\n \t \r \\ \" \xHH \0)
//              and `${...}` interpolation regions kept as literal
//              characters (the expression evaluator handles them).
//   bytes:     "hello":bytes  or  0xDEAD_BEEF:N
//   enum tag:  bare identifier resolved against a caller-supplied table
//
// The parser does **not** resolve paths or aliases — that is the
// expression parser's job.

#ifndef GS_OBJECT_PARSE_H
#define GS_OBJECT_PARSE_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward.
struct parse_pos;

// Parse a single literal value starting at *p. On success, advances *p
// past the literal and returns the parsed value. On failure, returns a
// V_ERROR with a descriptive message and leaves *p on the failing
// position. Trailing whitespace is not consumed.
//
// `enum_table`/`n_enum` may be NULL/0; when supplied, a bare identifier
// matching one of the entries parses as a V_ENUM with that index.
value_t parse_literal(const char **p, const char *const *enum_table, size_t n_enum);

// Parse a complete literal occupying the whole string `s` (with any
// surrounding whitespace). Returns V_ERROR if any trailing characters
// remain past the literal.
value_t parse_literal_full(const char *s, const char *const *enum_table, size_t n_enum);

// Parse just an integer literal. Convenience wrapper used by the
// expression lexer; on success consumes the digits and any underscore
// separators, applies the `u`/`i` suffix, and returns V_UINT or V_INT.
// Returns V_ERROR if the cursor is not on an integer literal.
value_t parse_integer_literal(const char **p);

// Parse a quoted "..." string literal. On entry *p must point at the
// opening quote. On success advances *p past the closing quote and
// returns V_STRING. The body is kept verbatim for the expression
// evaluator to handle ${...} interpolation; only the standard escapes
// are decoded.
value_t parse_string_literal(const char **p);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_PARSE_H
