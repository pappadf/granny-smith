// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_symbol.h
// Unified symbol resolver: CPU registers, FPU registers, MMU registers,
// and Mac low-memory globals. Used by the command framework.

#pragma once

#ifndef CMD_SYMBOL_H
#define CMD_SYMBOL_H

#include "cmd_types.h"

// Resolve a $-prefixed symbol name to its address, value, and kind.
// The name should NOT include the '$' prefix.
// Returns true if the symbol was resolved.
bool resolve_symbol(const char *name, struct resolved_symbol *out);

// Complete symbol names matching a prefix.
// The prefix should NOT include the '$'.
void complete_symbols(const char *prefix, struct completion *out);

#endif // CMD_SYMBOL_H
