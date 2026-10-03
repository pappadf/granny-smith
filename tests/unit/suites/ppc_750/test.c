// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// test.c — directed unit tests for the PPC core's MPC750 model
// (src/core/cpu/ppc/, cpu_model == CPU_MODEL_PPC750).
//
// The 750 is the 604's programming model plus a handful of implementation
// registers (MPC750UM Tables 2-48/2-49).  suites/ppc_vectors replays the
// shared instruction set under the 750; this suite pins what differs:
//   - reset state (Table 2-19): MSR = $40, PVR = $00080202, HID0 = L2CR = 0,
//     DEC = all-ones;
//   - L2CR (SPR 1017): stored bits, L2IP read-only, and the global
//     invalidate handshake the beige-G3 boot program spins on (§9.1.5);
//   - HID1 (SPR 1009): read-only PLL_CFG image supplied by the profile;
//   - HID0: ICFI/DCFI self-clear (Table 2-4);
//   - THRM1-3 (1020-1022): TIV valid after a V=1 threshold with THRM3[E],
//     TIN per TID against the synthetic temperature (Table 2-15);
//   - ICTC (1019), MMCR0/1, PMC1-4, SIA store-and-readback; user mirrors
//     936-942 readable from user mode and not writable;
//   - mtdec signals the decrementer request on a 0 -> 1 change of bit 0;
//   - PIR (1023) and SDA (959) do not exist on the 750; tlbia and fsqrt
//     take the illegal-instruction program exception (§2.3.1.2, §2.3.6.3.3).

#include "ppc_internal.h"

#include "harness.h"
#include "memory.h"

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

// mfspr/mtspr encoding (the SPR number's halves swapped, PEM mfspr page)
static uint32_t e_spr(uint32_t rt, uint32_t spr, int to_spr) {
    uint32_t f = ((spr & 0x1Fu) << 16) | ((spr >> 5) << 11);
    return (31u << 26) | (rt << 21) | f | ((to_spr ? 467u : 339u) << 1);
}

// Execute one word at $1000.
static void step1(uint32_t iw) {
    memory_write_uint32(0x1000, iw);
    P->pc = 0x1000;
    uint32_t budget = 1;
    ppc_run(P, &budget);
}

// Reset into the 750 with EP cleared (vectors at $000xxxxx), FP on.
static void fresh750(void) {
    ppc_reset(P);
    P->msr = PPC_MSR_ME | PPC_MSR_FP;
    ppc_update_active_maps(P);
}

// Execute one word and expect the illegal-instruction program exception.
static void expect_illegal(uint32_t iw) {
    fresh750();
    step1(iw);
    CHECK_EQ(P->pc, 0x00000700u);
    CHECK_EQ(P->srr0, 0x1000u);
    CHECK(P->srr1 & PPC_SRR1_PROG_ILLEGAL);
}

// Execute one word from user mode and expect the privileged-instruction
// program exception.
static void expect_priv(uint32_t iw) {
    fresh750();
    P->msr |= PPC_MSR_PR;
    ppc_update_active_maps(P);
    step1(iw);
    CHECK_EQ(P->pc, 0x00000700u);
    CHECK(P->srr1 & PPC_SRR1_PROG_PRIV);
}

static void test_reset_state(void) {
    ppc_reset(P);
    CHECK_EQ(P->msr, 0x00000040u); // IP only (750UM Table 2-19)
    CHECK_EQ(P->pc, 0xFFF00100u);
    CHECK_EQ(P->pvr, 0x00080202u); // stock beige-G3 750 rev 2.2
    CHECK_EQ(P->hid0, 0);
    CHECK_EQ(P->l2cr, 0);
    CHECK_EQ(P->dec, 0xFFFFFFFFu);
    CHECK(ppc_is_604(P)); // the 604's programming model
    CHECK(ppc_is_750(P));
}

// The profile identity: PVR and HID1 survive a hard reset; mtspr HID1 and
// mtspr PVR are no-ops (750UM §2.3.2.4.3).
static void test_identity_and_hid1(void) {
    ppc_set_identity(P, 0x00080201u, 0xA0000000u);
    fresh750();
    CHECK_EQ(P->pvr, 0x00080201u);
    P->gpr[4] = 0x55555555u;
    step1(e_spr(4, 1009, 1)); // mtspr hid1,r4 — no-op
    CHECK_EQ(P->pc, 0x1004u);
    step1(e_spr(3, 1009, 0)); // mfspr r3,hid1
    CHECK_EQ(P->gpr[3], 0xA0000000u);
    step1(e_spr(3, 287, 0)); // mfspr r3,pvr
    CHECK_EQ(P->gpr[3], 0x00080201u);
    ppc_set_identity(P, 0, 0xA0000000u); // back to the default part
    fresh750();
    CHECK_EQ(P->pvr, 0x00080202u);
    // HID1 is supervisor-only
    expect_priv(e_spr(3, 1009, 0));
}

// HID0: store-and-readback with ICFI/DCFI self-clearing.
static void test_hid0(void) {
    fresh750();
    P->gpr[4] = 0x0000CC00u; // ICE|DCE|ICFI|DCFI — Darwin's invl1 sequence
    step1(e_spr(4, 1008, 1));
    step1(e_spr(3, 1008, 0));
    CHECK_EQ(P->gpr[3], 0x0000C000u);
    P->gpr[4] = 0x0010C0A4u; // DPM|ICE|DCE|SGE|BTIC|BHT
    step1(e_spr(4, 1008, 1));
    CHECK_EQ(P->hid0, 0x0010C0A4u);
}

// L2CR: the enable/invalidate handshake (750UM §9.1.5), unbound path —
// L2IP reads 1 once after L2I is set, then 0.
static void test_l2cr_handshake(void) {
    fresh750();
    // Configure (L2SIZ=10, L2CLK=100, L2RAM=10), L2E clear.
    P->gpr[4] = 0x29000000u;
    step1(e_spr(4, 1017, 1));
    step1(e_spr(3, 1017, 0));
    CHECK_EQ(P->gpr[3], 0x29000000u);
    // Set L2I: the invalidate starts; L2IP reads 1, then 0.
    P->gpr[4] = 0x29200000u;
    step1(e_spr(4, 1017, 1));
    step1(e_spr(3, 1017, 0));
    CHECK_EQ(P->gpr[3], 0x29200001u);
    step1(e_spr(3, 1017, 0));
    CHECK_EQ(P->gpr[3], 0x29200000u);
    // Clear L2I, set L2E: the plausible enabled value for a 512 KB module.
    P->gpr[4] = 0xA9000000u;
    step1(e_spr(4, 1017, 1));
    step1(e_spr(3, 1017, 0));
    CHECK_EQ(P->gpr[3], 0xA9000000u);
    // L2IP is read-only: writing bit 31 does not stick.
    P->gpr[4] = 0xA9000001u;
    step1(e_spr(4, 1017, 1));
    step1(e_spr(3, 1017, 0));
    CHECK_EQ(P->gpr[3], 0xA9000000u);
    // L2I with L2E set still starts an invalidate (the ROM's sequence).
    P->gpr[4] = 0xA9200000u;
    step1(e_spr(4, 1017, 1));
    step1(e_spr(3, 1017, 0));
    CHECK_EQ(P->gpr[3], 0xA9200001u);
    step1(e_spr(3, 1017, 0));
    CHECK_EQ(P->gpr[3], 0xA9200000u);
    // Supervisor-only
    expect_priv(e_spr(3, 1017, 0));
}

// THRM1-3: TIV/TIN derived against the synthetic 40 °C junction.
static void test_thrm(void) {
    fresh750();
    // THRM1: threshold 30 °C, TID = 0 (above), V = 1; THRM3 disabled.
    P->gpr[4] = (30u << 23) | 1u;
    step1(e_spr(4, 1020, 1));
    step1(e_spr(3, 1020, 0));
    CHECK_EQ(P->gpr[3], (30u << 23) | 1u); // TIV clear while THRM3[E] = 0
    P->gpr[4] = 1u; // THRM3[E]
    step1(e_spr(4, 1022, 1));
    step1(e_spr(3, 1020, 0));
    CHECK_EQ(P->gpr[3], 0xC0000000u | (30u << 23) | 1u); // TIV, TIN (40 > 30)
    // THRM2: threshold 50 °C, above → TIV, TIN clear
    P->gpr[4] = (50u << 23) | 1u;
    step1(e_spr(4, 1021, 1));
    step1(e_spr(3, 1021, 0));
    CHECK_EQ(P->gpr[3], 0x40000000u | (50u << 23) | 1u);
    // TID = 1 (below 50) → TIN set
    P->gpr[4] = (50u << 23) | 4u | 1u;
    step1(e_spr(4, 1021, 1));
    step1(e_spr(3, 1021, 0));
    CHECK_EQ(P->gpr[3], 0xC0000000u | (50u << 23) | 5u);
    // TIN/TIV are never stored from a write
    P->gpr[4] = 0xC0000000u;
    step1(e_spr(4, 1020, 1));
    CHECK_EQ(P->thrm[0], 0);
    // ICTC: store-and-readback
    P->gpr[4] = 0x000001FFu;
    step1(e_spr(4, 1019, 1));
    step1(e_spr(3, 1019, 0));
    CHECK_EQ(P->gpr[3], 0x000001FFu);
}

// The 750 performance monitor map: supervisor MMCR0/1, PMC1-4, SIA
// store-and-readback; user mirrors read-only; no SDA.
static void test_perfmon(void) {
    static const uint32_t sup[] = {952, 953, 954, 955, 956, 957, 958};
    static const uint32_t usr[] = {936, 937, 938, 939, 940, 941, 942};
    fresh750();
    for (unsigned i = 0; i < 7; i++) {
        P->gpr[4] = 0x1000u + i;
        step1(e_spr(4, sup[i], 1));
    }
    for (unsigned i = 0; i < 7; i++) {
        step1(e_spr(3, sup[i], 0));
        CHECK_EQ(P->gpr[3], 0x1000u + i);
    }
    // User mirrors, read from user mode
    P->msr |= PPC_MSR_PR;
    ppc_update_active_maps(P);
    for (unsigned i = 0; i < 7; i++) {
        step1(e_spr(3, usr[i], 0));
        CHECK_EQ(P->pc, 0x1004u);
        CHECK_EQ(P->gpr[3], 0x1000u + i);
    }
    // The mirrors are mfspr-only: mtspr is an undefined SPR (illegal)
    expect_illegal(e_spr(3, 936, 1));
    // SDA (959) and PIR (1023) are 604 registers the 750 does not have
    expect_illegal(e_spr(3, 959, 0));
    expect_illegal(e_spr(3, 1023, 0));
    expect_illegal(e_spr(3, 1023, 1));
}

// Instruction-set deltas: tlbia and fsqrt are illegal; mftb and fsel work.
static void test_isa(void) {
    expect_illegal((31u << 26) | (370u << 1)); // tlbia
    expect_illegal((63u << 26) | (1u << 21) | (2u << 11) | (22u << 1)); // fsqrt f1,f2
    fresh750();
    P->rtcl = 0x12345678u;
    step1(e_spr(3, 268, 0) + (32u << 1)); // mftb r3
    CHECK_EQ(P->gpr[3], 0x12345678u);
    fresh750();
    step1((31u << 26) | (566u << 1)); // tlbsync: a no-op on the 750
    CHECK_EQ(P->pc, 0x1004u);
}

// The 601/604 models are untouched by the 750 registers.
// mtdec: a write that turns DEC bit 0 from 0 to 1 signals the decrementer
// request; any other write clears a latched one.  The NanoKernel re-posts an
// expired DEC with exactly this pair (mtdec $7FFF0000, mtdec <old value>).
static void test_dec_write_signal(void) {
    fresh750();
    P->gpr[4] = 0x7FFF0000u;
    step1(e_spr(4, 22, 1)); // mtdec r4 (positive)
    CHECK_EQ(P->dec_pending, 0);
    P->gpr[4] = 0xF5C35490u;
    step1(e_spr(4, 22, 1)); // mtdec r4: bit 0 goes 0 -> 1
    CHECK_EQ(P->dec_pending, 1);
    P->gpr[4] = 0xE0000000u;
    step1(e_spr(4, 22, 1)); // negative -> negative: no transition, cleared
    CHECK_EQ(P->dec_pending, 0);
    P->dec_pending = 1;
    P->gpr[4] = 0x00001000u;
    step1(e_spr(4, 22, 1)); // a positive write cancels the latched request
    CHECK_EQ(P->dec_pending, 0);
}

static void test_other_models_reject(void) {
    P->cpu_model = CPU_MODEL_PPC604;
    ppc_reset(P);
    P->msr = PPC_MSR_ME | PPC_MSR_FP;
    ppc_update_active_maps(P);
    step1(e_spr(3, 1017, 0)); // L2CR on a 604
    CHECK_EQ(P->pc, 0x00000700u);
    CHECK(P->srr1 & PPC_SRR1_PROG_ILLEGAL);
    P->msr = PPC_MSR_ME | PPC_MSR_FP;
    P->gpr[4] = 0x77u;
    step1(e_spr(4, 1023, 1)); // PIR survives on the 604
    CHECK_EQ(P->pir, 0x77u);
    P->cpu_model = CPU_MODEL_PPC750;
}

int main(void) {
    // 32-bit map with RAM at 0 (the vectors land at $00000xxx).
    test_context_t *ctx = calloc(1, sizeof(test_context_t));
    ctx->memory = memory_map_init(32, 0x800000, 0x20000, MEMORY_BUS_ERR_NONE, NULL);
    if (!ctx->memory) {
        printf("FAIL: memory_map_init\n");
        return 1;
    }
    memory_populate_pages(ctx->memory, 0x40800000u, 0x40820000u);
    test_set_active_context(ctx);

    P = ppc_init(NULL, CPU_MODEL_PPC750);
    if (!P) {
        printf("FAIL: ppc_init\n");
        return 1;
    }

    test_reset_state();
    test_identity_and_hid1();
    test_hid0();
    test_l2cr_handshake();
    test_thrm();
    test_perfmon();
    test_isa();
    test_dec_write_signal();
    test_other_models_reject();

    ppc_delete(P);
    printf("ppc_750: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
