// Debugger stubs for unit tests
// Provides no-op implementations of debugger functions.

#include <stdint.h>
#include <stddef.h>

// Forward declarations
struct debugger;

void debugger_init(struct debugger *debug) {
    (void)debug;
}

int debug_break_and_trace(void) {
    return 0;
}

int debugger_disasm(char *buf, uint32_t addr) {
    (void)addr;
    if (buf) buf[0] = '\0';
    return 0;
}
