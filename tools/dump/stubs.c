// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// stubs.c
// Linker stubs for emulator-only symbols referenced by the core source
// files we compile into the dump tool.  The real implementations live
// in src/core/debug/debug.c and friends — none of that is meaningful in
// a standalone reverse-engineering tool, so we provide minimal no-ops.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// gs_assert_fail backs the GS_ASSERT macros in common.h.  None of the
// code paths the dump tool exercises should ever fire an assert, so this
// is a hard-abort safety net rather than a real handler.
void gs_assert_fail(const char *expr, const char *file, int line, const char *func, const char *fmt, ...) {
    (void)expr;
    (void)file;
    (void)line;
    (void)func;
    (void)fmt;
    __builtin_trap();
}

// A-trap name lookup is NOT stubbed here any more: the canonical
// macos_atrap_name() now lives in src/core/debug/mac_traps_data.c, beside its
// table, which this tool already links.  The three former copies each
// re-declared the table with a `uint32_t trap` member where the definition has
// `uint16_t` -- C11 6.2.7 undefined behaviour across translation units.
