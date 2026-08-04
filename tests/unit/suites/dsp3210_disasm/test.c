/*
 * test_dsp3210.c — unit tests for the DSP3210 reference disassembler.
 *
 * Vectors come from three sources:
 *   1. Worked encoding examples in the DSP3210 Information Manual's
 *      per-instruction pages (chapter 4.6) — the words are either copied
 *      verbatim or rebuilt from the printed field values with the
 *      encoders below.  (Two of the manual's examples contain printing
 *      errors — the OR example encodes r15 with the pc code, and the
 *      SHIFT LEFT example shows the >> F-code — those are encoded here
 *      per Table 10-2, which the manual's own CALL/IRETURN/DO examples
 *      corroborate.)
 *   2. ROM-verified words from the 840av_660av dossier (docs/dsp3210.md
 *      §1.5.5), e.g. 9CFA2817 = "r22 = *r5++".
 *   3. The illegal-opcode list of IM §7.5.3.2.
 */

#include "dsp3210_disasm.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;

/* ---- encoders (mirror IM Table 10-1 field layouts) ---------------- */

/* DA multiply/accumulate: fmt (1-3), m, f, s, n, x, y, z */
static uint32_t da(unsigned fmt, unsigned m, unsigned f, unsigned s, unsigned n, unsigned x, unsigned y, unsigned z) {
    return (fmt << 29) | (m << 26) | (f << 24) | (s << 23) | (n << 21) | (x << 14) | (y << 7) | z;
}

/* DA special function: g, n, y, z */
static uint32_t sf(unsigned g, unsigned n, unsigned y, unsigned z) {
    return (0x0Fu << 27) | (g << 23) | (n << 21) | (y << 7) | z;
}

/* CA ALU register form (6a/6b) */
static uint32_t alu_r(unsigned e, unsigned f, unsigned rd, unsigned rs1, unsigned c, unsigned rs2) {
    return (e << 31) | (0x0Cu << 25) | (f << 21) | (rd << 16) | (rs1 << 11) | (c << 5) | rs2;
}

/* CA ALU immediate form (6c/6d) */
static uint32_t alu_i(unsigned e, unsigned f, unsigned rd, unsigned n16) {
    return (e << 31) | (0x0Du << 25) | (f << 21) | (rd << 16) | (n16 & 0xFFFFu);
}

/* ---- test harness -------------------------------------------------- */

static void expect_at(uint32_t word, uint32_t addr, const char *want, int want_status) {
    dsp3210_insn ins;

    checks++;
    dsp3210_disassemble(word, addr, &ins);
    if (want && strcmp(ins.text, want) != 0) {
        printf("FAIL %08x: got  \"%s\"\n              want \"%s\"\n", (unsigned)word, ins.text, want);
        failures++;
        return;
    }
    if (ins.status != want_status) {
        printf("FAIL %08x: status %s, want %s (\"%s\")\n", (unsigned)word, dsp3210_status_name(ins.status),
               dsp3210_status_name(want_status), ins.text);
        failures++;
    }
}

static void expect(uint32_t word, const char *want) {
    expect_at(word, 0, want, DSP3210_OK);
}

static void expect_target(uint32_t word, uint32_t addr, const char *want, uint32_t target) {
    dsp3210_insn ins;

    checks++;
    dsp3210_disassemble(word, addr, &ins);
    if (strcmp(ins.text, want) != 0) {
        printf("FAIL %08x: got  \"%s\"\n              want \"%s\"\n", (unsigned)word, ins.text, want);
        failures++;
        return;
    }
    if (!ins.has_target || ins.target != target) {
        printf("FAIL %08x: target %08x (has=%d), want %08x\n", (unsigned)word, (unsigned)ins.target, ins.has_target,
               (unsigned)target);
        failures++;
    }
}

int main(void) {
    /* ---- control (formats 0b/1b, 3a, 8a) — IM GOTO/NOP/IRETURN ---- */
    expect(0x80000000, "nop");
    expect(0x80210000, "goto r1"); /* IM GOTO example */
    expect(0x80410800, "if (pl) goto r1+0x800"); /* IM GOTO-COND */
    expect(0x803E0000, "ireturn"); /* IM IRETURN */
    expect(0x80340000, "goto r18"); /* IM RETURN (r18) */
    expect_target(0x80AF0010, 0x100, "if (eq) goto pc+0x10", 0x118);
    expect_target(0x802F0000, 0x100, "goto pc", 0x108);
    expect(0x0C22FF55, "if (r1-- >= 0) goto r2-0xab"); /* IM GOTO-LOOP */
    expect_target(0xA0001234, 0, "goto 0x1234", 0x1234);
    expect(0xA0220000, "goto r2+0x10000"); /* 8a, NE=1 */

    /* ---- call (4a, 8c) — IM CALL example ---- */
    expect_target(0x130F0111, 0x200, "call pc+0x111 (r20)", 0x319);
    expect_target(0x10200040, 0, "call 0x40 (r1)", 0x40);
    expect_target(0xE2583456, 0, "call 0x123456 (r20)", 0x123456);

    /* ---- do (3b/3c) — IM DO example ---- */
    expect(0x8E002018, "do 4, r20");
    expect(0x8C001064, "do 2, 100");
    expect(0x8D00080A, "dolock 1, 10");
    expect(0x8E800003, "doblock r3");

    /* ---- shift-or (4b) — dossier-verified words ---- */
    expect(0x93405003, "r22 = r0 <<| 0x5003");
    expect(0x90425003, "r2 = r2 <<| 0x5003");
    expect(0x9021ABCD, "r1 = r1 <<| 0xabcd");

    /* ---- 3-operand add / set (5a/5b) — IM ADD/SET examples ---- */
    expect(0x15610020, "r11 = (short) r1 + 0x20");
    expect(0x95610020, "r11 = r1 + 0x20");
    expect(0x14400800, "r2 = (short) 0x800"); /* IM SET */
    expect(0x95610000, "r11 = r1");

    /* ---- ALU register forms (6a/6b) — IM chapter 4.6 examples ---- */
    expect(0x98627024, "r2 = r14 # r4"); /* ADD-CARRY REV */
    expect(0x19C10902, "if (cc) r1 = (short) r1 & r2"); /* AND */
    expect(0x18C52023, "r5 = (short) r4 &~ r3"); /* AND-COMPLEMENT */
    expect(0x99E0282D, "r5 & r13"); /* BIT TEST */
    expect(0x99021D84, "if (ir0c) r2 = r3 ^ r4"); /* EXCLUSIVE OR */
    expect(alu_r(0, 0, 5, 5, 7, 23), "if (vs) r5 = (short) r5 + 1");
    expect(0x99653126, "if (cs) r5 = r6 <<<1"); /* ROTATE LEFT */
    expect(0x99253126, "if (cs) r5 = r6 >>>1"); /* ROTATE RIGHT */
    expect(0x988A0065, "if (mi) r10 = -r5"); /* SUBTRACT (4) */
    expect(0x99A4102D, "r4 = r2 $>> r13"); /* SHIFT R-ARITH */
    expect(alu_r(0, 1, 3, 2, 20, 23), "if (ane) r3 = (short) r2 << 1");
    expect(alu_r(1, 0, 3, 7, 1, 0), "r3 = r7"); /* assignment */
    expect(alu_r(1, 0, 3, 7, 1, 7), "r3 = r7 * 2"); /* multiply by 2 */
    expect(alu_r(1, 0, 25, 25, 1, 23), "sp = sp++"); /* INCR sp by 4 */
    expect(alu_r(1, 0, 25, 25, 1, 22), "sp = sp--");
    expect(alu_r(1, 0, 4, 9, 1, 22), "r4 = r9 - 1"); /* DECR */
    expect(alu_r(1, 4, 4, 9, 1, 4), "r4 = r9 - r4");
    expect_at(alu_r(1, 5, 1, 2, 1, 3), 0, NULL, DSP3210_RESERVED);

    /* ---- ALU immediate forms (6c/6d) ---- */
    expect(0x9AE1000F, "r1 - 0xf"); /* IM COMPARE */
    expect(0x9B510055, "r15 = r15 | 0x55"); /* IM OR (fixed) */
    expect(0x9B82001E, "r2 = r2 >> 30"); /* IM SHIFT RIGHT */
    expect(alu_i(1, 4, 2, 5), "r2 = r2 - 0x5");
    expect(alu_i(1, 2, 2, 5), "r2 = 0x5 - r2");
    expect(alu_i(0, 8, 3, 0xFFFF), "r3 = (short) r3 ^ -0x1");
    expect(alu_i(1, 15, 6, 0x0080), "r6 & 0x80"); /* bit test */

    /* ---- moves (7a-7d) — IM LOAD/STORE examples + dossier words ---- */
    expect(0x1C4A00AA, "r10 = (ushort) *0xaa"); /* IM LOAD (2) */
    expect(0x1CE100AA, "r1 = *0xaa");
    expect(0x1D651234, "*0x1234 = (short) r5");
    expect(0x9CE41017, "r4 = *r2++"); /* dossier */
    expect(0x9CE41012, "r4 = *r2++r16");
    expect(0x9D041817, "*r3++ = (byte) r4"); /* dossier */
    expect(0x9CFA2817, "r22 = *r5++"); /* dossier */
    expect(0x9DE00000, "*r0 = r0"); /* dossier */
    expect(0x9D830817, "*r1++ = (hbyte) r3"); /* IM STORE */
    expect(0x9E681000, "emr = (short) *r2"); /* IM LOAD-IOR */
    expect(0x9F600800, "*r1 = (short) ps"); /* IM STORE-IOR */
    expect(0x9D61040C, "pcw = (short) r1"); /* dossier */
    expect(0x9CE7040E, "r7 = dauc");
    expect(0x9C01040E, "r1 = (byte) dauc"); /* seen in dspf */

    /* ---- spc pseudo-instructions (dossier-verified words) ---- */
    expect(0x9DE0040A, "waiti");
    expect(0x9D60040A, "bkpt");
    expect(0x9D00040A, "sftrst");

    /* ---- 24-bit immediate load (8b) — IM SET24 example ---- */
    expect(0xC1020000, "r2 = (ushort24) 0x80000");
    expect(0xC0111234, "r15 = (ushort24) 0x1234"); /* r15 = code 17 */

    /* ---- DA multiply/accumulate — IM chapter 4.6 examples ---- */
    expect(da(1, 5, 0, 1, 1, 0x3F, 0x2E, 0x07), "a1 = *r5-- - *r7++"); /* FADD-STORE */
    expect(da(1, 6, 0, 1, 1, 0x0E, 0x03, 0x68), "a1 = (*r13 = a3) - *r1--"); /* FADD-TAP */
    expect(da(1, 4, 0, 0, 2, 0x00, 0x16, 0x08), "*r1 = a2 = *r2--"); /* FLOAD-STORE */
    expect(da(3, 1, 0, 1, 0, 0x1B, 0x16, 0x0F), "*r1++ = a0 = a1 - *r2-- * *r3++r17"); /* FMULT-ACC-STORE */
    expect(da(2, 1, 1, 0, 0, 0x0F, 0x1B, 0x16), "a0 = -a1 + (*r2-- = *r3++r17) * *r1++"); /* FMULT-ACC-TAP */
    expect(da(1, 0, 0, 1, 2, 0x56, 0x77, 0x12), "*r2++r16 = a2 = *r14++ - a0 * *r10--"); /* FMULT-ADD-STORE */
    expect(da(3, 4, 0, 0, 3, 0x5E, 0x67, 0x13), "*r2++r17 = a3 = *r12++ * *r11--"); /* FMULT-STORE */
    expect(da(2, 4, 0, 1, 0, 0x46, 0x08, 0x12), "a0 = -(*r2++r16 = *r1) * *r8--"); /* FMULT-TAP */
    expect(da(3, 0, 0, 0, 0, 0x00, 0x00, 0x07), "a0 = a0 + a0 * a0"); /* all defaults */
    /* Apple's assembler encodes "no Z write" as 0x7F (seen in the ROM
     * '3210' segments and enabler dspf modules), not the manual's 0x07 */
    expect(0x3440087F, "a2 = *r2 + a0");
    expect(0x3800079F, "a0 = (*r3++ = *r1++) + a0");

    /* ---- DA special functions — IM chapter 4.6 examples ---- */
    expect(sf(0, 0, 0x17, 0x0F), "*r1++ = a0 = ic(*r2++)");
    expect(sf(1, 1, 0x01, 0x07), "a1 = oc(a1)");
    expect(sf(2, 3, 0x10, 0x09), "*r1++r15 = a3 = float16(*r2)");
    expect(sf(8, 3, 0x10, 0x2B), "*r5++r17 = a3 = float32(*r2)");
    expect(sf(3, 0, 0x00, 0x18), "*r3 = a0 = int16(a0)");
    expect(sf(9, 2, 0x0F, 0x67), "*r12++ = a2 = int32(*r1++)");
    expect(sf(4, 3, 0x0E, 0x62), "*r12++r16 = a3 = round(*r1--)");
    expect(sf(5, 2, 0x3E, 0x5E), "*r11-- = a2 = ifalt(*r7--)");
    expect(sf(6, 0, 0x38, 0x07), "a0 = ifaeq(*r7)");
    expect(sf(7, 1, 0x4C, 0x68), "*r13 = a1 = ifagt(*r9++r18)");
    expect(sf(12, 1, 0x4F, 0x26), "*r4-- = a1 = ieee(*r9++)");
    expect(sf(13, 1, 0x4F, 0x0F), "*r1++ = a1 = dsp(*r9++)");
    expect(sf(14, 3, 0x4F, 0x07), "a3 = seed(*r9++)");
    expect_at(sf(10, 0, 0x00, 0x07), 0, NULL, DSP3210_RESERVED);

    /* ---- illegal opcodes (IM 7.5.3.2) ---- */
    expect_at(0x00000000u, 0, NULL, DSP3210_ILLEGAL); /* 000000 */
    expect_at(0x04000000u, 0, NULL, DSP3210_ILLEGAL); /* 000001 */
    expect_at(0x08000000u, 0, NULL, DSP3210_ILLEGAL); /* 000010 */
    expect_at(0x3C000000u, 0, NULL, DSP3210_ILLEGAL); /* 001111 */
    expect_at(0x58000000u, 0, NULL, DSP3210_ILLEGAL); /* 010110 */
    expect_at(0x5C000000u, 0, NULL, DSP3210_ILLEGAL); /* 010111 */
    expect_at(0x88000000u, 0, NULL, DSP3210_ILLEGAL); /* 100010 */

    printf("%d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
