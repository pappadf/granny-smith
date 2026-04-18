// Disassembler test - validates cpu_disasm() against a corpus of expected outputs.

#include "cpu.h"
#include "harness.h"
#include "test_assert.h"
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
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            continue;

        char *endp = NULL;
        long n_words_l = strtol(p, &endp, 10);
        if (p == endp || n_words_l <= 0 || n_words_l > 16) {
            fprintf(stderr, "[disasm] bad count l%d\n", line_no);
            fclose(f);
            return 0;
        }
        int n_words = (int)n_words_l;
        p = endp;
// cpu_disasm may peek at extension words beyond what it claims to
// consume, so unclaimed slots must use the same fill as gen.c used
// when producing disasm.txt — otherwise edge-case opcodes diverge.
#define DISASM_EXT_FILL 0x4E71
        uint16_t words[16];
        for (int w = 0; w < 16; w++)
            words[w] = DISASM_EXT_FILL;
        for (int i = 0; i < n_words; i++) {
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p) {
                fprintf(stderr, "[disasm] premature EOL l%d\n", line_no);
                fclose(f);
                return 0;
            }
            char *wend = NULL;
            unsigned long wv = strtoul(p, &wend, 16);
            if (p == wend || wv > 0xFFFFUL) {
                fprintf(stderr, "[disasm] bad word l%d\n", line_no);
                fclose(f);
                return 0;
            }
            words[i] = (uint16_t)wv;
            p = wend;
        }
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p) {
            fprintf(stderr, "[disasm] missing asm l%d\n", line_no);
            fclose(f);
            return 0;
        }
        const char *expected = p;
        char got[512];
        memset(got, 0, sizeof(got));
        int used = cpu_disasm(words, got);
        if (used != n_words || strcmp(expected, got) != 0) {
            fprintf(stderr, "[disasm] mismatch l%d n=%d used=%d\n  expected: %s\n  got     : %s\n", line_no, n_words,
                    used, expected, got);
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

TEST(disasm_all) {
    ASSERT_TRUE(run_all());
}

// Inline 68020+ full-extension-word cases. The 65536-line corpus is
// filled with 0x4E71 which has bit 8 clear (brief extension), so the
// full-extension-word code path is only exercised from this test.
//
// Each case is a fixed-length word vector + expected assembly text. The
// last "used" count is checked so we catch EA word-count bugs.
struct full_ext_case {
    const char *desc;
    uint16_t words[8];
    int n_words;
    int expected_used;
    const char *expected;
};

static const struct full_ext_case full_ext_cases[] = {
    // From note 16: MOVE.B $11004DA6(,D7.W*1),D1
    // opcode = 0x1230 (MOVE.B src=mode 6/reg 0, dst=mode 0/reg 1)
    // ext    = 0x71B0 : D7.W*1 base=A0, full, BS=1, IS=0, BD=long, iis=0
    // BD     = 0x11004DA6
    {"BS=1, BD.L, Xn (D7.W*1)",                      {0x1230, 0x71B0, 0x1100, 0x4DA6}, 4, 4, "MOVE.B\t$11004DA6(,D7.W*1),D1"  },

    // BS=0, IS=1, BD.L -> 32-bit-displacement addressing: $12345678(A0)
    // ext bits: D/A=0 Xn=0 W/L=0 scale=0 full=1 BS=0 IS=1 BDsize=3 iis=0
    //           = 0x100 | 0x40 | 0x30 = 0x0170
    {"BS=0, IS=1, BD.L (pure 32-bit disp on An)",    {0x1030, 0x0170, 0x1234, 0x5678}, 4, 4, "MOVE.B\t$12345678(A0),D0"       },

    // BS=0, IS=0, BD.W, iis=0 -> $1234(A0,D0.L*4)
    // ext bits: D/A=0 Xn=0 W/L=1 scale=2 full=1 BS=0 IS=0 BDsize=2 iis=0
    //           = 0x800 | 0x400 | 0x100 | 0x20 = 0x0D20
    {"BS=0, IS=0, BD.W, iis=0 (no memory indirect)", {0x1030, 0x0D20, 0x1234},         3, 3, "MOVE.B\t$1234(A0,D0.L*4),D0"    },

    // BS=0, IS=0, BD.L, iis=0 -> $11004DA6(A0,D7.W*1)
    // ext = 0x7130 : D7.W*1, full, BS=0, IS=0, BD=long, iis=0
    {"BS=0, IS=0, BD.L, iis=0",                      {0x1030, 0x7130, 0x1100, 0x4DA6}, 4, 4, "MOVE.B\t$11004DA6(A0,D7.W*1),D0"},

    // Pre-indexed memory indirect with word OD: ([$1234,A0,D0.W*1],$5678)
    // ext = 0x0122 : D0.W*1, full, BS=0, IS=0, BD=word, iis=2 (preindex + OD.W)
    {"preindex memory indirect, BD.W, OD.W",
     {0x1030, 0x0122, 0x1234, 0x5678},
     4,                                                                                   4,
     "MOVE.B\t([$1234,A0,D0.W*1],$5678),D0"                                                                                   },

    // Post-indexed memory indirect with long OD: ([$1234,A0],D0.W*1,$56781234)
    // ext = 0x0127 : D0.W*1, full, BS=0, IS=0, BD=word, iis=7 (postindex + OD.L)
    {"postindex memory indirect, BD.W, OD.L",
     {0x1030, 0x0127, 0x1234, 0x5678, 0x1234},
     5,                                                                                   5,
     "MOVE.B\t([$1234,A0],D0.W*1,$56781234),D0"                                                                               },

    // Memory indirect, IS=1 (no index), BD.L, null OD: ([$11004DA6,A0])
    // ext bits: D/A=0 Xn=0 W/L=0 scale=0 full=1 BS=0 IS=1 BDsize=3 iis=1
    //           = 0x100 | 0x40 | 0x30 | 0x1 = 0x0171
    {"IS=1 memory indirect, BD.L",                   {0x1030, 0x0171, 0x1100, 0x4DA6}, 4, 4, "MOVE.B\t([$11004DA6,A0]),D0"    },

    // BS=1 + IS=1 + BD.L + iis=0 -> absolute-like: $11004DA6
    // ext = 0x01F0 with BS=1 -> bit7=1: ext = 0x01F0 | 0x80 = 0x01F0 ^= hmm
    // Let me recompute: bits: d/a=0, xn=unused=0, wl=0, scale=0, full=1, bs=1, is=1, bdsize=11 (long), 0, iis=000
    // -> 0000 0001 1111 0000 = 0x01F0 with BS bit (0x80) -> 0x01F0 | 0x0080 = 0x01F0? no that's wrong
    // 0x01F0 = 0000 0001 1111 0000: bit 8=1 (full), bit 7=1 (BS), bit 6=1 (IS), bits 5-4=11 (BD.L). So that's already
    // BS=1.
    {"BS=1, IS=1, BD.L (absolute)",                  {0x1030, 0x01F0, 0x1100, 0x4DA6}, 4, 4, "MOVE.B\t$11004DA6,D0"           },

    // PC-relative full ext, mode 7 reg 3: $1234(PC,D0.W*1)
    // MOVE.B (d,PC,Xn),D0 = opcode 0x103B
    // ext = 0x0120 : D0.W*1, full, BS=0, IS=0, BD=word, iis=0
    {"PC-rel full ext, BD.W, Xn",                    {0x103B, 0x0120, 0x1234},         3, 3, "MOVE.B\t$1234(PC,D0.W*1),D0"    },

    // MOVE with full-ext source and mode-6 brief dest -- verifies that
    // SRC_EXT_WORDS() correctly advances the dst fetch past the source's
    // BD/OD words. Source = $11004DA6(A0,D7.W*1), dst = $10(A1,D2.W*1).
    // MOVE.L opcode: 00 size(10=L) dstreg dstmode srcmode srcreg
    //   = 0010 001 110 110 000 = 0x23B0
    // src ext = 0x7130 (full, BS=0, IS=0, BD=long, iis=0, D7.W*1)
    // src BD  = 0x1100 0x4DA6
    // dst ext = 0x2010 (brief, D2.W*1, disp=0x10)
    {"MOVE.L with full-ext source, brief dest",
     {0x23B0, 0x7130, 0x1100, 0x4DA6, 0x2010},
     5,                                                                                   5,
     "MOVE.L\t$11004DA6(A0,D7.W*1),$10(A1,D2.W*1)"                                                                            },
};

TEST(disasm_full_ext_words) {
    int fail = 0;
    for (size_t i = 0; i < sizeof(full_ext_cases) / sizeof(full_ext_cases[0]); i++) {
        const struct full_ext_case *c = &full_ext_cases[i];
        uint16_t buf_words[16];
        for (int w = 0; w < 16; w++)
            buf_words[w] = 0;
        for (int w = 0; w < c->n_words; w++)
            buf_words[w] = c->words[w];
        char got[512];
        memset(got, 0, sizeof(got));
        int used = cpu_disasm(buf_words, got);
        if (used != c->expected_used || strcmp(got, c->expected) != 0) {
            fprintf(stderr,
                    "[disasm_full_ext] case %zu (%s) mismatch\n"
                    "  expected: used=%d %s\n"
                    "  got     : used=%d %s\n",
                    i, c->desc, c->expected_used, c->expected, used, got);
            fail = 1;
        }
    }
    ASSERT_TRUE(fail == 0);
}

int main(void) {
    // Initialize test harness (creates CPU and memory for us)
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }

    RUN(disasm_all);
    RUN(disasm_full_ext_words);

    test_harness_destroy(ctx);
    return 0;
}
