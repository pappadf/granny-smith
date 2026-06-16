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

// A-trap name lookup.  cpu_disasm.c and annotate_disasm.c both call
// macos_atrap_name() to render `_DoSomething`-style annotations.  The
// data table is in src/core/debug/mac_traps_data.c (already linked); the
// emulator's debug_mac.c provides the full implementation but pulls in
// heavy dependencies.  Mirror tools/disasm/trap_lookup.c verbatim so
// both tools render identical trap names.
extern struct {
    const char *name;
    uint32_t trap;
} macos_atraps[];
extern const size_t macos_atraps_count;

static const char *lookup_atrap(uint16_t trap) {
    for (size_t i = 0; i < macos_atraps_count; i++) {
        if (macos_atraps[i].trap == trap)
            return macos_atraps[i].name;
    }
    return NULL;
}

const char *macos_atrap_name(uint16_t trap) {
    static char buffer[32];
    const char *name;
    if (trap & 0x0800) { // toolbox trap
        if ((name = lookup_atrap(trap & 0xFBFF)))
            return name;
    } else { // OS trap
        if ((name = lookup_atrap(trap)))
            return name;
    }
    snprintf(buffer, sizeof(buffer), "_%04X", trap);
    return buffer;
}
