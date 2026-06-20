// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// symbols.h
// Per-segment symbol table for the `re` disassembler.  Accumulates
// addresses-of-interest from the four annotation passes (MacsBug name
// trailers, CODE 0 jump-table entries, low-memory global references,
// and the function-boundary heuristic) so the writer can emit labels
// inline and a consolidated symbols.txt at the top of re.dump's output.

#pragma once

#ifndef GS_RE_SYMBOLS_H
#define GS_RE_SYMBOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Lightweight ASCII tag for the symbol's origin.  Kept as a static
// string pointer rather than an enum so symbols.txt is readable
// without an external legend.
#define RE_SYMSRC_MACSBUG   "macsbug"
#define RE_SYMSRC_JUMPTABLE "jumptable"
#define RE_SYMSRC_BOUNDARY  "boundary"

typedef struct re_symbol {
    uint32_t addr; // address local to the segment (post-header offset)
    char *name; // owned
    const char *source; // pointer to a static label like RE_SYMSRC_MACSBUG
    int16_t code_id; // owning CODE resource id (for symbols.txt cross-listing)
} re_symbol_t;

typedef struct re_symbols {
    re_symbol_t *items;
    size_t count;
    size_t capacity;
} re_symbols_t;

// Construct/destroy an empty table.
void re_symbols_init(re_symbols_t *t);
void re_symbols_free(re_symbols_t *t);

// Add a (addr, name, source) entry.  Duplicates against an existing
// addr+name pair are silently ignored.  `name` is copied; `source` is
// borrowed (typically an RE_SYMSRC_* string literal).
void re_symbols_add(re_symbols_t *t, int16_t code_id, uint32_t addr, const char *name, const char *source);

// Find the first symbol at exactly `addr`, or NULL.  Used by the writer
// to emit a label line before the instruction at that address.
const re_symbol_t *re_symbols_find(const re_symbols_t *t, int16_t code_id, uint32_t addr);

// Write a flat symbols.txt to `fp`.  Sorted by (code_id, addr).
void re_symbols_write_txt(const re_symbols_t *t, FILE *fp);

#endif // GS_RE_SYMBOLS_H
