// Disassembler test - validates cpu_disasm() against a corpus of expected outputs.

#include "cpu.h"
#include "test_assert.h"
#include "harness.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward from original monolithic test (trimmed & adapted):
static void trim_line_end(char *s) {
    if (!s)
        return;
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

static int run_all(void) {
    FILE *f = fopen("disasm.txt", "r");
    if (!f) {
        const char *force = getenv("REQUIRE_DISASM_CORPUS");
        if (force && *force) {
            fprintf(stderr, "[disasm] disasm.txt missing and REQUIRE_DISASM_CORPUS set -> failing.\n");
            return 0; // hard fail
        }
        // If the corpus file isn't present (common in lightweight dev runs),
        // treat as a skipped test rather than hard failure so the rest of the
        // unit suite can still succeed. Report and return success.
        fprintf(stderr, "[disasm] disasm.txt not present; skipping full corpus test.\n");
        return 1; // skip = success
    }
    char line[512];
    int executed = 0;
    int line_no = 0;
    while (fgets(line, sizeof line, f)) {
        ++line_no;
        trim_line_end(line);
        if (!line[0])
            continue; // skip blank
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;

        char *endp = NULL;
        long n_words_l = strtol(p, &endp, 10);
        if (p == endp || n_words_l <= 0 || n_words_l > 16) {
            fprintf(stderr, "[disasm] bad count l%d\n", line_no);
            fclose(f);
            return 0;
        }
        int n_words = (int)n_words_l;
        p = endp;
        uint16_t words[16];
        memset(words, 0, sizeof(words));
        for (int i = 0; i < n_words; i++) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) { fprintf(stderr, "[disasm] premature EOL l%d\n", line_no); fclose(f); return 0; }
            char *wend = NULL;
            unsigned long wv = strtoul(p, &wend, 16);
            if (p == wend || wv > 0xFFFFUL) { fprintf(stderr, "[disasm] bad word l%d\n", line_no); fclose(f); return 0; }
            words[i] = (uint16_t)wv;
            p = wend;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) { fprintf(stderr, "[disasm] missing asm l%d\n", line_no); fclose(f); return 0; }
        const char *expected = p;
        char got[512];
        memset(got, 0, sizeof(got));
        int used = cpu_disasm(words, got);
        if (used != n_words || strcmp(expected, got) != 0) {
            fprintf(stderr, "[disasm] mismatch l%d n=%d used=%d\n  expected: %s\n  got     : %s\n", line_no, n_words, used, expected, got);
            fclose(f);
            return 0;
        }
        executed++;
    }
    fclose(f);
    fprintf(stderr, "[disasm] executed %d lines (full)\n", executed);
    // Expect the full 65536 line coverage; tolerate >= 65536 in case of header lines (not expected currently).
    if (executed < 65536) {
        fprintf(stderr, "[disasm] expected 65536 lines, saw %d\n", executed);
        return 0;
    }
    return 1;
}

TEST(disasm_all) { ASSERT_TRUE(run_all()); }

int main(void) {
    // Initialize test harness (creates CPU and memory for us)
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }
    
    RUN(disasm_all);
    
    test_harness_destroy(ctx);
    return 0;
}
