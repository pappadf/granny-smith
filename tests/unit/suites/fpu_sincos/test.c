// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// FSINCOS quadrant/sign tests.
//
// fpu_op_sincos() evaluates sin and cos together and picks the result
// signs with simplified expressions derived from the FPSP's bit twiddling
// (NODD: sin from the cos polynomial and cos from the sin polynomial;
// NEVEN: one shared sign).  This suite checks, for arguments in every
// quadrant N mod 4 (positive and negative, fast and general reduction),
// that FSINCOS agrees with the separate FSIN and FCOS paths to within a
// couple of ULPs and has the mathematically correct sign.

#include "fpu.h"
#include "harness.h"
#include "test_assert.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

// Unpacked extended value of a host double (exact: 53 bits fit in 64)
static fpu_unpacked_t from_double(double d) {
    fpu_unpacked_t r = {d < 0, 0, 0, 0};
    int e;
    double m = frexp(fabs(d), &e); // m in [0.5, 1)
    r.exponent = e - 1;
    r.mantissa_hi = (uint64_t)ldexp(m, 64);
    return r;
}

// Host double of a float80 value (sign and magnitude, for checks only)
static double to_double(float80_reg_t f) {
    if (fp80_is_zero(f))
        return 0.0;
    double m = ldexp((double)f.mantissa, -63);
    double v = ldexp(m, fp80_exp(f) - FPU_EXP_BIAS);
    return fp80_sign(f) ? -v : v;
}

// |a - b| in units of a's last place (both finite, non-zero, same binade-ish)
static double ulps(float80_reg_t a, float80_reg_t b) {
    double ulp = ldexp(1.0, fp80_exp(a) - FPU_EXP_BIAS - 63);
    return fabs(to_double(a) - to_double(b)) / ulp;
}

static void check_one(double x) {
    fpu_state_t *fpu = fpu_init();
    fpu_unpacked_t src = from_double(x);
    float80_reg_t raw = fpu_pack(fpu, src);

    float80_reg_t s = fpu_pack(fpu, fpu_op_sin(fpu, src, raw));
    float80_reg_t c = fpu_pack(fpu, fpu_op_cos(fpu, src, raw));
    float80_reg_t sc_s = fpu_pack(fpu, fpu_op_sincos(fpu, src, raw, 7));
    float80_reg_t sc_c = fpu->fp[7];

    // Signs agree with the separate paths and with the host libm
    ASSERT_EQ_INT(fp80_sign(sc_s), fp80_sign(s));
    ASSERT_EQ_INT(fp80_sign(sc_c), fp80_sign(c));
    ASSERT_EQ_INT(fp80_sign(sc_s), sin(x) < 0);
    ASSERT_EQ_INT(fp80_sign(sc_c), cos(x) < 0);

    // Magnitudes agree to a few ULPs (different Horner orderings)
    if (ulps(s, sc_s) > 4 || ulps(c, sc_c) > 4) {
        fprintf(stderr, "x=%.17g sin %g ulps, cos %g ulps\n", x, ulps(s, sc_s), ulps(c, sc_c));
        ASSERT_TRUE(0);
    }
    // And with the host to double precision
    ASSERT_TRUE(fabs(to_double(sc_s) - sin(x)) < 1e-15);
    ASSERT_TRUE(fabs(to_double(sc_c) - cos(x)) < 1e-15);
    fpu_free(fpu);
}

// Every quadrant N mod 4 in [-20, 20], at offsets inside each quadrant;
// |X| < 15*pi takes the PITBL fast path, larger the general reduction.
TEST(sincos_all_quadrants_fast_path) {
    const double offs[] = {0.1, 0.3, 0.6, -0.1, -0.3, -0.6};
    for (int n = -20; n <= 20; n++)
        for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
            check_one(n * M_PI_2 + offs[i]);
}

TEST(sincos_all_quadrants_general_path) {
    const double offs[] = {0.2, -0.2, 0.5, -0.5};
    for (int n = 40; n <= 47; n++)
        for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
            check_one(n * M_PI_2 + offs[i]);
            check_one(-(n * M_PI_2 + offs[i]));
        }
}

int main(void) {
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }

    RUN(sincos_all_quadrants_fast_path);
    RUN(sincos_all_quadrants_general_path);

    test_harness_destroy(ctx);
    return 0;
}
