// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// stubs.c
// Minimal stubs for symbols referenced by cpu_disasm.c but not needed
// in the standalone disasm tool.

#include <stdint.h>

// gs_assert_fail referenced by common.h / GS_ASSERT macros — should never fire
// during pure disassembly, but we provide a stub to satisfy the linker.
void gs_assert_fail(const char *expr, const char *file, int line, const char *func, const char *fmt, ...) {
    (void)expr;
    (void)file;
    (void)line;
    (void)func;
    (void)fmt;
    // In this tool the assert should never trigger
    __builtin_trap();
}
