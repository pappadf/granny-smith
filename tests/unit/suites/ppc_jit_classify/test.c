// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// test.c — the translation tier's classifier (ppc_jit_classify.h) and the
// FP helper's decoder (ppc_execute_fp), run natively.
//
// The translator itself exists only in the wasm build; its classifier is
// a decode-tree instantiation with no wasm dependency, so its rules are
// checked here: a word the interpreter rejects (an invalid BO, a reserved
// field set on a row that checks it) is never translated -- the hand
// classifier the tree replaced translated `addme`/`addze`/`subfme`/
// `subfze`/`neg` with rB set, and `bc`/`bclr`/`bcctr` with an invalid BO
// --, the translator's shapes carry the fields its emitter reads, and the
// 601's floating point and dcbz stay with the predecoded loop.

#include "ppc_internal.h"

#include "harness.h"
#include "memory.h"
#include "predecode.h"

#include "ppc_jit_classify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ASSERT_COUNTING
#include "test_assert.h"

#define IPC     0x00123450u
#define PAGE_LO 0x00123000u

static uint16_t cls(uint32_t iw, bool is601, pd_entry_t *e) {
    memset(e, 0xA5, sizeof *e);
    return ppc_jit_classify(iw, IPC, PAGE_LO, is601, e);
}

static uint32_t e_xo(uint32_t rt, uint32_t ra, uint32_t rb, uint32_t oe, uint32_t xo9, uint32_t rc) {
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (oe << 10) | (xo9 << 1) | rc;
}
static uint32_t e_x(uint32_t op, uint32_t rt, uint32_t ra, uint32_t rb, uint32_t xo, uint32_t rc) {
    return (op << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
}
static uint32_t e_a(uint32_t op, uint32_t t, uint32_t a, uint32_t b, uint32_t c, uint32_t xo5, uint32_t rc) {
    return (op << 26) | (t << 21) | (a << 16) | (b << 11) | (c << 6) | (xo5 << 1) | rc;
}

// Every encoding the interpreter's tree rejects stays the generic illegal
// id: over every primary and extended opcode, with the other fields
// pseudo-random (an xorshift sweep, the same every run), on both models.
TEST(test_illegal_never_translated) {
    uint32_t x = 0x9E3779B9u, bad = 0, illegal = 0;
    for (uint32_t op = 0; op < 64; op++)
        for (uint32_t xo = 0; xo < 1024; xo++)
            for (int k = 0; k < 48; k++) {
                x ^= x << 13, x ^= x >> 17, x ^= x << 5;
                uint32_t iw = op << 26 | (x & 0x03FFF800u) | xo << 1 | (x & 1u);
                pd_entry_t e;
                if (ppc_pd_classify_word(iw, IPC, PAGE_LO, &e) != T1_ILLEGAL)
                    continue;
                illegal++;
                for (int m = 0; m < 2; m++)
                    if (cls(iw, m, &e) != T1_ILLEGAL && bad++ < 8)
                        printf("FAIL illegal word %08X classified %u (601=%d)\n", iw, cls(iw, m, &e), m);
            }
    CHECK(illegal > 100000u);
    CHECK_EQ(bad, 0);
    // The forms the hand classifier translated anyway
    pd_entry_t e;
    CHECK_EQ(cls(e_xo(3, 4, 5, 0, 234, 0), false, &e), T1_ILLEGAL); // addme with rB set
    CHECK_EQ(cls(e_xo(3, 4, 1, 0, 202, 0), false, &e), T1_ILLEGAL); // addze
    CHECK_EQ(cls(e_xo(3, 4, 1, 1, 232, 0), false, &e), T1_ILLEGAL); // subfmeo
    CHECK_EQ(cls(e_xo(3, 4, 1, 0, 200, 1), false, &e), T1_ILLEGAL); // subfze.
    CHECK_EQ(cls(e_xo(3, 4, 1, 0, 104, 0), false, &e), T1_ILLEGAL); // neg
    CHECK_EQ(cls(16u << 26 | 0x1Du << 21 | 0x10u, false, &e), T1_ILLEGAL); // bc with BO = 11101 (z set)
    CHECK_EQ(cls(e_x(19, 0x1E, 0, 0, 16, 0), false, &e), T1_ILLEGAL); // bclr, BO = 11110
    CHECK_EQ(cls(e_x(19, 0x16, 0, 0, 528, 1), false, &e), T1_ILLEGAL); // bcctrl, BO = 10110
}

// The translator's shapes and the fields its emitter reads
TEST(test_shapes) {
    pd_entry_t e;
    // addme. r3,r4 (its valid form): JX_ADDX, kind << 8, Rc << 17
    CHECK_EQ(cls(e_xo(3, 4, 0, 0, 234, 1), false, &e), JX_ADDX);
    CHECK_EQ(e.a, PPD_GOFF(3));
    CHECK_EQ(e.b, PPD_GOFF(4));
    CHECK_EQ(e.c, (uint32_t)ADDK_ADDME << 8 | 1u << 17);
    // addo r3,r4,r5 (the OE form the T0 id leaves out)
    CHECK_EQ(cls(e_xo(3, 4, 5, 1, 266, 0), false, &e), JX_ADDX);
    CHECK_EQ(e.c, PPD_GOFF(5) | (uint32_t)ADDK_ADD << 8 | 1u << 16);
    // add r3,r4,r5 stays the predecoded tier's T0 id
    CHECK_EQ(cls(e_xo(3, 4, 5, 0, 266, 0), false, &e), PPD_ADD);
    // bcl 12,2,+8: JX_BC, a = BO, b = BI, c = the word
    uint32_t bcl = 16u << 26 | 12u << 21 | 2u << 16 | 8u | 1u;
    CHECK_EQ(cls(bcl, false, &e), JX_BC);
    CHECK_EQ(e.a, 12);
    CHECK_EQ(e.b, 2);
    CHECK_EQ(e.c, bcl);
    // beqlr: JX_BCLR, c = LK
    CHECK_EQ(cls(e_x(19, 12, 2, 0, 16, 0), false, &e), JX_BCLR);
    CHECK_EQ(e.c, 0);
    // lwz r3,8(0): the T0 id with b = JX_RA0
    CHECK_EQ(cls(32u << 26 | 3u << 21 | 8u, false, &e), PPD_LWZ);
    CHECK_EQ(e.b, JX_RA0);
    CHECK_EQ(e.c, 8);
    // lwzux r3,r0,r5 (the r0-inhibited update) stays generic; lwzx r3,0,r5 does not
    CHECK_EQ(cls(e_x(31, 3, 0, 5, 55, 0), false, &e), T1_LWZUX);
    CHECK_EQ(cls(e_x(31, 3, 0, 5, 23, 0), false, &e), JX_LWZX);
    CHECK_EQ(e.b, JX_RA0);
    CHECK_EQ(e.c, PPD_GOFF(5));
    // mfspr r3,SRR0 / mtspr SPRG2,r4: the ppc_t field offsets
    CHECK_EQ(cls(e_x(31, 3, 26, 0, 339, 0), false, &e), JX_MFSPR);
    CHECK_EQ(e.c, offsetof(ppc_t, srr0));
    CHECK_EQ(cls(e_x(31, 4, 274 & 31u, 274 >> 5, 467, 0), false, &e), JX_MTSPR);
    CHECK_EQ(e.c, offsetof(ppc_t, sprg[2]));
    // mfspr r3,HID0 (not one of the translator's SPRs) stays generic
    CHECK_EQ(cls(e_x(31, 3, 1008 & 31u, 1008 >> 5, 339, 0), false, &e), T1_MFSPR);
    // mftbu r3: c = 1
    CHECK_EQ(cls(e_x(31, 3, 269 & 31u, 269 >> 5, 371, 0), false, &e), JX_MFTB);
    CHECK_EQ(e.c, 1);
    // divwo stays generic; divw. is JX_ARITH with Rc << 16
    CHECK_EQ(cls(e_xo(3, 4, 5, 1, 491, 0), false, &e), T1_DIVW);
    CHECK_EQ(cls(e_xo(3, 4, 5, 0, 491, 1), false, &e), JX_ARITH);
    CHECK_EQ(e.c, PPD_GOFF(5) | (uint32_t)XK_DIVW << 8 | 1u << 16);
    // mcrxr cr2; with a reserved bit set it stays generic
    CHECK_EQ(cls(e_x(31, 2u << 2, 0, 0, 512, 0), false, &e), JX_MCRXR);
    CHECK_EQ(e.a, 2);
    CHECK_EQ(cls(e_x(31, 2u << 2, 1, 0, 512, 0), false, &e), T1_MCRXR);
}

// Floating point and dcbz: the 604/750 only, record forms in C, the JX_FP
// kind naming the operation
TEST(test_fp_and_models) {
    pd_entry_t e;
    uint32_t fadd = e_a(63, 1, 2, 3, 0, 21, 0), fmadds = e_a(59, 1, 2, 3, 4, 29, 0), fneg = e_x(63, 1, 0, 2, 40, 0);
    CHECK_EQ(cls(fadd, false, &e), JX_FP);
    CHECK_EQ(e.a, PPC_SF_ADD);
    CHECK_EQ(e.c, fadd);
    CHECK_EQ(cls(fmadds, false, &e), JX_FP);
    CHECK_EQ(e.a, PPC_SF_MADD);
    CHECK_EQ(cls(fneg, false, &e), JX_FP);
    CHECK_EQ(e.a, JF_FNEG);
    CHECK_EQ(cls(e_x(63, 1, 0, 2, 15, 0), false, &e), JX_FP); // fctiwz
    CHECK_EQ(e.a, JF_FCTIWZ);
    CHECK_EQ(cls(e_a(63, 1, 2, 3, 4, 23, 0), false, &e), JX_FP); // fsel
    CHECK_EQ(e.a, JF_FSEL);
    CHECK_EQ(cls(fadd | 1u, false, &e), T1_FADD); // fadd.
    CHECK_EQ(cls(fadd, true, &e), T1_FADD); // the 601
    CHECK_EQ(cls(e_x(63, 3u << 2, 1, 2, 32, 0), false, &e), JX_FCMP); // fcmpo cr3
    CHECK_EQ(e.a, 3);
    CHECK_EQ(cls(e_x(63, 3u << 2, 1, 2, 32, 0), true, &e), T1_FCMPO);
    CHECK_EQ(cls(e_x(63, 1, 0, 0, 583, 0), false, &e), T1_MFFS); // the FPSCR moves stay in C
    CHECK_EQ(cls(48u << 26 | 1u << 21 | 4u << 16 | 16u, false, &e), JX_LFS); // lfs f1,16(r4)
    CHECK_EQ(e.a, 1);
    CHECK_EQ(e.b, PPD_GOFF(4));
    CHECK_EQ(cls(49u << 26 | 1u << 21 | 16u, false, &e), T1_LFSU); // lfsu with rA = 0
    CHECK_EQ(cls(48u << 26 | 1u << 21 | 4u << 16 | 16u, true, &e), T1_LFS);
    CHECK_EQ(cls(e_x(31, 0, 4, 5, 1014, 0), false, &e), JX_DCBZ);
    CHECK_EQ(cls(e_x(31, 0, 4, 5, 1014, 0), true, &e), T1_DCBZ);
}

// ppc_execute_fp: the decode tree with only the FP register-to-register
// leaves live -- the translation tier's FP helper
TEST(test_execute_fp) {
    ppc_t *p = ppc_init(NULL, CPU_MODEL_PPC604);
    CHECK(p != NULL);
    if (!p)
        return;
    p->msr |= PPC_MSR_FP;
    p->fpscr = 0;
    double two = 2.0, three = 3.0, five;
    memcpy(&p->fpr[2], &two, 8);
    memcpy(&p->fpr[3], &three, 8);
    CHECK_EQ(ppc_execute_fp(p, e_a(63, 1, 2, 3, 0, 21, 0)), 0); // fadd f1,f2,f3
    memcpy(&five, &p->fpr[1], 8);
    CHECK(five == 5.0);
    CHECK_EQ(ppc_execute_fp(p, e_x(63, 4, 0, 3, 40, 0)), 0); // fneg f4,f3
    CHECK_EQ64(p->fpr[4], p->fpr[3] ^ 0x8000000000000000ull);
    uint32_t cr = p->cr;
    CHECK_EQ(ppc_execute_fp(p, e_x(63, 2u << 2, 2, 3, 0, 0)), 0); // fcmpu cr2,f2,f3: less than
    CHECK_EQ((p->cr >> (28 - 8)) & 0xFu, 0x8u);
    p->cr = cr;
    // anything else is declined, untouched
    uint32_t r3 = p->gpr[3] = 0x1234u;
    CHECK(ppc_execute_fp(p, e_xo(3, 4, 5, 0, 266, 0)) != 0); // add
    CHECK_EQ(p->gpr[3], r3);
    CHECK(ppc_execute_fp(p, 48u << 26 | 1u << 21 | 4u << 16) != 0); // lfs
    ppc_delete(p);
}

int main(void) {
    test_context_t *ctx = calloc(1, sizeof(test_context_t));
    ctx->memory = memory_map_init(32, 0x800000, 0x20000, MEMORY_BUS_ERR_NONE, NULL, NULL);
    if (!ctx->memory) {
        printf("FAIL: memory_map_init\n");
        return 1;
    }
    test_set_active_context(ctx);

    RUN(test_illegal_never_translated);
    RUN(test_shapes);
    RUN(test_fp_and_models);
    RUN(test_execute_fp);
    return CHECK_SUMMARY("ppc_jit_classify");
}
