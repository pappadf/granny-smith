// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// stubs.c
// Minimal stubs for symbols referenced by cpu_disasm.c and the shared
// annotate_disasm.c but not needed in the standalone disasm tool.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// aux_syscall_name() is called by the shared annotate_disasm.c when the
// A/UX syscall annotation pass fires (TRAP #0).  The standalone disasm
// tool never produces A/UX-aware output so a NULL stub is sufficient —
// the annotator drops the annotation silently and renders the raw TRAP.
const char *aux_syscall_name(uint32_t num) {
    (void)num;
    return NULL;
}

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

// re_annotate_disasm_write (used by the `re` orchestrator) references
// these.  The standalone tool's main loop never calls them — it uses
// only re_annotate_branch_destination — so the implementations stay
// minimal stubs that satisfy the linker.

const char *debug_mac_lookup_global_name(uint32_t address) {
    (void)address;
    return NULL;
}

// symbols.c lives next to annotate_disasm.c in src/core/re and provides
// a real implementation of these.  The standalone tool doesn't link
// symbols.c (it never builds a symbol table), so the linker resolves
// the references via these stubs.
struct re_symbols;
struct re_symbol;
void re_symbols_init(struct re_symbols *t) {
    (void)t;
}
void re_symbols_free(struct re_symbols *t) {
    (void)t;
}
void re_symbols_add(struct re_symbols *t, int16_t code_id, uint32_t addr, const char *name, const char *source) {
    (void)t;
    (void)code_id;
    (void)addr;
    (void)name;
    (void)source;
}
const struct re_symbol *re_symbols_find(const struct re_symbols *t, int16_t code_id, uint32_t addr) {
    (void)t;
    (void)code_id;
    (void)addr;
    return NULL;
}
