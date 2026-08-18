// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// test.c — directed + oracle tests for the 601 FPU datapath
// (src/core/cpu/ppc/ppc_softfp.c + ppc_fpu.c), proposal §3.6 / Phase E.
//
// Three layers:
//   1. Directed kernel semantics from the 601UM action lists (§5.4.7):
//      NaN selection/quieting, the invalid/zero-divide suppression cases,
//      signed zeros, rounding modes, denormalization, disabled overflow's
//      per-mode results, the ±1536/±192 trap-enabled exponent wraps,
//      FR/FI/XX, fctiw saturation, and the FPSCR-instruction write rules.
//      AUTHORITY-PENDING cases (Appendix F absent from the manual — the
//      §11 acquisition item) are marked inline: frsp/fctiw NaN payload
//      truncation, fctiw's rounded-vs-unrounded VXCVI boundary, FR
//      magnitude-increment reading, single-op NaN payload truncation.
//   2. A randomized host-double oracle: values (and FI for finite
//      results) must match the host's IEEE arithmetic exactly — host FP
//      is the proposal's correctness-by-definition reference, used here
//      as the ORACLE while the emulator itself stays integer-only.
//   3. Ops-level integration through the interpreter: CR1 records, the
//      MSR[FP] gate, and the precise FEX program exception (SRR0 = the
//      causing instruction, SRR1[11], frD suppression).
//
// The shared corpus hash (fpu_corpus.h) is printed at the end; `make
// wasm-check` builds the same sweep with emcc and diffs the hashes — the
// native-vs-WASM byte-exactness acceptance.

#include "ppc_internal.h"
#include "ppc_softfp.h"

#include "fpu_corpus.h"
#include "harness.h"
#include "memory.h"

#include <fenv.h>
#include <math.h>
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

#define CHECK_EQ64(got, want)                                                                                          \
    do {                                                                                                               \
        checks++;                                                                                                      \
        uint64_t g_ = (got), w_ = (want);                                                                              \
        if (g_ != w_) {                                                                                                \
            printf("FAIL %s:%d: %s = $%016llX, want $%016llX\n", __FILE__, __LINE__, #got, (unsigned long long)g_,     \
                   (unsigned long long)w_);                                                                            \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ32(got, want)                                                                                          \
    do {                                                                                                               \
        checks++;                                                                                                      \
        uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);                                                          \
        if (g_ != w_) {                                                                                                \
            printf("FAIL %s:%d: %s = $%08X, want $%08X\n", __FILE__, __LINE__, #got, g_, w_);                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// === Kernel-call helpers ====================================================

// One arithmetic call from a clean FPSCR (mode + enables as given);
// returns the result (or $DEAD... marker when suppressed) and leaves the
// final FPSCR in *fpscr_out.
static uint64_t karith(ppc_sf_op_t op, uint64_t a, uint64_t b, uint64_t c, int single, uint32_t fpscr_in,
                       uint32_t *fpscr_out) {
    uint32_t f = fpscr_in;
    uint64_t frt = 0xDEADDEADDEADDEADull;
    int wrote = ppc_sf_arith(op, a, b, c, single, &f, &frt);
    if (fpscr_out)
        *fpscr_out = f;
    return wrote ? frt : 0xDEADDEADDEADDEADull;
}

static uint64_t d2u(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static double u2d(uint64_t u) {
    double d;
    memcpy(&d, &u, 8);
    return d;
}

#define QNAN 0x7FF8000000000000ull
#define SNAN 0x7FF0000000000001ull
#define PINF 0x7FF0000000000000ull
#define NINF 0xFFF0000000000000ull
#define ONE  0x3FF0000000000000ull
#define TWO  0x4000000000000000ull

// === 1. Directed kernel semantics ===========================================

static void test_values_basic(void) {
    uint32_t f;
    // 1 + 2 = 3, exact, FPRF +normal
    CHECK_EQ64(karith(PPC_SF_ADD, ONE, TWO, 0, 0, 0, &f), 0x4008000000000000ull);
    CHECK_EQ32(f & (PPC_FPSCR_FR | PPC_FPSCR_FI | PPC_FPSCR_XX), 0);
    CHECK_EQ32((f >> 12) & 0x1Fu, 0x04u);
    // 0.1 + 0.2 (inexact)
    CHECK_EQ64(karith(PPC_SF_ADD, d2u(0.1), d2u(0.2), 0, 0, 0, &f), d2u(0.1 + 0.2));
    CHECK(f & PPC_FPSCR_FI);
    CHECK(f & PPC_FPSCR_XX);
    CHECK(f & PPC_FPSCR_FX); // XX transitioned 0->1
    // subtraction, multiplication, division values
    CHECK_EQ64(karith(PPC_SF_SUB, d2u(5.0), d2u(3.25), 0, 0, 0, &f), d2u(1.75));
    CHECK_EQ64(karith(PPC_SF_MUL, d2u(1.5), 0, d2u(2.5), 0, 0, &f), d2u(3.75));
    CHECK_EQ64(karith(PPC_SF_DIV, d2u(1.0), d2u(3.0), 0, 0, 0, &f), d2u(1.0 / 3.0));
    CHECK(f & PPC_FPSCR_FI);
    // fused madd: (2^27+1)^2 needs the 106-bit intermediate
    double big = 134217729.0; // 2^27 + 1
    CHECK_EQ64(karith(PPC_SF_MADD, d2u(big), d2u(-big * big), d2u(big), 0, 0, &f), d2u(fma(big, big, -big * big)));
    // nmadd/nmsub negate after rounding
    CHECK_EQ64(karith(PPC_SF_NMADD, ONE, d2u(3.0), TWO, 0, 0, &f), d2u(-5.0));
    CHECK_EQ64(karith(PPC_SF_NMSUB, ONE, d2u(3.0), TWO, 0, 0, &f), d2u(1.0));
    // single variant rounds once to 24 bits
    CHECK_EQ64(karith(PPC_SF_MUL, d2u(1.5), 0, d2u(1.5), 1, 0, &f), d2u(2.25));
    CHECK_EQ64(karith(PPC_SF_DIV, ONE, d2u(3.0), 0, 1, 0, &f), d2u((double)(1.0f / 3.0f)));
}

static void test_nan_rules(void) {
    uint32_t f;
    uint64_t qa = 0x7FF8000000000ABCull, qb = 0x7FF8000000000DEFull;
    // frA priority
    CHECK_EQ64(karith(PPC_SF_ADD, qa, qb, 0, 0, 0, &f), qa);
    CHECK_EQ32(f & PPC_FPSCR_VX_ANY, 0); // QNaN propagation is not invalid
    CHECK_EQ32((f >> 12) & 0x1Fu, 0x11u); // FPRF QNaN
    // frB before frC on the madd family (the addend beats the multiplicand)
    CHECK_EQ64(karith(PPC_SF_MADD, ONE, qb, qa, 0, 0, &f), qb);
    // SNaN quieting: high fraction bit forced, payload and sign preserved
    uint64_t sn = 0xFFF0000000000123ull;
    CHECK_EQ64(karith(PPC_SF_ADD, sn, ONE, 0, 0, 0, &f), 0xFFF8000000000123ull);
    CHECK(f & PPC_FPSCR_VXSNAN);
    CHECK(f & PPC_FPSCR_VX);
    CHECK(f & PPC_FPSCR_FX);
    // fnmadd must NOT negate a propagated NaN
    CHECK_EQ64(karith(PPC_SF_NMADD, qa, 0, ONE, 0, 0, &f), qa);
    // generated default QNaN: inf - inf
    CHECK_EQ64(karith(PPC_SF_ADD, PINF, NINF, 0, 0, 0, &f), QNAN);
    CHECK(f & PPC_FPSCR_VXISI);
    // inf + inf is valid
    CHECK_EQ64(karith(PPC_SF_ADD, PINF, PINF, 0, 0, 0, &f), PINF);
    CHECK_EQ32(f & PPC_FPSCR_VX_ANY, 0);
    // fsub of same-sign infinities is the invalid magnitude subtraction
    CHECK_EQ64(karith(PPC_SF_SUB, PINF, PINF, 0, 0, 0, &f), QNAN);
    CHECK(f & PPC_FPSCR_VXISI);
    // inf/inf, 0/0, inf*0
    CHECK_EQ64(karith(PPC_SF_DIV, PINF, NINF, 0, 0, 0, &f), QNAN);
    CHECK(f & PPC_FPSCR_VXIDI);
    CHECK_EQ64(karith(PPC_SF_DIV, 0, 0x8000000000000000ull, 0, 0, 0, &f), QNAN);
    CHECK(f & PPC_FPSCR_VXZDZ);
    CHECK_EQ64(karith(PPC_SF_MUL, PINF, 0, 0x8000000000000000ull, 0, 0, &f), QNAN);
    CHECK(f & PPC_FPSCR_VXIMZ);
    // madd: NaN addend wins the RESULT, but the inf*0 product still
    // raises VXIMZ (601UM §5.4.7 multiple-bit case)
    CHECK_EQ64(karith(PPC_SF_MADD, PINF, qb, 0, 0, 0, &f), qb);
    CHECK(f & PPC_FPSCR_VXIMZ);
    // madd: product inf + opposite-sign inf addend -> VXISI
    CHECK_EQ64(karith(PPC_SF_MADD, PINF, NINF, TWO, 0, 0, &f), QNAN);
    CHECK(f & PPC_FPSCR_VXISI);
    // enabled invalid (VE=1): suppressed — no write, FR/FI cleared, FPRF
    // unchanged, FEX derived
    uint32_t in = PPC_FPSCR_VE | PPC_FPSCR_FR | PPC_FPSCR_FI | 0x0001F000u;
    CHECK_EQ64(karith(PPC_SF_ADD, sn, ONE, 0, 0, in, &f), 0xDEADDEADDEADDEADull);
    CHECK(f & PPC_FPSCR_VXSNAN);
    CHECK(f & PPC_FPSCR_FEX);
    CHECK_EQ32(f & (PPC_FPSCR_FR | PPC_FPSCR_FI), 0);
    CHECK_EQ32(f & PPC_FPSCR_FPRF, 0x0001F000u); // untouched
}

static void test_zero_divide(void) {
    uint32_t f;
    // finite/0 -> signed inf, ZX, FR/FI cleared
    CHECK_EQ64(karith(PPC_SF_DIV, d2u(-3.0), 0, 0, 0, 0, &f), NINF);
    CHECK(f & PPC_FPSCR_ZX);
    CHECK_EQ32(f & (PPC_FPSCR_FR | PPC_FPSCR_FI), 0);
    CHECK_EQ32((f >> 12) & 0x1Fu, 0x09u); // FPRF -inf
    // ZE=1: suppressed
    CHECK_EQ64(karith(PPC_SF_DIV, ONE, 0x8000000000000000ull, 0, 0, PPC_FPSCR_ZE, &f), 0xDEADDEADDEADDEADull);
    CHECK(f & PPC_FPSCR_ZX);
    CHECK(f & PPC_FPSCR_FEX);
    // 0/finite is just zero (no ZX)
    CHECK_EQ64(karith(PPC_SF_DIV, 0x8000000000000000ull, TWO, 0, 0, 0, &f), 0x8000000000000000ull);
    CHECK_EQ32(f & PPC_FPSCR_ZX, 0);
}

static void test_signed_zero(void) {
    uint32_t f;
    // (+0) + (-0) = +0 in RN/RZ/RP, -0 in RM (601UM §2.5.3)
    CHECK_EQ64(karith(PPC_SF_ADD, 0, 0x8000000000000000ull, 0, 0, PPC_RN_NEAREST, &f), 0);
    CHECK_EQ64(karith(PPC_SF_ADD, 0, 0x8000000000000000ull, 0, 0, PPC_RN_MINUS, &f), 0x8000000000000000ull);
    // exact cancellation x + (-x)
    CHECK_EQ64(karith(PPC_SF_SUB, d2u(7.5), d2u(7.5), 0, 0, PPC_RN_NEAREST, &f), 0);
    CHECK_EQ64(karith(PPC_SF_SUB, d2u(7.5), d2u(7.5), 0, 0, PPC_RN_MINUS, &f), 0x8000000000000000ull);
    // (-0) * (+5) = -0; (-0)+(-0) = -0
    CHECK_EQ64(karith(PPC_SF_MUL, 0x8000000000000000ull, 0, d2u(5.0), 0, 0, &f), 0x8000000000000000ull);
    CHECK_EQ64(karith(PPC_SF_ADD, 0x8000000000000000ull, 0x8000000000000000ull, 0, 0, 0, &f), 0x8000000000000000ull);
    // fnmadd of an exact +0 result negates to -0
    CHECK_EQ64(karith(PPC_SF_NMADD, ONE, d2u(-2.0), TWO, 0, 0, &f), 0x8000000000000000ull);
    // FPRF -zero
    CHECK_EQ32((f >> 12) & 0x1Fu, 0x12u);
}

static void test_rounding_modes(void) {
    uint32_t f;
    // 1 + 2^-53: the exact tie between 1.0 and its successor
    uint64_t tie = 0x3CA0000000000000ull; // 2^-53
    CHECK_EQ64(karith(PPC_SF_ADD, ONE, tie, 0, 0, PPC_RN_NEAREST, &f), ONE); // ties-to-even
    CHECK(f & PPC_FPSCR_FI);
    CHECK(!(f & PPC_FPSCR_FR));
    CHECK_EQ64(karith(PPC_SF_ADD, ONE, tie, 0, 0, PPC_RN_PLUS, &f), 0x3FF0000000000001ull);
    CHECK(f & PPC_FPSCR_FR); // fraction incremented
    CHECK_EQ64(karith(PPC_SF_ADD, ONE, tie, 0, 0, PPC_RN_ZERO, &f), ONE);
    CHECK_EQ64(karith(PPC_SF_ADD, ONE, tie, 0, 0, PPC_RN_MINUS, &f), ONE);
    // negative side: RM rounds away, RP truncates
    CHECK_EQ64(karith(PPC_SF_ADD, 0xBFF0000000000000ull, d2u(-u2d(tie)), 0, 0, PPC_RN_MINUS, &f),
               0xBFF0000000000001ull);
    CHECK_EQ64(karith(PPC_SF_ADD, 0xBFF0000000000000ull, d2u(-u2d(tie)), 0, 0, PPC_RN_PLUS, &f), 0xBFF0000000000000ull);
}

static void test_underflow(void) {
    uint32_t f;
    uint64_t min_den = 1ull; // 2^-1074
    // 2^-1074 * 0.5: exact half of the min denormal — RN ties-to-even -> 0
    CHECK_EQ64(karith(PPC_SF_MUL, min_den, 0, d2u(0.5), 0, PPC_RN_NEAREST, &f), 0);
    CHECK(f & PPC_FPSCR_UX); // tiny AND inexact
    CHECK(f & PPC_FPSCR_FI);
    CHECK_EQ32((f >> 12) & 0x1Fu, 0x02u); // FPRF +zero
    // RP delivers the min denormal instead
    CHECK_EQ64(karith(PPC_SF_MUL, min_den, 0, d2u(0.5), 0, PPC_RN_PLUS, &f), min_den);
    CHECK(f & PPC_FPSCR_FR);
    CHECK_EQ32((f >> 12) & 0x1Fu, 0x14u); // FPRF +denormal
    // min_normal * 0.5 = 2^-1023: tiny but EXACT -> no UX (disabled case
    // needs loss of accuracy)
    CHECK_EQ64(karith(PPC_SF_MUL, 0x0010000000000000ull, 0, d2u(0.5), 0, 0, &f), 0x0008000000000000ull);
    CHECK(!(f & PPC_FPSCR_UX));
    CHECK(!(f & PPC_FPSCR_FI));
    // UE=1: UX on tininess alone, exponent wrapped +1536: 2^-1023 delivers
    // as 2^513 (biased 1536 = $600)
    CHECK_EQ64(karith(PPC_SF_MUL, 0x0010000000000000ull, 0, d2u(0.5), 0, PPC_FPSCR_UE, &f), 0x6000000000000000ull);
    CHECK(f & PPC_FPSCR_UX);
    CHECK(f & PPC_FPSCR_FEX);
    CHECK(!(f & PPC_FPSCR_FI));
    CHECK_EQ32((f >> 12) & 0x1Fu, 0x04u); // FPRF +normal (wrapped)
}

static void test_overflow(void) {
    uint32_t f;
    uint64_t maxd = 0x7FEFFFFFFFFFFFFFull;
    // RN -> inf; OX+XX set; FR/FI CLEARED (the §5.4.7.4.1 rule)
    CHECK_EQ64(karith(PPC_SF_MUL, maxd, 0, TWO, 0, PPC_RN_NEAREST, &f), PINF);
    CHECK(f & PPC_FPSCR_OX);
    CHECK(f & PPC_FPSCR_XX);
    CHECK_EQ32(f & (PPC_FPSCR_FR | PPC_FPSCR_FI), 0);
    CHECK_EQ32((f >> 12) & 0x1Fu, 0x05u); // FPRF +inf
    // RZ -> +max finite, FPRF +normal
    CHECK_EQ64(karith(PPC_SF_MUL, maxd, 0, TWO, 0, PPC_RN_ZERO, &f), maxd);
    CHECK_EQ32((f >> 12) & 0x1Fu, 0x04u);
    // RM on positive -> +max; RP on negative -> -max
    CHECK_EQ64(karith(PPC_SF_MUL, maxd, 0, TWO, 0, PPC_RN_MINUS, &f), maxd);
    CHECK_EQ64(karith(PPC_SF_MUL, d2u(-u2d(maxd)), 0, TWO, 0, PPC_RN_PLUS, &f), 0xFFEFFFFFFFFFFFFFull);
    // OE=1: wrapped exponent (-1536), exact -> FI=0, FPRF +normal
    CHECK_EQ64(karith(PPC_SF_MUL, maxd, 0, TWO, 0, PPC_FPSCR_OE, &f), maxd - (1535ull << 52));
    CHECK(f & PPC_FPSCR_OX);
    CHECK(f & PPC_FPSCR_FEX);
    CHECK(!(f & PPC_FPSCR_FI));
    CHECK_EQ32((f >> 12) & 0x1Fu, 0x04u);
}

static void test_frsp(void) {
    uint32_t f = 0;
    uint64_t frt;
    // In-range value passes through rounded to 24 bits
    CHECK(ppc_sf_frsp(d2u(1.0 + 0x1.0p-28), &f, &frt));
    CHECK_EQ64(frt, ONE);
    CHECK(f & PPC_FPSCR_FI);
    CHECK(!(f & PPC_FPSCR_FR));
    // Single-range overflow: 2^128 -> inf with OX under RN
    f = 0;
    CHECK(ppc_sf_frsp(0x47F0000000000000ull, &f, &frt));
    CHECK_EQ64(frt, PINF);
    CHECK(f & PPC_FPSCR_OX);
    // Single denormal delivery (exact): 2^-140 is a single-denormal value
    f = 0;
    CHECK(ppc_sf_frsp(d2u(0x1.0p-140), &f, &frt));
    CHECK_EQ64(frt, d2u(0x1.0p-140));
    CHECK(!(f & PPC_FPSCR_UX));
    // Single-denormal inexact: 2^-140 + 2^-160 loses the tail
    f = 0;
    CHECK(ppc_sf_frsp(d2u(0x1.0p-140 + 0x1.0p-160), &f, &frt));
    CHECK_EQ64(frt, d2u(0x1.0p-140));
    CHECK(f & PPC_FPSCR_UX);
    CHECK(f & PPC_FPSCR_FI);
    // SNaN: quieted, payload truncated to single (low 29 bits cleared) —
    // AUTHORITY-PENDING (Appendix F): truncation from folio 2-70's
    // "low-order 29 fraction bits are zero" rule
    f = 0;
    CHECK(ppc_sf_frsp(0xFFF000001FFFFFFFull, &f, &frt));
    CHECK_EQ64(frt, 0xFFF8000000000000ull);
    CHECK(f & PPC_FPSCR_VXSNAN);
    // VE=1 suppresses
    f = PPC_FPSCR_VE;
    CHECK(!ppc_sf_frsp(SNAN, &f, &frt));
    CHECK(f & PPC_FPSCR_FEX);
}

static void test_fctiw(void) {
    uint32_t f = 0;
    uint64_t frt;
    const uint64_t HI = 0xFFF8000000000000ull; // the 601's high word
    // ties-to-even both ways
    CHECK(ppc_sf_fctiw(d2u(2.5), 0, &f, &frt));
    CHECK_EQ64(frt, HI | 2u);
    CHECK(f & PPC_FPSCR_FI);
    CHECK(!(f & PPC_FPSCR_FR)); // truncated, not incremented
    f = 0;
    CHECK(ppc_sf_fctiw(d2u(3.5), 0, &f, &frt));
    CHECK_EQ64(frt, HI | 4u);
    CHECK(f & PPC_FPSCR_FR); // magnitude incremented (AUTHORITY-PENDING reading)
    f = 0;
    CHECK(ppc_sf_fctiw(d2u(-2.5), 0, &f, &frt));
    CHECK_EQ64(frt, HI | 0xFFFFFFFEull);
    // fctiwz truncates toward zero
    f = 0;
    CHECK(ppc_sf_fctiw(d2u(2.9), 1, &f, &frt));
    CHECK_EQ64(frt, HI | 2u);
    f = 0;
    CHECK(ppc_sf_fctiw(d2u(-2.9), 1, &f, &frt));
    CHECK_EQ64(frt, HI | 0xFFFFFFFEull);
    // exact conversion: no FI
    f = 0;
    CHECK(ppc_sf_fctiw(d2u(-12345.0), 0, &f, &frt));
    CHECK_EQ64(frt, HI | (uint64_t)(uint32_t)(-12345));
    CHECK(!(f & PPC_FPSCR_FI));
    // saturation: 2^31 -> $7FFFFFFF + VXCVI; -2^31 exact -> no VXCVI
    f = 0;
    CHECK(ppc_sf_fctiw(d2u(0x1.0p31), 0, &f, &frt));
    CHECK_EQ64(frt, HI | 0x7FFFFFFFull);
    CHECK(f & PPC_FPSCR_VXCVI);
    f = 0;
    CHECK(ppc_sf_fctiw(d2u(-0x1.0p31), 0, &f, &frt));
    CHECK_EQ64(frt, HI | 0x80000000ull);
    CHECK(!(f & PPC_FPSCR_VXCVI));
    // the rounded result decides representability (2^31 - 0.4 rounds to
    // 2^31): VXCVI + positive saturation — AUTHORITY-PENDING (Appendix F)
    f = 0;
    CHECK(ppc_sf_fctiw(d2u(0x1.0p31 - 0.4), 0, &f, &frt));
    CHECK_EQ64(frt, HI | 0x7FFFFFFFull);
    CHECK(f & PPC_FPSCR_VXCVI);
    // ...but fctiwz truncates the same value into range: no VXCVI
    f = 0;
    CHECK(ppc_sf_fctiw(d2u(0x1.0p31 - 0.4), 1, &f, &frt));
    CHECK_EQ64(frt, HI | 0x7FFFFFFFull);
    CHECK(!(f & PPC_FPSCR_VXCVI));
    // NaN -> most negative + VXCVI (+VXSNAN for signaling)
    f = 0;
    CHECK(ppc_sf_fctiw(SNAN, 0, &f, &frt));
    CHECK_EQ64(frt, HI | 0x80000000ull);
    CHECK(f & PPC_FPSCR_VXCVI);
    CHECK(f & PPC_FPSCR_VXSNAN);
    // ±inf saturate by sign
    f = 0;
    CHECK(ppc_sf_fctiw(PINF, 0, &f, &frt));
    CHECK_EQ64(frt, HI | 0x7FFFFFFFull);
    f = 0;
    CHECK(ppc_sf_fctiw(NINF, 0, &f, &frt));
    CHECK_EQ64(frt, HI | 0x80000000ull);
    // VE=1 suppresses the invalid convert
    f = PPC_FPSCR_VE;
    CHECK(!ppc_sf_fctiw(QNAN, 0, &f, &frt));
    CHECK(f & PPC_FPSCR_FEX);
}

// === 2. Randomized host-double oracle =======================================

static const int host_rounding[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};

static int is_nan_bits(uint64_t u) {
    return ((u >> 52) & 0x7FFu) == 0x7FFu && (u & 0xFFFFFFFFFFFFFull) != 0;
}

static void test_host_oracle(void) {
    uint64_t rng = 0x0601C0DEB00Cull;
    int compared = 0;
    for (int n = 0; n < 200000; n++) {
        uint64_t a = corpus_rng(&rng), b = corpus_rng(&rng), c = corpus_rng(&rng);
        if (n & 1) { // bias exponents toward each other so ops interact
            a = (a & ~0x7FF0000000000000ull) | ((uint64_t)(0x380 + (corpus_rng(&rng) % 0x100)) << 52);
            b = (b & ~0x7FF0000000000000ull) | ((uint64_t)(0x380 + (corpus_rng(&rng) % 0x100)) << 52);
            c = (c & ~0x7FF0000000000000ull) | ((uint64_t)(0x380 + (corpus_rng(&rng) % 0x100)) << 52);
        }
        if (is_nan_bits(a) || is_nan_bits(b) || is_nan_bits(c))
            continue; // NaN semantics are PPC-specific; directed tests own them
        uint32_t rn = (uint32_t)(corpus_rng(&rng) & 3u);
        int op = (int)(corpus_rng(&rng) % 8u);
        uint32_t f = rn;
        uint64_t frt;
        if (!ppc_sf_arith((ppc_sf_op_t)op, a, b, c, 0, &f, &frt))
            continue; // suppression needs enables; none set here
        volatile double va = u2d(a), vb = u2d(b), vc = u2d(c);
        double want;
        fesetround(host_rounding[rn]);
        feclearexcept(FE_ALL_EXCEPT);
        switch (op) {
        case PPC_SF_ADD:
            want = va + vb;
            break;
        case PPC_SF_SUB:
            want = va - vb;
            break;
        case PPC_SF_MUL:
            want = va * vc;
            break;
        case PPC_SF_DIV:
            want = va / vb;
            break;
        case PPC_SF_MADD:
            want = fma(va, vc, vb);
            break;
        case PPC_SF_MSUB:
            want = fma(va, vc, -vb);
            break;
        case PPC_SF_NMADD:
            want = -fma(va, vc, vb);
            break;
        default:
            want = -fma(va, vc, -vb);
            break;
        }
        int host_inexact = fetestexcept(FE_INEXACT) != 0;
        fesetround(FE_TONEAREST);
        uint64_t wbits = d2u(want);
        if (is_nan_bits(wbits))
            continue; // invalid combos: PPC NaN rules differ from host
        // The nm-forms negate AFTER rounding; for exact-zero sums the
        // host's -(±0) matches, so compare bits directly.
        checks++;
        if (frt != wbits) {
            printf("FAIL oracle op=%d rn=%u a=%016llX b=%016llX c=%016llX: got %016llX want %016llX\n", op, rn,
                   (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)frt,
                   (unsigned long long)wbits);
            failures++;
            if (failures > 20)
                return;
        }
        if (f & PPC_FPSCR_OX)
            continue; // disabled overflow CLEARS FR/FI (§5.4.7.4.1); the
                      // host reports inexact — directed tests own this
        checks++;
        if (((f & PPC_FPSCR_FI) != 0) != host_inexact) {
            printf("FAIL oracle FI op=%d rn=%u a=%016llX b=%016llX c=%016llX: FI=%d host=%d\n", op, rn,
                   (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (f & PPC_FPSCR_FI) != 0,
                   host_inexact);
            failures++;
            if (failures > 20)
                return;
        }
        compared++;
    }
    printf("oracle: %d comparisons\n", compared);

    // Single-precision ops against host float arithmetic (operands drawn
    // single-representable, per the architecture's input requirement).
    rng = 0xF10A7B17ull;
    compared = 0;
    for (int n = 0; n < 100000; n++) {
        float fa, fb, fc;
        uint32_t ua = (uint32_t)corpus_rng(&rng), ub = (uint32_t)corpus_rng(&rng), uc = (uint32_t)corpus_rng(&rng);
        memcpy(&fa, &ua, 4);
        memcpy(&fb, &ub, 4);
        memcpy(&fc, &uc, 4);
        if (isnan(fa) || isnan(fb) || isnan(fc))
            continue;
        uint32_t rn = (uint32_t)(corpus_rng(&rng) & 3u);
        int op = (int)(corpus_rng(&rng) % 8u);
        uint32_t f = rn;
        uint64_t frt;
        if (!ppc_sf_arith((ppc_sf_op_t)op, d2u((double)fa), d2u((double)fb), d2u((double)fc), 1, &f, &frt))
            continue;
        volatile float xa = fa, xb = fb, xc = fc;
        float wantf;
        fesetround(host_rounding[rn]);
        switch (op) {
        case PPC_SF_ADD:
            wantf = xa + xb;
            break;
        case PPC_SF_SUB:
            wantf = xa - xb;
            break;
        case PPC_SF_MUL:
            wantf = xa * xc;
            break;
        case PPC_SF_DIV:
            wantf = xa / xb;
            break;
        case PPC_SF_MADD:
            wantf = fmaf(xa, xc, xb);
            break;
        case PPC_SF_MSUB:
            wantf = fmaf(xa, xc, -xb);
            break;
        case PPC_SF_NMADD:
            wantf = -fmaf(xa, xc, xb);
            break;
        default:
            wantf = -fmaf(xa, xc, -xb);
            break;
        }
        fesetround(FE_TONEAREST);
        uint64_t wbits = d2u((double)wantf);
        if (is_nan_bits(wbits))
            continue;
        checks++;
        if (frt != wbits) {
            printf("FAIL oracle-s op=%d rn=%u a=%08X b=%08X c=%08X: got %016llX want %016llX\n", op, rn, ua, ub, uc,
                   (unsigned long long)frt, (unsigned long long)wbits);
            failures++;
            if (failures > 20)
                return;
        }
        compared++;
    }
    printf("oracle-s: %d comparisons\n", compared);
}

// === 3. Ops-level integration (interpreter, CR1, trap) ======================

static ppc_t *P;

static void fresh(void) {
    ppc_reset(P);
    P->msr = PPC_MSR_ME | PPC_MSR_FP;
    ppc_update_active_maps(P);
}

static void run_at(uint32_t addr, int n) {
    P->pc = addr;
    uint32_t budget = (uint32_t)n;
    ppc_run(P, &budget);
}

// A-form encoder: op frD,frA,frB,frC
static uint32_t e_a(uint32_t primary, uint32_t d, uint32_t a, uint32_t b, uint32_t c, uint32_t xo5, uint32_t rc) {
    return (primary << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo5 << 1) | rc;
}

static void test_interpreter_integration(void) {
    // fadd. : CR1 gets FPSCR[0-3] (FX FEX VX OX)
    fresh();
    P->fpr[1] = SNAN;
    P->fpr[2] = ONE;
    memory_write_uint32(0x1000, e_a(63, 3, 1, 2, 0, 21, 1)); // fadd. f3,f1,f2
    run_at(0x1000, 1);
    CHECK_EQ64(P->fpr[3], 0x7FF8000000000001ull); // quieted frA
    CHECK(P->fpscr & PPC_FPSCR_VXSNAN);
    CHECK_EQ32((P->cr >> 24) & 0xFu, (P->fpscr >> 28)); // CR1 = final FX/FEX/VX/OX
    CHECK_EQ32((P->cr >> 24) & 0xFu, 0xAu); // FX + VX, no FEX (VE off)

    // Enabled invalid + MSR[FE0]: precise program exception — SRR0 = the
    // causing instruction, SRR1[11], frD unchanged, VXSNAN recorded
    fresh();
    P->msr |= PPC_MSR_FE0;
    ppc_update_active_maps(P);
    P->fpscr = PPC_FPSCR_VE;
    P->fpr[1] = SNAN;
    P->fpr[2] = ONE;
    P->fpr[3] = 0x1111111111111111ull;
    memory_write_uint32(0x1000, e_a(63, 3, 1, 2, 0, 21, 0)); // fadd f3,f1,f2
    run_at(0x1000, 1);
    CHECK_EQ32(P->pc, 0x00000700u);
    CHECK_EQ32(P->srr0, 0x1000u);
    CHECK(P->srr1 & PPC_SRR1_PROG_FPENABLED);
    CHECK_EQ64(P->fpr[3], 0x1111111111111111ull); // suppressed
    CHECK(P->fpscr & PPC_FPSCR_FEX);
    CHECK(!(P->msr & PPC_MSR_FE0)); // cleared on entry

    // FE bits clear -> no exception even with FEX set
    fresh();
    P->fpscr = PPC_FPSCR_VE;
    P->fpr[1] = SNAN;
    P->fpr[2] = ONE;
    memory_write_uint32(0x1000, e_a(63, 3, 1, 2, 0, 21, 0));
    run_at(0x1000, 1);
    CHECK_EQ32(P->pc, 0x1004u);
    CHECK(P->fpscr & PPC_FPSCR_FEX);

    // mtfsb1 on XX with XE enabled raises FEX and traps precisely
    fresh();
    P->msr |= PPC_MSR_FE1;
    ppc_update_active_maps(P);
    P->fpscr = PPC_FPSCR_XE;
    memory_write_uint32(0x1000, (63u << 26) | (6u << 21) | (38u << 1)); // mtfsb1 6 (XX)
    run_at(0x1000, 1);
    CHECK_EQ32(P->pc, 0x00000700u);
    CHECK(P->srr1 & PPC_SRR1_PROG_FPENABLED);
    CHECK(P->fpscr & PPC_FPSCR_XX);
    CHECK(P->fpscr & PPC_FPSCR_FEX);

    // mtfsf: FX comes verbatim from the source; FEX/VX are not writable
    fresh();
    P->fpr[5] = PPC_FPSCR_FX | PPC_FPSCR_FEX | PPC_FPSCR_VX | PPC_FPSCR_OX;
    memory_write_uint32(0x1000, (63u << 26) | (0xFFu << 17) | (5u << 11) | (711u << 1)); // mtfsf 0xFF,f5
    run_at(0x1000, 1);
    CHECK(P->fpscr & PPC_FPSCR_FX);
    CHECK(P->fpscr & PPC_FPSCR_OX);
    CHECK(!(P->fpscr & PPC_FPSCR_VX)); // derived: no VXxxx set
    CHECK(!(P->fpscr & PPC_FPSCR_FEX)); // derived: OX set but OE clear
    // and writing all-zero clears FX explicitly (no transition rule)
    fresh();
    P->fpscr = PPC_FPSCR_FX | PPC_FPSCR_OX;
    P->fpr[5] = 0;
    memory_write_uint32(0x1000, (63u << 26) | (0xFFu << 17) | (5u << 11) | (711u << 1));
    run_at(0x1000, 1);
    CHECK_EQ32(P->fpscr, 0);

    // mcrfs field 3: copies VXVC/FR/FI/C, clears ONLY VXVC
    fresh();
    P->fpscr = PPC_FPSCR_VXVC | PPC_FPSCR_FR | PPC_FPSCR_FI | PPC_FPSCR_C;
    memory_write_uint32(0x1000, (63u << 26) | (2u << 23) | (3u << 18) | (64u << 1)); // mcrfs cr2,3
    run_at(0x1000, 1);
    CHECK_EQ32((P->cr >> 20) & 0xFu, 0xFu); // the whole field was copied
    CHECK(!(P->fpscr & PPC_FPSCR_VXVC));
    CHECK(P->fpscr & PPC_FPSCR_FR);
    CHECK(P->fpscr & PPC_FPSCR_FI);
    CHECK(P->fpscr & PPC_FPSCR_C);

    // fcmpo: QNaN -> VXVC; SNaN with VE=0 -> VXSNAN + VXVC
    fresh();
    P->fpr[1] = QNAN;
    P->fpr[2] = ONE;
    memory_write_uint32(0x1000, (63u << 26) | (0u << 23) | (1u << 16) | (2u << 11) | (32u << 1)); // fcmpo cr0
    run_at(0x1000, 1);
    CHECK(P->fpscr & PPC_FPSCR_VXVC);
    CHECK(!(P->fpscr & PPC_FPSCR_VXSNAN));
    CHECK_EQ32(P->cr >> 28, 1u); // unordered
    fresh();
    P->fpscr = PPC_FPSCR_VE;
    P->fpr[1] = SNAN;
    P->fpr[2] = ONE;
    memory_write_uint32(0x1000, (63u << 26) | (0u << 23) | (1u << 16) | (2u << 11) | (32u << 1));
    run_at(0x1000, 1);
    CHECK(P->fpscr & PPC_FPSCR_VXSNAN);
    CHECK(!(P->fpscr & PPC_FPSCR_VXVC)); // VE=1 keeps VXVC out
}

int main(void) {
    // 32-bit context, hand-built like the ppc suite (the harness's own
    // init is 24-bit-Plus-shaped).
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

    test_values_basic();
    test_nan_rules();
    test_zero_divide();
    test_signed_zero();
    test_rounding_modes();
    test_underflow();
    test_overflow();
    test_frsp();
    test_fctiw();
    test_host_oracle();
    test_interpreter_integration();

    // The cross-host corpus digest: `make wasm-check` compares this line
    // against the emcc/node build of the same sweep.
    printf("corpus-hash: %016llX\n", (unsigned long long)ppc_fpu_corpus_hash());

    printf("ppc_fpu: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
