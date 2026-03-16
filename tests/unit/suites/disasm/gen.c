// Regenerate disasm.txt using the same dependencies as the disasm test suite
#include "cpu.h"
#include "harness.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // Initialize test harness (creates CPU and memory for us)
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }

    for (unsigned op = 0; op < 65536; op++) {
        uint16_t words[16];
        // fill extension words with 0x4E71 (NOP) so constants are non-trivial;
        // test.c must use the same fill — see DISASM_EXT_FILL there
        for (int i = 0; i < 16; i++)
            words[i] = 0x4E71;
        words[0] = (uint16_t)op;
        char buf[512] = {0};
        int used = cpu_disasm(words, buf);
        printf("%d", used);
        // opcode in lowercase hex, extension words in uppercase (matches original)
        printf("\t0x%04x", words[0]);
        for (int i = 1; i < used; i++)
            printf("\t0x%04X", words[i]);
        printf("\t%s\n", buf);
    }
    return 0;
}
