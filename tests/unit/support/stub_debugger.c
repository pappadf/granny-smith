// Debugger stubs for unit tests
// Provides no-op implementations of debugger functions.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations
struct debugger;

void debugger_init(struct debugger *debug) {
    (void)debug;
}

int debug_break_and_trace(void) {
    return 0;
}

int debugger_disasm(char *buf, size_t buf_size, uint32_t addr) {
    (void)addr;
    if (buf && buf_size > 0)
        buf[0] = '\0';
    return 0;
}

// Identity translation stub (real impl in src/core/debug/addr_format.c).
// Referenced by the 68K main-CPU debug-if adapter in cpu.c.
uint32_t debug_translate_address(uint32_t logical_addr, bool *is_identity, bool *tt_hit, bool *valid) {
    if (is_identity)
        *is_identity = true;
    if (tt_hit)
        *tt_hit = false;
    if (valid)
        *valid = true;
    return logical_addr;
}

// Exception trace ring stub (real impl in src/core/debug/debug.c).
// Unit tests don't link debug.c, but the bus-error paths in cpu_internal.h
// reference this symbol — provide a no-op so they link.
void exc_trace_record(uint32_t vector, uint32_t faulting_pc, uint32_t saved_pc, uint32_t fault_addr, uint32_t rw,
                      uint32_t vbr, uint16_t sr, uint16_t format_frame, int double_fault_kind) {
    (void)vector;
    (void)faulting_pc;
    (void)saved_pc;
    (void)fault_addr;
    (void)rw;
    (void)vbr;
    (void)sr;
    (void)format_frame;
    (void)double_fault_kind;
}
