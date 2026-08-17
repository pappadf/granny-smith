// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

/*
 * test.c — unit tests for the DSP3210 core (src/core/cpu/dsp3210/).
 *
 * The instruction-semantics tests are ported from the validated reference
 * suite for the standalone DSP3210 reference emulator.
 * Programs are hand-encoded from the field layouts of IM chapter 10
 * (the same layouts the validated reference disassembler uses); one
 * vector (the ROM boot stub's `r5 = pc - 0x24`) is a raw ROM word.
 *
 * New tests at the bottom cover the repo adaptation's additions: on-chip
 * timer + BIO MMIO decode, the BIO output callback (the AV DSP→host
 * doorbell), PS.IR0/IR1 pin mirrors, the dsp3210_run burn-down/idle
 * contract, and the 7-word host bootstrap handshake against a mock bus
 * (the multi-CPU proposal's Phase B acceptance test).
 */

#include "dsp3210.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* ---- register codes (IM Table 10-2) ---- */
static unsigned RC(int r) /* r0-r22 */
{
    if (r == 0)
        return 0;
    if (r <= 14)
        return (unsigned)r;
    if (r <= 19)
        return (unsigned)r + 2;
    return (unsigned)r + 4;
}
#define RC_PC   15u
#define RC_MIN  22u /* -n */
#define RC_PLUS 23u /* +n */
#define RC_PCSH 30u

/* ---- encoders ---- */
static uint32_t e_cgoto(unsigned c, unsigned rb, int n) /* 0b/1b */
{
    return 0x80000000u | (c << 21) | (rb << 16) | ((uint32_t)n & 0xFFFFu);
}
#define E_NOP     e_cgoto(0, 0, 0)
#define E_IRETURN e_cgoto(1, RC_PCSH, 0)

static uint32_t e_loop(unsigned rm, unsigned rb, int n) /* 3a */
{
    return (0x03u << 26) | (rm << 21) | (rb << 16) | ((uint32_t)n & 0xFFFFu);
}

static uint32_t e_call(unsigned rm, unsigned rb, int n) /* 4a */
{
    return (0x04u << 26) | (rm << 21) | (rb << 16) | ((uint32_t)n & 0xFFFFu);
}

static uint32_t e_add3(int lng, unsigned rd, unsigned rs3, int n) /* 5a/5b */
{
    return ((lng ? 0x25u : 0x05u) << 26) | (rd << 21) | (rs3 << 16) | ((uint32_t)n & 0xFFFFu);
}

static uint32_t e_alur(int lng, unsigned f, unsigned rd, unsigned rs1, unsigned c, unsigned rs2) /* 6a/6b */
{
    return ((uint32_t)lng << 31) | (0x0Cu << 25) | (f << 21) | (rd << 16) | (rs1 << 11) | (c << 5) | rs2;
}

static uint32_t e_alui(int lng, unsigned f, unsigned rd, int n) /* 6c/6d */
{
    return ((uint32_t)lng << 31) | (0x0Du << 25) | (f << 21) | (rd << 16) | ((uint32_t)n & 0xFFFFu);
}

static uint32_t e_mvdir(unsigned t, unsigned w, unsigned rh, unsigned L) {
    return (0x07u << 26) | (t << 24) | (w << 21) | (rh << 16) | L;
} /* 7a */

static uint32_t e_mvind(unsigned t, unsigned w, unsigned rh, unsigned rp, unsigned ri) /* 7c */
{
    return (0x27u << 26) | (t << 24) | (w << 21) | (rh << 16) | (rp << 11) | ri;
}

static uint32_t e_mvior(unsigned t, unsigned w, unsigned rh, unsigned ior) {
    return (0x27u << 26) | (t << 24) | (w << 21) | (rh << 16) | (1u << 10) | ior;
} /* 7b */

static uint32_t e_do(unsigned k, unsigned l) /* 3b */
{
    return (0x23u << 26) | (k << 11) | l;
}

static uint32_t e_doreg(unsigned k, unsigned rm) /* 3c */
{
    return (0x23u << 26) | (1u << 25) | (k << 11) | rm;
}

static uint32_t e_shor(unsigned rd, unsigned rs, unsigned n) /* 4b */
{
    return (0x24u << 26) | (rd << 21) | (rs << 16) | (n & 0xFFFFu);
}

static uint32_t e_goto24(unsigned rb, uint32_t m) /* 8a */
{
    return (5u << 29) | (((m >> 16) & 0xFFu) << 21) | (rb << 16) | (m & 0xFFFFu);
}

static uint32_t e_set24(unsigned rd, uint32_t m) /* 8b */
{
    return (6u << 29) | (((m >> 16) & 0xFFu) << 21) | (rd << 16) | (m & 0xFFFFu);
}

static uint32_t e_call24(unsigned rm, uint32_t m) /* 8c */
{
    return (7u << 29) | (((m >> 16) & 0xFFu) << 21) | (rm << 16) | (m & 0xFFFFu);
}

/* DA fields: X/Y/Z 7-bit p:i */
static unsigned DF(unsigned p, unsigned i) {
    return (p << 3) | i;
}
#define DF_ACC(n) DF(0, n)
#define DF_NOWR   0x07u

static uint32_t e_damac(unsigned fmt, unsigned m, unsigned fs, unsigned ss, unsigned n, unsigned x, unsigned y,
                        unsigned z) {
    return (fmt << 29) | (m << 26) | (fs << 24) | (ss << 23) | (n << 21) | (x << 14) | (y << 7) | z;
}

static uint32_t e_daspec(unsigned g, unsigned n, unsigned y, unsigned z) {
    return (0x0Fu << 27) | (g << 23) | (n << 21) | (y << 7) | z;
}

/* ALU F codes */
enum {
    F_ADD = 0,
    F_SHL = 1,
    F_RSUB = 2,
    F_CRADD = 3,
    F_SUB = 4,
    F_ANDC = 6,
    F_CMP = 7,
    F_XOR = 8,
    F_ROR = 9,
    F_OR = 10,
    F_ROL = 11,
    F_SHR = 12,
    F_ASR = 13,
    F_AND = 14,
    F_BTST = 15
};
/* condition codes */
enum { C_FALSE = 0, C_TRUE = 1, C_PL = 2, C_MI = 3, C_NE = 4, C_EQ = 5, C_CS = 9 };
/* W field */
enum { W_BYTE = 0, W_CHAR = 1, W_USHORT = 2, W_SHORT = 3, W_HBYTE = 4, W_LONG = 7 };

/* ---- harness ---- */
#define MEMSZ (1u << 20)
static uint8_t membuf[MEMSZ];
static dsp3210_t S;

static void prog_at(uint32_t addr, const uint32_t *words, int n) {
    int i;
    memset(membuf, 0, sizeof membuf);
    dsp3210_init(&S, membuf, MEMSZ);
    for (i = 0; i < n; i++)
        dsp3210_poke(&S, addr + 4u * (uint32_t)i, 4, words[i]);
    S.pc = addr;
    S.npc = addr + 4;
}

static void prog(const uint32_t *words, int n) {
    prog_at(0, words, n);
}

static int run(int max) {
    int st = DSP3210_STEP_OK, i;
    for (i = 0; i < max && st == DSP3210_STEP_OK; i++)
        st = dsp3210_step(&S);
    return st;
}

static void steps(int n) {
    while (n--)
        dsp3210_step(&S);
}

/* ---- tests ---- */

static void test_set24_shiftor(void) {
    const uint32_t p[] = {
        e_set24(RC(2), 0x123456), /* r2 = (ushort24) 0x123456 */
        e_shor(RC(22), RC(0), 0x5003), /* r22 = r0 <<| 0x5003 */
        e_shor(RC(3), RC(2), 0x5003), /* r3 = r2 <<| 0x5003 */
    };
    prog(p, 3);
    steps(3);
    CHECK(S.r[2] == 0x123456);
    CHECK(S.r[22] == 0x50030000);
    CHECK(S.r[3] == 0x50133456);
    /* cross-check against the ROM boot stub encoding 93405003 */
    CHECK(e_shor(RC(22), RC(0), 0x5003) == 0x93405003u);
}

static void test_pc_reads_plus8(void) {
    /* the ROM's own oracle: at address 0x1C, `r5 = pc - 0x24` (word
     * 0x94AFFFDC) must produce r5 = 0 [dsp3210dis README] */
    const uint32_t p[] = {0x94AFFFDCu};
    prog_at(0x1C, p, 1);
    steps(1);
    CHECK(S.r[5] == 0);
    CHECK(e_add3(1, RC(5), RC_PC, -0x24) == 0x94AFFFDCu);
}

static void test_add_flags(void) {
    const uint32_t p[] = {
        e_add3(0, RC(1), RC(0), 0x7FFF), /* r1 = (short) 0x7fff */
        e_add3(0, RC(1), RC(1), 1), /* r1 = (short) r1 + 1 */
    };
    prog(p, 2);
    steps(2);
    CHECK(S.r[1] == 0xFFFF8000u); /* sign-extended */
    CHECK((S.ps & 0xF) == (DSP3210_PS_n | DSP3210_PS_v));

    /* 32-bit carry */
    const uint32_t q[] = {
        e_set24(RC(1), 0), e_alui(1, F_SUB, RC(1), 1), /* r1 = r1 - 1 = -1 */
        e_alur(1, F_ADD, RC(2), RC(1), C_TRUE, RC(1)), /* -1 + -1 */
    };
    prog(q, 3);
    steps(3);
    CHECK(S.r[2] == 0xFFFFFFFEu);
    CHECK((S.ps & DSP3210_PS_c) != 0); /* carry out */
    CHECK((S.ps & DSP3210_PS_v) == 0);
}

static void test_sub_borrow_and_negate(void) {
    const uint32_t p[] = {
        e_add3(0, RC(1), RC(0), 5), e_alui(1, F_CMP, RC(1), 7), /* r1 - 7: borrow */
    };
    prog(p, 2);
    steps(2);
    CHECK((S.ps & DSP3210_PS_c) != 0);
    CHECK((S.ps & DSP3210_PS_n) != 0);
    CHECK(S.r[1] == 5); /* compare stores nothing */

    /* negate special cases (SUBTRACT page) */
    const uint32_t q[] = {
        e_alur(1, F_SUB, RC(2), RC(0), C_TRUE, RC(0)), /* r2 = -r0 = 0 */
    };
    prog(q, 1);
    steps(1);
    CHECK(S.r[2] == 0);
    CHECK((S.ps & DSP3210_PS_c) == 0); /* c = 0 only for rS = 0 */
}

static void test_delay_slot(void) {
    const uint32_t p[] = {
        e_add3(0, RC(1), RC(0), 5), /* 00: r1 = 5 */
        e_alui(1, F_CMP, RC(1), 5), /* 04: r1 - 5 → eq */
        e_cgoto(C_EQ, RC(0), 0x20), /* 08: if (eq) goto 0x20 */
        e_add3(0, RC(3), RC(0), 1), /* 0c: delay slot: runs */
        e_add3(0, RC(4), RC(0), 2), /* 10: skipped */
        E_NOP,
        E_NOP,
        E_NOP, /* 14/18/1c */
        e_add3(0, RC(5), RC(0), 3), /* 20: target */
    };
    prog(p, 9);
    steps(5);
    CHECK(S.r[3] == 1); /* delay slot executed */
    CHECK(S.r[4] == 0); /* fall-through skipped */
    CHECK(S.r[5] == 3); /* target reached */
}

static void test_back_to_back_branches(void) {
    /* I1: goto A / I2: goto B executes I1, I2, A, B [IM §4.4.2.3] */
    const uint32_t p[] = {
        e_cgoto(C_TRUE, RC(0), 0x20), /* 00: goto 0x20 (A) */
        e_cgoto(C_TRUE, RC(0), 0x30), /* 04: goto 0x30 (B) */
        0,
        0,
        0,
        0,
        0,
        0,
        e_add3(0, RC(1), RC(0), 0xA), /* 20: A: r1 = 0xa */
        0,
        0,
        0,
        e_add3(0, RC(2), RC(0), 0xB), /* 30: B: r2 = 0xb */
    };
    prog(p, 13);
    steps(4);
    CHECK(S.r[1] == 0xA);
    CHECK(S.r[2] == 0xB);
    CHECK(S.pc == 0x34);
}

static void test_call_return(void) {
    const uint32_t p[] = {
        e_call(RC(18), RC(0), 0x20), /* 00: call 0x20 (r18) */
        e_add3(0, RC(1), RC(0), 1), /* 04: delay slot */
        e_add3(0, RC(2), RC(0), 2), /* 08: after return */
        0,
        0,
        0,
        0,
        0,
        e_add3(0, RC(3), RC(0), 3), /* 20: sub body */
        e_cgoto(C_TRUE, RC(18), 0), /* 24: return (r18) */
        E_NOP, /* 28: delay slot */
    };
    prog(p, 11);
    steps(6);
    CHECK(S.r[18] == 0x08); /* insn + 8 */
    CHECK(S.r[1] == 1 && S.r[3] == 3 && S.r[2] == 2);
}

static void test_call24_goto24(void) {
    const uint32_t p[] = {
        e_goto24(RC(0), 0x40), /* 00: goto 0x40 */
        E_NOP,
    };
    prog(p, 2);
    steps(2);
    CHECK(S.pc == 0x40);

    const uint32_t q[] = {e_call24(RC(17), 0x123400), E_NOP};
    prog(q, 2);
    steps(2);
    CHECK(S.r[17] == 8);
    CHECK(S.pc == 0x123400);
}

static void test_incr_decr_sp(void) {
    const uint32_t p[] = {
        e_add3(0, RC(1), RC(0), 7),
        e_alur(1, F_ADD, RC(2), RC(1), C_TRUE, RC_PLUS), /* r2 = r1 + 1 */
        e_alur(1, F_ADD, RC(3), RC(1), C_TRUE, RC_MIN), /* r3 = r1 - 1 */
        e_add3(0, RC(21), RC(0), 100), /* sp = 100 */
        e_alur(1, F_ADD, RC(21), RC(21), C_TRUE, RC_PLUS), /* sp = sp++ */
        e_alur(1, F_ADD, RC(21), RC(21), C_TRUE, RC_MIN), /* sp = sp-- */
        e_alur(1, F_ADD, RC(21), RC(21), C_TRUE, RC_MIN), /* sp = sp-- */
    };
    prog(p, 7);
    steps(7);
    CHECK(S.r[2] == 8);
    CHECK(S.r[3] == 6);
    CHECK(S.r[21] == 96); /* 100 + 4 - 4 - 4 */
}

static void test_conditional_alu(void) {
    const uint32_t p[] = {
        e_add3(0, RC(1), RC(0), 1), /* flags: n=0 z=0 */
        e_alur(1, F_ADD, RC(2), RC(1), C_MI, RC(1)), /* not taken */
        e_alur(1, F_ADD, RC(3), RC(1), C_PL, RC(1)), /* taken */
    };
    prog(p, 3);
    steps(3);
    CHECK(S.r[2] == 0);
    CHECK(S.r[3] == 2);
}

static void test_goto_loop(void) {
    const uint32_t p[] = {
        e_add3(0, RC(1), RC(0), 3), /* 00: counter = 3 */
        e_add3(0, RC(2), RC(0), 0), /* 04: r2 = 0 */
        e_add3(1, RC(2), RC(2), 1), /* 08: L: r2++ */
        e_loop(RC(1), RC(0), 0x08), /* 0c: if(r1-->=0) goto L */
        E_NOP, /* 10: delay slot */
        e_add3(0, RC(3), RC(0), 9), /* 14: after */
    };
    prog(p, 6);
    run(40);
    /* body runs once + 4 taken branches (counter 3,2,1,0) = 5 times */
    CHECK(S.r[2] == 5);
    CHECK(S.r[1] == (uint32_t)-2);
    CHECK(S.r[3] == 9);
}

static void test_do_loops(void) {
    const uint32_t p[] = {
        e_do(1, 3), /* next 2 insns, 4 times */
        e_add3(1, RC(2), RC(2), 1), e_add3(1, RC(3), RC(3), 2), e_add3(0, RC(4), RC(0), 7), /* after loop */
    };
    prog(p, 4);
    steps(1 + 8 + 1);
    CHECK(S.r[2] == 4);
    CHECK(S.r[3] == 8);
    CHECK(S.r[4] == 7);

    const uint32_t q[] = {
        e_add3(0, RC(5), RC(0), 9), /* count-1 in r5 */
        e_doreg(0, RC(5)), /* 1 insn, 10 times */
        e_add3(1, RC(2), RC(2), 1),
        e_add3(0, RC(4), RC(0), 1),
    };
    prog(q, 4);
    steps(2 + 10 + 1);
    CHECK(S.r[2] == 10);
    CHECK(S.r[4] == 1);
}

static void test_moves_and_endianness(void) {
    const uint32_t p[] = {
        e_set24(RC(1), 0x100),
        e_set24(RC(2), 0x11223344), /* only 24 bits: 0x223344 */
        e_shor(RC(2), RC(2), 0x1122), /* r2 = 0x11223344 */
        e_mvind(1, W_LONG, RC(2), RC(1), RC_PLUS), /* *r1++ = r2 */
        e_set24(RC(1), 0x100),
        e_mvind(0, W_BYTE, RC(3), RC(1), RC_PLUS), /* r3 = (byte) *r1++ */
        e_mvind(0, W_CHAR, RC(4), RC(1), 0), /* r4 = (char) *r1 */
        e_set24(RC(1), 0x100),
        e_mvind(0, W_USHORT, RC(5), RC(1), RC_PLUS), /* r5 = (ushort)*r1++ */
        e_mvind(0, W_SHORT, RC(6), RC(1), 0), /* r6 = (short) *r1 */
        e_mvind(0, W_HBYTE, RC(7), RC(1), 0), /* r7 = (hbyte) *r1 */
    };
    prog(p, 11);
    steps(11);
    CHECK(S.r[2] == 0x11223344);
    uint32_t v;
    dsp3210_peek(&S, 0x100, 4, &v);
    CHECK(v == 0x11223344); /* big-endian in memory */
    CHECK(S.r[3] == 0x11); /* first byte is the MSB */
    CHECK(S.r[4] == 0x22);
    CHECK(S.r[5] == 0x1122);
    CHECK(S.r[6] == 0x3344);
    CHECK(S.r[7] == 0x3300); /* hbyte: bits 15-8 */

    /* hbyte store */
    const uint32_t q[] = {
        e_set24(RC(2), 0xABCD), e_set24(RC(1), 0x200),
        e_mvind(1, W_HBYTE, RC(2), RC(1), RC_PLUS), /* *r1++ = (hbyte) r2 */
    };
    prog(q, 3);
    steps(3);
    dsp3210_peek(&S, 0x200, 1, &v);
    CHECK(v == 0xAB);
    CHECK(S.r[1] == 0x201); /* byte-sized post-inc */
}

static void test_direct_moves_window(void) {
    /* processor mode: *L targets $5003xxxx [IM §3.5.6] */
    const uint32_t p[] = {
        e_add3(0, RC(1), RC(0), 0x1234), e_mvdir(1, W_LONG, RC(1), 0xE000), /* *0xe000 = r1 */
        e_mvdir(0, W_LONG, RC(2), 0xE000), /* r2 = *0xe000 */
    };
    prog(p, 3);
    steps(3);
    CHECK(S.r[2] == 0x1234);
    uint32_t v;
    CHECK(dsp3210_peek(&S, 0x5003E000u, 4, &v) == 0);
    CHECK(v == 0x1234);
    uint32_t low;
    dsp3210_peek(&S, 0xE000, 4, &low);
    CHECK(low == 0); /* not written at $0000E000 */
}

static void test_io_registers(void) {
    const uint32_t p[] = {
        e_add3(0, RC(1), RC(0), 0x0F),
        e_mvior(1, W_SHORT, RC(1), 0), /* ps = (short) r1 */
        e_mvior(0, W_SHORT, RC(2), 0), /* r2 = (short) ps */
        e_set24(RC(3), 0x3B80),
        e_mvior(1, W_SHORT, RC(3), 12), /* pcw = (short) r3 (locks) */
        e_add3(0, RC(4), RC(0), 0x38F | 0x400), /* attempt rewrite */
        e_mvior(1, W_SHORT, RC(4), 12), /* ignored: locked */
        e_mvior(0, W_SHORT, RC(5), 12), /* r5 = pcw */
    };
    prog(p, 8);
    steps(2);
    CHECK((S.ps & 0xF) == 0xF); /* only CAU flags written */
    steps(6); /* NB: the r2 = ps load itself then rewrites the
                 flags from the loaded value (loads set flags) */
    CHECK((S.r[2] & 0xF) == 0xF);
    CHECK(S.r[5] == 0x3B80); /* lock held */
    CHECK(S.pcw_locked);
}

static void test_waiti_bkpt(void) {
    const uint32_t p[] = {0x9DE0040Au /* waiti */, E_NOP};
    prog(p, 2);
    CHECK(run(10) == DSP3210_STEP_WAITI);

    const uint32_t q[] = {0x9D60040Au /* bkpt */};
    prog(q, 1);
    CHECK(run(10) == DSP3210_STEP_BKPT);

    /* the three spc encodings, byte-for-byte [DOC §1.5.4] */
    CHECK(e_mvior(1, W_LONG, RC(0), 10) == 0x9DE0040Au);
    CHECK(e_mvior(1, W_SHORT, RC(0), 10) == 0x9D60040Au);
    CHECK(e_mvior(1, W_BYTE, RC(0), 10) == 0x9D00040Au);
}

static void test_illegal_opcode_error(void) {
    const uint32_t p[] = {
        e_set24(RC(22), 0x400), /* evtp = 0x400 */
        0x00000000u, /* 04: illegal opcode */
        e_add3(0, RC(9), RC(0), 1), /* 08: never reached */
    };
    prog(p, 3);
    /* handler at evtp + 2*8 = 0x410 */
    dsp3210_poke(&S, 0x410, 4, e_add3(0, RC(7), RC(0), 0xEE));
    dsp3210_poke(&S, 0x414, 4, 0x9D00040Au); /* sftrst */
    steps(4);
    CHECK(S.r[7] == 0xEE);
    CHECK(S.r[20] == 0x04 + 8); /* error trace register */
    CHECK(S.r[9] == 0);
    CHECK(S.level == DSP3210_LVL_BASE); /* sftrst dropped level */
}

static void test_interrupt_ireturn(void) {
    const uint32_t p[] = {
        e_set24(RC(22), 0x400), /* 00: evtp */
        e_set24(RC(1), 0x100), /* 04: emr = EXT0 */
        e_mvior(1, W_SHORT, RC(1), 8), /* 08: */
        e_add3(1, RC(2), RC(2), 1), /* 0c: main loop body */
        e_add3(1, RC(2), RC(2), 1), /* 10: */
        e_add3(1, RC(2), RC(2), 1), /* 14: */
        e_add3(0, RC(3), RC(0), 5), /* 18: */
    };
    prog(p, 7);
    /* quick interrupt at evtp + 8*8 = 0x440: r7 = 1 ; ireturn */
    dsp3210_poke(&S, 0x440, 4, e_add3(0, RC(7), RC(0), 1));
    dsp3210_poke(&S, 0x444, 4, E_IRETURN);
    steps(4); /* through 0x0c */
    dsp3210_acc_set(&S, 0, 42.0);
    dsp3210_request_interrupt(&S, DSP3210_VEC_EXT0);
    dsp3210_step(&S); /* dispatch: handler insn */
    CHECK(S.level == DSP3210_LVL_INTERRUPT);
    CHECK(S.pcsh == 0x14); /* resume + 4: the insn at
                              0x10 is held in irsh */
    CHECK(S.irsh_addr == 0x10);
    dsp3210_acc_set(&S, 0, -1.0); /* ISR clobbers a0... */
    steps(1); /* handler body */
    steps(1); /* ireturn */
    CHECK(S.r[7] == 1);
    CHECK(S.level == DSP3210_LVL_BASE);
    CHECK(dsp3210_acc_get(&S, 0) == 42.0); /* ...shadow restored it */
    steps(3); /* 10, 14, 18 */
    CHECK(S.r[2] == 3); /* main flow intact */
    CHECK(S.r[3] == 5);
}

static void test_waiti_wakeup(void) {
    const uint32_t p[] = {
        e_set24(RC(22), 0x400),
        e_set24(RC(1), 0x100),
        e_mvior(1, W_SHORT, RC(1), 8), /* emr = EXT0 */
        0x9DE0040Au, /* 0c: waiti */
        e_add3(0, RC(4), RC(0), 7), /* 10: latent instruction */
        e_add3(0, RC(5), RC(0), 8), /* 14: after interrupt */
    };
    prog(p, 6);
    dsp3210_poke(&S, 0x440, 4, e_add3(0, RC(7), RC(0), 1));
    dsp3210_poke(&S, 0x444, 4, E_IRETURN);
    steps(4); /* into waiti */
    CHECK(dsp3210_step(&S) == DSP3210_STEP_WAITI);
    dsp3210_request_interrupt(&S, DSP3210_VEC_EXT0);
    steps(1); /* latent insn runs first */
    CHECK(S.r[4] == 7);
    CHECK(S.r[7] == 0);
    steps(2); /* handler + ireturn */
    CHECK(S.r[7] == 1);
    steps(1);
    CHECK(S.r[5] == 8); /* resumed after latent */
}

static void test_address_error(void) {
    const uint32_t p[] = {
        e_set24(RC(22), 0x400), e_set24(RC(1), 0x101), /* misaligned */
        e_mvind(0, W_LONG, RC(2), RC(1), 0), /* r2 = *r1 → AERR */
        e_add3(0, RC(3), RC(0), 1), /* reached only if masked */
    };
    prog(p, 4);
    dsp3210_poke(&S, 0x100, 4, 0xCAFEBABEu);
    steps(4); /* emr[4] = 0: masked */
    CHECK(S.r[3] == 1);
    CHECK(S.level == DSP3210_LVL_BASE);
    CHECK(S.last_vector == DSP3210_VEC_AERR);
    /* the masked misaligned access completes with the low address bits
     * ignored (the AV ROM kernel's tcon write depends on this) */
    CHECK(S.r[2] == 0xCAFEBABEu);

    prog(p, 4);
    S.emr = 1u << 4; /* enable AERR */
    dsp3210_poke(&S, 0x420, 4, e_add3(0, RC(7), RC(0), 0xAE));
    steps(4);
    CHECK(S.r[7] == 0xAE);
    CHECK(S.level == DSP3210_LVL_ERROR);
}

static void test_cradd_rotates(void) {
    const uint32_t p[] = {
        e_add3(0, RC(1), RC(0), 11), /* 0b1011 */
        e_alur(1, F_CRADD, RC(2), RC(1), C_TRUE, RC(1)), /* >>1, LSB→c */
        e_alur(1, F_ROR, RC(3), RC(1), C_TRUE, RC(1)), /* through carry */
    };
    prog(p, 3);
    steps(2);
    CHECK(S.r[2] == 5);
    CHECK((S.ps & DSP3210_PS_c) != 0);
    steps(1); /* ror: 11>>1 | c<<31 */
    CHECK(S.r[3] == (5u | 0x80000000u));
    CHECK((S.ps & DSP3210_PS_c) != 0); /* bit 0 of 11 */
}

static void test_shifts(void) {
    const uint32_t p[] = {
        e_set24(RC(1), 0x8001), e_alui(1, F_SHL, RC(1), 16), /* 0x80010000 */
        e_alui(1, F_ASR, RC(1), 16), /* arithmetic → sign ext */
        e_alui(0, F_SHR, RC(1), 1), /* (short) logical */
    };
    prog(p, 4);
    steps(2);
    CHECK(S.r[1] == 0x80010000u);
    steps(1);
    CHECK(S.r[1] == 0xFFFF8001u);
    steps(1);
    CHECK(S.r[1] == 0x4000u); /* short shr zero-extends */
}

static void test_da_fadd_store(void) {
    /* *r3++ = a0 = *r1 + *r2   (fmt 1, M=101 → 1.0 * X) */
    const uint32_t p[] = {
        e_set24(RC(1), 0x100),
        e_set24(RC(2), 0x104),
        e_set24(RC(3), 0x108),
        e_damac(1, 5, 0, 0, 0, DF(2, 0), DF(1, 0), DF(3, 7)),
    };
    prog(p, 4);
    dsp3210_poke(&S, 0x100, 4, dsp3210_double_to_dsp32(3.25));
    dsp3210_poke(&S, 0x104, 4, dsp3210_double_to_dsp32(1.5));
    steps(4);
    CHECK(fabs(dsp3210_acc_get(&S, 0) - 4.75) < 1e-6);
    uint32_t v;
    dsp3210_peek(&S, 0x108, 4, &v);
    CHECK(fabs(dsp3210_dsp32_to_double(v) - 4.75) < 1e-6);
    CHECK(S.r[3] == 0x10C); /* Z post-increment */
    CHECK((S.ps & DSP3210_PS_N) == 0);
    CHECK((S.ps & DSP3210_PS_Z) == 0);
    CHECK((S.ctr & 1) == 0);
}

static void test_da_mult_acc(void) {
    /* a1 = a0 + *r1++ * *r2   (fmt 3, M = a0) */
    const uint32_t p[] = {
        e_set24(RC(1), 0x100),
        e_set24(RC(2), 0x104),
        e_damac(3, 0, 0, 0, 1, DF(2, 0), DF(1, 7), DF_NOWR),
        /* a2 = -a1  (fmt 1, M=100 → 0.0 product, F sign on Y=a1) */
        e_damac(1, 4, 1, 0, 2, DF_ACC(0), DF_ACC(1), DF_NOWR),
    };
    prog(p, 4);
    dsp3210_poke(&S, 0x100, 4, dsp3210_double_to_dsp32(2.0));
    dsp3210_poke(&S, 0x104, 4, dsp3210_double_to_dsp32(-8.0));
    dsp3210_acc_set(&S, 0, 100.0);
    steps(3);
    CHECK(fabs(dsp3210_acc_get(&S, 1) - 84.0) < 1e-5); /* 100 + 2*-8 */
    CHECK(S.r[1] == 0x104);
    steps(1);
    CHECK(fabs(dsp3210_acc_get(&S, 2) + 84.0) < 1e-5);
    CHECK((S.ps & DSP3210_PS_N) != 0);
    CHECK((S.ctr & 1) == 1);
}

static void test_da_tap(void) {
    /* a0 = a3 + (*r4++ = *r1++) * *r2  (fmt 2 FMULT-ACC-TAP) */
    const uint32_t p[] = {
        e_set24(RC(1), 0x100),
        e_set24(RC(2), 0x104),
        e_set24(RC(4), 0x110),
        e_damac(2, 3, 0, 0, 0, DF(2, 0), DF(1, 7), DF(4, 7)),
    };
    prog(p, 4);
    dsp3210_poke(&S, 0x100, 4, dsp3210_double_to_dsp32(1.25));
    dsp3210_poke(&S, 0x104, 4, dsp3210_double_to_dsp32(4.0));
    dsp3210_acc_set(&S, 3, 10.0);
    steps(4);
    CHECK(fabs(dsp3210_acc_get(&S, 0) - 15.0) < 1e-5);
    uint32_t v;
    dsp3210_peek(&S, 0x110, 4, &v);
    CHECK(v == dsp3210_double_to_dsp32(1.25)); /* tap passes Y through */
    CHECK(S.r[4] == 0x114);
}

static void test_da_int_float(void) {
    /* a0 = float16(*r1++) ; *r2 = a1 = int16(a0) */
    const uint32_t p[] = {
        e_set24(RC(1), 0x100), e_set24(RC(2), 0x104), e_daspec(2, 0, DF(1, 7), DF_NOWR), /* float16 */
        e_daspec(3, 1, DF_ACC(0), DF(2, 0)), /* int16 */
    };
    prog(p, 4);
    dsp3210_poke(&S, 0x100, 2, 0xFFFB); /* -5 as int16 */
    steps(4);
    CHECK(dsp3210_acc_get(&S, 0) == -5.0);
    CHECK(S.r[1] == 0x102); /* float16 Y post-inc = 2 */
    uint32_t v;
    dsp3210_peek(&S, 0x104, 2, &v);
    CHECK(v == 0xFFFB); /* halfword Z write */

    /* int32 saturation + rounding modes.  INT32 does NOT leave a float
     * in the accumulator: the result goes into the 32 MSBs — mantissa
     * and guard bits — with the rest "unpredictable" [IM INT32 page], so
     * it is read back the two ways the hardware offers: the Z write, and
     * float32().  Apple's sound-input rate converter needs exactly that
     * round trip (`*r8 = a0 = int32(a3)` ; `a0 = float32(a0)` splits its
     * phase accumulator into integer and fractional parts); modelling
     * INT32 as "leave the numeric value" turned every recording the
     * Sound control panel made into a saturated derivative (errata E16). */
    const uint32_t q[] = {
        e_set24(RC(2), 0x104), e_daspec(9, 0, DF_ACC(1), DF(2, 0)), /* *r2 = a0 = int32(a1) */
        e_daspec(8, 0, DF_ACC(0), DF_NOWR), /* a0 = float32(a0) */
    };
    uint32_t w;
    prog(q, 3);
    dsp3210_acc_set(&S, 1, 1e30);
    steps(3);
    dsp3210_peek(&S, 0x104, 4, &w);
    CHECK(w == 0x7FFFFFFF); /* saturated */
    CHECK(dsp3210_acc_get(&S, 0) == 2147483647.0);

    prog(q, 3);
    dsp3210_acc_set(&S, 1, -2.5);
    steps(3);
    dsp3210_peek(&S, 0x104, 4, &w);
    CHECK(w == 0xFFFFFFFE); /* nearest, ties up */
    CHECK(dsp3210_acc_get(&S, 0) == -2.0);
    prog(q, 3);
    S.dauc = 1u << 4; /* truncate to -inf */
    dsp3210_acc_set(&S, 1, -2.5);
    steps(3);
    dsp3210_peek(&S, 0x104, 4, &w);
    CHECK(w == 0xFFFFFFFD);
    CHECK(dsp3210_acc_get(&S, 0) == -3.0);
    prog(q, 3);
    S.dauc = 3u << 4; /* truncate to 0 */
    dsp3210_acc_set(&S, 1, -2.5);
    steps(3);
    dsp3210_peek(&S, 0x104, 4, &w);
    CHECK(w == 0xFFFFFFFE);
    CHECK(dsp3210_acc_get(&S, 0) == -2.0);
}

static void test_da_inplace_convert(void) {
    /* Z with p=1111, I!=111 — the manual calls p=1111 not allowed, but
     * Apple's assembler emits it for in-place conversions (AppleSRC
     * 'src' module, ROM sound path): the result stores back through the
     * Y operand's address.  `a0 = float32(*r1)` in this spelling must
     * leave the DSP32 float at *r1, or the module later multiplies by
     * the raw integer reinterpreted as a float (2^112 for 0xF0). */
    const uint32_t p[] = {
        e_set24(RC(1), 0x100), e_daspec(8, 0, DF(1, 0), DF(15, 0)), /* float32 in-place */
    };
    prog(p, 2);
    dsp3210_poke(&S, 0x100, 4, 240); /* raw int */
    steps(2);
    CHECK(dsp3210_acc_get(&S, 0) == 240.0);
    uint32_t v;
    dsp3210_peek(&S, 0x100, 4, &v);
    CHECK(v == 0x70000087); /* 1.875 * 2^7 in DSP32 format */
    CHECK(S.r[1] == 0x100); /* no post-modify from either field */

    /* MAC form, Z=$7F with a memory Y (the Midput gain loop,
     * `a0 = *r2 * a1`): stores the result through *r2, then Z's
     * post-increment advances r2 */
    const uint32_t q[] = {
        e_set24(RC(2), 0x100),
        e_damac(3, 4, 0, 0, 0, DF_ACC(1), DF(2, 0), DF(15, 7)),
    };
    prog(q, 2);
    dsp3210_poke(&S, 0x100, 4, 0x00000081); /* 2.0 */
    dsp3210_acc_set(&S, 1, 3.0);
    steps(2);
    CHECK(dsp3210_acc_get(&S, 0) == 6.0);
    dsp3210_peek(&S, 0x100, 4, &v);
    CHECK(v == 0x40000082); /* 6.0 written through the Y address */
    CHECK(S.r[2] == 0x104); /* Z's I field advanced Y's pointer */

    /* Z=$7F with an accumulator Y: nothing to store through */
    const uint32_t q2[] = {
        e_set24(RC(1), 0x100), e_daspec(9, 0, DF_ACC(1), 0x7F), /* a0 = int32(a1) */
    };
    prog(q2, 2);
    dsp3210_poke(&S, 0x100, 4, 240);
    dsp3210_acc_set(&S, 1, 5.0);
    steps(2);
    dsp3210_peek(&S, 0x100, 4, &v);
    CHECK(v == 240); /* untouched */
    CHECK(S.r[1] == 0x100);
}

static void test_da_ieee_dsp(void) {
    /* a0 = dsp(*r1) ; *r2 = a1 = ieee(a0) */
    const uint32_t p[] = {
        e_set24(RC(1), 0x100), e_set24(RC(2), 0x104), e_daspec(13, 0, DF(1, 0), DF_NOWR), /* dsp */
        e_daspec(12, 1, DF_ACC(0), DF(2, 0)), /* ieee */
    };
    prog(p, 4);
    float f = 12.375f;
    uint32_t fb;
    memcpy(&fb, &f, 4);
    dsp3210_poke(&S, 0x100, 4, fb);
    steps(4);
    CHECK(dsp3210_acc_get(&S, 0) == 12.375);
    uint32_t v;
    dsp3210_peek(&S, 0x104, 4, &v);
    CHECK(v == fb); /* round trip */

    /* IEEE +inf saturates, NaN raises vector 6 when enabled */
    prog(p, 4);
    dsp3210_poke(&S, 0x100, 4, 0x7F800000u); /* +inf */
    steps(3);
    CHECK(dsp3210_double_to_dsp32(dsp3210_acc_get(&S, 0)) == 0x7FFFFFFFu);
    CHECK((S.ps & DSP3210_PS_V) != 0);

    prog(p, 3);
    dsp3210_poke(&S, 0x100, 4, 0x7FC00000u); /* NaN */
    steps(3);
    CHECK(S.last_vector == DSP3210_VEC_NAN); /* raised (masked) */
}

static void test_da_seed(void) {
    const uint32_t p[] = {e_daspec(14, 0, DF_ACC(1), DF_NOWR)};
    double xs[] = {2.0, 0.75, 1000.0, -3.5};
    int i;
    for (i = 0; i < 4; i++) {
        prog(p, 1);
        dsp3210_acc_set(&S, 1, xs[i]);
        steps(1);
        CHECK(fabs(dsp3210_acc_get(&S, 0) * xs[i] - 1.0) < 0.30); /* ~3-bit seed */
        CHECK((dsp3210_acc_get(&S, 0) < 0) == (xs[i] < 0));
    }
}

static void test_da_ic_oc(void) {
    /* unsigned linear byte in/out (dauc CONV = 1111) */
    const uint32_t p[] = {
        e_daspec(0, 0, DF(1, 7), DF_NOWR), /* a0 = ic(*r1++) */
        e_daspec(1, 1, DF_ACC(0), DF(2, 0)), /* *r2 = a1 = oc(a0) */
    };
    prog(p, 2);
    S.dauc = 0x0F;
    S.r[1] = 0x100;
    S.r[2] = 0x104;
    dsp3210_poke(&S, 0x100, 1, 200);
    steps(2);
    CHECK(dsp3210_acc_get(&S, 0) == 200.0);
    CHECK(S.r[1] == 0x101); /* ic post-inc is ±1 */
    uint32_t v;
    dsp3210_peek(&S, 0x104, 1, &v);
    CHECK(v == 200);

    /* µ-law: code with all-complemented zero decodes to 0; roundtrip a
     * mid-scale value through decode(encode()) */
    prog(p, 2);
    S.dauc = 0; /* µ-law in and out */
    S.r[1] = 0x100;
    S.r[2] = 0x104;
    dsp3210_poke(&S, 0x100, 1, 0xFF); /* ~0xFF = 0: M=0,N=0,+ */
    steps(2);
    CHECK(dsp3210_acc_get(&S, 0) == 0.0);

    prog(p, 2);
    S.dauc = 0;
    S.r[1] = 0x100;
    S.r[2] = 0x104;
    dsp3210_poke(&S, 0x100, 1, 0x9A);
    steps(2);
    dsp3210_peek(&S, 0x104, 1, &v);
    CHECK(v == 0x9A); /* encode(decode(b)) == b */
}

static void test_da_ifalt(void) {
    const uint32_t p[] = {
        /* a0 = a1 - a2 → sets N */
        e_damac(1, 5, 0, 1, 0, DF_ACC(2), DF_ACC(1), DF_NOWR), e_daspec(5, 3, DF_ACC(1), DF_NOWR), /* a3 = ifalt(a1) */
    };
    prog(p, 2);
    dsp3210_acc_set(&S, 1, 1.0);
    dsp3210_acc_set(&S, 2, 5.0);
    dsp3210_acc_set(&S, 3, 99.0);
    steps(2);
    CHECK(dsp3210_acc_get(&S, 0) == -4.0);
    CHECK(dsp3210_acc_get(&S, 3) == 1.0); /* N was set → loaded */

    prog(p, 2);
    dsp3210_acc_set(&S, 1, 9.0);
    dsp3210_acc_set(&S, 2, 5.0);
    dsp3210_acc_set(&S, 3, 99.0);
    steps(2);
    CHECK(dsp3210_acc_get(&S, 3) == 99.0); /* N clear → kept */
}

/* ---- the DAU pipeline latencies [IM §4.4.2] (errata.md E12-E14) ---- */

/* Latency 1 [IM §4.4.2.1]: a DA instruction's memory write "is not
 * available to be read from that location until four instructions later".
 * The manual's own example writes *r3 twice and reads it in I5, and the
 * value read is the one written in I1, not I2. */
static void test_latency_da_store(void) {
    const uint32_t p[] = {
        e_set24(RC(1), 0x100), /* r1 -> the location under test  */
        e_set24(RC(2), 0x104), /* r2 -> the value to store       */
        /* *r1 = a0 = *r2 + 0.0*X  (a DA store of 7.0 to $100)   */
        e_damac(1, 4, 0, 0, 0, DF_ACC(0), DF(2, 0), DF(1, 0)),
        /* a1 = *r1 at +1, a2 = *r1 at +2, a3 = *r1 at +3 ...    */
        e_damac(1, 4, 0, 0, 1, DF_ACC(0), DF(1, 0), DF_NOWR),
        e_damac(1, 4, 0, 0, 2, DF_ACC(0), DF(1, 0), DF_NOWR),
        e_damac(1, 4, 0, 0, 3, DF_ACC(0), DF(1, 0), DF_NOWR),
        /* ...and at +4, which is the first instruction that sees it */
        e_damac(1, 4, 0, 0, 1, DF_ACC(0), DF(1, 0), DF_NOWR),
    };
    prog(p, 7);
    dsp3210_poke(&S, 0x100, 4, dsp3210_double_to_dsp32(1.0)); /* the old value */
    dsp3210_poke(&S, 0x104, 4, dsp3210_double_to_dsp32(7.0)); /* the new one   */
    steps(6);
    /* memory itself holds the new value; the three reads inside the
     * shadow all returned the pre-write word */
    CHECK(fabs(dsp3210_acc_get(&S, 1) - 1.0) < 1e-9);
    CHECK(fabs(dsp3210_acc_get(&S, 2) - 1.0) < 1e-9);
    CHECK(fabs(dsp3210_acc_get(&S, 3) - 1.0) < 1e-9);
    steps(1);
    CHECK(fabs(dsp3210_acc_get(&S, 1) - 7.0) < 1e-9); /* four later: visible */
    uint32_t v;
    dsp3210_peek(&S, 0x100, 4, &v);
    CHECK(fabs(dsp3210_dsp32_to_double(v) - 7.0) < 1e-9);
}

/* Latency 2 [IM §4.4.2.2]: an accumulator feeding the MULTIPLIER "is
 * established no sooner than three instructions prior"; an accumulator
 * feeding the ADDER has no latency at all. */
static void test_latency_acc_multiplier(void) {
    const uint32_t p[] = {
        e_set24(RC(1), 0x100),
        /* a0 = *r1 + 0.0*X   -> a0 = 5.0                        */
        e_damac(1, 4, 0, 0, 0, DF_ACC(0), DF(1, 0), DF_NOWR),
        /* a1 = a0 * *r1  ONE instruction later: the multiplier still
         * sees the pre-load a0 (2.0), not 5.0                    */
        e_damac(3, 4, 0, 0, 1, DF_ACC(0), DF(1, 0), DF_NOWR),
        E_NOP,
        E_NOP,
        /* the same multiply three instructions later now sees 5.0 */
        e_damac(3, 4, 0, 0, 2, DF_ACC(0), DF(1, 0), DF_NOWR),
        /* a3 = a0 + 0.0*X — the ADDER lane, always current       */
        e_damac(1, 4, 0, 0, 3, DF_ACC(0), DF_ACC(0), DF_NOWR),
    };
    prog(p, 7);
    dsp3210_poke(&S, 0x100, 4, dsp3210_double_to_dsp32(5.0));
    dsp3210_acc_set(&S, 0, 2.0);
    steps(3); /* set24, a0 = 5.0, a1 = a0*5 with the stale a0 */
    CHECK(fabs(dsp3210_acc_get(&S, 1) - 10.0) < 1e-6); /* 2.0 * 5.0 */
    steps(3); /* two nops, then the same multiply */
    CHECK(fabs(dsp3210_acc_get(&S, 2) - 25.0) < 1e-6); /* 5.0 * 5.0 */
    steps(1);
    CHECK(fabs(dsp3210_acc_get(&S, 3) - 5.0) < 1e-6); /* adder: current */
}

/* Latency 4 [IM §4.4.2.4]: a DAU condition tested by a conditional branch
 * is the one established four instructions earlier — the manual's example
 * has I5's `if (agt)` test the flags of I1, not I2. */
static void test_latency_dau_condition(void) {
    const uint32_t q[] = {
        /* I1 sets agt true, I2 sets it false, and the conditional four
         * instructions after I1 must still see I1's answer. */
        e_set24(RC(1), 0x100),
        e_set24(RC(2), 0x104),
        e_set24(RC(5), 0),
        e_set24(RC(6), 1),
        e_damac(1, 4, 0, 0, 0, DF_ACC(0), DF(1, 0), DF_NOWR), /* +4 */
        e_damac(1, 4, 0, 0, 1, DF_ACC(0), DF(2, 0), DF_NOWR), /* -4 */
        E_NOP,
        E_NOP,
        e_alur(1, 0, RC(5), RC(6), 24u, 0), /* if (agt) r5 = r6 + r0 */
    };
    prog(q, 9);
    dsp3210_poke(&S, 0x100, 4, dsp3210_double_to_dsp32(4.0));
    dsp3210_poke(&S, 0x104, 4, dsp3210_double_to_dsp32(-4.0));
    steps(9);
    /* the live flags are I2's (negative, agt false); the pipeline serves
     * I1's (positive, agt true) — so the conditional executed */
    CHECK((S.ps & DSP3210_PS_N) != 0);
    CHECK(S.r[5] == 1);
}

static void test_dsp32_format(void) {
    /* spot values from the format definition [DOC §1.5.6] */
    CHECK(dsp3210_dsp32_to_double(0) == 0.0);
    CHECK(dsp3210_dsp32_to_double(0x00000080u) == 1.0); /* 1.0×2^0 */
    CHECK(dsp3210_dsp32_to_double(0x00000081u) == 2.0);
    CHECK(dsp3210_dsp32_to_double(0x80000081u) == -4.0); /* -2×2^1 */
    CHECK(dsp3210_double_to_dsp32(1.0) == 0x00000080u);
    CHECK(dsp3210_double_to_dsp32(-4.0) == 0x80000081u);
    CHECK(dsp3210_double_to_dsp32(0.0) == 0);
    /* dirty zero: e = 0 with junk mantissa is still zero */
    CHECK(dsp3210_dsp32_to_double(0x12345600u) == 0.0);
    /* -1.0 = -2 × 2^-1 */
    CHECK(dsp3210_dsp32_to_double(dsp3210_double_to_dsp32(-1.0)) == -1.0);
    /* round-trip a pile of values */
    double vals[] = {3.14159, -0.001, 1e20, -1e-20, 65536.0, -65535.5};
    int i;
    for (i = 0; i < 6; i++) {
        double r = dsp3210_dsp32_to_double(dsp3210_double_to_dsp32(vals[i]));
        CHECK(fabs(r - vals[i]) <= fabs(vals[i]) * 0x1p-22);
    }
}

/* ---- repo-adaptation tests: timer, BIO, PS.IR mirrors, run/idle,
 *      and the host bootstrap against a mock bus ---- */

/* Timer via the MMIO window: count 5 at CKI/4 with auto-reload raises
 * vector 9 after five executed instructions, then keeps a N+1 period. */
static void test_timer_mmio(void) {
    const uint32_t p[] = {
        e_set24(RC(2), 5), /* r2 = 5 (count) */
        e_set24(RC(1), 0x0414),
        e_shor(RC(1), RC(1), 0x5003), /* r1 = $50030414 (timer) */
        e_mvind(1, W_LONG, RC(2), RC(1), 0), /* *r1 = (long) r2 */
        e_set24(RC(3), 0x23), /* enable, reload, CKI/4 */
        e_set24(RC(4), 0x0410),
        e_shor(RC(4), RC(4), 0x5003), /* r4 = $50030410 (tcon word) */
        e_mvind(1, W_LONG, RC(3), RC(4), 0), /* *r4 = (long) r3 → tcon=$23 */
        E_NOP,
        E_NOP,
        E_NOP,
        E_NOP,
        E_NOP,
        E_NOP,
        E_NOP,
        E_NOP,
    };
    prog(p, 16);
    steps(8); /* through the tcon write */
    CHECK(S.tcon == 0x23);
    CHECK(S.timer_count == 4); /* the enabling insn ticks too */
    CHECK(!(S.pending & (1u << DSP3210_VEC_TIMER)));
    steps(5); /* 4 → 0 raises, then reloads */
    CHECK(S.pending & (1u << DSP3210_VEC_TIMER));
    CHECK(S.timer_count == 5); /* auto-reload from N */
    /* the timer read returns the live counter through MMIO */
    {
        const uint32_t q[] = {
            e_set24(RC(1), 0x0414), e_shor(RC(1), RC(1), 0x5003),
            e_mvind(0, W_LONG, RC(5), RC(1), 0), /* r5 = (long) *r1 */
        };
        prog(q, 3);
        S.tcon = 0x23;
        S.timer_count = 100;
        S.timer_reload = 100;
        steps(3);
        CHECK(S.r[5] == 100 - 3 + 1); /* read happens before the
                                         insn's own tick lands */
    }
}

/* BIO: bioc direction + the 2-bit op fields, with the output-transition
 * callback the AV glue hangs the PSC L5 latch on. */
static int bio_calls;
static uint8_t bio_last_old, bio_last_new;
static void bio_cb(void *ctx, uint8_t old_pins, uint8_t new_pins) {
    (void)ctx;
    bio_calls++;
    bio_last_old = old_pins;
    bio_last_new = new_pins;
}

static void test_bio_doorbell(void) {
    const uint32_t p[] = {
        e_set24(RC(2), 1), /* BIO0 output */
        e_set24(RC(1), 0x0418),
        e_shor(RC(1), RC(1), 0x5003), /* r1 = $50030418 (bioc word) */
        e_mvind(1, W_LONG, RC(2), RC(1), 0), /* bioc = 1 */
        e_set24(RC(3), 3), /* op %11 on BF0: complement */
        e_set24(RC(4), 0x041E),
        e_shor(RC(4), RC(4), 0x5003), /* r4 = $5003041E (bio) */
        e_mvind(1, W_SHORT, RC(3), RC(4), 0), /* *r4 = (short) r3 — toggle */
        e_mvind(1, W_SHORT, RC(3), RC(4), 0), /* toggle back */
        e_mvind(0, W_SHORT, RC(5), RC(4), 0), /* r5 = (short) *r4 */
    };
    prog(p, 10);
    bio_calls = 0;
    S.bio_fn = bio_cb;
    steps(8); /* through the first toggle */
    CHECK(S.bioc == 1);
    CHECK(S.bio_out == 1);
    CHECK(bio_calls == 1);
    CHECK(bio_last_old == 0 && bio_last_new == 1);
    steps(1); /* toggle back */
    CHECK(bio_calls == 2);
    CHECK(bio_last_old == 1 && bio_last_new == 0);
    steps(1); /* read: pin 0 low, out-reg 0 */
    CHECK(S.r[5] == 0);
}

/* PS.IR0/IR1 read the LIVE pin level (1 = negated): dsp3210_ext_pulse
 * asserts the pin for a bounded window of core time and edge-latches the
 * request; taking the interrupt does not touch the pin, and an emr write
 * with bit 0 set drops a latched-but-untaken EXT1 request (§3.5). */
static void test_ps_ir_mirror(void) {
    const uint32_t p[] = {
        E_NOP,
        E_NOP,
        e_alui(0, F_OR, RC(1), 1), /* r1 = r1 | 1 (r1 = 1) */
        e_mvior(1, W_SHORT, RC(1), 8), /* emr = (short) r1 — bit-0 pulse */
        E_NOP,
        E_NOP,
    };
    prog(p, 6);
    /* idle: both pins negated */
    CHECK(S.ps & DSP3210_PS_IR0);
    CHECK(S.ps & DSP3210_PS_IR1);
    dsp3210_ext_pulse(&S, DSP3210_VEC_EXT1, 2); /* 2-slot active-low pulse */
    CHECK(!(S.ps & DSP3210_PS_IR1)); /* pin asserted */
    CHECK(S.ps & DSP3210_PS_IR0); /* EXT0 untouched */
    CHECK(S.pending & (1u << DSP3210_VEC_EXT1)); /* edge latched */
    steps(2); /* emr = 0: window expires, request stays latched */
    CHECK(S.ps & DSP3210_PS_IR1); /* pin negated again */
    CHECK(S.pending & (1u << DSP3210_VEC_EXT1));
    steps(2); /* the kernel's emr bit-0 write drops the untaken edge */
    CHECK(!(S.pending & (1u << DSP3210_VEC_EXT1)));
    /* dispatch path: a fresh pulse is taken when unmasked; the dispatch
     * consumes the request but never touches the pin mirror */
    dsp3210_reset(&S, 0);
    prog(p, 6);
    dsp3210_poke(&S, 0x100 + 8 * DSP3210_VEC_EXT1, 4, E_NOP); /* handler */
    dsp3210_ext_pulse(&S, DSP3210_VEC_EXT1, 8);
    S.emr = 1u << DSP3210_VEC_EXT1;
    S.r[22] = 0x100; /* evtp */
    steps(1); /* taken */
    CHECK(S.level == DSP3210_LVL_INTERRUPT);
    CHECK(!(S.pending & (1u << DSP3210_VEC_EXT1))); /* request consumed */
    CHECK(!(S.ps & DSP3210_PS_IR1)); /* pin still inside its window */
}

/* dsp3210_run: a dead sleep leaves the budget unspent (park); a sleep with
 * the timer ticking consumes it and wakes on vector 9; bkpt halts. */
static void test_run_idle_contract(void) {
    /* waiti with nothing to wake on: parked, budget unspent */
    const uint32_t p[] = {
        e_mvior(1, W_LONG, RC(0), 10), /* waiti */
        E_NOP,
        e_cgoto(C_TRUE, RC(0), 0x08), /* self-loop after wake */
        E_NOP,
        E_NOP,
    };
    uint32_t budget = 100;
    prog(p, 5);
    dsp3210_run(&S, &budget);
    CHECK(budget == 99); /* one insn (waiti) executed */
    CHECK(S.waiting);
    CHECK(dsp3210_is_idle(&S));

    /* waiti with the timer running: budget burns while asleep, the timer
     * wakes the kernel through vector 9, the handler proves it ran */
    prog(p, 5);
    dsp3210_poke(&S, 0x200 + 8 * DSP3210_VEC_TIMER, 4, e_set24(RC(6), 0xBEEF)); /* handler: r6 = $BEEF */
    dsp3210_poke(&S, 0x200 + 8 * DSP3210_VEC_TIMER + 4, 4, E_IRETURN);
    dsp3210_poke(&S, 0x200 + 8 * DSP3210_VEC_TIMER + 8, 4, E_NOP);
    S.emr = 1u << DSP3210_VEC_TIMER;
    S.r[22] = 0x200; /* evtp */
    S.tcon = 0x23; /* enable, reload, CKI/4 */
    S.timer_count = 10;
    S.timer_reload = 50;
    budget = 100;
    dsp3210_run(&S, &budget);
    CHECK(budget == 0); /* asleep time burned budget */
    CHECK(S.r[6] == 0xBEEF); /* the timer wake happened */
    CHECK(!S.waiting);

    /* bkpt: halted, crashed until reset, budget preserved */
    {
        const uint32_t q[] = {e_mvior(1, W_USHORT, RC(0), 10), E_NOP};
        prog(q, 2);
        budget = 100;
        dsp3210_run(&S, &budget);
        CHECK(S.halted);
        CHECK(budget == 99);
        CHECK(dsp3210_is_idle(&S));
        budget = 50;
        dsp3210_run(&S, &budget); /* stays halted */
        CHECK(budget == 50);
        dsp3210_reset(&S, 0);
        CHECK(!S.halted);
    }
}

/* The 7-word host bootstrap (StartProcessorRoutine) against a mock bus:
 * external memory lives behind hooks only, and the stage-1 handshake write
 * ($18 = $18, the call's link-register store in the latent slot) must be
 * observed through the hook — the Phase B acceptance test. */
static uint8_t mock_bus[0x2000];
static int mock_writes;

static uint32_t mock_read(void *ctx, uint32_t addr, int size, int *fault) {
    uint32_t v = 0;
    int i;
    (void)ctx;
    if (addr + (uint32_t)size > sizeof mock_bus) {
        *fault = 1;
        return 0;
    }
    for (i = 0; i < size; i++)
        v |= (uint32_t)mock_bus[addr + (uint32_t)i] << (8 * (size - 1 - i));
    return v;
}

static void mock_write(void *ctx, uint32_t addr, uint32_t val, int size, int *fault) {
    int i;
    (void)ctx;
    if (addr + (uint32_t)size > sizeof mock_bus) {
        *fault = 1;
        return;
    }
    mock_writes++;
    for (i = 0; i < size; i++)
        mock_bus[addr + (uint32_t)i] = (uint8_t)(val >> (8 * (size - 1 - i)));
}

static uint32_t mock_peek32(uint32_t addr) {
    int fault = 0;
    return mock_read(NULL, addr, 4, &fault);
}

static void mock_poke32(uint32_t addr, uint32_t v) {
    int fault = 0;
    mock_write(NULL, addr, v, 4, &fault);
    mock_writes--; /* setup writes don't count */
}

static void test_bootstrap_mock_bus(void) {
    uint32_t entry = 0x1000;
    int i, st = DSP3210_STEP_OK;

    memset(mock_bus, 0, sizeof mock_bus);
    mock_writes = 0;
    /* the ROM's exact 7-word stub ($08 deliberately untouched — the 68040
     * bus-error vector) [DOC §4] */
    mock_poke32(0x00, 0x802F0004u); /* goto pc+4 */
    mock_poke32(0x04, 0xC0010000u | (entry & 0xFFFFu)); /* r1 = lo16 */
    mock_poke32(0x0C, 0x90210000u | (entry >> 16)); /* r1 <<| hi16 */
    mock_poke32(0x10, 0x10210000u); /* call r1 (r1) */
    mock_poke32(0x14, 0x9DE10800u); /* *r1 = r1 */
    mock_poke32(0x18, 0); /* stage-1 cell */
    /* a parked target: waiti */
    mock_poke32(entry, e_mvior(1, W_LONG, RC(0), 10)); /* waiti */

    dsp3210_init(&S, NULL, 0);
    dsp3210_reset(&S, 0); /* processor mode, ext fetch */
    S.read_fn = mock_read;
    S.write_fn = mock_write;

    for (i = 0; i < 32 && st == DSP3210_STEP_OK; i++)
        st = dsp3210_step(&S);

    CHECK((mock_peek32(0x18) & 0x000FFFFFu) == 0x18);
    CHECK(mock_writes == 1); /* only the handshake store */
    CHECK(st == DSP3210_STEP_WAITI);
    CHECK(dsp3210_is_idle(&S));
}

int main(void) {
    test_set24_shiftor();
    test_pc_reads_plus8();
    test_add_flags();
    test_sub_borrow_and_negate();
    test_delay_slot();
    test_back_to_back_branches();
    test_call_return();
    test_call24_goto24();
    test_incr_decr_sp();
    test_conditional_alu();
    test_goto_loop();
    test_do_loops();
    test_moves_and_endianness();
    test_direct_moves_window();
    test_io_registers();
    test_waiti_bkpt();
    test_illegal_opcode_error();
    test_interrupt_ireturn();
    test_waiti_wakeup();
    test_address_error();
    test_cradd_rotates();
    test_shifts();
    test_da_fadd_store();
    test_da_mult_acc();
    test_da_tap();
    test_da_int_float();
    test_da_inplace_convert();
    test_da_ieee_dsp();
    test_da_seed();
    test_da_ic_oc();
    test_da_ifalt();
    test_dsp32_format();
    test_latency_da_store();
    test_latency_acc_multiplier();
    test_latency_dau_condition();

    /* repo-adaptation coverage */
    test_timer_mmio();
    test_bio_doorbell();
    test_ps_ir_mirror();
    test_run_idle_contract();
    test_bootstrap_mock_bus();

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
