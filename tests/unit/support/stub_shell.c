// Shell stubs for unit tests.
//
// Provides no-op implementations of the shell entry points so objects
// that reference them (memory.o, system.o, …) link without pulling in
// the full shell/script interpreter. The typed methods the suites do
// exercise (e.g. memory.dump) operate directly on the object model and
// need no shell support.

#include <stdbool.h>
#include <stdint.h>

#include "addr_format.h"

int shell_init(void) {
    return 0;
}

uint64_t shell_dispatch(char *line) {
    (void)line;
    return 0;
}

// memory.o references parse_address (memory.dump resolves symbol-string
// addresses through it). The suites never dump memory, so a stub that
// declines all input is enough to link.
bool parse_address(const char *str, uint32_t *addr_out, addr_space_t *space_out) {
    (void)str;
    (void)addr_out;
    (void)space_out;
    return false;
}
