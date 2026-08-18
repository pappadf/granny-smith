// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// test.c — directed unit tests for the PPC (MPC601) core
// (src/core/cpu/ppc/), proposal-powerpc-601-pdm.md §7 layer 2.
//
// No public 601 instruction-level test corpus exists (the proposal's
// largest stated correctness risk), so these are directed semantics tests
// written from the 601UM chapter-10 RTL and chapter-5 exception tables:
// carry/overflow/record-form matrices, rotate-mask edges, every POWER
// holdover against its documented RTL (MQ side effects included), the SPR
// read/write asymmetries, the exception model, and the §5.4.6 alignment
// rules.  Encodings are built by the helpers below (chapter-10 field
// layouts); the same words are cross-checked against the objdump-validated
// disassembler so a field-layout typo here cannot silently agree with the
// same typo in the decoder.

#include "ppc_internal.h"

#include "harness.h"
#include "memory.h"
#include "ppc_disasm.h"
#include "ppc_softfp.h" // PPC_FPSCR_* bit names

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(got, want)                                                                                            \
    do {                                                                                                               \
        checks++;                                                                                                      \
        uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);                                                          \
        if (g_ != w_) {                                                                                                \
            printf("FAIL %s:%d: %s = $%08X, want $%08X\n", __FILE__, __LINE__, #got, g_, w_);                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static ppc_t *P;

// === Encoders (601UM chapter-10 field layouts) ===
static uint32_t e_d(uint32_t op, uint32_t rt, uint32_t ra, uint32_t imm16) {
    return (op << 26) | (rt << 21) | (ra << 16) | (imm16 & 0xFFFFu);
}
static uint32_t e_x(uint32_t rt, uint32_t ra, uint32_t rb, uint32_t xo, uint32_t rc) {
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
}
static uint32_t e_xo(uint32_t rt, uint32_t ra, uint32_t rb, uint32_t oe, uint32_t xo9, uint32_t rc) {
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (oe << 10) | (xo9 << 1) | rc;
}
static uint32_t e_rlw(uint32_t op, uint32_t rs, uint32_t ra, uint32_t sh, uint32_t mb, uint32_t me, uint32_t rc) {
    return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc;
}
static uint32_t e_bc(uint32_t bo, uint32_t bi, int32_t bd, uint32_t aa, uint32_t lk) {
    return (16u << 26) | (bo << 21) | (bi << 16) | ((uint32_t)bd & 0xFFFCu) | (aa << 1) | lk;
}
// Opcode-63 X-form (mffs, mtfsb0/mtfsb1, fcmpu/fcmpo)
static uint32_t e_x63(uint32_t d, uint32_t a, uint32_t b, uint32_t xo, uint32_t rc) {
    return (63u << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | rc;
}
static uint32_t e_bcctr(uint32_t bo, uint32_t bi, uint32_t lk) {
    return (19u << 26) | (bo << 21) | (bi << 16) | (528u << 1) | lk;
}
static uint32_t e_spr(uint32_t rt, uint32_t spr, int to_spr) {
    uint32_t f = ((spr & 0x1Fu) << 16) | ((spr >> 5) << 11);
    return (31u << 26) | (rt << 21) | f | ((to_spr ? 467u : 339u) << 1);
}

// === Execution helpers ===

// Write one instruction at `addr` and execute exactly n instructions
// starting there.
static void run_at(uint32_t addr, int n) {
    P->pc = addr;
    uint32_t budget = (uint32_t)n;
    ppc_run(P, &budget);
}

static void step1(uint32_t iw) {
    memory_write_uint32(0x1000, iw);
    run_at(0x1000, 1);
}

// Execute one instruction and cross-check the decoder against the
// objdump-validated disassembler: an executed word must disassemble, and
// expected-illegal words must not.
static void step1_valid(uint32_t iw) {
    ppc_insn ins;
    ppc_disassemble(iw, 0x1000, &ins);
    CHECK(ins.status == PPC_DIS_OK);
    step1(iw);
}

// Reset to a clean supervisor state with EP cleared so exception vectors
// land at $00000xxx (mapped RAM), translation off, FP on.
static void fresh(void) {
    ppc_reset(P);
    P->msr = PPC_MSR_ME | PPC_MSR_FP;
    ppc_update_active_maps(P);
}

// Give every segment a T=1 memory-forced identity mapping (the HWInit
// state, $87F0000n) so MSR[DT] tests translate EA=PA — with zeroed SRs a
// DT=1 access would take the loud Phase-D T=0 DSI instead.
static void identity_segments(void) {
    for (uint32_t i = 0; i < 16; i++)
        P->sr[i] = 0x87F00000u | i;
}

// === Tests ===

static void test_reset_state(void) {
    ppc_reset(P);
    CHECK_EQ(P->msr, 0x00001040u); // ME + EP (601UM Table 5-8)
    CHECK_EQ(P->pc, 0xFFF00100u);
    CHECK_EQ(P->pvr, 0x00010001u);
    CHECK_EQ(P->hid0, 0x80010080u);
    CHECK_EQ(P->cr, 0);
    CHECK_EQ(P->gpr[1], 0);
}

// addic/addc/adde carry and add/addo overflow matrices
static void test_add_carry_overflow(void) {
    static const struct {
        uint32_t a, b;
        uint32_t r;
        int ca, ov;
    } v[] = {
        {1,           2,           3,           0, 0},
        {0xFFFFFFFFu, 1,           0,           1, 0},
        {0x7FFFFFFFu, 1,           0x80000000u, 0, 1},
        {0x80000000u, 0x80000000u, 0,           1, 1},
        {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFEu, 1, 0},
    };
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        fresh();
        P->gpr[4] = v[i].a;
        P->gpr[5] = v[i].b;
        step1_valid(e_xo(3, 4, 5, 1, 10, 0)); // addco r3,r4,r5
        CHECK_EQ(P->gpr[3], v[i].r);
        CHECK_EQ((P->xer & PPC_XER_CA) != 0, v[i].ca);
        CHECK_EQ((P->xer & PPC_XER_OV) != 0, v[i].ov);
    }
    // adde chains the carry
    fresh();
    P->gpr[4] = 0xFFFFFFFFu;
    P->gpr[5] = 1;
    memory_write_uint32(0x1000, e_xo(3, 4, 5, 0, 10, 0)); // addc: CA=1
    memory_write_uint32(0x1004, e_xo(6, 4, 4, 0, 138, 0)); // adde r6,r4,r4 (+CA)
    run_at(0x1000, 2);
    CHECK_EQ(P->gpr[3], 0);
    CHECK_EQ(P->gpr[6], 0xFFFFFFFFu); // FFFFFFFF+FFFFFFFF+1
    CHECK(P->xer & PPC_XER_CA);
    // addze/addme
    fresh();
    P->xer = PPC_XER_CA;
    P->gpr[4] = 41;
    step1_valid(e_xo(3, 4, 0, 0, 202, 0)); // addze
    CHECK_EQ(P->gpr[3], 42);
    fresh();
    P->xer = 0;
    P->gpr[4] = 42;
    step1_valid(e_xo(3, 4, 0, 0, 234, 0)); // addme: 42 + (-1) + 0
    CHECK_EQ(P->gpr[3], 41);
    CHECK(P->xer & PPC_XER_CA); // 42 + FFFFFFFF carries
    // SO is sticky: a later non-overflowing op keeps it
    fresh();
    P->gpr[4] = 0x7FFFFFFFu;
    P->gpr[5] = 1;
    memory_write_uint32(0x1000, e_xo(3, 4, 5, 1, 10, 0)); // addco → OV+SO
    memory_write_uint32(0x1004, e_xo(3, 5, 5, 1, 10, 0)); // 1+1: OV clears, SO stays
    run_at(0x1000, 2);
    CHECK(!(P->xer & PPC_XER_OV));
    CHECK(P->xer & PPC_XER_SO);
}

static void test_subf_carry(void) {
    // subfc: CA = NOT borrow (~a + b + 1 carries)
    fresh();
    P->gpr[4] = 1;
    P->gpr[5] = 2;
    step1_valid(e_xo(3, 4, 5, 0, 8, 0)); // subfc r3,r4,r5 = 2-1
    CHECK_EQ(P->gpr[3], 1);
    CHECK(P->xer & PPC_XER_CA);
    fresh();
    P->gpr[4] = 2;
    P->gpr[5] = 1;
    step1_valid(e_xo(3, 4, 5, 0, 8, 0)); // 1-2 borrows
    CHECK_EQ(P->gpr[3], 0xFFFFFFFFu);
    CHECK(!(P->xer & PPC_XER_CA));
    // subfic
    fresh();
    P->gpr[4] = 3;
    step1_valid(e_d(8, 3, 4, 10)); // subfic r3,r4,10
    CHECK_EQ(P->gpr[3], 7);
    CHECK(P->xer & PPC_XER_CA);
    // neg of most-negative
    fresh();
    P->gpr[4] = 0x80000000u;
    step1_valid(e_xo(3, 4, 0, 1, 104, 0)); // nego
    CHECK_EQ(P->gpr[3], 0x80000000u);
    CHECK(P->xer & PPC_XER_OV);
}

static void test_record_and_compare(void) {
    fresh();
    P->gpr[4] = 0x80000000u;
    P->gpr[5] = 0;
    step1_valid(e_x(4, 3, 5, 444, 1)); // or. r3,r4,r5 (negative result)
    CHECK_EQ(P->cr >> 28, 8u); // LT
    fresh();
    P->gpr[4] = 0;
    step1_valid(e_d(28, 4, 3, 0xFF)); // andi. r3,r4,$FF → zero
    CHECK_EQ(P->cr >> 28, 2u); // EQ
    // cmpwi / cmplwi into cr5
    fresh();
    P->gpr[4] = 0xFFFFFFFFu; // -1 signed, huge unsigned
    step1_valid(e_d(11, 5 << 2, 4, 5)); // cmpwi cr5,r4,5
    CHECK_EQ((P->cr >> 8) & 0xFu, 8u); // LT signed
    step1_valid(e_d(10, 5 << 2, 4, 5)); // cmplwi cr5,r4,5
    CHECK_EQ((P->cr >> 8) & 0xFu, 4u); // GT unsigned
    // SO mirrors into the compare field
    fresh();
    P->xer = PPC_XER_SO;
    P->gpr[4] = 1;
    step1_valid(e_d(11, 0, 4, 1)); // cmpwi cr0,r4,1
    CHECK_EQ(P->cr >> 28, 3u); // EQ|SO
}

static void test_shifts(void) {
    fresh();
    P->gpr[4] = 0x80000001u;
    P->gpr[5] = 4;
    step1_valid(e_x(4, 3, 5, 24, 0)); // slw r3,r4,r5
    CHECK_EQ(P->gpr[3], 0x00000010u);
    P->gpr[5] = 32; // rb bit 26 set → zero
    step1_valid(e_x(4, 3, 5, 24, 0));
    CHECK_EQ(P->gpr[3], 0);
    // srawi: CA only when 1s shift out of a negative value
    fresh();
    P->gpr[4] = 0xFFFFFFF0u;
    step1_valid(e_x(4, 3, 4, 824, 0)); // srawi r3,r4,4 — no 1s lost
    CHECK_EQ(P->gpr[3], 0xFFFFFFFFu);
    CHECK(!(P->xer & PPC_XER_CA));
    P->gpr[4] = 0xFFFFFFF8u;
    step1_valid(e_x(4, 3, 4, 824, 0)); // srawi r3,r4,4 — a 1 shifts out
    CHECK_EQ(P->gpr[3], 0xFFFFFFFFu);
    CHECK(P->xer & PPC_XER_CA);
    P->gpr[4] = 0x00000008u; // positive: never CA
    step1_valid(e_x(4, 3, 4, 824, 0));
    CHECK(!(P->xer & PPC_XER_CA));
    // sraw with rb bit 26: fill with sign, CA if any 1 bits and negative
    fresh();
    P->gpr[4] = 0x80000000u;
    P->gpr[5] = 33;
    step1_valid(e_x(4, 3, 5, 792, 0));
    CHECK_EQ(P->gpr[3], 0xFFFFFFFFu);
    CHECK(P->xer & PPC_XER_CA);
}

static void test_rotates(void) {
    fresh();
    P->gpr[4] = 0x12345678u;
    step1_valid(e_rlw(21, 4, 3, 8, 0, 31, 0)); // rlwinm r3,r4,8,0,31
    CHECK_EQ(P->gpr[3], 0x34567812u);
    step1_valid(e_rlw(21, 4, 3, 0, 24, 31, 0)); // clrlwi 24 → low byte
    CHECK_EQ(P->gpr[3], 0x00000078u);
    // wrapping mask MB>ME
    step1_valid(e_rlw(21, 4, 3, 0, 28, 3, 0)); // mask = F000000F
    CHECK_EQ(P->gpr[3], 0x10000008u);
    // rlwimi inserts under mask
    fresh();
    P->gpr[4] = 0xAABBCCDDu;
    P->gpr[3] = 0x11223344u;
    step1_valid(e_rlw(20, 4, 3, 0, 0, 7, 0)); // insert top byte
    CHECK_EQ(P->gpr[3], 0xAA223344u);
    // rlwnm rotate by register
    fresh();
    P->gpr[4] = 0x000000FFu;
    P->gpr[5] = 8;
    step1_valid(e_rlw(23, 4, 3, 5, 0, 31, 0) | 0); // note: SH field = rb index
    CHECK_EQ(P->gpr[3], 0x0000FF00u);
}

static void test_mul_div(void) {
    fresh();
    P->gpr[4] = 0x10000u;
    P->gpr[5] = 0x10000u;
    step1_valid(e_x(3, 4, 5, 75, 0)); // mulhw
    CHECK_EQ(P->gpr[3], 1);
    step1_valid(e_xo(3, 4, 5, 1, 235, 0)); // mullwo — overflows
    CHECK_EQ(P->gpr[3], 0);
    CHECK(P->xer & PPC_XER_OV);
    fresh();
    P->gpr[4] = 0xFFFFFFFFu; // -1
    P->gpr[5] = 0xFFFFFFFFu;
    step1_valid(e_x(3, 4, 5, 11, 0)); // mulhwu: (2^32-1)^2 high
    CHECK_EQ(P->gpr[3], 0xFFFFFFFEu);
    step1_valid(e_d(7, 3, 4, 0xFFF9)); // mulli r3,r4,-7
    CHECK_EQ(P->gpr[3], 7);
    // divw signed + overflow determinism
    fresh();
    P->gpr[4] = 0xFFFFFFF9u; // -7
    P->gpr[5] = 2;
    step1_valid(e_xo(3, 4, 5, 0, 491, 0)); // divw
    CHECK_EQ(P->gpr[3], 0xFFFFFFFDu); // -3 (truncation toward zero)
    P->gpr[5] = 0;
    step1_valid(e_xo(3, 4, 5, 1, 491, 0)); // divwo by zero
    CHECK_EQ(P->gpr[3], 0); // deterministic undefined result
    CHECK(P->xer & PPC_XER_OV);
    fresh();
    P->gpr[4] = 0x80000000u;
    P->gpr[5] = 0xFFFFFFFFu;
    step1_valid(e_xo(3, 4, 5, 1, 491, 0)); // divwo INT_MIN/-1
    CHECK_EQ(P->gpr[3], 0);
    CHECK(P->xer & PPC_XER_OV);
    // divwu
    fresh();
    P->gpr[4] = 7;
    P->gpr[5] = 2;
    step1_valid(e_xo(3, 4, 5, 0, 459, 0));
    CHECK_EQ(P->gpr[3], 3);
}

// POWER holdovers against their 601UM RTL
static void test_power_arith(void) {
    fresh();
    P->gpr[4] = 0xFFFFFF00u;
    step1_valid(e_xo(3, 4, 0, 0, 360, 0)); // abs
    CHECK_EQ(P->gpr[3], 0x100u);
    P->gpr[4] = 0x80000000u;
    step1_valid(e_xo(3, 4, 0, 1, 360, 0)); // abso of INT_MIN
    CHECK_EQ(P->gpr[3], 0x80000000u);
    CHECK(P->xer & PPC_XER_OV);
    // nabs never overflows; OE=1 clears OV, keeps SO
    step1_valid(e_xo(3, 4, 0, 1, 488, 0)); // nabso
    CHECK_EQ(P->gpr[3], 0x80000000u);
    CHECK(!(P->xer & PPC_XER_OV));
    CHECK(P->xer & PPC_XER_SO); // sticky from the abso above
    // doz
    fresh();
    P->gpr[4] = 5;
    P->gpr[5] = 3;
    step1_valid(e_xo(3, 4, 5, 0, 264, 0)); // a > b → 0
    CHECK_EQ(P->gpr[3], 0);
    P->gpr[4] = 3;
    P->gpr[5] = 5;
    step1_valid(e_xo(3, 4, 5, 0, 264, 0));
    CHECK_EQ(P->gpr[3], 2);
    step1_valid(e_d(9, 3, 4, 10)); // dozi r3,r4,10
    CHECK_EQ(P->gpr[3], 7);
    step1_valid(e_d(9, 3, 4, 1)); // 3 > 1 → 0
    CHECK_EQ(P->gpr[3], 0);
    // mul: rD = high, MQ = low; Rc records on MQ
    fresh();
    P->gpr[4] = 0x10000u;
    P->gpr[5] = 0x12345u;
    step1_valid(e_xo(3, 4, 5, 0, 107, 1)); // mul. r3,r4,r5
    CHECK_EQ(P->gpr[3], 1);
    CHECK_EQ(P->mq, 0x23450000u);
    CHECK_EQ(P->cr >> 28, 4u); // CR0 reflects MQ (positive → GT)
    // div: (rA||MQ)/rB, remainder → MQ (sign follows dividend)
    fresh();
    P->gpr[4] = 0;
    P->mq = 100;
    P->gpr[5] = 7;
    step1_valid(e_xo(3, 4, 5, 0, 331, 0)); // div
    CHECK_EQ(P->gpr[3], 14);
    CHECK_EQ(P->mq, 2);
    fresh();
    P->gpr[4] = 0xFFFFFFFFu; // dividend -100 (64-bit sign through rA)
    P->mq = (uint32_t)-100;
    P->gpr[5] = 7;
    step1_valid(e_xo(3, 4, 5, 0, 331, 0));
    CHECK_EQ(P->gpr[3], (uint32_t)-14);
    CHECK_EQ(P->mq, (uint32_t)-2); // remainder sign follows dividend
    // divs
    fresh();
    P->gpr[4] = (uint32_t)-100;
    P->gpr[5] = 7;
    step1_valid(e_xo(3, 4, 5, 0, 363, 0));
    CHECK_EQ(P->gpr[3], (uint32_t)-14);
    CHECK_EQ(P->mq, (uint32_t)-2);
    fresh();
    P->gpr[4] = 0x80000000u;
    P->gpr[5] = 0xFFFFFFFFu;
    step1_valid(e_xo(3, 4, 5, 1, 363, 0)); // divso INT_MIN/-1: documented values
    CHECK_EQ(P->gpr[3], 0x80000000u);
    CHECK_EQ(P->mq, 0);
    CHECK(P->xer & PPC_XER_OV);
}

static void test_power_masks_shifts(void) {
    // maskg: the three cases of the 601UM page
    fresh();
    P->gpr[4] = 8; // mstart
    P->gpr[5] = 15; // mstop
    step1_valid(e_x(4, 3, 5, 29, 0)); // maskg r3,r4,r5
    CHECK_EQ(P->gpr[3], 0x00FF0000u); // ones bits 8..15 (BE)
    P->gpr[4] = 16;
    P->gpr[5] = 15; // mstart = mstop+1 → all ones
    step1_valid(e_x(4, 3, 5, 29, 0));
    CHECK_EQ(P->gpr[3], 0xFFFFFFFFu);
    P->gpr[4] = 17;
    P->gpr[5] = 15; // mstart > mstop+1 → zeros in 16..16
    step1_valid(e_x(4, 3, 5, 29, 0));
    CHECK_EQ(P->gpr[3], 0xFFFF7FFFu);
    // maskir
    fresh();
    P->gpr[4] = 0xAAAAAAAAu; // rS
    P->gpr[3] = 0x11111111u; // rA
    P->gpr[5] = 0xFF00FF00u; // mask
    step1_valid(e_x(4, 3, 5, 541, 0));
    CHECK_EQ(P->gpr[3], 0xAA11AA11u);
    // rrib
    fresh();
    P->gpr[4] = 0x80000000u;
    P->gpr[3] = 0;
    P->gpr[5] = 4;
    step1_valid(e_x(4, 3, 5, 537, 0));
    CHECK_EQ(P->gpr[3], 0x08000000u);
    // sle / sleq
    fresh();
    P->gpr[4] = 0x12345678u;
    P->gpr[5] = 8;
    step1_valid(e_x(4, 3, 5, 153, 0)); // sle
    CHECK_EQ(P->gpr[3], 0x34567800u);
    CHECK_EQ(P->mq, 0x34567812u); // rotated word
    P->mq = 0x000000FFu;
    step1_valid(e_x(4, 3, 5, 217, 0)); // sleq merges old MQ under mask
    CHECK_EQ(P->gpr[3], 0x345678FFu);
    CHECK_EQ(P->mq, 0x34567812u);
    // sliq / slliq
    fresh();
    P->gpr[4] = 0x12345678u;
    step1_valid(e_x(4, 3, 4, 184, 0)); // sliq r3,r4,4
    CHECK_EQ(P->gpr[3], 0x23456780u);
    CHECK_EQ(P->mq, 0x23456781u);
    P->mq = 0x0000000Fu;
    step1_valid(e_x(4, 3, 4, 248, 0)); // slliq merges MQ
    CHECK_EQ(P->gpr[3], 0x2345678Fu);
    // sllq bit26 cases (64-bit shift second word)
    fresh();
    P->gpr[4] = 0xAABBCCDDu;
    P->gpr[5] = 8; // bit26 clear
    P->mq = 0x11223344u;
    step1_valid(e_x(4, 3, 5, 216, 0)); // sllq
    CHECK_EQ(P->gpr[3], 0xBBCCDD44u); // (rot & m) | (MQ & ~m)
    CHECK_EQ(P->mq, 0x11223344u); // MQ unaltered
    P->gpr[5] = 8 | 32; // bit26 set
    step1_valid(e_x(4, 3, 5, 216, 0));
    CHECK_EQ(P->gpr[3], 0x11223300u); // MQ & (ones<<8)
    // slq
    fresh();
    P->gpr[4] = 0x12345678u;
    P->gpr[5] = 8;
    step1_valid(e_x(4, 3, 5, 152, 0));
    CHECK_EQ(P->gpr[3], 0x34567800u);
    CHECK_EQ(P->mq, 0x34567812u);
    P->gpr[5] = 8 | 32;
    step1_valid(e_x(4, 3, 5, 152, 0)); // bit26 → zero result, MQ still set
    CHECK_EQ(P->gpr[3], 0);
    CHECK_EQ(P->mq, 0x34567812u);
    // sre / sreq / sriq / srliq / srq / srlq
    fresh();
    P->gpr[4] = 0x12345678u;
    P->gpr[5] = 8;
    step1_valid(e_x(4, 3, 5, 665, 0)); // sre
    CHECK_EQ(P->gpr[3], 0x00123456u);
    CHECK_EQ(P->mq, 0x78123456u);
    P->mq = 0xFF000000u;
    step1_valid(e_x(4, 3, 5, 729, 0)); // sreq merges MQ
    CHECK_EQ(P->gpr[3], 0xFF123456u);
    fresh();
    P->gpr[4] = 0x12345678u;
    step1_valid(e_x(4, 3, 8, 696, 0)); // sriq r3,r4,8
    CHECK_EQ(P->gpr[3], 0x00123456u);
    CHECK_EQ(P->mq, 0x78123456u);
    fresh();
    P->gpr[4] = 0x12345678u;
    P->gpr[5] = 8;
    P->mq = 0xAABBCCDDu;
    step1_valid(e_x(4, 3, 5, 728, 0)); // srlq bit26=0
    CHECK_EQ(P->gpr[3], 0xAA123456u); // (rot & ones>>8) | (MQ & ~..)
    CHECK_EQ(P->mq, 0xAABBCCDDu);
    P->gpr[5] = 8 | 32;
    step1_valid(e_x(4, 3, 5, 728, 0)); // bit26=1: MQ & (ones>>8)
    CHECK_EQ(P->gpr[3], 0x00BBCCDDu);
    // sraq / sraiq / srea CA
    fresh();
    P->gpr[4] = 0xFFFFFF01u; // negative, 1s in low byte
    P->gpr[5] = 8;
    step1_valid(e_x(4, 3, 5, 920, 0)); // sraq
    CHECK_EQ(P->gpr[3], 0xFFFFFFFFu);
    CHECK(P->xer & PPC_XER_CA); // 1s shifted out of a negative
    P->gpr[4] = 0xFFFFFF00u; // no 1s shifted out
    step1_valid(e_x(4, 3, 5, 920, 0));
    CHECK(!(P->xer & PPC_XER_CA));
    fresh();
    P->gpr[4] = 0xFFFFFF01u;
    step1_valid(e_x(4, 3, 8, 952, 0)); // sraiq r3,r4,8
    CHECK_EQ(P->gpr[3], 0xFFFFFFFFu);
    CHECK(P->xer & PPC_XER_CA);
    // rlmi
    fresh();
    P->gpr[4] = 0x000000FFu;
    P->gpr[5] = 8;
    P->gpr[3] = 0x11111111u;
    step1_valid(e_rlw(22, 4, 3, 5, 16, 23, 0)); // rlmi: insert rot<<8 under mask
    CHECK_EQ(P->gpr[3], 0x1111FF11u);
    // clcs returns 64
    fresh();
    P->gpr[4] = 12;
    step1_valid(e_x(3, 4, 0, 531, 0));
    CHECK_EQ(P->gpr[3], 64);
}

static void test_branches(void) {
    // b / bl
    fresh();
    memory_write_uint32(0x1000, (18u << 26) | 0x100 | 1); // bl +0x100
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x1100u);
    CHECK_EQ(P->lr, 0x1004u);
    // bc: bdnz loop takes then falls through
    fresh();
    P->ctr = 2;
    memory_write_uint32(0x1000, e_bc(16, 0, 0, 0, 0)); // bdnz .
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x1000u); // taken (ctr 2→1)
    CHECK_EQ(P->ctr, 1);
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x1004u); // not taken (ctr 1→0)
    // beq on CR0
    fresh();
    P->cr = 0x20000000u; // CR0 = EQ
    memory_write_uint32(0x1000, e_bc(12, 2, 0x20, 0, 0)); // beq +0x20
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x1020u);
    P->cr = 0;
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x1004u);
    // bclr
    fresh();
    P->lr = 0x2000;
    memory_write_uint32(0x1000, (19u << 26) | (20u << 21) | (16u << 1)); // blr
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x2000u);
    // bcctr
    fresh();
    P->ctr = 0x3000;
    memory_write_uint32(0x1000, (19u << 26) | (20u << 21) | (528u << 1)); // bctr
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x3000u);
    // absolute branch
    fresh();
    memory_write_uint32(0x1000, (18u << 26) | 0x2000 | 2); // ba $2000
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x2000u);
}

static void test_cr_ops(void) {
    fresh();
    P->cr = 0;
    // crset bit 0 via creqv 0,0,0
    memory_write_uint32(0x1000, (19u << 26) | (0u << 21) | (0u << 16) | (0u << 11) | (289u << 1));
    run_at(0x1000, 1);
    CHECK_EQ(P->cr, 0x80000000u);
    // crxor clears
    memory_write_uint32(0x1000, (19u << 26) | (0u << 21) | (0u << 16) | (0u << 11) | (193u << 1));
    run_at(0x1000, 1);
    CHECK_EQ(P->cr, 0);
    // mtcrf field mask
    fresh();
    P->gpr[4] = 0xFFFFFFFFu;
    step1_valid((31u << 26) | (4u << 21) | (0x40u << 12) | (144u << 1)); // mtcrf $40 (field 1)
    CHECK_EQ(P->cr, 0x0F000000u);
    // mfcr
    step1_valid(e_x(3, 0, 0, 19, 0));
    CHECK_EQ(P->gpr[3], 0x0F000000u);
    // mcrf
    fresh();
    P->cr = 0x0A000000u; // field 1 = A
    memory_write_uint32(0x1000, (19u << 26) | (3u << 23) | (1u << 18)); // mcrf cr3,cr1
    run_at(0x1000, 1);
    CHECK_EQ((P->cr >> 16) & 0xFu, 0xAu);
    // mcrxr copies XER[0-3] and clears them
    fresh();
    P->xer = 0xE0000000u | 5;
    step1_valid((31u << 26) | (2u << 23) | (512u << 1)); // mcrxr cr2
    CHECK_EQ((P->cr >> 20) & 0xFu, 0xEu);
    CHECK_EQ(P->xer, 5u);
}

static void test_loads_stores(void) {
    fresh();
    memory_write_uint32(0x2000, 0x11223344u);
    P->gpr[4] = 0x2000;
    step1_valid(e_d(32, 3, 4, 0)); // lwz
    CHECK_EQ(P->gpr[3], 0x11223344u);
    step1_valid(e_d(34, 3, 4, 1)); // lbz +1
    CHECK_EQ(P->gpr[3], 0x22u);
    step1_valid(e_d(40, 3, 4, 2)); // lhz +2
    CHECK_EQ(P->gpr[3], 0x3344u);
    memory_write_uint16(0x2004, 0x8000u);
    step1_valid(e_d(42, 3, 4, 4)); // lha → sign extends
    CHECK_EQ(P->gpr[3], 0xFFFF8000u);
    // stores + update forms
    fresh();
    P->gpr[3] = 0xCAFEBABEu;
    P->gpr[4] = 0x2000;
    step1_valid(e_d(37, 3, 4, 0x10)); // stwu r3,16(r4)
    CHECK_EQ(P->gpr[4], 0x2010u);
    CHECK_EQ(memory_read_uint32(0x2010), 0xCAFEBABEu);
    // indexed + byte-reversed
    fresh();
    P->gpr[4] = 0x2000;
    P->gpr[5] = 4;
    memory_write_uint32(0x2004, 0x01020304u);
    step1_valid(e_x(3, 4, 5, 534, 0)); // lwbrx
    CHECK_EQ(P->gpr[3], 0x04030201u);
    step1_valid(e_x(3, 4, 5, 790, 0)); // lhbrx
    CHECK_EQ(P->gpr[3], 0x0201u);
    P->gpr[3] = 0x0A0B0C0Du;
    step1_valid(e_x(3, 4, 5, 662, 0)); // stwbrx
    CHECK_EQ(memory_read_uint32(0x2004), 0x0D0C0B0Au);
    // (rA|0): ra=0 reads as zero not r0
    fresh();
    P->gpr[0] = 0xDEAD0000u;
    memory_write_uint32(0x3000, 0x55667788u);
    step1_valid(e_d(32, 3, 0, 0x3000)); // lwz r3,0x3000(0)
    CHECK_EQ(P->gpr[3], 0x55667788u);
}

static void test_multiple_and_strings(void) {
    // lmw/stmw round trip
    fresh();
    for (int i = 0; i < 8; i++)
        P->gpr[24 + i] = 0x1010101u * (uint32_t)(i + 1);
    P->gpr[4] = 0x2400;
    step1_valid(e_d(47, 24, 4, 0)); // stmw r24,0(r4)
    for (int i = 0; i < 8; i++)
        P->gpr[24 + i] = 0;
    step1_valid(e_d(46, 24, 4, 0)); // lmw r24,0(r4)
    CHECK_EQ(P->gpr[24], 0x1010101u);
    CHECK_EQ(P->gpr[31], 0x8080808u);
    // lswi with partial last register (zero fill)
    fresh();
    memory_write_uint32(0x2500, 0xAABBCCDDu);
    memory_write_uint32(0x2504, 0xEE000000u);
    P->gpr[4] = 0x2500;
    P->gpr[6] = 0xFFFFFFFFu;
    step1_valid(e_x(5, 4, 5, 597, 0)); // lswi r5,r4,5
    CHECK_EQ(P->gpr[5], 0xAABBCCDDu);
    CHECK_EQ(P->gpr[6], 0xEE000000u); // 1 byte + zero fill
    // stswi
    fresh();
    P->gpr[5] = 0x11223344u;
    P->gpr[6] = 0x55000000u;
    P->gpr[4] = 0x2600;
    step1_valid(e_x(5, 4, 5, 725, 0)); // stswi r5,r4,5
    CHECK_EQ(memory_read_uint32(0x2600), 0x11223344u);
    CHECK_EQ(memory_read_uint8(0x2604), 0x55u);
    // lswx takes the count from XER[25-31]
    fresh();
    memory_write_uint32(0x2700, 0x99887766u);
    P->gpr[4] = 0x2700;
    P->gpr[5] = 0;
    P->xer = 4; // 4 bytes
    step1_valid(e_x(7, 4, 5, 533, 0)); // lswx r7,r4,r5
    CHECK_EQ(P->gpr[7], 0x99887766u);
    // lscbx: stops at the XER compare byte, updates the count
    fresh();
    memory_write_uint32(0x2800, 0x41424344u); // "ABCD"
    memory_write_uint32(0x2804, 0x45464748u); // "EFGH"
    P->gpr[4] = 0x2800;
    P->gpr[5] = 0;
    P->xer = (0x45u << 8) | 8; // compare byte 'E', count 8
    step1_valid(e_x(7, 4, 5, 277, 1)); // lscbx. r7,r4,r5
    CHECK_EQ(P->xer & PPC_XER_BYTES, 5u); // A,B,C,D,E
    CHECK_EQ(P->cr >> 28, 2u); // match found → EQ position
    CHECK_EQ(P->gpr[7], 0x41424344u);
    // no match: count unchanged
    fresh();
    P->gpr[4] = 0x2800;
    P->gpr[5] = 0;
    P->xer = (0xEEu << 8) | 4;
    step1_valid(e_x(7, 4, 5, 277, 1));
    CHECK_EQ(P->xer & PPC_XER_BYTES, 4u);
    CHECK_EQ((P->cr >> 28) & 2u, 0u);
}

static void test_atomics_and_dcbz(void) {
    fresh();
    P->gpr[4] = 0x2900;
    P->gpr[5] = 0;
    memory_write_uint32(0x2900, 123);
    step1_valid(e_x(3, 4, 5, 20, 0)); // lwarx
    CHECK_EQ(P->gpr[3], 123u);
    P->gpr[3] = 456;
    step1_valid(e_x(3, 4, 5, 150, 1)); // stwcx. — reservation held
    CHECK_EQ(memory_read_uint32(0x2900), 456u);
    CHECK_EQ(P->cr >> 28, 2u); // EQ = stored
    step1_valid(e_x(3, 4, 5, 150, 1)); // no reservation now
    CHECK_EQ(P->cr >> 28, 0u);
    // dcbz zeroes the aligned 32-byte block
    fresh();
    for (uint32_t a = 0x2A00; a < 0x2A40; a += 4)
        memory_write_uint32(a, 0xFFFFFFFFu);
    P->gpr[5] = 0x2A28; // inside the second block
    step1_valid(e_x(0, 0, 5, 1014, 0)); // dcbz 0,r5
    CHECK_EQ(memory_read_uint32(0x2A20), 0u);
    CHECK_EQ(memory_read_uint32(0x2A3C), 0u);
    CHECK_EQ(memory_read_uint32(0x2A1C), 0xFFFFFFFFu);
}

static void test_sprs(void) {
    fresh();
    P->gpr[4] = 0x12345678u;
    step1_valid(e_spr(4, 8, 1)); // mtlr
    CHECK_EQ(P->lr, 0x12345678u);
    step1_valid(e_spr(3, 8, 0)); // mflr
    CHECK_EQ(P->gpr[3], 0x12345678u);
    // RTC asymmetry: write via SPR 20/21, read via 4/5
    fresh();
    P->gpr[4] = 0x1234u;
    step1_valid(e_spr(4, 20, 1)); // mtspr rtcu
    P->gpr[4] = 0xFFFFFFFFu;
    step1_valid(e_spr(4, 21, 1)); // mtspr rtcl — masked
    step1_valid(e_spr(3, 4, 0)); // mfspr r3,rtcu (SPR 4!)
    CHECK_EQ(P->gpr[3], 0x1234u);
    step1_valid(e_spr(3, 5, 0)); // mfspr r3,rtcl (SPR 5)
    CHECK_EQ(P->gpr[3], 0x3FFFFF80u); // RTCL implemented-bits mask
    // PVR reads, writes are a no-op (invalid mtspr target)
    fresh();
    step1_valid(e_spr(3, 287, 0));
    CHECK_EQ(P->gpr[3], 0x00010001u);
    // sprg round trip + BATs
    fresh();
    P->gpr[4] = 0xAA55AA55u;
    step1_valid(e_spr(4, 273, 1));
    CHECK_EQ(P->sprg[1], 0xAA55AA55u);
    step1_valid(e_spr(4, 534, 1)); // IBAT3U
    CHECK_EQ(P->batu[3], 0xAA55AA55u);
    // segment registers
    fresh();
    P->gpr[4] = 0x87F00005u; // the HWInit T=1 SR value (§3.4)
    step1_valid((31u << 26) | (4u << 21) | (5u << 16) | (210u << 1)); // mtsr 5,r4
    CHECK_EQ(P->sr[5], 0x87F00005u);
    step1_valid((31u << 26) | (3u << 21) | (5u << 16) | (595u << 1)); // mfsr r3,5
    CHECK_EQ(P->gpr[3], 0x87F00005u);
    // mtsrin/mfsrin index from rB high nibble
    fresh();
    P->gpr[4] = 0xBEEF;
    P->gpr[5] = 0xA0000000u;
    step1_valid(e_x(4, 0, 5, 242, 0)); // mtsrin r4,r5 → SR10
    CHECK_EQ(P->sr[10], 0xBEEFu);
}

static void test_exceptions(void) {
    // sc: SRR0 = next instruction, MSR bits cleared, EP-relative vector
    fresh(); // EP=0 → vectors at $000xxxxx
    memory_write_uint32(0x1000, 0x44000002u);
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x00000C00u);
    CHECK_EQ(P->srr0, 0x1004u);
    CHECK_EQ(P->srr1 & 0xFFFFu, PPC_MSR_ME | PPC_MSR_FP); // saved low MSR
    CHECK_EQ(P->msr, PPC_MSR_ME); // EE/PR/FP/IT/DT cleared, ME kept
    // EP=1 prefixes the vector
    ppc_reset(P); // MSR = ME|EP
    P->msr |= PPC_MSR_FP;
    memory_write_uint32(0x1000, 0x44000002u);
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0xFFF00C00u);
    // illegal: mftb is not a 601 instruction
    fresh();
    memory_write_uint32(0x1000, e_spr(3, 268, 0) + (32u << 1)); // mftb encoding (xo 371)
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x00000700u);
    CHECK_EQ(P->srr0, 0x1000u); // faulting instruction
    CHECK(P->srr1 & PPC_SRR1_PROG_ILLEGAL);
    // trap
    fresh();
    P->gpr[4] = 0;
    step1(e_d(3, 31, 4, 0)); // twi 31,r4,0 — unconditional
    CHECK_EQ(P->pc, 0x00000700u);
    CHECK(P->srr1 & PPC_SRR1_PROG_TRAP);
    fresh();
    P->gpr[4] = 5;
    step1(e_d(3, 16, 4, 3)); // twi lt,r4,3 — 5 < 3 false
    CHECK_EQ(P->pc, 0x1004u); // fell through
    // privileged from user mode + SoA switch.  The user maps hold the
    // MMU's logical fills, so they are active only for TRANSLATED user
    // data (PR=1 AND DT=1); user mode with translation off runs on the
    // identity view like everything else (proposal §3.5 as amended).
    fresh();
    identity_segments();
    P->msr |= PPC_MSR_PR;
    ppc_update_active_maps(P);
    CHECK(g_active_read == g_supervisor_read); // PR alone: identity view
    P->msr |= PPC_MSR_DT;
    ppc_update_active_maps(P);
    CHECK(g_active_read == g_user_read); // PR+DT: the MMU's logical maps
    step1(e_x(3, 0, 0, 83, 0)); // mfmsr from user mode
    CHECK_EQ(P->pc, 0x00000700u);
    CHECK(P->srr1 & PPC_SRR1_PROG_PRIV);
    CHECK(g_active_read == g_supervisor_read); // exception entry restores
    // FP unavailable
    fresh();
    P->msr &= ~PPC_MSR_FP;
    step1(e_d(50, 3, 4, 0)); // lfd
    CHECK_EQ(P->pc, 0x00000800u);
    // rfi round trip (into translated user state — the L14 shape)
    fresh();
    identity_segments();
    P->srr0 = 0x2000;
    P->srr1 = PPC_MSR_ME | PPC_MSR_FP | PPC_MSR_EE | PPC_MSR_PR | PPC_MSR_DT;
    memory_write_uint32(0x1000, (19u << 26) | (50u << 1)); // rfi
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x2000u);
    CHECK_EQ(P->msr, PPC_MSR_ME | PPC_MSR_FP | PPC_MSR_EE | PPC_MSR_PR | PPC_MSR_DT);
    CHECK(g_active_read == g_user_read); // PR+DT restored the user maps
    // rfi is privileged
    step1((19u << 26) | (50u << 1));
    CHECK_EQ(P->pc, 0x00000700u);
    CHECK(P->srr1 & PPC_SRR1_PROG_PRIV);
}

static void test_external_interrupt(void) {
    // Level-sensitive: taken when EE=1, deferred while EE=0
    fresh();
    ppc_set_ext_irq(P, true);
    memory_write_uint32(0x1000, e_d(14, 3, 0, 1)); // li r3,1
    run_at(0x1000, 1); // EE=0: instruction executes
    CHECK_EQ(P->gpr[3], 1u);
    CHECK_EQ(P->pc, 0x1004u);
    // EE=1: the exception redirects, then the budget executes the
    // handler's first instruction (same sprint model as the 68K).
    memory_write_uint32(0x00000500u, e_d(14, 3, 0, 7)); // handler: li r3,7
    P->msr |= PPC_MSR_EE;
    run_at(0x1004, 1);
    CHECK_EQ(P->pc, 0x00000504u);
    CHECK_EQ(P->gpr[3], 7u);
    CHECK_EQ(P->srr0, 0x1004u); // resumes at the untaken instruction
    CHECK(!(P->msr & PPC_MSR_EE));
    // the line stays asserted (level-sensitive): re-enabling EE retakes
    ppc_poll_interrupt(P); // EE=0 → nothing
    CHECK_EQ(P->pc, 0x00000504u);
    P->msr |= PPC_MSR_EE;
    ppc_poll_interrupt(P);
    CHECK_EQ(P->pc, 0x00000500u); // re-entered; SRR0 holds the old pc
    CHECK_EQ(P->srr0, 0x00000504u);
    ppc_set_ext_irq(P, false);
}

static void test_alignment(void) {
    // Translation off: only 256 MB boundary crossings fault
    fresh();
    P->gpr[4] = 0x0FFFFFFEu;
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0)); // lwz crossing 256MB
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x00000600u);
    CHECK_EQ(P->dar, 0x0FFFFFFEu);
    CHECK_EQ(P->srr0, 0x1000u);
    // unaligned page-crossing scalar is FINE with translation off
    fresh();
    memory_write_uint32(0x1FFC, 0x11111111u);
    memory_write_uint32(0x2000, 0x22222222u);
    P->gpr[4] = 0x1FFE;
    step1(e_d(32, 3, 4, 0));
    CHECK_EQ(P->gpr[3], 0x11112222u);
    // Translation on (MSR[DT]): page-crossing scalar faults...
    fresh();
    P->msr |= PPC_MSR_DT;
    identity_segments();
    P->gpr[4] = 0x1FFE;
    step1(e_d(32, 3, 4, 0));
    CHECK_EQ(P->pc, 0x00000600u);
    CHECK_EQ(P->dar, 0x1FFEu);
    // ...but a within-page unaligned scalar does not
    fresh();
    P->msr |= PPC_MSR_DT;
    identity_segments();
    P->gpr[4] = 0x2002;
    memory_write_uint32(0x2000, 0xAABBCCDDu);
    memory_write_uint32(0x2004, 0xEEFF0011u);
    step1(e_d(32, 3, 4, 0));
    CHECK_EQ(P->gpr[3], 0xCCDDEEFFu);
    // unaligned lmw crossing a page faults under DT
    fresh();
    P->msr |= PPC_MSR_DT;
    identity_segments();
    P->gpr[4] = 0x1FFE;
    step1(e_d(46, 30, 4, 0)); // lmw r30,0(r4): 8 bytes, unaligned, page-crossing
    CHECK_EQ(P->pc, 0x00000600u);
    // word-aligned lmw crossing a page is fine
    fresh();
    P->msr |= PPC_MSR_DT;
    identity_segments();
    P->gpr[4] = 0x1FFC;
    memory_write_uint32(0x1FFC, 0x11111111u);
    memory_write_uint32(0x2000, 0x22222222u);
    step1(e_d(46, 30, 4, 0));
    CHECK_EQ(P->gpr[30], 0x11111111u);
    CHECK_EQ(P->gpr[31], 0x22222222u);
    // lwarx: a misaligned EA is NOT an alignment exception by itself.
    // §5.4.6.1.1 faults a direct-translation access only on a 256 MB
    // crossing, and the chapter-3 lwarx page agrees ("the alignment
    // exception handler will be invoked if the word loaded crosses a page
    // boundary, or the results may be undefined").
    fresh();
    P->gpr[4] = 0x2002;
    P->gpr[5] = 0;
    memory_write_uint32(0x2002, 0x5A5A1234u);
    step1(e_x(3, 4, 5, 20, 0));
    CHECK_EQ(P->pc, 0x1004u);
    CHECK_EQ(P->gpr[3], 0x5A5A1234u);
    CHECK_EQ(P->reserve, 1u);
    // ...but crossing a 256 MB boundary with translation off does fault,
    // aligned or not, and faults before the access is attempted.
    fresh();
    P->gpr[4] = 0x0FFFFFFEu;
    P->gpr[5] = 0;
    step1(e_x(3, 4, 5, 20, 0));
    CHECK_EQ(P->pc, 0x00000600u);
}

// === powerpc-test conformance regressions ===
//
// Behaviors the third-party vector tier (tests/unit/suites/ppc_vectors/)
// caught, each restated against the 601UM passage that settles it so the
// rule stays pinned here whether or not that submodule is checked out.
static void test_conformance_regressions(void) {
    // §3.5.2: a load with update and rA = 0 performs the access but does
    // NOT update r0 -- the POWER-compatible reading of what the PowerPC
    // architecture calls an invalid form.
    fresh();
    P->gpr[0] = 0x2000;
    memory_write_uint32(0x2000, 0xAABBCCDDu);
    step1_valid(e_d(33, 5, 0, 0)); // lwzu r5,0(r0)
    CHECK_EQ(P->gpr[5], 0xAABBCCDDu);
    CHECK_EQ(P->gpr[0], 0x2000u);

    // §3.5.3 says the same for the stores, and §3.5.8 for the FP forms.
    fresh();
    P->gpr[0] = 0x2000;
    P->gpr[5] = 0x12345678u;
    step1_valid(e_d(37, 5, 0, 4)); // stwu r5,4(r0)
    CHECK_EQ(memory_read_uint32(0x2004), 0x12345678u);
    CHECK_EQ(P->gpr[0], 0x2000u);

    // A nonzero rA still updates, of course.
    fresh();
    P->gpr[4] = 0x2000;
    step1_valid(e_d(33, 5, 4, 8)); // lwzu r5,8(r4)
    CHECK_EQ(P->gpr[4], 0x2008u);

    // §3.5.10.1: the store-single conversion is a bit extraction
    // (WORD[2-31] <- frS[5-34]), not a rounding conversion.  Round-to-
    // nearest would make this $C2264889.
    fresh();
    P->fpr[3] = 0xC044C911174F7A54ull;
    P->gpr[4] = 0x2000;
    step1_valid(e_d(52, 3, 4, 0)); // stfs f3,0(r4)
    CHECK_EQ(memory_read_uint32(0x2000), 0xC2264888u);

    // Table 3-15: mffs fills frD[0-31] with $FFFFFFFF on the 601.
    fresh();
    P->fpscr = PPC_FPSCR_OX | PPC_FPSCR_XX;
    step1_valid(e_x63(3, 0, 0, 583, 0)); // mffs f3
    CHECK(P->fpr[3] == (0xFFFFFFFF00000000ull | (uint64_t)(PPC_FPSCR_OX | PPC_FPSCR_XX)));

    // fcmpu across signs: +1.0 is GREATER than -1.00390625.  (The bit-
    // pattern ordering trick this used to use inverted every mixed-sign
    // compare, and same-sign directed cases could not see it.)
    fresh();
    P->fpr[3] = 0x3FF0000000000000ull;
    P->fpr[4] = 0xBFF0100000000002ull;
    step1_valid(e_x63(7u << 2, 3, 4, 0, 0)); // fcmpu cr7,f3,f4
    CHECK_EQ(P->cr & 0xFu, 4u); // GT
    fresh();
    P->fpr[3] = 0x3FF0000000000000ull;
    P->fpr[4] = 0xBFF0100000000002ull;
    step1_valid(e_x63(7u << 2, 4, 3, 0, 0)); // fcmpu cr7,f4,f3
    CHECK_EQ(P->cr & 0xFu, 8u); // LT

    // Table 2-1 bit 0: driving an exception condition bit 0 -> 1 sets FX,
    // and mtfsb1 is not exempt.  VX derives on top.
    fresh();
    P->fpscr = 0;
    step1_valid(e_x63(11, 0, 0, 38, 0)); // mtfsb1 11 (VXIMZ)
    CHECK_EQ(P->fpscr, PPC_FPSCR_FX | PPC_FPSCR_VX | PPC_FPSCR_VXIMZ);
    // An enable bit is not an exception condition, so FX stays put.
    fresh();
    P->fpscr = 0;
    step1_valid(e_x63(25, 0, 0, 38, 0)); // mtfsb1 25 (OE)
    CHECK_EQ(P->fpscr, PPC_FPSCR_OE);

    // Table 3-31: bcctr with BO[2] = 0 is an invalid form the 601 still
    // executes -- CTR is decremented and tested, but the fetch address is
    // the NON-decremented CTR (here: CTR 1 -> 0, so the branch is not
    // taken even though the CR condition holds).
    fresh();
    P->ctr = 1;
    P->cr = 0x00100000u; // CR bit 11 set
    step1(e_bcctr(8, 11, 1)); // bcctrl 8,11
    CHECK_EQ(P->pc, 0x1004u);
    CHECK_EQ(P->ctr, 0u);
    CHECK_EQ(P->lr, 0x1004u);
    // The ordinary branch-always form is untouched by that.
    fresh();
    P->ctr = 0x3000;
    step1(e_bcctr(20, 0, 0));
    CHECK_EQ(P->pc, 0x3000u);

    // Table 5-22: sc loads SRR1[0-15] from instruction bits 16-31 (the
    // POWER svc field) -- §5.4.11's prose calls those bits undefined, but
    // the table is the specific rule.
    fresh();
    step1(0x44000002u); // sc 2
    CHECK_EQ(P->pc, 0x00000C00u);
    CHECK_EQ(P->srr0, 0x1004u);
    CHECK_EQ(P->srr1, 0x00020000u | PPC_MSR_ME | PPC_MSR_FP);
}

static void test_fp_surface(void) {
    // lfs single→double conversion
    fresh();
    memory_write_uint32(0x2000, 0x3F800000u); // 1.0f
    P->gpr[4] = 0x2000;
    step1_valid(e_d(48, 3, 4, 0)); // lfs f3
    CHECK(P->fpr[3] == 0x3FF0000000000000ull);
    // denormal single widens to a normal double
    memory_write_uint32(0x2000, 0x00000001u); // smallest denormal
    step1_valid(e_d(48, 3, 4, 0));
    CHECK(P->fpr[3] == 0x36A0000000000000ull); // 2^-149
    // NaN payload preserved both directions
    memory_write_uint32(0x2000, 0x7FC00001u);
    step1_valid(e_d(48, 3, 4, 0));
    CHECK(P->fpr[3] == 0x7FF8000020000000ull);
    step1_valid(e_d(52, 3, 4, 4)); // stfs f3 → +4
    CHECK_EQ(memory_read_uint32(0x2004), 0x7FC00001u);
    // -0 round trips
    memory_write_uint32(0x2000, 0x80000000u);
    step1_valid(e_d(48, 3, 4, 0));
    CHECK(P->fpr[3] == 0x8000000000000000ull);
    // lfd/stfd raw 64-bit
    fresh();
    memory_write_uint32(0x2000, 0x400921FBu);
    memory_write_uint32(0x2004, 0x54442D18u);
    P->gpr[4] = 0x2000;
    step1_valid(e_d(50, 5, 4, 0)); // lfd f5
    CHECK(P->fpr[5] == 0x400921FB54442D18ull);
    step1_valid(e_d(54, 5, 4, 8)); // stfd f5 → +8
    CHECK_EQ(memory_read_uint32(0x2008), 0x400921FBu);
    CHECK_EQ(memory_read_uint32(0x200C), 0x54442D18u);
    // moves are bit-exact
    fresh();
    P->fpr[4] = 0x7FF8000000000001ull; // NaN with payload
    memory_write_uint32(0x1000, (63u << 26) | (3u << 21) | (4u << 11) | (40u << 1)); // fneg f3,f4
    run_at(0x1000, 1);
    CHECK(P->fpr[3] == 0xFFF8000000000001ull);
    memory_write_uint32(0x1000, (63u << 26) | (3u << 21) | (3u << 11) | (264u << 1)); // fabs f3,f3
    run_at(0x1000, 1);
    CHECK(P->fpr[3] == 0x7FF8000000000001ull);
    // fcmpu: NaN → unordered; SNaN raises VXSNAN
    fresh();
    P->fpr[3] = 0x3FF0000000000000ull; // 1.0
    P->fpr[4] = 0x7FF0000000000001ull; // SNaN
    memory_write_uint32(0x1000, (63u << 26) | (0u << 23) | (3u << 16) | (4u << 11)); // fcmpu cr0
    run_at(0x1000, 1);
    CHECK_EQ(P->cr >> 28, 1u); // FU
    CHECK(P->fpscr & 0x01000000u); // VXSNAN
    // ordered compare
    fresh();
    P->fpr[3] = 0x3FF0000000000000ull; // 1.0
    P->fpr[4] = 0x4000000000000000ull; // 2.0
    memory_write_uint32(0x1000, (63u << 26) | (0u << 23) | (3u << 16) | (4u << 11));
    run_at(0x1000, 1);
    CHECK_EQ(P->cr >> 28, 8u); // LT
    // -0 == +0
    fresh();
    P->fpr[3] = 0x8000000000000000ull;
    P->fpr[4] = 0;
    memory_write_uint32(0x1000, (63u << 26) | (0u << 23) | (3u << 16) | (4u << 11));
    run_at(0x1000, 1);
    CHECK_EQ(P->cr >> 28, 2u); // EQ
    // FP arithmetic is live since Phase E (the deep coverage lives in
    // tests/unit/suites/ppc_fpu; this is the integration smoke check)
    fresh();
    P->fpr[3] = 0x3FF0000000000000ull; // 1.0
    P->fpr[4] = 0x4000000000000000ull; // 2.0
    memory_write_uint32(0x1000, (63u << 26) | (3u << 21) | (3u << 16) | (4u << 11) | (21u << 1)); // fadd f3,f3,f4
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x1004u);
    CHECK_EQ(P->fpr[3], 0x4008000000000000ull); // 3.0
    CHECK_EQ((P->fpscr >> 12) & 0x1Fu, 0x04u); // FPRF: +normal
}

// Phase-C translation subset: T=1 memory-forced segments (incl. the HWInit
// SR-toggle aliasing trick), the 601-format BATs, and the loud unimplemented
// T=0 path (proposal §3.5).
static void test_translation(void) {
    // T=1 memory-forced: SR low nibble selects the physical segment.  The
    // flash-probe pattern: sr[5] → segment 4 makes EA $50800000 read the
    // ROM at PA $40800000.
    fresh();
    identity_segments();
    P->msr |= PPC_MSR_DT;
    ppc_update_active_maps(P);
    P->sr[5] = 0x87F00004u;
    P->gpr[4] = 0x50800000u;
    step1(e_d(32, 3, 4, 0)); // lwz r3,0(r4)
    CHECK_EQ(P->gpr[3], memory_read_uint32(0x40800000u));
    // Identity T=1: EA=PA within the segment
    fresh();
    identity_segments();
    P->msr |= PPC_MSR_DT;
    memory_write_uint32(0x3000, 0xC0DEC0DEu);
    P->gpr[4] = 0x3000;
    step1(e_d(32, 3, 4, 0));
    CHECK_EQ(P->gpr[3], 0xC0DEC0DEu);
    // T=1 with a non-memory-forced BUID takes the 601 $00A00 exception
    fresh();
    identity_segments();
    P->msr |= PPC_MSR_DT;
    P->sr[0] = 0x80100000u; // T=1, BUID $001
    P->gpr[4] = 0x4000;
    step1(e_d(32, 3, 4, 0));
    CHECK_EQ(P->pc, 0x00000A00u);
    // T=0 is the Phase-D hashed walk: loud DSI with DSISR "not found"
    fresh();
    P->msr |= PPC_MSR_DT; // SRs all zero → T=0
    P->gpr[4] = 0x5000;
    step1(e_d(36, 3, 4, 0)); // stw
    CHECK_EQ(P->pc, 0x00000300u);
    CHECK_EQ(P->dar, 0x5000u);
    CHECK(P->dsisr & PPC_DSISR_NOTFOUND);
    CHECK(P->dsisr & PPC_DSISR_STORE);
    // 601-format BAT beats the segment: 128 KB block EA $00700000 → PA
    // $00300000 (BATL: PBN | V; BSM 0 = 128 KB)
    fresh();
    P->msr |= PPC_MSR_DT; // deliberately NO identity segments: BAT must hit
    P->batu[0] = 0x00700000u;
    P->batl[0] = 0x00300000u | 0x40u;
    memory_write_uint32(0x00300010u, 0xBA7BA7u);
    P->gpr[4] = 0x00700010u;
    step1(e_d(32, 3, 4, 0));
    CHECK_EQ(P->gpr[3], 0xBA7BA7u);
    // ...and the block-size mask widens the match (BSM $7 = 1 MB)
    fresh();
    P->msr |= PPC_MSR_DT;
    P->batu[0] = 0x00400000u;
    P->batl[0] = 0x00000000u | 0x40u | 0x7u; // 1 MB block → PA base 0
    memory_write_uint32(0x000C0000u, 0x1234ABCDu);
    P->gpr[4] = 0x004C0000u;
    step1(e_d(32, 3, 4, 0));
    CHECK_EQ(P->gpr[3], 0x1234ABCDu);
    // Instruction translation: IBAT identity block covers the fetch; a
    // fetch outside every BAT raises ISI (translation-not-found SRR1 bit)
    fresh();
    P->batu[0] = 0x00000000u;
    P->batl[0] = 0x00000000u | 0x40u | 0x7u; // 1 MB identity at 0
    P->msr |= PPC_MSR_IT;
    ppc_update_active_maps(P);
    P->gpr[4] = 7;
    P->gpr[5] = 8;
    memory_write_uint32(0x1000, e_xo(3, 4, 5, 0, 266, 0)); // add r3,r4,r5
    run_at(0x1000, 1);
    CHECK_EQ(P->gpr[3], 15u);
    fresh();
    P->batu[0] = 0x00000000u;
    P->batl[0] = 0x00000000u | 0x40u; // 128 KB block: $120000 is outside
    P->msr |= PPC_MSR_IT;
    ppc_update_active_maps(P);
    // The redirect consumes the budget on the handler's first instruction
    // (68K-identical), so give the vector a real one to execute.
    memory_write_uint32(0x400, 0x60000000u); // nop at the ISI vector
    run_at(0x120000, 1);
    CHECK_EQ(P->pc & 0xFFFFFu, 0x00404u); // nop at the ISI vector ran
    CHECK_EQ(P->srr0, 0x120000u);
    CHECK(P->srr1 & 0x40000000u);
    CHECK(!(P->msr & PPC_MSR_IT)); // exception entry cleared IT
}

static void test_mq_spr(void) {
    fresh();
    P->gpr[4] = 0x13579BDFu;
    step1_valid(e_spr(4, 0, 1)); // mtspr mq
    CHECK_EQ(P->mq, 0x13579BDFu);
    step1_valid(e_spr(3, 0, 0)); // mfspr r3,mq
    CHECK_EQ(P->gpr[3], 0x13579BDFu);
}

int main(void) {
    // 32-bit address space: 8 MB RAM at 0, 128 KB ROM at $40800000 (the
    // canonical OS base) — the harness's own init is Plus-shaped (24-bit),
    // so the context is built by hand here.
    test_context_t *ctx = calloc(1, sizeof(test_context_t));
    ctx->memory = memory_map_init(32, 0x800000, 0x20000, NULL);
    if (!ctx->memory) {
        printf("FAIL: memory_map_init\n");
        return 1;
    }
    memory_populate_pages(ctx->memory, 0x40800000u, 0x40820000u);
    test_set_active_context(ctx);

    P = ppc_init(NULL);
    if (!P) {
        printf("FAIL: ppc_init\n");
        return 1;
    }

    test_reset_state();
    test_add_carry_overflow();
    test_subf_carry();
    test_record_and_compare();
    test_shifts();
    test_rotates();
    test_mul_div();
    test_power_arith();
    test_power_masks_shifts();
    test_branches();
    test_cr_ops();
    test_loads_stores();
    test_multiple_and_strings();
    test_atomics_and_dcbz();
    test_sprs();
    test_exceptions();
    test_external_interrupt();
    test_alignment();
    test_translation();
    test_fp_surface();
    test_mq_spr();
    test_conformance_regressions();

    ppc_delete(P);
    printf("ppc: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
