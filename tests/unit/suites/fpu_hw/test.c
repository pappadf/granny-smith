// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// FPU edge cases pinned to the hardware's documented results.
//
// Each case executes a real 68030 F-line general operation through
// fpu_general_op() (register-to-register, or a store to memory) and checks
// the result register and FPSR against the MC68881/MC68882 User's Manual
// (2nd ed., 1989) and, for the 68040, the M68040 FPSP:
//
//  - unnormal operands are normalized before use, an unnormal zero being a
//    zero (UM §3.5.1): 0/0 is an operand error, sqrt(-0) is -0, FTST sees
//    a zero, FSCALE by an unnormal 1.0 scales by one, and a store converts
//    the value, never writing an unnormal (UM §3.5.2);
//  - FGETMAN of a denormal returns its normalized mantissa (FPSP sgetmand);
//  - FSCALE by an infinity is an operand error for any non-NaN destination,
//    and |scale| >= 2^14 always over/underflows (UM FSCALE; FPSP sscale);
//  - a single/double store of an exactly representable denormal sets UNFL
//    but not INEX2 (UM §6.1.5/§6.1.7, FPSP round);
//  - FCOS of a denormal is exactly 1.0 in every rounding mode (FPSP scosd)
//    and FATAN of a denormal is X with UNFL and INEX2 (FPSP satand);
//  - the packed-decimal ILOG is exact next to the large powers of ten;
//  - FDIV is correctly rounded (UM §4.3.1), its sticky bit taking the whole
//    final remainder, and so is fpu_op_div for 128-bit internal divisors.

#include "cpu.h"
#include "cpu_internal.h"
#include "fpu.h"
#include "fpu_internal.h"
#include "harness.h"
#include "memory.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DATA_ADDR 0x002000u

// FPCR rounding modes (bits 5:4) and precisions (bits 7:6)
#define RN  0x00u
#define RZ  0x10u
#define RM  0x20u
#define RP  0x30u
#define SGL 0x40u

#define OP_FTST    0x3Au
#define OP_FSQRT   0x04u
#define OP_FATAN   0x0Au
#define OP_FCOS    0x1Du
#define OP_FGETMAN 0x1Fu
#define OP_FDIV    0x20u
#define OP_FSCALE  0x26u

static cpu_t *g_cpu;
static fpu_state_t *g_fpu;

static float80_reg_t x80(uint16_t se, uint64_t mant) {
    float80_reg_t r = {se, mant};
    return r;
}

// Run `op` with FP1 = src and FP2 = dst; the result lands in FP2.
static float80_reg_t run_op(unsigned op, unsigned fpcr, float80_reg_t src, float80_reg_t dst) {
    g_fpu->fp[1] = src;
    g_fpu->fp[2] = dst;
    g_fpu->fpcr = fpcr;
    g_fpu->fpsr = 0;
    g_cpu->instruction_pc = 0x1000;
    g_cpu->pc = 0x1004;
    fpu_general_op(g_cpu, g_fpu, 0xF200, (uint16_t)((1u << 10) | (2u << 7) | op));
    return g_fpu->fp[2];
}

// FMOVE.<fmt> FP1,(A0); returns the first longword written
static uint32_t store_fp1(unsigned fmt, unsigned fpcr, float80_reg_t v) {
    g_fpu->fp[1] = v;
    g_fpu->fpcr = fpcr;
    g_fpu->fpsr = 0;
    g_cpu->a[0] = DATA_ADDR;
    g_cpu->instruction_pc = 0x1000;
    g_cpu->pc = 0x1004;
    fpu_general_op(g_cpu, g_fpu, 0xF210, (uint16_t)(0x6000u | (fmt << 10) | (1u << 7)));
    return memory_read_uint32(DATA_ADDR);
}

static uint32_t exc(void) {
    return g_fpu->fpsr & 0xFF00u;
}

static void assert_reg(float80_reg_t r, uint16_t se, uint64_t mant) {
    if (r.exponent != se || r.mantissa != mant) {
        fprintf(stderr, "got %04X:%016llX want %04X:%016llX\n", r.exponent, (unsigned long long)r.mantissa, se,
                (unsigned long long)mant);
        ASSERT_TRUE(0);
    }
}

static const float80_reg_t UNNORM_ZERO = {0x3FFF, 0}; // exponent of 1.0, mantissa 0
static const float80_reg_t NEG_UNNORM_ZERO = {0xBFFF, 0};
static const float80_reg_t UNNORM_ONE = {0x403E, 1}; // 1 * 2^(63-63)
static const float80_reg_t ONE = {0x3FFF, 0x8000000000000000ULL};
static const float80_reg_t PI = {0x4000, 0xC90FDAA22168C235ULL};

// ---------------------------------------------------------------------------
// Unnormal operands
// ---------------------------------------------------------------------------

TEST(fdiv_unnormal_zero_by_unnormal_zero_is_operr) {
    float80_reg_t r = run_op(OP_FDIV, RN, UNNORM_ZERO, UNNORM_ZERO);
    ASSERT_TRUE(fp80_is_nan(r));
    ASSERT_TRUE(exc() & FPEXC_OPERR);
    ASSERT_TRUE(!(exc() & FPEXC_DZ));
    // A true zero dividend over an unnormal-zero divisor, too
    r = run_op(OP_FDIV, RN, UNNORM_ZERO, FP80_ZERO);
    ASSERT_TRUE(fp80_is_nan(r));
    ASSERT_TRUE(exc() & FPEXC_OPERR);
}

TEST(fdiv_by_unnormal_zero_is_divide_by_zero) {
    float80_reg_t r = run_op(OP_FDIV, RN, UNNORM_ZERO, ONE);
    assert_reg(r, 0x7FFF, 0);
    ASSERT_EQ_INT(FPEXC_DZ, exc());
}

TEST(fdiv_unpacked_unnormal_zero_is_a_zero) {
    // Direct callers can hand fpu_op_div an unpacked unnormal zero
    fpu_unpacked_t z = {false, 0, 0, 0};
    g_fpu->fpsr = 0;
    fpu_unpacked_t r = fpu_op_div(g_fpu, z, z);
    ASSERT_TRUE(r.exponent == FPU_EXP_INF && r.mantissa_hi != 0);
    ASSERT_TRUE(g_fpu->fpsr & FPEXC_OPERR);
}

TEST(fsqrt_negative_unnormal_zero_is_negative_zero) {
    float80_reg_t r = run_op(OP_FSQRT, RN, NEG_UNNORM_ZERO, ONE);
    assert_reg(r, 0x8000, 0);
    ASSERT_EQ_INT(0, exc());
    // The unpacked form, directly
    fpu_unpacked_t z = {true, 5, 0, 0};
    g_fpu->fpsr = 0;
    fpu_unpacked_t u = fpu_op_sqrt(g_fpu, z);
    ASSERT_TRUE(u.exponent == FPU_EXP_ZERO && u.sign);
    ASSERT_EQ_INT(0, g_fpu->fpsr);
}

TEST(ftst_unnormal_zero_sets_z) {
    run_op(OP_FTST, RN, UNNORM_ZERO, ONE);
    ASSERT_TRUE(g_fpu->fpsr & FPCC_Z);
}

TEST(fscale_by_unnormal_one_doubles) {
    float80_reg_t r = run_op(OP_FSCALE, RN, UNNORM_ONE, PI);
    assert_reg(r, 0x4001, 0xC90FDAA22168C235ULL);
    ASSERT_EQ_INT(0, exc());
}

TEST(fgetman_of_denormal_is_normalized) {
    float80_reg_t r = run_op(OP_FGETMAN, RN, x80(0x0000, 0x0000123456789ABCULL), ONE);
    ASSERT_EQ_INT(0x3FFF, r.exponent);
    ASSERT_TRUE(r.mantissa & 0x8000000000000000ULL);
}

TEST(stores_convert_unnormals_by_value) {
    // FMOVE.L of an unnormal 1.0 is 1, not an integer overflow
    uint32_t w = store_fp1(0, RN, UNNORM_ONE);
    ASSERT_EQ_INT(1, (int)w);
    ASSERT_EQ_INT(0, exc());
    // FMOVE.X never writes an unnormal: 0.5 normalized, an unnormal zero a zero
    w = store_fp1(2, RN, x80(0x3FFF, 0x4000000000000000ULL));
    ASSERT_EQ_INT((int)0x3FFE0000u, (int)w);
    ASSERT_EQ_INT((int)0x80000000u, (int)memory_read_uint32(DATA_ADDR + 4));
    w = store_fp1(2, RN, NEG_UNNORM_ZERO);
    ASSERT_EQ_INT((int)0x80000000u, (int)w);
    ASSERT_EQ_INT(0, (int)memory_read_uint32(DATA_ADDR + 4));
    ASSERT_EQ_INT(0, exc());
}

// ---------------------------------------------------------------------------
// FSCALE operation table
// ---------------------------------------------------------------------------

TEST(fscale_by_infinity_is_operr) {
    const float80_reg_t dsts[] = {ONE, FP80_ZERO, FP80_INF, x80(0x0000, 1)};
    for (unsigned i = 0; i < sizeof(dsts) / sizeof(dsts[0]); i++) {
        float80_reg_t r = run_op(OP_FSCALE, RN, FP80_NEG_INF, dsts[i]);
        ASSERT_TRUE(fp80_is_nan(r));
        ASSERT_TRUE(exc() & FPEXC_OPERR);
        r = run_op(OP_FSCALE, RN, FP80_INF, dsts[i]);
        ASSERT_TRUE(fp80_is_nan(r));
        ASSERT_TRUE(exc() & FPEXC_OPERR);
    }
}

TEST(fscale_by_2_14_always_overflows) {
    // 2^-16400 * 2^16384 = 2^-16 would fit, but |src| >= 2^14 overflows
    float80_reg_t tiny = x80(0x0000, 0x0000800000000000ULL); // denormal
    float80_reg_t r = run_op(OP_FSCALE, RN, x80(0x400D, 0x8000000000000000ULL), tiny);
    assert_reg(r, 0x7FFF, 0);
    ASSERT_TRUE(exc() & FPEXC_OVFL);
    // ... and negative underflows even from the top of the range
    r = run_op(OP_FSCALE, RN, x80(0xC00D, 0x8000000000000000ULL), x80(0x7FFE, 0x8000000000000000ULL));
    ASSERT_TRUE(fp80_is_zero(r));
    ASSERT_TRUE(exc() & FPEXC_UNFL);
}

// ---------------------------------------------------------------------------
// Denormal stores
// ---------------------------------------------------------------------------

TEST(single_store_exact_denormal_is_unfl_only) {
    // 1.5 * 2^-127 = single denormal 0x00600000 exactly
    uint32_t w = store_fp1(1, RN, x80(0x3F80, 0xC000000000000000ULL));
    ASSERT_EQ_INT(0x00600000, (int)w);
    ASSERT_EQ_INT(FPEXC_UNFL, exc());
    ASSERT_TRUE(!(g_fpu->fpsr & FPACC_UNFL)); // accrued UNFL needs INEX2 too
}

TEST(single_store_inexact_denormal_is_unfl_and_inex2) {
    uint32_t w = store_fp1(1, RZ, x80(0x3F80, 0xC000000000000001ULL));
    ASSERT_EQ_INT(0x00600000, (int)w);
    ASSERT_EQ_INT(FPEXC_UNFL | FPEXC_INEX2, exc());
}

TEST(double_store_exact_denormal_is_unfl_only) {
    // 2^-1023 = double denormal 0x0008000000000000
    uint32_t w = store_fp1(5, RN, x80(0x3C00, 0x8000000000000000ULL));
    ASSERT_EQ_INT(0x00080000, (int)w);
    ASSERT_EQ_INT(0, (int)memory_read_uint32(DATA_ADDR + 4));
    ASSERT_EQ_INT(FPEXC_UNFL, exc());
}

// ---------------------------------------------------------------------------
// Transcendentals of denormals
// ---------------------------------------------------------------------------

TEST(fcos_of_denormal_is_exactly_one) {
    const unsigned modes[] = {RN, RZ, RM};
    for (unsigned i = 0; i < 3; i++) {
        float80_reg_t r = run_op(OP_FCOS, modes[i], x80(0x0000, 0x4000000000000000ULL), FP80_ZERO);
        assert_reg(r, 0x3FFF, 0x8000000000000000ULL);
        ASSERT_EQ_INT(FPEXC_INEX2, exc());
    }
    // A normalized tiny argument rounds 1 - 2^-126 toward zero instead
    float80_reg_t r = run_op(OP_FCOS, RZ, x80(0x3F00, 0x8000000000000000ULL), FP80_ZERO);
    assert_reg(r, 0x3FFE, 0xFFFFFFFFFFFFFFFFULL);
}

TEST(fsincos_of_denormal_gives_x_and_one) {
    float80_reg_t x = x80(0x0000, 0x4000000000000000ULL);
    float80_reg_t s = run_op(0x33, RZ, x, FP80_ZERO); // FSINCOS FP1,FP3:FP2
    assert_reg(s, 0x0000, 0x4000000000000000ULL);
    assert_reg(g_fpu->fp[3], 0x3FFF, 0x8000000000000000ULL);
    ASSERT_TRUE(exc() & FPEXC_UNFL);
    ASSERT_TRUE(exc() & FPEXC_INEX2);
}

TEST(fatan_of_denormal_is_x) {
    float80_reg_t x = x80(0x8000, 0x0000123456789ABCULL);
    float80_reg_t r = run_op(OP_FATAN, RN, x, FP80_ZERO);
    assert_reg(r, 0x8000, 0x0000123456789ABCULL);
    ASSERT_EQ_INT(FPEXC_UNFL | FPEXC_INEX2, exc());
}

// ---------------------------------------------------------------------------
// Packed decimal ILOG
// ---------------------------------------------------------------------------

static int32_t flog(int32_t e, uint64_t m) {
    fpu_unpacked_t x = {false, e, m, 0};
    return fpu_floor_log10(g_fpu, x);
}

TEST(floor_log10_exact_next_to_powers_of_ten) {
    // 10^27 is exact in 64 bits; its neighbours either side
    ASSERT_EQ_INT(27, flog(89, 0xCECB8F27F4200F3AULL));
    ASSERT_EQ_INT(26, flog(89, 0xCECB8F27F4200F39ULL));
    // 10^28: 64-bit floor and ceiling of the 65-bit value
    ASSERT_EQ_INT(27, flog(93, 0x813F3978F8940984ULL));
    ASSERT_EQ_INT(28, flog(93, 0x813F3978F8940985ULL));
    // 10^4096: the 68882 ROM's 64-bit value is just below it
    ASSERT_EQ_INT(4095, flog(13606, 0xC46052028A20979AULL));
    ASSERT_EQ_INT(4096, flog(13606, 0xC46052028A20979BULL));
    // 10^-4950, at the bottom of the denormal range: ceiling and floor
    ASSERT_EQ_INT(-4950, flog(-16444, 0xAF92C8FC34030AD9ULL));
    ASSERT_EQ_INT(-4951, flog(-16444, 0xAF92C8FC34030AD8ULL));
    ASSERT_EQ_INT(0, flog(0, 0x8000000000000000ULL));
    ASSERT_EQ_INT(-1, flog(-1, 0xFFFFFFFFFFFFFFFFULL));
}

// ---------------------------------------------------------------------------
// Division
// ---------------------------------------------------------------------------

TEST(fdiv_final_remainder_bit_is_sticky) {
    // a / b with a < b whose last partial remainder is exactly 2^63: the
    // bit shifted out of the 64-bit remainder register is still a nonzero
    // remainder, so the quotient is inexact and rounds up toward +infinity
    float80_reg_t a = x80(0x3FFF, 0x87530C327018A704ULL);
    float80_reg_t b = x80(0x3FFF, 0x90B1B1B48B529B4BULL);
    float80_reg_t r = run_op(OP_FDIV, RN, b, a);
    assert_reg(r, 0x3FFE, 0xEF6C321E71CF699DULL);
    ASSERT_EQ_INT(FPEXC_INEX2, exc());
    r = run_op(OP_FDIV, RP, b, a);
    assert_reg(r, 0x3FFE, 0xEF6C321E71CF699EULL);
    ASSERT_EQ_INT(FPEXC_INEX2, exc());
}

TEST(fdiv_wide_divisor_is_correctly_rounded) {
    // Every divisor bit counts: the low 64 bits move the quotient by an ulp
    fpu_unpacked_t a = {true, -37, 0xFF33706A2787ABD4ULL, 0xC9D6178590AD5215ULL};
    fpu_unpacked_t b = {false, -51, 0xFFFFFFFFFFFFFFFEULL, 0x5D9266F9916B3594ULL};
    g_fpu->fpcr = RN;
    g_fpu->fpsr = 0;
    float80_reg_t r = fpu_pack(g_fpu, fpu_op_div(g_fpu, a, b));
    assert_reg(r, 0xC00C, 0xFF33706A2787ABD6ULL);
    ASSERT_EQ_INT(FPEXC_INEX2, exc());
}

int main(void) {
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }
    g_cpu = ctx->cpu;
    g_cpu->cpu_model = CPU_MODEL_68030;
    if (!g_cpu->fpu)
        g_cpu->fpu = fpu_init();
    g_fpu = (fpu_state_t *)g_cpu->fpu;

    RUN(fdiv_unnormal_zero_by_unnormal_zero_is_operr);
    RUN(fdiv_by_unnormal_zero_is_divide_by_zero);
    RUN(fdiv_unpacked_unnormal_zero_is_a_zero);
    RUN(fsqrt_negative_unnormal_zero_is_negative_zero);
    RUN(ftst_unnormal_zero_sets_z);
    RUN(fscale_by_unnormal_one_doubles);
    RUN(fgetman_of_denormal_is_normalized);
    RUN(stores_convert_unnormals_by_value);
    RUN(fscale_by_infinity_is_operr);
    RUN(fscale_by_2_14_always_overflows);
    RUN(single_store_exact_denormal_is_unfl_only);
    RUN(single_store_inexact_denormal_is_unfl_and_inex2);
    RUN(double_store_exact_denormal_is_unfl_only);
    RUN(fcos_of_denormal_is_exactly_one);
    RUN(fsincos_of_denormal_gives_x_and_one);
    RUN(fatan_of_denormal_is_x);
    RUN(floor_log10_exact_next_to_powers_of_ten);
    RUN(fdiv_final_remainder_bit_is_sticky);
    RUN(fdiv_wide_divisor_is_correctly_rounded);

    test_harness_destroy(ctx);
    printf("[fpu_hw] all tests passed\n");
    return 0;
}
