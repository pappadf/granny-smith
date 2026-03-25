// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fpu_transc.c
// Motorola 68882 FPU transcendental functions. An attempt to implement the
// exact algorithms from the Motorola FPSP (Floating Point Software Package),
// but in this case using the soft-float unpacked arithmetic from fpu.c.
// This should in theory (hopefully) produce bit-identical results to the 68882 hardware.

#include "fpu.h"

// ============================================================================
// Helpers for building fpu_unpacked_t constants from FPSP data
// ============================================================================

// Build unpacked value from FPSP extended-precision triple
// Format: w0 = { sign(1) + biased_exp(15) } << 16, w1w2 = 64-bit mantissa
static inline fpu_unpacked_t fpsp_ext(uint32_t w0, uint32_t w1, uint32_t w2) {
    fpu_unpacked_t r;
    r.sign = (w0 >> 31) & 1;
    uint16_t biased = (w0 >> 16) & 0x7FFF;
    r.exponent = (int32_t)biased - FPU_EXP_BIAS;
    r.mantissa_hi = ((uint64_t)w1 << 32) | w2;
    r.mantissa_lo = 0;
    return r;
}

// Build unpacked value from IEEE 754 double (two big-endian uint32 halves)
static inline fpu_unpacked_t fpsp_dbl(uint32_t hi, uint32_t lo) {
    uint64_t bits = ((uint64_t)hi << 32) | lo;
    fpu_unpacked_t r;
    r.sign = (bits >> 63) & 1;
    int biased = (bits >> 52) & 0x7FF;
    uint64_t frac = bits & 0x000FFFFFFFFFFFFFULL;
    r.exponent = biased - 1023;
    // Implicit 1 + 52-bit fraction → place 53 significant bits at top of 64
    r.mantissa_hi = (0x0010000000000000ULL | frac) << 11;
    r.mantissa_lo = 0;
    return r;
}

// Build unpacked value from IEEE 754 single (one big-endian uint32)
static inline fpu_unpacked_t fpsp_sgl(uint32_t w) {
    fpu_unpacked_t r;
    r.sign = (w >> 31) & 1;
    int biased = (w >> 23) & 0xFF;
    uint32_t frac = w & 0x007FFFFF;
    if (biased == 0) {
        // Zero or subnormal single
        r.exponent = FPU_EXP_ZERO;
        r.mantissa_hi = 0;
    } else {
        r.exponent = biased - 127;
        // Implicit 1 + 23-bit fraction → 24 significant bits at top of 64
        r.mantissa_hi = ((uint64_t)(0x00800000u | frac)) << 40;
    }
    r.mantissa_lo = 0;
    return r;
}

// Round to 64-bit mantissa (round-to-nearest-even), simulating the 68882's
// register precision. The 68882 rounds each fmulx/faddx result to 64-bit
// mantissa (+ guard/round/sticky). Our soft-float keeps 128 bits; this
// helper discards excess precision to match the hardware at each step.
static inline fpu_unpacked_t fp_rnd64(fpu_unpacked_t v) {
    if (v.exponent == FPU_EXP_ZERO || v.exponent == FPU_EXP_INF) {
        v.mantissa_lo = 0;
        return v;
    }
    // Guard bit = MSB of mantissa_lo (bit 63), round/sticky = rest
    uint64_t guard = v.mantissa_lo >> 63;
    uint64_t sticky = v.mantissa_lo & 0x7FFFFFFFFFFFFFFFULL;
    // Round to nearest even: round up if guard=1 AND (sticky!=0 OR lsb of mantissa_hi=1)
    if (guard && (sticky || (v.mantissa_hi & 1))) {
        v.mantissa_hi++;
        if (v.mantissa_hi == 0) {
            // Overflow: mantissa wrapped around
            v.mantissa_hi = 0x8000000000000000ULL;
            v.exponent++;
        }
    }
    v.mantissa_lo = 0;
    return v;
}

// ============================================================================
// FPSP constants
// ============================================================================

// Compact-form bounds for near-1 detection: [~15/16, ~17/16]
#define BOUNDS1_LO 0x3FFEF07Du
#define BOUNDS1_HI 0x3FFF8841u

// Compact-form bounds for lognp1 half/three-halves range: [1/2, 3/2]
#define BOUNDS2_LO 0x3FFE8000u
#define BOUNDS2_HI 0x3FFFC000u

// ln(2) in extended precision
static const fpu_unpacked_t LOGOF2 = {false, -1, 0xB17217F7D1CF79ACULL, 0};

// LOGTBL: 64 entry pairs of (1/F, log(F)) in extended precision.
// Each entry is 3 uint32: { sign+biased_exp<<16, mant_hi, mant_lo }.
// Pairs: [invF_0, logF_0, invF_1, logF_1, ..., invF_63, logF_63]
static const uint32_t logtbl_data[64 * 2 * 3] = {
    0x3FFE0000, 0xFE03F80F, 0xE03F80FE, 0x3FF70000, 0xFF015358, 0x833C47E2, 0x3FFE0000, 0xFA232CF2, 0x52138AC0,
    0x3FF90000, 0xBDC8D83E, 0xAD88D549, 0x3FFE0000, 0xF6603D98, 0x0F6603DA, 0x3FFA0000, 0x9CF43DCF, 0xF5EAFD48,
    0x3FFE0000, 0xF2B9D648, 0x0F2B9D65, 0x3FFA0000, 0xDA16EB88, 0xCB8DF614, 0x3FFE0000, 0xEF2EB71F, 0xC4345238,
    0x3FFB0000, 0x8B29B775, 0x1BD70743, 0x3FFE0000, 0xEBBDB2A5, 0xC1619C8C, 0x3FFB0000, 0xA8D839F8, 0x30C1FB49,
    0x3FFE0000, 0xE865AC7B, 0x7603A197, 0x3FFB0000, 0xC61A2EB1, 0x8CD907AD, 0x3FFE0000, 0xE525982A, 0xF70C880E,
    0x3FFB0000, 0xE2F2A47A, 0xDE3A18AF, 0x3FFE0000, 0xE1FC780E, 0x1FC780E2, 0x3FFB0000, 0xFF64898E, 0xDF55D551,
    0x3FFE0000, 0xDEE95C4C, 0xA037BA57, 0x3FFC0000, 0x8DB956A9, 0x7B3D0148, 0x3FFE0000, 0xDBEB61EE, 0xD19C5958,
    0x3FFC0000, 0x9B8FE100, 0xF47BA1DE, 0x3FFE0000, 0xD901B203, 0x6406C80E, 0x3FFC0000, 0xA9372F1D, 0x0DA1BD17,
    0x3FFE0000, 0xD62B80D6, 0x2B80D62C, 0x3FFC0000, 0xB6B07F38, 0xCE90E46B, 0x3FFE0000, 0xD3680D36, 0x80D3680D,
    0x3FFC0000, 0xC3FD0329, 0x06488481, 0x3FFE0000, 0xD0B69FCB, 0xD2580D0B, 0x3FFC0000, 0xD11DE0FF, 0x15AB18CA,
    0x3FFE0000, 0xCE168A77, 0x25080CE1, 0x3FFC0000, 0xDE1433A1, 0x6C66B150, 0x3FFE0000, 0xCB8727C0, 0x65C393E0,
    0x3FFC0000, 0xEAE10B5A, 0x7DDC8ADD, 0x3FFE0000, 0xC907DA4E, 0x871146AD, 0x3FFC0000, 0xF7856E5E, 0xE2C9B291,
    0x3FFE0000, 0xC6980C69, 0x80C6980C, 0x3FFD0000, 0x82012CA5, 0xA68206D7, 0x3FFE0000, 0xC4372F85, 0x5D824CA6,
    0x3FFD0000, 0x882C5FCD, 0x7256A8C5, 0x3FFE0000, 0xC1E4BBD5, 0x95F6E947, 0x3FFD0000, 0x8E44C60B, 0x4CCFD7DE,
    0x3FFE0000, 0xBFA02FE8, 0x0BFA02FF, 0x3FFD0000, 0x944AD09E, 0xF4351AF6, 0x3FFE0000, 0xBD691047, 0x07661AA3,
    0x3FFD0000, 0x9A3EECD4, 0xC3EAA6B2, 0x3FFE0000, 0xBB3EE721, 0xA54D880C, 0x3FFD0000, 0xA0218434, 0x353F1DE8,
    0x3FFE0000, 0xB92143FA, 0x36F5E02E, 0x3FFD0000, 0xA5F2FCAB, 0xBBC506DA, 0x3FFE0000, 0xB70FBB5A, 0x19BE3659,
    0x3FFD0000, 0xABB3B8BA, 0x2AD362A5, 0x3FFE0000, 0xB509E68A, 0x9B94821F, 0x3FFD0000, 0xB1641795, 0xCE3CA97B,
    0x3FFE0000, 0xB30F6352, 0x8917C80B, 0x3FFD0000, 0xB7047551, 0x5D0F1C61, 0x3FFE0000, 0xB11FD3B8, 0x0B11FD3C,
    0x3FFD0000, 0xBC952AFE, 0xEA3D13E1, 0x3FFE0000, 0xAF3ADDC6, 0x80AF3ADE, 0x3FFD0000, 0xC2168ED0, 0xF458BA4A,
    0x3FFE0000, 0xAD602B58, 0x0AD602B6, 0x3FFD0000, 0xC788F439, 0xB3163BF1, 0x3FFE0000, 0xAB8F69E2, 0x8359CD11,
    0x3FFD0000, 0xCCECAC08, 0xBF04565D, 0x3FFE0000, 0xA9C84A47, 0xA07F5638, 0x3FFD0000, 0xD2420487, 0x2DD85160,
    0x3FFE0000, 0xA80A80A8, 0x0A80A80B, 0x3FFD0000, 0xD7894992, 0x3BC3588A, 0x3FFE0000, 0xA655C439, 0x2D7B73A8,
    0x3FFD0000, 0xDCC2C4B4, 0x9887DACC, 0x3FFE0000, 0xA4A9CF1D, 0x96833751, 0x3FFD0000, 0xE1EEBD3E, 0x6D6A6B9E,
    0x3FFE0000, 0xA3065E3F, 0xAE7CD0E0, 0x3FFD0000, 0xE70D785C, 0x2F9F5BDC, 0x3FFE0000, 0xA16B312E, 0xA8FC377D,
    0x3FFD0000, 0xEC1F392C, 0x5179F283, 0x3FFE0000, 0x9FD809FD, 0x809FD80A, 0x3FFD0000, 0xF12440D3, 0xE36130E6,
    0x3FFE0000, 0x9E4CAD23, 0xDD5F3A20, 0x3FFD0000, 0xF61CCE92, 0x346600BB, 0x3FFE0000, 0x9CC8E160, 0xC3FB19B9,
    0x3FFD0000, 0xFB091FD3, 0x8145630A, 0x3FFE0000, 0x9B4C6F9E, 0xF03A3CAA, 0x3FFD0000, 0xFFE97042, 0xBFA4C2AD,
    0x3FFE0000, 0x99D722DA, 0xBDE58F06, 0x3FFE0000, 0x825EFCED, 0x49369330, 0x3FFE0000, 0x9868C809, 0x868C8098,
    0x3FFE0000, 0x84C37A7A, 0xB9A905C9, 0x3FFE0000, 0x97012E02, 0x5C04B809, 0x3FFE0000, 0x87224C2E, 0x8E645FB7,
    0x3FFE0000, 0x95A02568, 0x095A0257, 0x3FFE0000, 0x897B8CAC, 0x9F7DE298, 0x3FFE0000, 0x94458094, 0x45809446,
    0x3FFE0000, 0x8BCF55DE, 0xC4CD05FE, 0x3FFE0000, 0x92F11384, 0x0497889C, 0x3FFE0000, 0x8E1DC0FB, 0x89E125E5,
    0x3FFE0000, 0x91A2B3C4, 0xD5E6F809, 0x3FFE0000, 0x9066E68C, 0x955B6C9B, 0x3FFE0000, 0x905A3863, 0x3E06C43B,
    0x3FFE0000, 0x92AADE74, 0xC7BE59E0, 0x3FFE0000, 0x8F1779D9, 0xFDC3A219, 0x3FFE0000, 0x94E9BFF6, 0x15845643,
    0x3FFE0000, 0x8DDA5202, 0x37694809, 0x3FFE0000, 0x9723A1B7, 0x20134203, 0x3FFE0000, 0x8CA29C04, 0x6514E023,
    0x3FFE0000, 0x995899C8, 0x90EB8990, 0x3FFE0000, 0x8B70344A, 0x139BC75A, 0x3FFE0000, 0x9B88BDAA, 0x3A3DAE2F,
    0x3FFE0000, 0x8A42F870, 0x5669DB46, 0x3FFE0000, 0x9DB4224F, 0xFFE1157C, 0x3FFE0000, 0x891AC73A, 0xE9819B50,
    0x3FFE0000, 0x9FDADC26, 0x8B7A12DA, 0x3FFE0000, 0x87F78087, 0xF78087F8, 0x3FFE0000, 0xA1FCFF17, 0xCE733BD4,
    0x3FFE0000, 0x86D90544, 0x7A34ACC6, 0x3FFE0000, 0xA41A9E8F, 0x5446FB9F, 0x3FFE0000, 0x85BF3761, 0x2CEE3C9B,
    0x3FFE0000, 0xA633CD7E, 0x6771CD8B, 0x3FFE0000, 0x84A9F9C8, 0x084A9F9D, 0x3FFE0000, 0xA8489E60, 0x0B435A5E,
    0x3FFE0000, 0x83993052, 0x3FBE3368, 0x3FFE0000, 0xAA59233C, 0xCCA4BD49, 0x3FFE0000, 0x828CBFBE, 0xB9A020A3,
    0x3FFE0000, 0xAC656DAE, 0x6BCC4985, 0x3FFE0000, 0x81848DA8, 0xFAF0D277, 0x3FFE0000, 0xAE6D8EE3, 0x60BB2468,
    0x3FFE0000, 0x80808080, 0x80808081, 0x3FFE0000, 0xB07197A2, 0x3C46C654,
};

// Fetch 1/F for LOGTBL entry i (0..63)
static fpu_unpacked_t logtbl_inv_f(int i) {
    const uint32_t *p = &logtbl_data[i * 6];
    return fpsp_ext(p[0], p[1], p[2]);
}

// Fetch log(F) for LOGTBL entry i (0..63)
static fpu_unpacked_t logtbl_log_f(int i) {
    const uint32_t *p = &logtbl_data[i * 6 + 3];
    return fpsp_ext(p[0], p[1], p[2]);
}

// ============================================================================
// FLOGN — natural logarithm (opcode 0x14)
// ============================================================================
// Algorithm from Motorola FPSP.
//
// LOGMAIN (general case, |X-1| >= 1/16):
//   K = biased_exp - 16383 + ADJK
//   Y = mantissa with exponent = 0 (i.e. 1 <= Y < 2)
//   F = 1.xxxxxx1 (first 7 bits of Y + round bit)
//   U = (Y - F) * (1/F) via table lookup of 1/F
//   V = U * U
//   poly = U + V*(A1+V*(A3+V*A5)) + U*V*(A2+V*(A4+V*A6))
//   result = K*ln(2) + log(F) + poly
//
// LOGNEAR1 (|X-1| < 1/16):
//   U = 2(X-1)/(X+1)
//   V = U*U, W = V*V
//   result = U + U*V*([B1+W*(B3+W*B5)] + [V*(B2+W*B4)])

fpu_unpacked_t fpu_op_logn(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // NaN: propagate, converting SNaN to QNaN
    if (src.exponent == FPU_EXP_INF && src.mantissa_hi != 0) {
        if (!(src.mantissa_hi & 0x4000000000000000ULL))
            fpu->fpsr |= FPEXC_SNAN;
        src.mantissa_hi |= 0x4000000000000000ULL;
        return src;
    }

    // Zero: divide by zero → -infinity
    if (src.exponent == FPU_EXP_ZERO) {
        fpu->fpsr |= FPEXC_DZ;
        return (fpu_unpacked_t){true, FPU_EXP_INF, 0, 0};
    }

    // Negative: operand error → NaN
    if (src.sign) {
        fpu->fpsr |= FPEXC_OPERR;
        return (fpu_unpacked_t){false, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
    }

    // +Infinity: return +infinity
    if (src.exponent == FPU_EXP_INF)
        return src;

    // Normalize unnormalized extended inputs (J-bit clear, non-zero exponent).
    // The 68882 normalizes these before computing transcendentals.
    uint16_t biased_exp = FP80_EXP(raw);
    if (biased_exp != 0 && biased_exp != 0x7FFF && !(src.mantissa_hi & 0x8000000000000000ULL)) {
        fpu_normalize(&src);
        // Recompute biased exponent from the adjusted unpacked exponent
        biased_exp = (uint16_t)(src.exponent + 16383);
    }

    // Build compact form: (biased_exp << 16) | (upper 16 of mantissa)
    uint32_t compact = ((uint32_t)biased_exp << 16) | (uint32_t)(src.mantissa_hi >> 48);

    // Save FPCR and clear to 0 (round-to-nearest) for intermediate ops.
    // The FPSP does this to avoid rounding artifacts (e.g. -0 from X-1=0).
    // Final rounding is applied by fpu_pack in the caller.
    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    fpu_unpacked_t result;

    // Check for near-1 case: BOUNDS1 = [0x3FFEF07D, 0x3FFF8841]
    if (compact >= BOUNDS1_LO && compact <= BOUNDS1_HI && biased_exp != 0) {
        // ---- LOGNEAR1 path ----
        // U = 2*(X-1)/(X+1), polynomial in U
        fpu_unpacked_t one = {false, 0, 0x8000000000000000ULL, 0};
        fpu_unpacked_t fp0 = src; // X
        fpu_unpacked_t fp1 = src; // X

        fp1 = fp_rnd64(fpu_op_sub(fpu, fp1, one)); // X - 1
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, one)); // X + 1
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, fp1)); // 2*(X-1)
        fp1 = fp_rnd64(fpu_op_div(fpu, fp1, fp0)); // U = 2*(X-1)/(X+1)

        // V = U*U
        fp0 = fp_rnd64(fpu_op_mul(fpu, fp1, fp1));
        fpu_unpacked_t saveu = fp1;

        // W = V*V
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp0, fp0));

        // Polynomial: [B1+W*(B3+W*B5)] + [V*(B2+W*B4)]
        fpu_unpacked_t LOGB5 = fpsp_dbl(0x3F175496, 0xADD7DAD6);
        fpu_unpacked_t LOGB4 = fpsp_dbl(0x3F3C71C2, 0xFE80C7E0);
        fpu_unpacked_t LOGB3 = fpsp_dbl(0x3F624924, 0x928BCCFF);
        fpu_unpacked_t LOGB2 = fpsp_dbl(0x3F899999, 0x999995EC);
        fpu_unpacked_t LOGB1 = fpsp_dbl(0x3FB55555, 0x55555555);

        fpu_unpacked_t fp3 = fp_rnd64(fpu_op_mul(fpu, fp1, LOGB5)); // W*B5
        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, fp1, LOGB4)); // W*B4
        fp3 = fp_rnd64(fpu_op_add(fpu, fp3, LOGB3)); // B3+W*B5
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, LOGB2)); // B2+W*B4
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp1, fp3)); // W*(B3+W*B5)
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp0, fp2)); // V*(B2+W*B4)
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, LOGB1)); // B1+W*(B3+W*B5)
        fp0 = fp_rnd64(fpu_op_mul(fpu, saveu, fp0)); // U*V
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, fp2)); // full poly coefficient
        fp0 = fp_rnd64(fpu_op_mul(fpu, fp0, fp1)); // U*V * poly
        result = fpu_op_add(fpu, saveu, fp0); // U + U*V*poly
    } else {
        // ---- LOGMAIN path ----
        // K = biased_exp - 0x3FFF + adjk
        // For normals: adjk = 0. For denormals: FPSP uses biased_exp=0,
        // and fpu_unpack already normalized the mantissa and adjusted exponent.
        // K(FPSP) = biased_exp - 16383 + adjk. With our unpacked exponent:
        //   normals:  k = src.exponent  (same as biased - 16383)
        //   denormals: k = src.exponent - 1  (FPSP uses 0-16383, not 1-16383)
        int32_t k;
        if (biased_exp == 0)
            k = src.exponent - 1; // FPSP convention for denormals
        else
            k = src.exponent;

        // Y = mantissa with exponent = 0 (biased 0x3FFF → unbiased 0)
        fpu_unpacked_t y = {false, 0, src.mantissa_hi, src.mantissa_lo};

        // F = first 7 bits of Y + bit at position 8
        // FPSP: FFRAC = (XFRAC & 0xFE000000) | 0x01000000
        uint32_t xfrac_hi = (uint32_t)(src.mantissa_hi >> 32);
        uint32_t ffrac_hi = (xfrac_hi & 0xFE000000) | 0x01000000;
        fpu_unpacked_t f = {false, 0, (uint64_t)ffrac_hi << 32, 0};

        // Table index: bits 30:25 of ffrac_hi (6 index bits after J-bit)
        int tbl_idx = (ffrac_hi >> 25) & 0x3F;
        fpu_unpacked_t inv_f = logtbl_inv_f(tbl_idx);
        fpu_unpacked_t log_f = logtbl_log_f(tbl_idx);

        // fp0 = Y - F
        fpu_unpacked_t fp0 = fp_rnd64(fpu_op_sub(fpu, y, f));

        // fp1 = K as floating-point
        fpu_unpacked_t fp1;
        if (k == 0) {
            fp1 = (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
        } else {
            fp1.sign = (k < 0);
            int32_t abs_k = k < 0 ? -k : k;
            fp1.mantissa_hi = (uint64_t)abs_k;
            fp1.mantissa_lo = 0;
            fp1.exponent = 63;
            fpu_normalize(&fp1);
        }

        // U = (Y-F) * (1/F) — table lookup avoids division
        fp0 = fp_rnd64(fpu_op_mul(fpu, fp0, inv_f));

        // K * log(2)
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp1, LOGOF2));

        // V = U*U
        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, fp0, fp0));
        fpu_unpacked_t klog2 = fp1; // save K*log(2) (already truncated)

        // Polynomial: U + V*(A1+V*(A3+V*A5)) + U*V*(A2+V*(A4+V*A6))
        // Evaluated as two halves for pipeline efficiency.
        fpu_unpacked_t LOGA6 = fpsp_dbl(0x3FC2499A, 0xB5E4040B);
        fpu_unpacked_t LOGA5 = fpsp_dbl(0xBFC555B5, 0x848CB7DB);
        fpu_unpacked_t LOGA4 = fpsp_dbl(0x3FC99999, 0x987D8730);
        fpu_unpacked_t LOGA3 = fpsp_dbl(0xBFCFFFFF, 0xFF6F7E97);
        fpu_unpacked_t LOGA2 = fpsp_dbl(0x3FD55555, 0x555555A4);
        fpu_unpacked_t LOGA1 = fpsp_dbl(0xBFE00000, 0x00000008);

        fpu_unpacked_t fp3 = fp2; // V copy

        fp1 = fp_rnd64(fpu_op_mul(fpu, fp2, LOGA6)); // V*A6
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp2, LOGA5)); // V*A5
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, LOGA4)); // A4+V*A6
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, LOGA3)); // A3+V*A5
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp3, fp1)); // V*(A4+V*A6)
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp3, fp2)); // V*(A3+V*A5)
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, LOGA2)); // A2+V*(A4+V*A6)
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, LOGA1)); // A1+V*(A3+V*A5)
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp3, fp1)); // V*(A2+V*(A4+V*A6))
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp3, fp2)); // V*(A1+V*(A3+V*A5))

        fp1 = fp_rnd64(fpu_op_mul(fpu, fp0, fp1)); // U*V*(A2+V*(A4+V*A6)))
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp2)); // U+V*(A1+V*(A3+V*A5))

        // log(F) + U*V*(A2+V*(A4+V*A6))
        fp1 = fp_rnd64(fpu_op_add(fpu, log_f, fp1));

        // [U+V*(...)] + [log(F)+U*V*(...)]
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp1));

        // Final: K*log(2) + log(F) + log(1+u)
        result = fpu_op_add(fpu, klog2, fp0);
    }

    // Restore FPCR
    fpu->fpcr = saved_fpcr;

    // Set inexact (all log results of normal inputs)
    fpu->fpsr |= FPEXC_INEX2;

    return result;
}

// ============================================================================
// FLOG10: base-10 logarithm (opcode 0x15)
// ============================================================================
// Algorithm from Motorola FPSP: log10(X) = ln(X) * (1/ln(10))

// INV_L10 = 1/ln(10) in extended precision
static const uint32_t INV_L10_DATA[3] = {0x3FFD0000, 0xDE5BD8A9, 0x37287195};

fpu_unpacked_t fpu_op_log10(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // Compute Y = ln(X) via fpu_op_logn (handles all special cases)
    fpu_unpacked_t y = fpu_op_logn(fpu, src, raw);

    // If Y is NaN, Inf, or zero, return as-is (no multiply)
    if (y.exponent == FPU_EXP_INF || y.exponent == FPU_EXP_ZERO)
        return y;

    // Round Y to 64-bit (matches 68882 storing to fp0 register)
    y = fp_rnd64(y);

    // log10(X) = Y * INV_L10, with user FPCR active for final rounding
    fpu_unpacked_t inv_l10 = fpsp_ext(INV_L10_DATA[0], INV_L10_DATA[1], INV_L10_DATA[2]);
    return fpu_op_mul(fpu, y, inv_l10);
}

// ============================================================================
// FLOG2: base-2 logarithm (opcode 0x16)
// ============================================================================
// Algorithm from Motorola FPSP: log2(X) = ln(X) * (1/ln(2))
// Shortcut: if X is an exact power of 2, return the integer exponent.

// INV_L2 = 1/ln(2) in extended precision
static const uint32_t INV_L2_DATA[3] = {0x3FFF0000, 0xB8AA3B29, 0x5C17F0BC};

fpu_unpacked_t fpu_op_log2(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // Power-of-2 shortcut: if mantissa is exactly 0x8000000000000000 and
    // biased exponent is normal, return (biased_exp - 16383) as integer
    uint16_t biased_exp = FP80_EXP(raw);
    if (biased_exp != 0 && biased_exp != 0x7FFF && raw.mantissa == 0x8000000000000000ULL) {
        int32_t k = (int32_t)biased_exp - 0x3FFF;
        fpu_unpacked_t result;
        if (k == 0) {
            result = (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
        } else {
            result.sign = (k < 0);
            int32_t abs_k = k < 0 ? -k : k;
            result.mantissa_hi = (uint64_t)abs_k;
            result.mantissa_lo = 0;
            result.exponent = 63;
            fpu_normalize(&result);
        }
        // FPSP sets INEX2 even for exact power-of-2 results
        fpu->fpsr |= FPEXC_INEX2;
        return result;
    }

    // Compute Y = ln(X) via fpu_op_logn (handles all special cases)
    fpu_unpacked_t y = fpu_op_logn(fpu, src, raw);

    // If Y is NaN, Inf, or zero, return as-is (no multiply)
    if (y.exponent == FPU_EXP_INF || y.exponent == FPU_EXP_ZERO)
        return y;

    // Round Y to 64-bit (matches 68882 storing to fp0 register)
    y = fp_rnd64(y);

    // log2(X) = Y * INV_L2, with user FPCR active for final rounding
    fpu_unpacked_t inv_l2 = fpsp_ext(INV_L2_DATA[0], INV_L2_DATA[1], INV_L2_DATA[2]);
    return fpu_op_mul(fpu, y, inv_l2);
}

// ============================================================================
// FLOGNP1 — log(1+X) (opcode 0x06)
// ============================================================================
// Algorithm from Motorola FPSP.
//
// Paths:
//   TINY:     |X| <= 2^(-102)       → return X (first-order approximation)
//   LOGMAIN:  1+X outside [1/2,3/2] → standard table-driven log on round(1+X)
//   LP1ONE16: 1+X in BOUNDS1        → U=2X/(round(1+X)+1), odd polynomial
//   LP1CARE:  1+X in [1/2,3/2]\BOUNDS1 → careful (Y-F) preserving X's bits

fpu_unpacked_t fpu_op_lognp1(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // NaN: propagate, converting SNaN to QNaN
    if (src.exponent == FPU_EXP_INF && src.mantissa_hi != 0) {
        if (!(src.mantissa_hi & 0x4000000000000000ULL))
            fpu->fpsr |= FPEXC_SNAN;
        src.mantissa_hi |= 0x4000000000000000ULL;
        return src;
    }

    // -Infinity: log(1+(-inf)) → OPERR
    if (src.exponent == FPU_EXP_INF && src.sign) {
        fpu->fpsr |= FPEXC_OPERR;
        return (fpu_unpacked_t){false, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
    }

    // +Infinity: log(1+inf) = +inf
    if (src.exponent == FPU_EXP_INF)
        return src;

    // Normalize unnormalized extended inputs (J-bit clear, non-zero exponent)
    uint16_t biased_exp = FP80_EXP(raw);
    if (biased_exp != 0 && biased_exp != 0x7FFF && !(src.mantissa_hi & 0x8000000000000000ULL)) {
        fpu_normalize(&src);
    }

    // Tiny: |X| <= LTHOLD (2^-102) → log(1+X) ≈ X with INEX2
    // Covers zero (FPU_EXP_ZERO = -32768) and denormals (very negative exponent)
    if (src.exponent < -102) {
        fpu->fpsr |= FPEXC_INEX2;
        return src;
    }

    // Save FPCR, clear for intermediate ops (round-to-nearest, extended)
    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    fpu_unpacked_t one = {false, 0, 0x8000000000000000ULL, 0};
    fpu_unpacked_t z = src; // Z = input X

    // Y = round(1+Z)
    fpu_unpacked_t y = fp_rnd64(fpu_op_add(fpu, z, one));

    // Y <= 0: log(1+X) undefined
    if (y.exponent == FPU_EXP_ZERO) {
        // 1+Z = 0 → log(0) = -Inf, divide by zero
        fpu->fpcr = saved_fpcr;
        fpu->fpsr |= FPEXC_DZ;
        return (fpu_unpacked_t){true, FPU_EXP_INF, 0, 0};
    }
    if (y.sign) {
        // 1+Z < 0 → OPERR
        fpu->fpcr = saved_fpcr;
        fpu->fpsr |= FPEXC_OPERR;
        return (fpu_unpacked_t){false, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
    }

    // Build compact form of Y = round(1+Z)
    uint16_t y_biased = (uint16_t)(y.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)y_biased << 16) | (uint32_t)(y.mantissa_hi >> 48);

    fpu_unpacked_t result;

    if (compact < BOUNDS2_LO || compact > BOUNDS2_HI) {
        // ---- LOGMAIN path on Y (Y outside [1/2, 3/2]) ----
        // Y = round(1+Z) has enough precision for standard log
        int32_t k = y.exponent;

        // Y with exponent normalized to 0 (mantissa in [1,2))
        fpu_unpacked_t yn = {false, 0, y.mantissa_hi, y.mantissa_lo};

        // F = first 7 bits of mantissa + round bit at position 8
        uint32_t xfrac_hi = (uint32_t)(y.mantissa_hi >> 32);
        uint32_t ffrac_hi = (xfrac_hi & 0xFE000000) | 0x01000000;
        fpu_unpacked_t f = {false, 0, (uint64_t)ffrac_hi << 32, 0};

        // Table index: 6-bit index from bits 30:25 of ffrac
        int tbl_idx = (ffrac_hi >> 25) & 0x3F;
        fpu_unpacked_t inv_f = logtbl_inv_f(tbl_idx);
        fpu_unpacked_t log_f = logtbl_log_f(tbl_idx);

        // U = (Y_norm - F) * (1/F)
        fpu_unpacked_t fp0 = fp_rnd64(fpu_op_sub(fpu, yn, f));

        // K as floating-point
        fpu_unpacked_t fp1;
        if (k == 0) {
            fp1 = (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
        } else {
            fp1.sign = (k < 0);
            int32_t abs_k = k < 0 ? -k : k;
            fp1.mantissa_hi = (uint64_t)abs_k;
            fp1.mantissa_lo = 0;
            fp1.exponent = 63;
            fpu_normalize(&fp1);
        }

        fp0 = fp_rnd64(fpu_op_mul(fpu, fp0, inv_f)); // U
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp1, LOGOF2)); // K*log(2)
        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, fp0, fp0)); // V = U*U
        fpu_unpacked_t klog2 = fp1;

        // Polynomial coefficients (IEEE double format)
        fpu_unpacked_t LOGA6 = fpsp_dbl(0x3FC2499A, 0xB5E4040B);
        fpu_unpacked_t LOGA5 = fpsp_dbl(0xBFC555B5, 0x848CB7DB);
        fpu_unpacked_t LOGA4 = fpsp_dbl(0x3FC99999, 0x987D8730);
        fpu_unpacked_t LOGA3 = fpsp_dbl(0xBFCFFFFF, 0xFF6F7E97);
        fpu_unpacked_t LOGA2 = fpsp_dbl(0x3FD55555, 0x555555A4);
        fpu_unpacked_t LOGA1 = fpsp_dbl(0xBFE00000, 0x00000008);

        fpu_unpacked_t fp3 = fp2; // V copy
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp2, LOGA6));
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp2, LOGA5));
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, LOGA4));
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, LOGA3));
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp3, fp1));
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp3, fp2));
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, LOGA2));
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, LOGA1));
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp3, fp1));
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp3, fp2));
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp0, fp1));
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp2));
        fp1 = fp_rnd64(fpu_op_add(fpu, log_f, fp1));
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp1));
        result = fpu_op_add(fpu, klog2, fp0);

    } else if (compact >= BOUNDS1_LO && compact <= BOUNDS1_HI) {
        // ---- LP1ONE16 path (Y near 1, within BOUNDS1) ----
        // U = 2Z / (Y + 1), then odd polynomial in U
        fpu_unpacked_t fp0 = fp_rnd64(fpu_op_add(fpu, y, one)); // Y + 1
        fpu_unpacked_t fp1 = fp_rnd64(fpu_op_add(fpu, z, z)); // 2Z
        fp1 = fp_rnd64(fpu_op_div(fpu, fp1, fp0)); // U = 2Z/(Y+1)

        // V = U*U, W = V*V
        fp0 = fp_rnd64(fpu_op_mul(fpu, fp1, fp1)); // V
        fpu_unpacked_t saveu = fp1;
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp0, fp0)); // W

        // Polynomial coefficients
        fpu_unpacked_t LOGB5 = fpsp_dbl(0x3F175496, 0xADD7DAD6);
        fpu_unpacked_t LOGB4 = fpsp_dbl(0x3F3C71C2, 0xFE80C7E0);
        fpu_unpacked_t LOGB3 = fpsp_dbl(0x3F624924, 0x928BCCFF);
        fpu_unpacked_t LOGB2 = fpsp_dbl(0x3F899999, 0x999995EC);
        fpu_unpacked_t LOGB1 = fpsp_dbl(0x3FB55555, 0x55555555);

        // [B1+W*(B3+W*B5)] + [V*(B2+W*B4)]
        fpu_unpacked_t fp3 = fp_rnd64(fpu_op_mul(fpu, fp1, LOGB5));
        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, fp1, LOGB4));
        fp3 = fp_rnd64(fpu_op_add(fpu, fp3, LOGB3));
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, LOGB2));
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp1, fp3)); // W*(B3+W*B5)
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp0, fp2)); // V*(B2+W*B4)
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, LOGB1)); // B1+W*(B3+W*B5)
        fp0 = fp_rnd64(fpu_op_mul(fpu, saveu, fp0)); // U*V
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, fp2)); // full poly coeff
        fp0 = fp_rnd64(fpu_op_mul(fpu, fp0, fp1)); // U*V*poly
        result = fpu_op_add(fpu, saveu, fp0); // U + U*V*poly

    } else {
        // ---- LP1CARE path (Y in [1/2,3/2] but not BOUNDS1) ----
        // Careful Y-F computation to preserve Z's low bits.
        // F = first 7 mantissa bits of Y + round bit, with exponent 0
        uint32_t xfrac_hi = (uint32_t)(y.mantissa_hi >> 32);
        uint32_t ffrac_hi = (xfrac_hi & 0xFE000000) | 0x01000000;
        fpu_unpacked_t f = {false, 0, (uint64_t)ffrac_hi << 32, 0};

        int tbl_idx = (ffrac_hi >> 25) & 0x3F;
        fpu_unpacked_t inv_f = logtbl_inv_f(tbl_idx);
        fpu_unpacked_t log_f = logtbl_log_f(tbl_idx);

        fpu_unpacked_t fp0, fp1;
        int32_t k;

        if (compact >= 0x3FFF8000u) {
            // KISZERO: Y >= 1.0, K = 0
            // Y-F = (1-F) + Z — preserves Z's full precision
            k = 0;
            fp0 = fp_rnd64(fpu_op_sub(fpu, one, f)); // 1 - F (exact)
            fp0 = fp_rnd64(fpu_op_add(fpu, fp0, z)); // (1-F) + Z
        } else {
            // KISNEG1: Y < 1.0, K = -1
            // M-F = (2-F) + 2Z where M = 2*Y (normalized mantissa)
            k = -1;
            fpu_unpacked_t two = {false, 1, 0x8000000000000000ULL, 0};
            fp0 = fp_rnd64(fpu_op_sub(fpu, two, f)); // 2 - F (exact)
            fpu_unpacked_t twoz = fp_rnd64(fpu_op_add(fpu, z, z)); // 2Z
            fp0 = fp_rnd64(fpu_op_add(fpu, fp0, twoz)); // (2-F) + 2Z
        }

        // K as floating-point
        if (k == 0) {
            fp1 = (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
        } else {
            fp1.sign = (k < 0);
            int32_t abs_k = k < 0 ? -k : k;
            fp1.mantissa_hi = (uint64_t)abs_k;
            fp1.mantissa_lo = 0;
            fp1.exponent = 63;
            fpu_normalize(&fp1);
        }

        // Continue with LOGMAIN polynomial (LP1CONT1)
        fp0 = fp_rnd64(fpu_op_mul(fpu, fp0, inv_f)); // U = (Y-F)*(1/F)
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp1, LOGOF2)); // K*log(2)
        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, fp0, fp0)); // V = U*U
        fpu_unpacked_t klog2 = fp1;

        fpu_unpacked_t LOGA6 = fpsp_dbl(0x3FC2499A, 0xB5E4040B);
        fpu_unpacked_t LOGA5 = fpsp_dbl(0xBFC555B5, 0x848CB7DB);
        fpu_unpacked_t LOGA4 = fpsp_dbl(0x3FC99999, 0x987D8730);
        fpu_unpacked_t LOGA3 = fpsp_dbl(0xBFCFFFFF, 0xFF6F7E97);
        fpu_unpacked_t LOGA2 = fpsp_dbl(0x3FD55555, 0x555555A4);
        fpu_unpacked_t LOGA1 = fpsp_dbl(0xBFE00000, 0x00000008);

        fpu_unpacked_t fp3 = fp2;
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp2, LOGA6));
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp2, LOGA5));
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, LOGA4));
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, LOGA3));
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp3, fp1));
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp3, fp2));
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, LOGA2));
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, LOGA1));
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp3, fp1));
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp3, fp2));
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp0, fp1));
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp2));
        fp1 = fp_rnd64(fpu_op_add(fpu, log_f, fp1));
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp1));
        result = fpu_op_add(fpu, klog2, fp0);
    }

    // Restore FPCR, set inexact
    fpu->fpcr = saved_fpcr;
    fpu->fpsr |= FPEXC_INEX2;
    return result;
}

// ============================================================================
// EXPTBL: 64 entries for 2^(J/64), each has T (extended) + t (single tail)
// ============================================================================
// T + t ≈ 2^(J/64) to ~85 bits. T is rounded to 62 mantissa bits.
// Format: { sign+biased_exp<<16, mant_hi, mant_lo, t_single }
static const uint32_t exptbl_data[64 * 4] = {
    0x3FFF0000, 0x80000000, 0x00000000, 0x00000000, 0x3FFF0000, 0x8164D1F3, 0xBC030774, 0x9F841A9B, 0x3FFF0000,
    0x82CD8698, 0xAC2BA1D8, 0x9FC1D5B9, 0x3FFF0000, 0x843A28C3, 0xACDE4048, 0xA0728369, 0x3FFF0000, 0x85AAC367,
    0xCC487B14, 0x1FC5C95C, 0x3FFF0000, 0x871F6196, 0x9E8D1010, 0x1EE85C9F, 0x3FFF0000, 0x88980E80, 0x92DA8528,
    0x9FA20729, 0x3FFF0000, 0x8A14D575, 0x496EFD9C, 0xA07BF9AF, 0x3FFF0000, 0x8B95C1E3, 0xEA8BD6E8, 0xA0020DCF,
    0x3FFF0000, 0x8D1ADF5B, 0x7E5BA9E4, 0x205A63DA, 0x3FFF0000, 0x8EA4398B, 0x45CD53C0, 0x1EB70051, 0x3FFF0000,
    0x9031DC43, 0x1466B1DC, 0x1F6EB029, 0x3FFF0000, 0x91C3D373, 0xAB11C338, 0xA0781494, 0x3FFF0000, 0x935A2B2F,
    0x13E6E92C, 0x9EB319B0, 0x3FFF0000, 0x94F4EFA8, 0xFEF70960, 0x2017457D, 0x3FFF0000, 0x96942D37, 0x20185A00,
    0x1F11D537, 0x3FFF0000, 0x9837F051, 0x8DB8A970, 0x9FB952DD, 0x3FFF0000, 0x99E04593, 0x20B7FA64, 0x1FE43087,
    0x3FFF0000, 0x9B8D39B9, 0xD54E5538, 0x1FA2A818, 0x3FFF0000, 0x9D3ED9A7, 0x2CFFB750, 0x1FDE494D, 0x3FFF0000,
    0x9EF53260, 0x91A111AC, 0x20504890, 0x3FFF0000, 0xA0B0510F, 0xB9714FC4, 0xA073691C, 0x3FFF0000, 0xA2704303,
    0x0C496818, 0x1F9B7A05, 0x3FFF0000, 0xA43515AE, 0x09E680A0, 0xA0797126, 0x3FFF0000, 0xA5FED6A9, 0xB15138EC,
    0xA071A140, 0x3FFF0000, 0xA7CD93B4, 0xE9653568, 0x204F62DA, 0x3FFF0000, 0xA9A15AB4, 0xEA7C0EF8, 0x1F283C4A,
    0x3FFF0000, 0xAB7A39B5, 0xA93ED338, 0x9F9A7FDC, 0x3FFF0000, 0xAD583EEA, 0x42A14AC8, 0xA05B3FAC, 0x3FFF0000,
    0xAF3B78AD, 0x690A4374, 0x1FDF2610, 0x3FFF0000, 0xB123F581, 0xD2AC2590, 0x9F705F90, 0x3FFF0000, 0xB311C412,
    0xA9112488, 0x201F678A, 0x3FFF0000, 0xB504F333, 0xF9DE6484, 0x1F32FB13, 0x3FFF0000, 0xB6FD91E3, 0x28D17790,
    0x20038B30, 0x3FFF0000, 0xB8FBAF47, 0x62FB9EE8, 0x200DC3CC, 0x3FFF0000, 0xBAFF5AB2, 0x133E45FC, 0x9F8B2AE6,
    0x3FFF0000, 0xBD08A39F, 0x580C36C0, 0xA02BBF70, 0x3FFF0000, 0xBF1799B6, 0x7A731084, 0xA00BF518, 0x3FFF0000,
    0xC12C4CCA, 0x66709458, 0xA041DD41, 0x3FFF0000, 0xC346CCDA, 0x24976408, 0x9FDF137B, 0x3FFF0000, 0xC5672A11,
    0x5506DADC, 0x201F1568, 0x3FFF0000, 0xC78D74C8, 0xABB9B15C, 0x1FC13A2E, 0x3FFF0000, 0xC9B9BD86, 0x6E2F27A4,
    0xA03F8F03, 0x3FFF0000, 0xCBEC14FE, 0xF2727C5C, 0x1FF4907D, 0x3FFF0000, 0xCE248C15, 0x1F8480E4, 0x9E6E53E4,
    0x3FFF0000, 0xD06333DA, 0xEF2B2594, 0x1FD6D45C, 0x3FFF0000, 0xD2A81D91, 0xF12AE45C, 0xA076EDB9, 0x3FFF0000,
    0xD4F35AAB, 0xCFEDFA20, 0x9FA6DE21, 0x3FFF0000, 0xD744FCCA, 0xD69D6AF4, 0x1EE69A2F, 0x3FFF0000, 0xD99D15C2,
    0x78AFD7B4, 0x207F439F, 0x3FFF0000, 0xDBFBB797, 0xDAF23754, 0x201EC207, 0x3FFF0000, 0xDE60F482, 0x5E0E9124,
    0x9E8BE175, 0x3FFF0000, 0xE0CCDEEC, 0x2A94E110, 0x20032C4B, 0x3FFF0000, 0xE33F8972, 0xBE8A5A50, 0x2004DFF5,
    0x3FFF0000, 0xE5B906E7, 0x7C8348A8, 0x1E72F47A, 0x3FFF0000, 0xE8396A50, 0x3C4BDC68, 0x1F722F22, 0x3FFF0000,
    0xEAC0C6E7, 0xDD243930, 0xA017E945, 0x3FFF0000, 0xED4F301E, 0xD9942B84, 0x1F401A5B, 0x3FFF0000, 0xEFE4B99B,
    0xDCDAF5CC, 0x9FB9A9E3, 0x3FFF0000, 0xF281773C, 0x59FFB138, 0x20744C05, 0x3FFF0000, 0xF5257D15, 0x2486CC2C,
    0x1F773A19, 0x3FFF0000, 0xF7D0DF73, 0x0AD13BB8, 0x1FFE90D5, 0x3FFF0000, 0xFA83B2DB, 0x722A033C, 0xA041ED22,
    0x3FFF0000, 0xFD3E0C0C, 0xF486C174, 0x1F853F3A,
};

// Fetch T (extended lead value) for EXPTBL entry j (0..63)
static fpu_unpacked_t exptbl_T(int j) {
    const uint32_t *p = &exptbl_data[j * 4];
    return fpsp_ext(p[0], p[1], p[2]);
}

// Fetch t (single-precision tail correction) for EXPTBL entry j (0..63)
static fpu_unpacked_t exptbl_t(int j) {
    return fpsp_sgl(exptbl_data[j * 4 + 3]);
}

// Convert fpu_unpacked_t to int32 with round-to-nearest-even
// Used for fmovel fp0,d0 equivalent in FETOX argument reduction.
static int32_t fpu_to_int32_rne(fpu_unpacked_t v) {
    if (v.exponent == FPU_EXP_ZERO || v.exponent == FPU_EXP_INF)
        return 0;
    if (v.exponent < -1)
        return 0; // |v| < 0.5
    if (v.exponent == -1) {
        // |v| in [0.5, 1): round to nearest even
        // 0.5 exactly → round to 0 (even)
        if (v.mantissa_hi > 0x8000000000000000ULL || (v.mantissa_hi == 0x8000000000000000ULL && v.mantissa_lo != 0))
            return v.sign ? -1 : 1;
        return 0;
    }
    if (v.exponent > 30)
        return v.sign ? (int32_t)0x80000000 : 0x7FFFFFFF;

    // shift = bits of mantissa_hi that are fractional
    int shift = 63 - v.exponent;
    int64_t n = (int64_t)(v.mantissa_hi >> shift);

    if (shift > 0) {
        uint64_t frac_mask = (1ULL << shift) - 1;
        uint64_t frac = v.mantissa_hi & frac_mask;
        uint64_t half = 1ULL << (shift - 1);
        uint64_t round_bits = (shift > 1) ? (frac & (half - 1)) : 0;
        bool guard = (frac >> (shift - 1)) & 1;
        bool sticky = (round_bits != 0) || (v.mantissa_lo != 0);
        if (guard && (sticky || (n & 1)))
            n++;
    }
    return (int32_t)(v.sign ? -n : n);
}

// ============================================================================
// FETOX — e^X (opcode 0x10)
// ============================================================================
// Algorithm from Motorola FPSP.
//
// Normal path (2^-65 <= |X| < 16380*ln2):
//   N = round(X * 64/log2), J = N mod 64, M = N/64
//   R = X - N*log2/64 (Cody-Waite reduction with L1+L2)
//   p = exp(R)-1 via degree-5 polynomial
//   ans = T*(1+p) + t, where T+t = 2^(J/64) from EXPTBL
//   result = 2^M * ans

fpu_unpacked_t fpu_op_etox(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // NaN: propagate, converting SNaN to QNaN
    if (src.exponent == FPU_EXP_INF && src.mantissa_hi != 0) {
        if (!(src.mantissa_hi & 0x4000000000000000ULL))
            fpu->fpsr |= FPEXC_SNAN;
        src.mantissa_hi |= 0x4000000000000000ULL;
        return src;
    }

    // +Infinity: e^(+inf) = +inf
    if (src.exponent == FPU_EXP_INF && !src.sign)
        return src;

    // -Infinity: e^(-inf) = +0
    if (src.exponent == FPU_EXP_INF && src.sign)
        return (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};

    // Zero: e^0 = 1.0 (exact)
    if (src.exponent == FPU_EXP_ZERO) {
        return (fpu_unpacked_t){false, 0, 0x8000000000000000ULL, 0};
    }

    // Normalize unnormalized extended inputs
    uint16_t biased_exp = FP80_EXP(raw);
    if (biased_exp != 0 && biased_exp != 0x7FFF && !(src.mantissa_hi & 0x8000000000000000ULL)) {
        fpu_normalize(&src);
    }

    // Build compact form for range checks (sign stripped)
    uint16_t bexp = (uint16_t)(src.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)bexp << 16) | (uint32_t)(src.mantissa_hi >> 48);

    // Step 7: tiny — |X| < 2^(-65), e^X ≈ 1+X
    if (compact < 0x3FBE0000u) {
        fpu->fpsr |= FPEXC_INEX2;
        fpu_unpacked_t one = {false, 0, 0x8000000000000000ULL, 0};
        return fpu_op_add(fpu, one, src);
    }

    // Save FPCR, clear for intermediate ops
    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    // Step 9: extreme — |X| > 16480*log2, guaranteed overflow/underflow
    if (compact > 0x400CB27Cu) {
        fpu->fpcr = saved_fpcr;
        if (src.sign) {
            // e^(large negative) → underflow toward +0
            fpu->fpsr |= FPEXC_UNFL | FPEXC_INEX2;
            return (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
        } else {
            // e^(large positive) → overflow toward +inf
            fpu->fpsr |= FPEXC_OVFL | FPEXC_INEX2;
            return (fpu_unpacked_t){false, FPU_EXP_INF, 0, 0};
        }
    }

    // Steps 2-6: normal and large paths combined
    // For normal: 2^-65 <= |X| < 16380*log2
    // For large:  16380*log2 <= |X| <= 16480*log2
    // Both use same computation; we handle scale with int32 exponent

    // Step 2: N = round(X * 64/log2)
    fpu_unpacked_t inv_l2_64 = fpsp_sgl(0x42B8AA3B); // 64/log2 single
    fpu_unpacked_t product = fp_rnd64(fpu_op_mul(fpu, src, inv_l2_64));
    int32_t n = fpu_to_int32_rne(product);

    // J = N mod 64 (unsigned), M = N >> 6 (arithmetic)
    int j = ((unsigned)n) & 0x3F;
    int32_t m = n >> 6; // arithmetic right shift for negative N

    // Convert N back to floating-point
    fpu_unpacked_t fp_n;
    if (n == 0) {
        fp_n = (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
    } else {
        fp_n.sign = (n < 0);
        int32_t abs_n = n < 0 ? -n : n;
        fp_n.mantissa_hi = (uint64_t)abs_n;
        fp_n.mantissa_lo = 0;
        fp_n.exponent = 63;
        fpu_normalize(&fp_n);
    }

    // Step 3: R = X + N*L1 + N*L2 (Cody-Waite reduction)
    fpu_unpacked_t l1_val = fpsp_sgl(0xBC317218); // L1 = -log2/64 (single leading)
    fpu_unpacked_t l2_val = fpsp_ext(0x3FDC0000, 0x82E30865, 0x4361C4C6); // L2 tail

    fpu_unpacked_t fp0 = fp_rnd64(fpu_op_mul(fpu, fp_n, l1_val)); // N*L1
    fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, fp_n, l2_val)); // N*L2
    fp0 = fp_rnd64(fpu_op_add(fpu, src, fp0)); // X + N*L1
    fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp2)); // R = X + N*L1 + N*L2

    // Step 4: polynomial p = exp(R)-1
    // p = [R + R*S*(A2+S*A4)] + [S*(A1+S*(A3+S*A5))]  where S = R*R
    fpu_unpacked_t fp1 = fp_rnd64(fpu_op_mul(fpu, fp0, fp0)); // S = R*R

    fpu_unpacked_t expa5 = fpsp_sgl(0x3AB60B70);
    fpu_unpacked_t expa4 = fpsp_sgl(0x3C088895);
    fpu_unpacked_t expa3 = fpsp_dbl(0x3FA55555, 0x55554431);
    fpu_unpacked_t expa2 = fpsp_dbl(0x3FC55555, 0x55554018);
    fpu_unpacked_t expa1 = fpsp_sgl(0x3F000000); // 0.5

    fp2 = fp_rnd64(fpu_op_mul(fpu, fp1, expa5)); // S*A5
    fpu_unpacked_t fp3 = fp1; // fp3 = S
    fp3 = fp_rnd64(fpu_op_mul(fpu, fp3, expa4)); // S*A4

    fp2 = fp_rnd64(fpu_op_add(fpu, fp2, expa3)); // A3+S*A5
    fp3 = fp_rnd64(fpu_op_add(fpu, fp3, expa2)); // A2+S*A4

    fp2 = fp_rnd64(fpu_op_mul(fpu, fp1, fp2)); // S*(A3+S*A5)
    fp3 = fp_rnd64(fpu_op_mul(fpu, fp1, fp3)); // S*(A2+S*A4)

    fp2 = fp_rnd64(fpu_op_add(fpu, fp2, expa1)); // A1+S*(A3+S*A5)
    fp3 = fp_rnd64(fpu_op_mul(fpu, fp0, fp3)); // R*S*(A2+S*A4)

    fp2 = fp_rnd64(fpu_op_mul(fpu, fp1, fp2)); // S*(A1+S*(A3+S*A5))
    fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp3)); // R+R*S*(A2+S*A4)

    // Step 5: reconstruction — 2^(J/64) * exp(R)
    fpu_unpacked_t T_val = exptbl_T(j);
    fpu_unpacked_t t_val = exptbl_t(j);

    fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp2)); // p = exp(R)-1
    fp0 = fp_rnd64(fpu_op_mul(fpu, T_val, fp0)); // T*p
    fp0 = fp_rnd64(fpu_op_add(fpu, fp0, t_val)); // T*p + t
    fp0 = fp_rnd64(fpu_op_add(fpu, fp0, T_val)); // T + (T*p + t)

    // Step 6: scale by 2^M (add M to exponent)
    fpu_unpacked_t result = fp0;
    result.exponent += m;

    // Restore FPCR, set inexact
    fpu->fpcr = saved_fpcr;
    fpu->fpsr |= FPEXC_INEX2;
    return result;
}

// ============================================================================
// FETOXM1 — e^X - 1 (opcode 0x08)
// ============================================================================
// Algorithm from Motorola FPSP.
//
// For |X| < 2^(-65):  result = X (with inexact)
// For 2^(-65) <= |X| < 1/4:  degree-12 polynomial in S=X*X
// For 1/4 <= |X| <= 70*log2:  argument reduction + degree-6 poly + reconstruction
// For |X| > 70*log2, X>0:  use FETOX (exp(X)-1 ≈ exp(X))
// For |X| > 70*log2, X<0:  result ≈ -1

fpu_unpacked_t fpu_op_etoxm1(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // NaN: propagate, converting SNaN to QNaN
    if (src.exponent == FPU_EXP_INF && src.mantissa_hi != 0) {
        if (!(src.mantissa_hi & 0x4000000000000000ULL))
            fpu->fpsr |= FPEXC_SNAN;
        src.mantissa_hi |= 0x4000000000000000ULL;
        return src;
    }

    // +Infinity: e^(+inf)-1 = +inf
    if (src.exponent == FPU_EXP_INF && !src.sign)
        return src;

    // -Infinity: e^(-inf)-1 = -1
    if (src.exponent == FPU_EXP_INF && src.sign)
        return (fpu_unpacked_t){true, 0, 0x8000000000000000ULL, 0};

    // Zero: e^0 - 1 = 0 (exact, preserve sign)
    if (src.exponent == FPU_EXP_ZERO)
        return src;

    // Normalize unnormalized extended inputs
    uint16_t biased_exp = FP80_EXP(raw);
    if (biased_exp != 0 && biased_exp != 0x7FFF && !(src.mantissa_hi & 0x8000000000000000ULL)) {
        fpu_normalize(&src);
    }

    // Biased exponent for range checks
    uint16_t bexp = (uint16_t)(src.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)bexp << 16) | (uint32_t)(src.mantissa_hi >> 48);

    // Tiny: |X| < 2^(-65), exp(X)-1 ≈ X
    if (bexp < 0x3FBE) {
        fpu->fpsr |= FPEXC_INEX2;
        return src;
    }

    // Save FPCR, clear for intermediate ops
    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    // ---- EM1POLY: 2^(-65) <= |X| < 1/4, degree-12 polynomial ----
    if (bexp < 0x3FFD) {
        fpu_unpacked_t x = src; // save X
        fpu_unpacked_t s = fp_rnd64(fpu_op_mul(fpu, src, src)); // S = X*X

        // Polynomial coefficients
        fpu_unpacked_t em1b12 = fpsp_sgl(0x2F30CAA8);
        fpu_unpacked_t em1b11 = fpsp_sgl(0x310F8290);
        fpu_unpacked_t em1b10 = fpsp_sgl(0x32D73220);
        fpu_unpacked_t em1b9 = fpsp_sgl(0x3493F281);
        fpu_unpacked_t em1b8 = fpsp_dbl(0x3EC71DE3, 0xA5774682);
        fpu_unpacked_t em1b7 = fpsp_dbl(0x3EFA01A0, 0x19D7CB68);
        fpu_unpacked_t em1b6 = fpsp_dbl(0x3F2A01A0, 0x1A019DF3);
        fpu_unpacked_t em1b5 = fpsp_dbl(0x3F56C16C, 0x16C170E2);
        fpu_unpacked_t em1b4 = fpsp_dbl(0x3F811111, 0x11111111);
        fpu_unpacked_t em1b3 = fpsp_dbl(0x3FA55555, 0x55555555);
        fpu_unpacked_t em1b2 = fpsp_ext(0x3FFC0000, 0xAAAAAAAA, 0xAAAAAAAB);
        fpu_unpacked_t em1b1 = fpsp_sgl(0x3F000000); // 0.5

        // Even chain (fp1): B2 + S*(B4 + S*(B6 + S*(B8 + S*(B10 + S*B12))))
        fpu_unpacked_t fp1 = fp_rnd64(fpu_op_mul(fpu, s, em1b12)); // S*B12
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, em1b10)); // B10+S*B12
        fp1 = fp_rnd64(fpu_op_mul(fpu, s, fp1)); // S*(B10+S*B12)
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, em1b8)); // B8+...
        fp1 = fp_rnd64(fpu_op_mul(fpu, s, fp1)); // S*(B8+...)
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, em1b6)); // B6+...
        fp1 = fp_rnd64(fpu_op_mul(fpu, s, fp1)); // S*(B6+...)
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, em1b4)); // B4+...
        fp1 = fp_rnd64(fpu_op_mul(fpu, s, fp1)); // S*(B4+...)
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, em1b2)); // B2+S*(B4+...)
        fp1 = fp_rnd64(fpu_op_mul(fpu, s, fp1)); // S*(B2+...)
        fp1 = fp_rnd64(fpu_op_mul(fpu, x, fp1)); // X*S*(B2+...)

        // Odd chain (fp2): S^2 * (B3 + S*(B5 + S*(B7 + S*(B9 + S*B11))))
        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, s, em1b11)); // S*B11
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, em1b9)); // B9+S*B11
        fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S*(B9+S*B11)
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, em1b7)); // B7+...
        fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S*(B7+...)
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, em1b5)); // B5+...
        fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S*(B5+...)
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, em1b3)); // B3+...
        fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S*(B3+...)
        fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S^2*(B3+...)

        // Combine: result = X + (S*B1 + Q)
        fpu_unpacked_t fp0 = fp_rnd64(fpu_op_mul(fpu, s, em1b1)); // S*B1 = S/2
        fpu_unpacked_t q = fp_rnd64(fpu_op_add(fpu, fp1, fp2)); // Q
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, q)); // S*B1 + Q
        fpu_unpacked_t result = fpu_op_add(fpu, x, fp0); // X + (S*B1 + Q)

        fpu->fpcr = saved_fpcr;
        fpu->fpsr |= FPEXC_INEX2;
        return result;
    }

    // ---- EM1BIG: |X| > 70*log2 ----
    if (compact > 0x4004C215u) {
        if (!src.sign) {
            // Large positive: exp(X)-1 ≈ exp(X), delegate to FETOX
            fpu->fpcr = saved_fpcr;
            return fpu_op_etox(fpu, src, raw);
        }
        // Large negative: exp(X)-1 ≈ -1, return -1+2^(-126) to trigger inexact
        fpu_unpacked_t neg1 = {true, 0, 0x8000000000000000ULL, 0};
        fpu_unpacked_t tiny = fpsp_sgl(0x00800000); // 2^(-126)
        fpu_unpacked_t result = fpu_op_add(fpu, neg1, tiny);
        fpu->fpcr = saved_fpcr;
        fpu->fpsr |= FPEXC_INEX2;
        return result;
    }

    // ---- EM1MAIN: 1/4 <= |X| <= 70*log2 ----
    // Same argument reduction as FETOX, different polynomial and reconstruction

    // Step 2: N = round(X * 64/log2)
    fpu_unpacked_t inv_l2_64 = fpsp_sgl(0x42B8AA3B); // 64/log2
    fpu_unpacked_t product = fp_rnd64(fpu_op_mul(fpu, src, inv_l2_64));
    int32_t n = fpu_to_int32_rne(product);

    // J = N mod 64 (unsigned), M = N >> 6 (arithmetic)
    int j = ((unsigned)n) & 0x3F;
    int32_t m = n >> 6;

    // Convert N back to floating-point
    fpu_unpacked_t fp_n;
    if (n == 0) {
        fp_n = (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
    } else {
        fp_n.sign = (n < 0);
        int32_t abs_n = n < 0 ? -n : n;
        fp_n.mantissa_hi = (uint64_t)abs_n;
        fp_n.mantissa_lo = 0;
        fp_n.exponent = 63;
        fpu_normalize(&fp_n);
    }

    // Step 3: R = X + N*L1 + N*L2 (Cody-Waite reduction)
    fpu_unpacked_t l1_val = fpsp_sgl(0xBC317218); // L1 lead
    fpu_unpacked_t l2_val = fpsp_ext(0x3FDC0000, 0x82E30865, 0x4361C4C6); // L2 tail

    fpu_unpacked_t fp0 = fp_rnd64(fpu_op_mul(fpu, fp_n, l1_val)); // N*L1
    fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, fp_n, l2_val)); // N*L2
    fp0 = fp_rnd64(fpu_op_add(fpu, src, fp0)); // X + N*L1
    fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp2)); // R

    // Step 4: exp(R)-1 via degree-6 polynomial
    // p = [R + S*(A1+S*(A3+S*A5))] + [R*S*(A2+S*(A4+S*A6))]
    fpu_unpacked_t r_val = fp0; // save R
    fpu_unpacked_t s = fp_rnd64(fpu_op_mul(fpu, fp0, fp0)); // S = R*R

    fpu_unpacked_t em1a6 = fpsp_sgl(0x3950097B);
    fpu_unpacked_t em1a5 = fpsp_sgl(0x3AB60B6A);
    fpu_unpacked_t em1a4 = fpsp_dbl(0x3F811111, 0x11174385);
    fpu_unpacked_t em1a3 = fpsp_dbl(0x3FA55555, 0x55554F5A);
    fpu_unpacked_t em1a2 = fpsp_dbl(0x3FC55555, 0x55555555);
    fpu_unpacked_t em1a1 = fpsp_sgl(0x3F000000); // 0.5

    fp2 = fp_rnd64(fpu_op_mul(fpu, s, em1a6)); // S*A6
    fpu_unpacked_t fp3 = fp_rnd64(fpu_op_mul(fpu, s, em1a5)); // S*A5

    fp2 = fp_rnd64(fpu_op_add(fpu, fp2, em1a4)); // A4+S*A6
    fp3 = fp_rnd64(fpu_op_add(fpu, fp3, em1a3)); // A3+S*A5

    fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S*(A4+S*A6)
    fp3 = fp_rnd64(fpu_op_mul(fpu, s, fp3)); // S*(A3+S*A5)

    fp2 = fp_rnd64(fpu_op_add(fpu, fp2, em1a2)); // A2+S*(A4+S*A6)
    fp3 = fp_rnd64(fpu_op_add(fpu, fp3, em1a1)); // A1+S*(A3+S*A5)

    fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S*(A2+S*(A4+S*A6))
    fpu_unpacked_t fp1 = fp_rnd64(fpu_op_mul(fpu, fp3, s)); // S*(A1+S*(A3+S*A5))

    fp2 = fp_rnd64(fpu_op_mul(fpu, r_val, fp2)); // R*S*(A2+...)
    fp0 = fp_rnd64(fpu_op_add(fpu, r_val, fp1)); // R+S*(A1+...)

    fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp2)); // p = exp(R)-1

    // Step 5: T*p
    fpu_unpacked_t T_val = exptbl_T(j);
    fpu_unpacked_t t_val = exptbl_t(j);
    fp0 = fp_rnd64(fpu_op_mul(fpu, T_val, fp0)); // T*p

    // Step 6: reconstruction — compute 2^M * (T*(1+p) + t - 2^(-M))
    // OnebySc = -2^(-M): after multiplying by 2^M, this contributes -1
    fpu_unpacked_t onebysc = {true, -m, 0x8000000000000000ULL, 0}; // -2^(-M)

    if (m >= 64) {
        // T + (T*p + (t + OnebySc))
        fp1 = fp_rnd64(fpu_op_add(fpu, t_val, onebysc)); // t+OnebySc
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp1)); // T*p+(t+OnebySc)
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, T_val)); // T+rest
    } else if (m <= -4) {
        // OnebySc + (T + (T*p + t))
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, t_val)); // T*p+t
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, T_val)); // T+(T*p+t)
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, onebysc)); // OnebySc+rest
    } else {
        // (T + OnebySc) + (T*p + t)  [-3 <= M <= 63]
        fp1 = fp_rnd64(fpu_op_add(fpu, fp0, t_val)); // T*p+t
        fp2 = fp_rnd64(fpu_op_add(fpu, T_val, onebysc)); // T+OnebySc
        fp0 = fp_rnd64(fpu_op_add(fpu, fp2, fp1)); // (T+OnebySc)+(T*p+t)
    }

    // Scale by 2^M
    fpu_unpacked_t result = fp0;
    result.exponent += m;

    // Restore FPCR, set inexact
    fpu->fpcr = saved_fpcr;
    fpu->fpsr |= FPEXC_INEX2;
    return result;
}

// ============================================================================
// STWOTOX/STENTOX shared EXPTBL: 64 entries for 2^(J/64)
// ============================================================================
// Each entry: { sign+biased_exp<<16, mant_hi, mant_lo, fact2_compact }
// FACT1 = first 3 words (extended T), FACT2 = 4th word (compact correction)
// FACT2 format: upper 16 bits = sign+biased_exp, lower 16 bits = mant_hi>>48
static const uint32_t stwotox_tbl[64 * 4] = {
    0x3FFF0000, 0x80000000, 0x00000000, 0x3F738000, 0x3FFF0000, 0x8164D1F3, 0xBC030773, 0x3FBEF7CA, 0x3FFF0000,
    0x82CD8698, 0xAC2BA1D7, 0x3FBDF8A9, 0x3FFF0000, 0x843A28C3, 0xACDE4046, 0x3FBCD7C9, 0x3FFF0000, 0x85AAC367,
    0xCC487B15, 0xBFBDE8DA, 0x3FFF0000, 0x871F6196, 0x9E8D1010, 0x3FBDE85C, 0x3FFF0000, 0x88980E80, 0x92DA8527,
    0x3FBEBBF1, 0x3FFF0000, 0x8A14D575, 0x496EFD9A, 0x3FBB80CA, 0x3FFF0000, 0x8B95C1E3, 0xEA8BD6E7, 0xBFBA8373,
    0x3FFF0000, 0x8D1ADF5B, 0x7E5BA9E6, 0xBFBE9670, 0x3FFF0000, 0x8EA4398B, 0x45CD53C0, 0x3FBDB700, 0x3FFF0000,
    0x9031DC43, 0x1466B1DC, 0x3FBEEEB0, 0x3FFF0000, 0x91C3D373, 0xAB11C336, 0x3FBBFD6D, 0x3FFF0000, 0x935A2B2F,
    0x13E6E92C, 0xBFBDB319, 0x3FFF0000, 0x94F4EFA8, 0xFEF70961, 0x3FBDBA2B, 0x3FFF0000, 0x96942D37, 0x20185A00,
    0x3FBE91D5, 0x3FFF0000, 0x9837F051, 0x8DB8A96F, 0x3FBE8D5A, 0x3FFF0000, 0x99E04593, 0x20B7FA65, 0xBFBCDE7B,
    0x3FFF0000, 0x9B8D39B9, 0xD54E5539, 0xBFBEBAAF, 0x3FFF0000, 0x9D3ED9A7, 0x2CFFB751, 0xBFBD86DA, 0x3FFF0000,
    0x9EF53260, 0x91A111AE, 0xBFBEBEDD, 0x3FFF0000, 0xA0B0510F, 0xB9714FC2, 0x3FBCC96E, 0x3FFF0000, 0xA2704303,
    0x0C496819, 0xBFBEC90B, 0x3FFF0000, 0xA43515AE, 0x09E6809E, 0x3FBBD1DB, 0x3FFF0000, 0xA5FED6A9, 0xB15138EA,
    0x3FBCE5EB, 0x3FFF0000, 0xA7CD93B4, 0xE965356A, 0xBFBEC274, 0x3FFF0000, 0xA9A15AB4, 0xEA7C0EF8, 0x3FBEA83C,
    0x3FFF0000, 0xAB7A39B5, 0xA93ED337, 0x3FBECB00, 0x3FFF0000, 0xAD583EEA, 0x42A14AC6, 0x3FBE9301, 0x3FFF0000,
    0xAF3B78AD, 0x690A4375, 0xBFBD8367, 0x3FFF0000, 0xB123F581, 0xD2AC2590, 0xBFBEF05F, 0x3FFF0000, 0xB311C412,
    0xA9112489, 0x3FBDFB3C, 0x3FFF0000, 0xB504F333, 0xF9DE6484, 0x3FBEB2FB, 0x3FFF0000, 0xB6FD91E3, 0x28D17791,
    0x3FBAE2CB, 0x3FFF0000, 0xB8FBAF47, 0x62FB9EE9, 0x3FBCDC3C, 0x3FFF0000, 0xBAFF5AB2, 0x133E45FB, 0x3FBEE9AA,
    0x3FFF0000, 0xBD08A39F, 0x580C36BF, 0xBFBEAEFD, 0x3FFF0000, 0xBF1799B6, 0x7A731083, 0xBFBCBF51, 0x3FFF0000,
    0xC12C4CCA, 0x66709456, 0x3FBEF88A, 0x3FFF0000, 0xC346CCDA, 0x24976407, 0x3FBD83B2, 0x3FFF0000, 0xC5672A11,
    0x5506DADD, 0x3FBDF8AB, 0x3FFF0000, 0xC78D74C8, 0xABB9B15D, 0xBFBDFB17, 0x3FFF0000, 0xC9B9BD86, 0x6E2F27A3,
    0xBFBEFE3C, 0x3FFF0000, 0xCBEC14FE, 0xF2727C5D, 0xBFBBB6F8, 0x3FFF0000, 0xCE248C15, 0x1F8480E4, 0xBFBCEE53,
    0x3FFF0000, 0xD06333DA, 0xEF2B2595, 0xBFBDA4AE, 0x3FFF0000, 0xD2A81D91, 0xF12AE45A, 0x3FBC9124, 0x3FFF0000,
    0xD4F35AAB, 0xCFEDFA1F, 0x3FBEB243, 0x3FFF0000, 0xD744FCCA, 0xD69D6AF4, 0x3FBDE69A, 0x3FFF0000, 0xD99D15C2,
    0x78AFD7B6, 0xBFB8BC61, 0x3FFF0000, 0xDBFBB797, 0xDAF23755, 0x3FBDF610, 0x3FFF0000, 0xDE60F482, 0x5E0E9124,
    0xBFBD8BE1, 0x3FFF0000, 0xE0CCDEEC, 0x2A94E111, 0x3FBACB12, 0x3FFF0000, 0xE33F8972, 0xBE8A5A51, 0x3FBB9BFE,
    0x3FFF0000, 0xE5B906E7, 0x7C8348A8, 0x3FBCF2F4, 0x3FFF0000, 0xE8396A50, 0x3C4BDC68, 0x3FBEF22F, 0x3FFF0000,
    0xEAC0C6E7, 0xDD24392F, 0xBFBDBF4A, 0x3FFF0000, 0xED4F301E, 0xD9942B84, 0x3FBEC01A, 0x3FFF0000, 0xEFE4B99B,
    0xDCDAF5CB, 0x3FBE8CAC, 0x3FFF0000, 0xF281773C, 0x59FFB13A, 0xBFBCBB3F, 0x3FFF0000, 0xF5257D15, 0x2486CC2C,
    0x3FBEF73A, 0x3FFF0000, 0xF7D0DF73, 0x0AD13BB9, 0xBFB8B795, 0x3FFF0000, 0xFA83B2DB, 0x722A033A, 0x3FBEF84B,
    0x3FFF0000, 0xFD3E0C0C, 0xF486C175, 0xBFBEF581,
};

// Fetch T (FACT1, extended lead) from stwotox table entry j (0..63)
static fpu_unpacked_t stwotox_T(int j) {
    const uint32_t *p = &stwotox_tbl[j * 4];
    return fpsp_ext(p[0], p[1], p[2]);
}

// Fetch t (FACT2, compact correction) from stwotox table entry j (0..63)
// Format: upper 16 bits = sign+biased_exp, lower 16 bits = mantissa>>48
static fpu_unpacked_t stwotox_t(int j) {
    uint32_t w = stwotox_tbl[j * 4 + 3];
    uint16_t sexp = w >> 16;
    uint16_t mant = w & 0xFFFF;
    fpu_unpacked_t r;
    r.sign = (sexp >> 15) & 1;
    uint16_t biased = sexp & 0x7FFF;
    if (biased == 0 && mant == 0) {
        r.exponent = FPU_EXP_ZERO;
        r.mantissa_hi = 0;
    } else {
        r.exponent = (int32_t)biased - FPU_EXP_BIAS;
        r.mantissa_hi = (uint64_t)mant << 48;
    }
    r.mantissa_lo = 0;
    return r;
}

// Shared expr subroutine for FTWOTOX/FTENTOX:
// Given R (reduced arg), j (table index), l (2^L scale factor):
// Compute 2^L * (T*(1+p) + t), where p = exp(R)-1 via degree-5 polynomial
static fpu_unpacked_t stwotox_expr(fpu_state_t *fpu, fpu_unpacked_t r, int j, int32_t l) {
    fpu_unpacked_t s = fp_rnd64(fpu_op_mul(fpu, r, r)); // S = R*R

    // Polynomial coefficients (from FPSP)
    fpu_unpacked_t ea5 = fpsp_dbl(0x3F56C16D, 0x6F7BD0B2);
    fpu_unpacked_t ea4 = fpsp_dbl(0x3F811112, 0x302C712C);
    fpu_unpacked_t ea3 = fpsp_dbl(0x3FA55555, 0x55554CC1);
    fpu_unpacked_t ea2 = fpsp_dbl(0x3FC55555, 0x55554A54);
    fpu_unpacked_t ea1 = fpsp_dbl(0x3FE00000, 0x00000000); // 0.5

    // p = [R + R*S*(A2+S*A4)] + [S*(A1+S*(A3+S*A5))]
    fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, s, ea5)); // S*A5
    fpu_unpacked_t fp3 = fp_rnd64(fpu_op_mul(fpu, s, ea4)); // S*A4

    fp2 = fp_rnd64(fpu_op_add(fpu, fp2, ea3)); // A3+S*A5
    fp3 = fp_rnd64(fpu_op_add(fpu, fp3, ea2)); // A2+S*A4

    fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S*(A3+S*A5)
    fp3 = fp_rnd64(fpu_op_mul(fpu, s, fp3)); // S*(A2+S*A4)

    fp2 = fp_rnd64(fpu_op_add(fpu, fp2, ea1)); // A1+S*(A3+S*A5)
    fp3 = fp_rnd64(fpu_op_mul(fpu, r, fp3)); // R*S*(A2+S*A4)

    fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S*(A1+S*(A3+S*A5))
    fpu_unpacked_t fp0 = fp_rnd64(fpu_op_add(fpu, r, fp3)); // R+R*S*(A2+S*A4)

    fp0 = fp_rnd64(fpu_op_add(fpu, fp0, fp2)); // p = exp(R)-1

    // Reconstruction: T*(1+p) + t, scaled by 2^L
    fpu_unpacked_t T_val = stwotox_T(j);
    fpu_unpacked_t t_val = stwotox_t(j);

    fp0 = fp_rnd64(fpu_op_mul(fpu, T_val, fp0)); // T*p
    fp0 = fp_rnd64(fpu_op_add(fpu, fp0, t_val)); // T*p + t
    fp0 = fp_rnd64(fpu_op_add(fpu, fp0, T_val)); // T + (T*p + t)

    // Scale by 2^L (equivalent to FPSP's FACT scaling + ADJFACT multiply)
    fp0.exponent += l;
    return fp0;
}

// ============================================================================
// FTWOTOX — 2^X (opcode 0x11)
// ============================================================================
// Algorithm from Motorola FPSP.
//
// Normal: N = round(64*X), r = X-N/64, R = r*ln(2), then expr subroutine.
// Tiny (|X| < 2^(-70)): result = 1 + X.
// Big (|X| > 16480): overflow or underflow.

fpu_unpacked_t fpu_op_twotox(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // NaN: propagate, converting SNaN to QNaN
    if (src.exponent == FPU_EXP_INF && src.mantissa_hi != 0) {
        if (!(src.mantissa_hi & 0x4000000000000000ULL))
            fpu->fpsr |= FPEXC_SNAN;
        src.mantissa_hi |= 0x4000000000000000ULL;
        return src;
    }

    // +Infinity: 2^(+inf) = +inf
    if (src.exponent == FPU_EXP_INF && !src.sign)
        return src;

    // -Infinity: 2^(-inf) = +0
    if (src.exponent == FPU_EXP_INF && src.sign)
        return (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};

    // Zero: 2^0 = 1.0 (exact)
    if (src.exponent == FPU_EXP_ZERO)
        return (fpu_unpacked_t){false, 0, 0x8000000000000000ULL, 0};

    // Normalize unnormalized extended inputs
    uint16_t biased_exp = FP80_EXP(raw);
    if (biased_exp != 0 && biased_exp != 0x7FFF && !(src.mantissa_hi & 0x8000000000000000ULL)) {
        fpu_normalize(&src);
    }

    // Compact form for range checks
    uint16_t bexp = (uint16_t)(src.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)bexp << 16) | (uint32_t)(src.mantissa_hi >> 48);

    // Tiny: |X| < 2^(-70), 2^X ≈ 1+X
    if (compact < 0x3FB98000u) {
        fpu->fpsr |= FPEXC_INEX2;
        fpu_unpacked_t one = {false, 0, 0x8000000000000000ULL, 0};
        return fpu_op_add(fpu, one, src);
    }

    // Big: |X| > 16480
    if (compact > 0x400D80C0u) {
        if (src.sign) {
            fpu->fpsr |= FPEXC_UNFL | FPEXC_INEX2;
            return (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
        } else {
            fpu->fpsr |= FPEXC_OVFL | FPEXC_INEX2;
            return (fpu_unpacked_t){false, FPU_EXP_INF, 0, 0};
        }
    }

    // Save FPCR, clear for intermediate ops
    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    // N = round(64*X)
    fpu_unpacked_t sixty_four = fpsp_sgl(0x42800000); // 64.0
    fpu_unpacked_t fp1 = fp_rnd64(fpu_op_mul(fpu, src, sixty_four));
    int32_t n = fpu_to_int32_rne(fp1);

    int j = ((unsigned)n) & 0x3F; // J = N mod 64
    int32_t l = n >> 6; // L = N / 64

    // Convert N to float
    fpu_unpacked_t fp_n;
    if (n == 0) {
        fp_n = (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
    } else {
        fp_n.sign = (n < 0);
        int32_t abs_n = n < 0 ? -n : n;
        fp_n.mantissa_hi = (uint64_t)abs_n;
        fp_n.mantissa_lo = 0;
        fp_n.exponent = 63;
        fpu_normalize(&fp_n);
    }

    // r = X - N/64
    fpu_unpacked_t inv64 = fpsp_sgl(0x3C800000); // 1/64
    fp1 = fp_rnd64(fpu_op_mul(fpu, fp_n, inv64)); // N/64
    fpu_unpacked_t fp0 = fp_rnd64(fpu_op_sub(fpu, src, fp1)); // r = X - N/64

    // R = r * ln(2)
    fpu_unpacked_t log2_ext = fpsp_ext(0x3FFE0000, 0xB17217F7, 0xD1CF79AC);
    fp0 = fp_rnd64(fpu_op_mul(fpu, fp0, log2_ext)); // R

    // Polynomial + reconstruction via shared expr
    fpu_unpacked_t result = stwotox_expr(fpu, fp0, j, l);

    // Restore FPCR, set inexact
    fpu->fpcr = saved_fpcr;
    fpu->fpsr |= FPEXC_INEX2;
    return result;
}

// ============================================================================
// FTENTOX — 10^X (opcode 0x12)
// ============================================================================
// Algorithm from Motorola FPSP.
//
// Normal: N = round(X*64*log10/log2), Cody-Waite reduction, R = r*ln(10).
// Tiny (|X| < 2^(-70)): result = 1 + X.
// Big (|X| > 16480*log2/log10): overflow or underflow.

fpu_unpacked_t fpu_op_tentox(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // NaN: propagate, converting SNaN to QNaN
    if (src.exponent == FPU_EXP_INF && src.mantissa_hi != 0) {
        if (!(src.mantissa_hi & 0x4000000000000000ULL))
            fpu->fpsr |= FPEXC_SNAN;
        src.mantissa_hi |= 0x4000000000000000ULL;
        return src;
    }

    // +Infinity: 10^(+inf) = +inf
    if (src.exponent == FPU_EXP_INF && !src.sign)
        return src;

    // -Infinity: 10^(-inf) = +0
    if (src.exponent == FPU_EXP_INF && src.sign)
        return (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};

    // Zero: 10^0 = 1.0 (exact)
    if (src.exponent == FPU_EXP_ZERO)
        return (fpu_unpacked_t){false, 0, 0x8000000000000000ULL, 0};

    // Normalize unnormalized extended inputs
    uint16_t biased_exp = FP80_EXP(raw);
    if (biased_exp != 0 && biased_exp != 0x7FFF && !(src.mantissa_hi & 0x8000000000000000ULL)) {
        fpu_normalize(&src);
    }

    // Compact form for range checks
    uint16_t bexp = (uint16_t)(src.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)bexp << 16) | (uint32_t)(src.mantissa_hi >> 48);

    // Tiny: |X| < 2^(-70), 10^X ≈ 1+X
    if (compact < 0x3FB98000u) {
        fpu->fpsr |= FPEXC_INEX2;
        fpu_unpacked_t one = {false, 0, 0x8000000000000000ULL, 0};
        return fpu_op_add(fpu, one, src);
    }

    // Big: |X| > 16480*log2/log10 (≈4963)
    if (compact > 0x400B9B07u) {
        if (src.sign) {
            fpu->fpsr |= FPEXC_UNFL | FPEXC_INEX2;
            return (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
        } else {
            fpu->fpsr |= FPEXC_OVFL | FPEXC_INEX2;
            return (fpu_unpacked_t){false, FPU_EXP_INF, 0, 0};
        }
    }

    // Save FPCR, clear for intermediate ops
    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    // N = round(X * 64*log10/log2)
    fpu_unpacked_t l2ten64 = fpsp_dbl(0x406A934F, 0x0979A371); // 64*log10/log2
    fpu_unpacked_t fp1 = fp_rnd64(fpu_op_mul(fpu, src, l2ten64));
    int32_t n = fpu_to_int32_rne(fp1);

    int j = ((unsigned)n) & 0x3F; // J = N mod 64
    int32_t l = n >> 6; // L = N / 64

    // Convert N to float
    fpu_unpacked_t fp_n;
    if (n == 0) {
        fp_n = (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
    } else {
        fp_n.sign = (n < 0);
        int32_t abs_n = n < 0 ? -n : n;
        fp_n.mantissa_hi = (uint64_t)abs_n;
        fp_n.mantissa_lo = 0;
        fp_n.exponent = 63;
        fpu_normalize(&fp_n);
    }

    // r = X - N*L10TWO1 - N*L10TWO2 (Cody-Waite with log2/(64*log10))
    fpu_unpacked_t l10two1 = fpsp_dbl(0x3F734413, 0x509F8000); // lead
    fpu_unpacked_t l10two2 = fpsp_ext(0xBFCD0000, 0xC0219DC1, 0xDA994FD2); // trail

    fp1 = fp_rnd64(fpu_op_mul(fpu, fp_n, l10two1)); // N*L10TWO1
    fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, fp_n, l10two2)); // N*L10TWO2
    fpu_unpacked_t fp0 = fp_rnd64(fpu_op_sub(fpu, src, fp1)); // X - N*L10TWO1
    fp0 = fp_rnd64(fpu_op_sub(fpu, fp0, fp2)); // r

    // R = r * ln(10)
    fpu_unpacked_t log10_ext = fpsp_ext(0x40000000, 0x935D8DDD, 0xAAA8AC17);
    fp0 = fp_rnd64(fpu_op_mul(fpu, fp0, log10_ext)); // R

    // Polynomial + reconstruction via shared expr
    fpu_unpacked_t result = stwotox_expr(fpu, fp0, j, l);

    // Restore FPCR, set inexact
    fpu->fpcr = saved_fpcr;
    fpu->fpsr |= FPEXC_INEX2;
    return result;
}

// ============================================================================
// FATAN — arctangent (opcode 0x0A)
// Algorithm from Motorola FPSP.
// ============================================================================

// 128-entry table of arctan(|F|) for F = +-2^k * 1.xxxx1 (k=-4..3, 5-bit frac)
// Each entry is 3 longs: { sign+biased_exp, mantissa_hi, mantissa_lo }
static const uint32_t atantbl_data[128 * 3] = {
    // k=-4 (exponent 0x3FFB), fractional patterns .0000_1 .. .1111_1
    0x3FFB0000,
    0x83D152C5,
    0x060B7A51,
    0x3FFB0000,
    0x8BC85445,
    0x65498B8B,
    0x3FFB0000,
    0x93BE4060,
    0x17626B0D,
    0x3FFB0000,
    0x9BB3078D,
    0x35AEC202,
    0x3FFB0000,
    0xA3A69A52,
    0x5DDCE7DE,
    0x3FFB0000,
    0xAB98E943,
    0x62765619,
    0x3FFB0000,
    0xB389E502,
    0xF9C59862,
    0x3FFB0000,
    0xBB797E43,
    0x6B09E6FB,
    0x3FFB0000,
    0xC367A5C7,
    0x39E5F446,
    0x3FFB0000,
    0xCB544C61,
    0xCFF7D5C6,
    0x3FFB0000,
    0xD33F62F8,
    0x2488533E,
    0x3FFB0000,
    0xDB28DA81,
    0x62404C77,
    0x3FFB0000,
    0xE310A407,
    0x8AD34F18,
    0x3FFB0000,
    0xEAF6B0A8,
    0x188EE1EB,
    0x3FFB0000,
    0xF2DAF194,
    0x9DBE79D5,
    0x3FFB0000,
    0xFABD5813,
    0x61D47E3E,
    // k=-3 (exponent 0x3FFC)
    0x3FFC0000,
    0x8346AC21,
    0x0959ECC4,
    0x3FFC0000,
    0x8B232A08,
    0x304282D8,
    0x3FFC0000,
    0x92FB70B8,
    0xD29AE2F9,
    0x3FFC0000,
    0x9ACF476F,
    0x5CCD1CB4,
    0x3FFC0000,
    0xA29E7630,
    0x4954F23F,
    0x3FFC0000,
    0xAA68C5D0,
    0x8AB85230,
    0x3FFC0000,
    0xB22DFFFD,
    0x9D539F83,
    0x3FFC0000,
    0xB9EDEF45,
    0x3E900EA5,
    0x3FFC0000,
    0xC1A85F1C,
    0xC75E3EA5,
    0x3FFC0000,
    0xC95D1BE8,
    0x28138DE6,
    0x3FFC0000,
    0xD10BF300,
    0x840D2DE4,
    0x3FFC0000,
    0xD8B4B2BA,
    0x6BC05E7A,
    0x3FFC0000,
    0xE0572A6B,
    0xB42335F6,
    0x3FFC0000,
    0xE7F32A70,
    0xEA9CAA8F,
    0x3FFC0000,
    0xEF888432,
    0x64ECEFAA,
    0x3FFC0000,
    0xF7170A28,
    0xECC06666,
    // k=-2 (exponent 0x3FFD)
    0x3FFD0000,
    0x812FD288,
    0x332DAD32,
    0x3FFD0000,
    0x88A8D1B1,
    0x218E4D64,
    0x3FFD0000,
    0x9012AB3F,
    0x23E4AEE8,
    0x3FFD0000,
    0x976CC3D4,
    0x11E7F1B9,
    0x3FFD0000,
    0x9EB68949,
    0x3889A227,
    0x3FFD0000,
    0xA5EF72C3,
    0x4487361B,
    0x3FFD0000,
    0xAD1700BA,
    0xF07A7227,
    0x3FFD0000,
    0xB42CBCFA,
    0xFD37EFB7,
    0x3FFD0000,
    0xBB303A94,
    0x0BA80F89,
    0x3FFD0000,
    0xC22115C6,
    0xFCAEBBAF,
    0x3FFD0000,
    0xC8FEF3E6,
    0x86331221,
    0x3FFD0000,
    0xCFC98330,
    0xB4000C70,
    0x3FFD0000,
    0xD6807AA1,
    0x102C5BF9,
    0x3FFD0000,
    0xDD2399BC,
    0x31252AA3,
    0x3FFD0000,
    0xE3B2A855,
    0x6B8FC517,
    0x3FFD0000,
    0xEA2D764F,
    0x64315989,
    // k=-1 (exponent transitions from 0x3FFD to 0x3FFE)
    0x3FFD0000,
    0xF3BF5BF8,
    0xBAD1A21D,
    0x3FFE0000,
    0x801CE39E,
    0x0D205C9A,
    0x3FFE0000,
    0x8630A2DA,
    0xDA1ED066,
    0x3FFE0000,
    0x8C1AD445,
    0xF3E09B8C,
    0x3FFE0000,
    0x91DB8F16,
    0x64F350E2,
    0x3FFE0000,
    0x97731420,
    0x365E538C,
    0x3FFE0000,
    0x9CE1C8E6,
    0xA0B8CDBA,
    0x3FFE0000,
    0xA22832DB,
    0xCADAAE09,
    0x3FFE0000,
    0xA746F2DD,
    0xB7602294,
    0x3FFE0000,
    0xAC3EC0FB,
    0x997DD6A2,
    0x3FFE0000,
    0xB110688A,
    0xEBDC6F6A,
    0x3FFE0000,
    0xB5BCC490,
    0x59ECC4B0,
    0x3FFE0000,
    0xBA44BC7D,
    0xD470782F,
    0x3FFE0000,
    0xBEA94144,
    0xFD049AAC,
    0x3FFE0000,
    0xC2EB4ABB,
    0x661628B6,
    0x3FFE0000,
    0xC70BD54C,
    0xE602EE14,
    // k=0..3
    0x3FFE0000,
    0xCD000549,
    0xADEC7159,
    0x3FFE0000,
    0xD48457D2,
    0xD8EA4EA3,
    0x3FFE0000,
    0xDB948DA7,
    0x12DECE3B,
    0x3FFE0000,
    0xE23855F9,
    0x69E8096A,
    0x3FFE0000,
    0xE8771129,
    0xC4353259,
    0x3FFE0000,
    0xEE57C16E,
    0x0D379C0D,
    0x3FFE0000,
    0xF3E10211,
    0xA87C3779,
    0x3FFE0000,
    0xF919039D,
    0x758B8D41,
    0x3FFE0000,
    0xFE058B8F,
    0x64935FB3,
    0x3FFF0000,
    0x8155FB49,
    0x7B685D04,
    0x3FFF0000,
    0x83889E35,
    0x49D108E1,
    0x3FFF0000,
    0x859CFA76,
    0x511D724B,
    0x3FFF0000,
    0x87952ECF,
    0xFF8131E7,
    0x3FFF0000,
    0x89732FD1,
    0x9557641B,
    0x3FFF0000,
    0x8B38CAD1,
    0x01932A35,
    0x3FFF0000,
    0x8CE7A8D8,
    0x301EE6B5,
    0x3FFF0000,
    0x8F46A39E,
    0x2EAE5281,
    0x3FFF0000,
    0x922DA7D7,
    0x91888487,
    0x3FFF0000,
    0x94D19FCB,
    0xDEDF5241,
    0x3FFF0000,
    0x973AB944,
    0x19D2A08B,
    0x3FFF0000,
    0x996FF00E,
    0x08E10B96,
    0x3FFF0000,
    0x9B773F95,
    0x12321DA7,
    0x3FFF0000,
    0x9D55CC32,
    0x0F935624,
    0x3FFF0000,
    0x9F100575,
    0x006CC571,
    0x3FFF0000,
    0xA0A9C290,
    0xD97CC06C,
    0x3FFF0000,
    0xA22659EB,
    0xEBC0630A,
    0x3FFF0000,
    0xA388B4AF,
    0xF6EF0EC9,
    0x3FFF0000,
    0xA4D35F10,
    0x61D292C4,
    0x3FFF0000,
    0xA60895DC,
    0xFBE3187E,
    0x3FFF0000,
    0xA72A51DC,
    0x7367BEAC,
    0x3FFF0000,
    0xA83A5153,
    0x0956168F,
    0x3FFF0000,
    0xA93A2007,
    0x7539546E,
    0x3FFF0000,
    0xAA9E7245,
    0x023B2605,
    0x3FFF0000,
    0xAC4C84BA,
    0x6FE4D58F,
    0x3FFF0000,
    0xADCE4A4A,
    0x606B9712,
    0x3FFF0000,
    0xAF2A2DCD,
    0x8D263C9C,
    0x3FFF0000,
    0xB0656F81,
    0xF22265C7,
    0x3FFF0000,
    0xB1846515,
    0x0F71496A,
    0x3FFF0000,
    0xB28AAA15,
    0x6F9ADA35,
    0x3FFF0000,
    0xB37B44FF,
    0x3766B895,
    0x3FFF0000,
    0xB458C3DC,
    0xE9630433,
    0x3FFF0000,
    0xB525529D,
    0x562246BD,
    0x3FFF0000,
    0xB5E2CCA9,
    0x5F9D88CC,
    0x3FFF0000,
    0xB692CADA,
    0x7ACA1ADA,
    0x3FFF0000,
    0xB736AEA7,
    0xA6925838,
    0x3FFF0000,
    0xB7CFAB28,
    0x7E9F7B36,
    0x3FFF0000,
    0xB85ECC66,
    0xCB219835,
    0x3FFF0000,
    0xB8E4FD5A,
    0x20A593DA,
    0x3FFF0000,
    0xB99F41F6,
    0x4AFF9BB5,
    0x3FFF0000,
    0xBA7F1E17,
    0x842BBE7B,
    0x3FFF0000,
    0xBB471285,
    0x7637E17D,
    0x3FFF0000,
    0xBBFABE8A,
    0x4788DF6F,
    0x3FFF0000,
    0xBC9D0FAD,
    0x2B689D79,
    0x3FFF0000,
    0xBD306A39,
    0x471ECD86,
    0x3FFF0000,
    0xBDB6C731,
    0x856AF18A,
    0x3FFF0000,
    0xBE31CAC5,
    0x02E80D70,
    0x3FFF0000,
    0xBEA2D55C,
    0xE33194E2,
    0x3FFF0000,
    0xBF0B10B7,
    0xC03128F0,
    0x3FFF0000,
    0xBF6B7A18,
    0xDACB778D,
    0x3FFF0000,
    0xBFC4EA46,
    0x63FA18F6,
    0x3FFF0000,
    0xC0181BDE,
    0x8B89A454,
    0x3FFF0000,
    0xC065B066,
    0xCFBF6439,
    0x3FFF0000,
    0xC0AE345F,
    0x56340AE6,
    0x3FFF0000,
    0xC0F22291,
    0x9CB9E6A7,
};

// Fetch ATANTBL entry i as fpu_unpacked_t (i = 0..127)
static inline fpu_unpacked_t atantbl_entry(int i) {
    return fpsp_ext(atantbl_data[i * 3], atantbl_data[i * 3 + 1], atantbl_data[i * 3 + 2]);
}

// FATAN: arctangent (opcode 0x0A)
fpu_unpacked_t fpu_op_atan(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // Handle special cases: zero, NaN, infinity
    if (src.exponent == FPU_EXP_ZERO) {
        return src; // atan(+-0) = +-0
    }
    if (src.exponent == FPU_EXP_INF) {
        if (src.mantissa_hi != 0) {
            // NaN
            if (fp80_is_snan(raw)) {
                fpu->fpsr |= FPEXC_SNAN;
            }
            // Return canonical QNaN
            fpu_unpacked_t nan = src;
            nan.mantissa_hi |= 0x4000000000000000ULL;
            return nan;
        }
        // atan(+-inf) = +-pi/2
        fpu_unpacked_t piby2 = fpsp_ext(0x3FFF0000, 0xC90FDAA2, 0x2168C235);
        piby2.sign = src.sign;
        fpu->fpsr |= FPEXC_INEX2;
        return piby2;
    }

    // Save and clear FPCR for intermediate calculations
    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    // Compute compact form: biased_exp << 16 | top 16 mantissa bits
    uint16_t biased_exp = (uint16_t)(src.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)biased_exp << 16) | (uint32_t)(src.mantissa_hi >> 48);

    fpu_unpacked_t fp0;

    if (compact < 0x3FFB8000) {
        // |X| < 1/16
        if (compact < 0x3FD78000) {
            // ATANTINY: |X| < 2^(-40), atan(X) = X
            fpu->fpcr = saved_fpcr;
            fpu->fpsr |= FPEXC_INEX2;
            return src;
        }
        // ATANSM: polynomial approximation for |X| < 1/16
        // atan(X) = X + X*Y*([B1+Z*(B3+Z*B5)] + [Y*(B2+Z*(B4+Z*B6))])
        // where Y = X*X, Z = Y*Y
        fpu_unpacked_t x = src;
        fpu_unpacked_t y = fp_rnd64(fpu_op_mul(fpu, x, x)); // Y = X*X
        fpu_unpacked_t z = fp_rnd64(fpu_op_mul(fpu, y, y)); // Z = Y*Y

        // Polynomial coefficients (IEEE 754 doubles)
        fpu_unpacked_t b1 = fpsp_dbl(0xBFD55555, 0x55555555);
        fpu_unpacked_t b2 = fpsp_dbl(0x3FC99999, 0x99998FA9);
        fpu_unpacked_t b3 = fpsp_dbl(0xBFC24924, 0x921872F9);
        fpu_unpacked_t b4 = fpsp_dbl(0x3FBC71C6, 0x46940220);
        fpu_unpacked_t b5 = fpsp_dbl(0xBFB744EE, 0x7FAF45DB);
        fpu_unpacked_t b6 = fpsp_dbl(0x3FB34444, 0x7F876989);

        // Even part: B6, B4, B2 path
        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, z, b6)); // Z*B6
        fp2 = fp_rnd64(fpu_op_add(fpu, b4, fp2)); // B4+Z*B6
        fp2 = fp_rnd64(fpu_op_mul(fpu, z, fp2)); // Z*(B4+Z*B6)
        fp2 = fp_rnd64(fpu_op_add(fpu, b2, fp2)); // B2+Z*(B4+Z*B6)

        // Odd part: B5, B3, B1 path
        fpu_unpacked_t fp3 = fp_rnd64(fpu_op_mul(fpu, z, b5)); // Z*B5
        fp3 = fp_rnd64(fpu_op_add(fpu, b3, fp3)); // B3+Z*B5
        fp3 = fp_rnd64(fpu_op_mul(fpu, z, fp3)); // Z*(B3+Z*B5)
        fp3 = fp_rnd64(fpu_op_add(fpu, b1, fp3)); // B1+Z*(B3+Z*B5)

        fp2 = fp_rnd64(fpu_op_mul(fpu, y, fp2)); // Y*(B2+Z*(B4+Z*B6))
        fpu_unpacked_t xy = fp_rnd64(fpu_op_mul(fpu, x, y)); // X*Y

        fp0 = fp_rnd64(fpu_op_add(fpu, fp3, fp2)); // [B1+...]+[Y*(B2+...)]
        fp0 = fp_rnd64(fpu_op_mul(fpu, fp0, xy)); // X*Y*([B1+...]+[Y*(B2+...)])

        fpu->fpcr = saved_fpcr;
        fp0 = fpu_op_add(fpu, x, fp0); // X + poly
        fpu->fpsr |= FPEXC_INEX2;
        return fp0;
    }

    if (compact <= 0x4002FFFF) {
        // ATANMAIN: 1/16 <= |X| < 16, table-based approach
        // F = +-2^k * 1.xxxx1 (first 5 mantissa bits, 6th bit forced to 1)
        // U = (X - F) / (1 + X*F)
        // atan(X) = sign(F)*atan(|F|) + atan(U)

        // Construct F from X: keep sign+exponent, keep top 5 mantissa bits,
        // set 6th bit to 1, clear rest
        fpu_unpacked_t f;
        f.sign = src.sign;
        f.exponent = src.exponent;
        f.mantissa_hi = (src.mantissa_hi & 0xF800000000000000ULL) | 0x0400000000000000ULL;
        f.mantissa_lo = 0;

        // Compute U = (X - F) / (1 + X*F)
        fpu_unpacked_t xf = fp_rnd64(fpu_op_mul(fpu, src, f)); // X*F (>0)
        fpu_unpacked_t xmf = fp_rnd64(fpu_op_sub(fpu, src, f)); // X-F
        fpu_unpacked_t one = fpsp_sgl(0x3F800000); // 1.0
        fpu_unpacked_t denom = fp_rnd64(fpu_op_add(fpu, one, xf)); // 1+X*F
        fpu_unpacked_t u = fp_rnd64(fpu_op_div(fpu, xmf, denom)); // U

        // Compute table index from compact form
        // FPSP: frac_bits = bits 14:11 of mantissa top 16, exp_offset = (K+4) scaled
        // The FPSP produces a 16-byte-entry byte offset at >>7; we need entry index
        uint32_t frac_bits = compact & 0x00007800; // 4 frac bits
        uint32_t exp_offset = (compact & 0x7FFF0000) - 0x3FFB0000; // K+4 (shifted)
        exp_offset >>= 1; // collapse
        uint32_t idx = (frac_bits + exp_offset) >> 11; // 7-bit entry index

        // Fetch atan(|F|) and apply sign of F
        fpu_unpacked_t atanf = atantbl_entry(idx);
        atanf.sign = src.sign;

        // Polynomial: atan(U) = U + A1*U*V*(A2 + V*(A3 + V))
        // where V = U*U
        fpu_unpacked_t a1 = fpsp_dbl(0xBFC2476F, 0x4E1DA28E);
        fpu_unpacked_t a2 = fpsp_dbl(0x4002AC69, 0x34A26DB3);
        fpu_unpacked_t a3 = fpsp_dbl(0xBFF6687E, 0x314987D8);

        fpu_unpacked_t v = fp_rnd64(fpu_op_mul(fpu, u, u)); // V = U*U

        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_add(fpu, a3, v)); // A3+V
        fp2 = fp_rnd64(fpu_op_mul(fpu, v, fp2)); // V*(A3+V)
        fpu_unpacked_t uv = fp_rnd64(fpu_op_mul(fpu, u, v)); // U*V
        fp2 = fp_rnd64(fpu_op_add(fpu, a2, fp2)); // A2+V*(A3+V)
        fpu_unpacked_t fp1 = fp_rnd64(fpu_op_mul(fpu, a1, uv)); // A1*U*V
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp1, fp2)); // A1*U*V*(A2+V*(A3+V))

        fp0 = fp_rnd64(fpu_op_add(fpu, u, fp1)); // atan(U)

        fpu->fpcr = saved_fpcr;
        fp0 = fpu_op_add(fpu, atanf, fp0); // atan(F) + atan(U)
        fpu->fpsr |= FPEXC_INEX2;
        return fp0;
    }

    // |X| >= 16
    if (compact > 0x40638000) {
        // ATANHUGE: |X| > 2^100, return sign(X)*(pi/2 - tiny)
        fpu_unpacked_t piby2;
        fpu_unpacked_t tiny;
        if (src.sign) {
            piby2 = fpsp_ext(0xBFFF0000, 0xC90FDAA2, 0x2168C235); // -pi/2
            tiny = fpsp_ext(0x80010000, 0x80000000, 0x00000000); // -tiny
        } else {
            piby2 = fpsp_ext(0x3FFF0000, 0xC90FDAA2, 0x2168C235); // +pi/2
            tiny = fpsp_ext(0x00010000, 0x80000000, 0x00000000); // +tiny
        }
        fpu->fpcr = saved_fpcr;
        fp0 = fpu_op_sub(fpu, piby2, tiny); // +-pi/2 - +-tiny
        fpu->fpsr |= FPEXC_INEX2;
        return fp0;
    }

    // ATANBIG: 16 <= |X| <= 2^100
    // Compute X' = -1/X, then atan(X') with C polynomial, then add +-pi/2
    fpu_unpacked_t neg_one = fpsp_sgl(0xBF800000); // -1.0
    fpu_unpacked_t xprime = fp_rnd64(fpu_op_div(fpu, neg_one, src)); // X' = -1/X

    // Y = X'*X', Z = Y*Y
    fpu_unpacked_t xp = xprime;
    fpu_unpacked_t y = fp_rnd64(fpu_op_mul(fpu, xp, xp)); // Y
    fpu_unpacked_t z = fp_rnd64(fpu_op_mul(fpu, y, y)); // Z

    // Polynomial coefficients C1..C5 (doubles)
    fpu_unpacked_t c1 = fpsp_dbl(0xBFD55555, 0x55555536);
    fpu_unpacked_t c2 = fpsp_dbl(0x3FC99999, 0x9996263E);
    fpu_unpacked_t c3 = fpsp_dbl(0xBFC24924, 0x827107B8);
    fpu_unpacked_t c4 = fpsp_dbl(0x3FBC7187, 0x962D1D7D);
    fpu_unpacked_t c5 = fpsp_dbl(0xBFB70BF3, 0x98539E6A);

    // Even: C4, C2 path
    fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, z, c4)); // Z*C4
    fp2 = fp_rnd64(fpu_op_add(fpu, c2, fp2)); // C2+Z*C4
    fp2 = fp_rnd64(fpu_op_mul(fpu, y, fp2)); // Y*(C2+Z*C4)

    // Odd: C5, C3, C1 path
    fpu_unpacked_t fp3 = fp_rnd64(fpu_op_mul(fpu, z, c5)); // Z*C5
    fp3 = fp_rnd64(fpu_op_add(fpu, c3, fp3)); // C3+Z*C5
    fp3 = fp_rnd64(fpu_op_mul(fpu, z, fp3)); // Z*(C3+Z*C5)
    fp3 = fp_rnd64(fpu_op_add(fpu, c1, fp3)); // C1+Z*(C3+Z*C5)

    fpu_unpacked_t xpy = fp_rnd64(fpu_op_mul(fpu, xp, y)); // X'*Y
    fp0 = fp_rnd64(fpu_op_add(fpu, fp2, fp3)); // [C1+...]+[Y*(C2+...)]
    fp0 = fp_rnd64(fpu_op_mul(fpu, fp0, xpy)); // X'*Y*([C1+...]+[Y*(C2+...)])
    fp0 = fp_rnd64(fpu_op_add(fpu, xp, fp0)); // X' + poly

    // Add +-pi/2 depending on sign of original X
    fpu_unpacked_t piby2;
    if (src.sign) {
        piby2 = fpsp_ext(0xBFFF0000, 0xC90FDAA2, 0x2168C235); // -pi/2
    } else {
        piby2 = fpsp_ext(0x3FFF0000, 0xC90FDAA2, 0x2168C235); // +pi/2
    }

    fpu->fpcr = saved_fpcr;
    fp0 = fpu_op_add(fpu, piby2, fp0); // +-pi/2 + atan(X')
    fpu->fpsr |= FPEXC_INEX2;
    return fp0;
}

// ============================================================================
// FSIN / FCOS / FSINCOS / FTAN — trigonometric functions
// Algorithms from Motorola FPSP.
// ============================================================================

// PITBL: 65 entries of N*PI/2 for N = -32..32
// Each entry is 4 longs: { sign+biased_exp, mantissa_hi, mantissa_lo, Y2_single }
// Y1 = first 3 longs (extended), Y2 = 4th long (single correction)
static const uint32_t pitbl_data[65 * 4] = {
    0xC0040000, 0xC90FDAA2, 0x2168C235, 0x21800000, 0xC0040000, 0xC2C75BCD, 0x105D7C23, 0xA0D00000, 0xC0040000,
    0xBC7EDCF7, 0xFF523611, 0xA1E80000, 0xC0040000, 0xB6365E22, 0xEE46F000, 0x21480000, 0xC0040000, 0xAFEDDF4D,
    0xDD3BA9EE, 0xA1200000, 0xC0040000, 0xA9A56078, 0xCC3063DD, 0x21FC0000, 0xC0040000, 0xA35CE1A3, 0xBB251DCB,
    0x21100000, 0xC0040000, 0x9D1462CE, 0xAA19D7B9, 0xA1580000, 0xC0040000, 0x96CBE3F9, 0x990E91A8, 0x21E00000,
    0xC0040000, 0x90836524, 0x88034B96, 0x20B00000, 0xC0040000, 0x8A3AE64F, 0x76F80584, 0xA1880000, 0xC0040000,
    0x83F2677A, 0x65ECBF73, 0x21C40000, 0xC0030000, 0xFB53D14A, 0xA9C2F2C2, 0x20000000, 0xC0030000, 0xEEC2D3A0,
    0x87AC669F, 0x21380000, 0xC0030000, 0xE231D5F6, 0x6595DA7B, 0xA1300000, 0xC0030000, 0xD5A0D84C, 0x437F4E58,
    0x9FC00000, 0xC0030000, 0xC90FDAA2, 0x2168C235, 0x21000000, 0xC0030000, 0xBC7EDCF7, 0xFF523611, 0xA1680000,
    0xC0030000, 0xAFEDDF4D, 0xDD3BA9EE, 0xA0A00000, 0xC0030000, 0xA35CE1A3, 0xBB251DCB, 0x20900000, 0xC0030000,
    0x96CBE3F9, 0x990E91A8, 0x21600000, 0xC0030000, 0x8A3AE64F, 0x76F80584, 0xA1080000, 0xC0020000, 0xFB53D14A,
    0xA9C2F2C2, 0x1F800000, 0xC0020000, 0xE231D5F6, 0x6595DA7B, 0xA0B00000, 0xC0020000, 0xC90FDAA2, 0x2168C235,
    0x20800000, 0xC0020000, 0xAFEDDF4D, 0xDD3BA9EE, 0xA0200000, 0xC0020000, 0x96CBE3F9, 0x990E91A8, 0x20E00000,
    0xC0010000, 0xFB53D14A, 0xA9C2F2C2, 0x1F000000, 0xC0010000, 0xC90FDAA2, 0x2168C235, 0x20000000, 0xC0010000,
    0x96CBE3F9, 0x990E91A8, 0x20600000, 0xC0000000, 0xC90FDAA2, 0x2168C235, 0x1F800000, 0xBFFF0000, 0xC90FDAA2,
    0x2168C235, 0x1F000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // N=0
    0x3FFF0000, 0xC90FDAA2, 0x2168C235, 0x9F000000, 0x40000000, 0xC90FDAA2, 0x2168C235, 0x9F800000, 0x40010000,
    0x96CBE3F9, 0x990E91A8, 0xA0600000, 0x40010000, 0xC90FDAA2, 0x2168C235, 0xA0000000, 0x40010000, 0xFB53D14A,
    0xA9C2F2C2, 0x9F000000, 0x40020000, 0x96CBE3F9, 0x990E91A8, 0xA0E00000, 0x40020000, 0xAFEDDF4D, 0xDD3BA9EE,
    0x20200000, 0x40020000, 0xC90FDAA2, 0x2168C235, 0xA0800000, 0x40020000, 0xE231D5F6, 0x6595DA7B, 0x20B00000,
    0x40020000, 0xFB53D14A, 0xA9C2F2C2, 0x9F800000, 0x40030000, 0x8A3AE64F, 0x76F80584, 0x21080000, 0x40030000,
    0x96CBE3F9, 0x990E91A8, 0xA1600000, 0x40030000, 0xA35CE1A3, 0xBB251DCB, 0xA0900000, 0x40030000, 0xAFEDDF4D,
    0xDD3BA9EE, 0x20A00000, 0x40030000, 0xBC7EDCF7, 0xFF523611, 0x21680000, 0x40030000, 0xC90FDAA2, 0x2168C235,
    0xA1000000, 0x40030000, 0xD5A0D84C, 0x437F4E58, 0x1FC00000, 0x40030000, 0xE231D5F6, 0x6595DA7B, 0x21300000,
    0x40030000, 0xEEC2D3A0, 0x87AC669F, 0xA1380000, 0x40030000, 0xFB53D14A, 0xA9C2F2C2, 0xA0000000, 0x40040000,
    0x83F2677A, 0x65ECBF73, 0xA1C40000, 0x40040000, 0x8A3AE64F, 0x76F80584, 0x21880000, 0x40040000, 0x90836524,
    0x88034B96, 0xA0B00000, 0x40040000, 0x96CBE3F9, 0x990E91A8, 0xA1E00000, 0x40040000, 0x9D1462CE, 0xAA19D7B9,
    0x21580000, 0x40040000, 0xA35CE1A3, 0xBB251DCB, 0xA1100000, 0x40040000, 0xA9A56078, 0xCC3063DD, 0xA1FC0000,
    0x40040000, 0xAFEDDF4D, 0xDD3BA9EE, 0x21200000, 0x40040000, 0xB6365E22, 0xEE46F000, 0xA1480000, 0x40040000,
    0xBC7EDCF7, 0xFF523611, 0x21E80000, 0x40040000, 0xC2C75BCD, 0x105D7C23, 0x20D00000, 0x40040000, 0xC90FDAA2,
    0x2168C235, 0xA1800000,
};

// Fetch PITBL Y1 (extended) for entry i (0=N=-32, 32=N=0, 64=N=+32)
static inline fpu_unpacked_t pitbl_y1(int i) {
    return fpsp_ext(pitbl_data[i * 4], pitbl_data[i * 4 + 1], pitbl_data[i * 4 + 2]);
}

// Fetch PITBL Y2 (single correction) for entry i
static inline fpu_unpacked_t pitbl_y2(int i) {
    return fpsp_sgl(pitbl_data[i * 4 + 3]);
}

// Trig argument reduction result
typedef struct {
    fpu_unpacked_t r; // reduced argument, |r| <= π/4
    int32_t n; // integer quotient (N)
} trig_reduced_t;

// General argument reduction (REDUCEX) for |X| > 15π
// Iterative Cody-Waite with double-length intermediate (R, r)
static trig_reduced_t trig_reduce_general(fpu_state_t *fpu, fpu_unpacked_t x) {
    trig_reduced_t result;
    fpu_unpacked_t fp0 = x; // R
    fpu_unpacked_t fp1; // r (low part)
    fp1.sign = false;
    fp1.exponent = FPU_EXP_ZERO;
    fp1.mantissa_hi = 0;
    fp1.mantissa_lo = 0;

    // Special case: if compact form = 0x7ffeffff (max finite), pre-reduce by
    // one pi/2 step to avoid overflow
    uint16_t biased = (uint16_t)(x.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)biased << 16) | (uint32_t)(x.mantissa_hi >> 48);
    if (compact == 0x7FFEFFFF) {
        // Create 2^16383 * pi/2 in two parts
        fpu_unpacked_t piby2_hi = fpsp_ext(0x7FFE0000, 0xC90FDAA2, 0x00000000);
        fpu_unpacked_t piby2_lo = fpsp_ext(0x7FDC0000, 0x85A308D3, 0x00000000);
        // Negate for positive argument, keep for negative
        if (!x.sign) {
            piby2_hi.sign = true;
            piby2_lo.sign = true;
        }
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, piby2_hi)); // high reduction
        fp1 = fp0; // save
        fp0 = fp_rnd64(fpu_op_add(fpu, fp0, piby2_lo)); // low reduction
        fp1 = fp_rnd64(fpu_op_sub(fpu, fp1, fp0)); // determine low comp
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, piby2_lo)); // fp0/fp1 reduced
    }

    int32_t n_final = 0;
    // max exponent 16383, each iteration reduces by ~27 → at most ~608 iters
    int max_iter = 700;
    for (;;) {
        if (--max_iter <= 0)
            break; // guard against non-termination
        // K = exponent of FP0
        int32_t k = fp0.exponent;
        int32_t l;
        bool is_last;
        if (k <= 28) {
            l = 0;
            is_last = true;
        } else {
            l = k - 27;
            is_last = false;
        }

        // Create 2^(-L) * (2/π) in extended
        fpu_unpacked_t inv_twopi;
        inv_twopi.sign = false;
        inv_twopi.exponent = -1 - l; // 2^(-L) * (2/π)
        inv_twopi.mantissa_hi = 0xA2F9836E4E44152AULL;
        inv_twopi.mantissa_lo = 0;

        // FP2 = FP0 * 2^(-L) * (2/π)
        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, fp0, inv_twopi));

        // Round FP2 to integer using sign(X)*2^63 trick
        // This matches the FPSP behavior exactly
        uint32_t twoto63_val = x.sign ? 0xDF000000 : 0x5F000000;
        fpu_unpacked_t twoto63 = fpsp_sgl(twoto63_val);
        fp2 = fp_rnd64(fpu_op_add(fpu, fp2, twoto63)); // round fractional
        fp2 = fp_rnd64(fpu_op_sub(fpu, fp2, twoto63)); // N as float

        // Create 2^L * (π/2) in two parts: PIby2_1 (exact) and PIby2_2
        fpu_unpacked_t piby2_1;
        piby2_1.sign = false;
        piby2_1.exponent = l; // 2^L * π/2
        piby2_1.mantissa_hi = 0xC90FDAA200000000ULL; // upper 32 bits
        piby2_1.mantissa_lo = 0;

        fpu_unpacked_t piby2_2;
        piby2_2.sign = false;
        piby2_2.exponent = l - 34; // 2^(L-34)
        piby2_2.mantissa_hi = 0x85A308D300000000ULL; // next 32 bits
        piby2_2.mantissa_lo = 0;

        // Compensated subtraction: (R+r) - N*P1 - N*P2
        // W = N*P1, w = N*P2
        fpu_unpacked_t w_big = fp_rnd64(fpu_op_mul(fpu, fp2, piby2_1)); // W
        fpu_unpacked_t w_sml = fp_rnd64(fpu_op_mul(fpu, fp2, piby2_2)); // w
        fpu_unpacked_t p = fp_rnd64(fpu_op_add(fpu, w_big, w_sml)); // P = W+w
        fpu_unpacked_t wp = fp_rnd64(fpu_op_sub(fpu, w_big, p)); // W-P

        fpu_unpacked_t a = fp_rnd64(fpu_op_sub(fpu, fp0, p)); // A = R-P
        fpu_unpacked_t pp = fp_rnd64(fpu_op_add(fpu, wp, w_sml)); // p = (W-P)+w

        fpu_unpacked_t a_save = a;
        fpu_unpacked_t a_part = fp_rnd64(fpu_op_sub(fpu, fp1, pp)); // a = r-p

        fp0 = fp_rnd64(fpu_op_add(fpu, a, a_part)); // R = A+a

        if (is_last) {
            n_final = fpu_to_int32_rne(fp2);
            break;
        }

        // Need to calculate r for next iteration
        fpu_unpacked_t ar = fp_rnd64(fpu_op_sub(fpu, a_save, fp0)); // A-R
        fp1 = fp_rnd64(fpu_op_add(fpu, ar, a_part)); // r = (A-R)+a
    }

    result.r = fp0;
    result.n = n_final;
    return result;
}

// Fast argument reduction for |X| <= 15π using PITBL lookup
static trig_reduced_t trig_reduce_fast(fpu_state_t *fpu, fpu_unpacked_t x) {
    trig_reduced_t result;

    // N = round(X * 2/π) → use double-precision 2/π
    fpu_unpacked_t twobypi = fpsp_dbl(0x3FE45F30, 0x6DC9C883);
    fpu_unpacked_t fp1 = fp_rnd64(fpu_op_mul(fpu, x, twobypi)); // X*2/π
    int32_t n = fpu_to_int32_rne(fp1); // N = round()

    // Clamp N to [-32, 32] (should not exceed for |X| <= 15π)
    // Look up N*π/2 from PITBL: entry index = N + 32
    int idx = n + 32;
    fpu_unpacked_t y1 = pitbl_y1(idx); // Y1 (extended)
    fpu_unpacked_t y2 = pitbl_y2(idx); // Y2 (single)

    // R = (X - Y1) - Y2
    fpu_unpacked_t fp0 = fp_rnd64(fpu_op_sub(fpu, x, y1)); // X - Y1
    fp0 = fp_rnd64(fpu_op_sub(fpu, fp0, y2)); // (X-Y1) - Y2

    result.r = fp0;
    result.n = n;
    return result;
}

// Evaluate sin polynomial (standalone SIN/COS form, T=S*S split)
// Computes sgn*sin(r) where sgn is determined by negate flag
// Returns: R' + R'*S*([A1+T(A3+T(A5+TA7))] + [S(A2+T(A4+TA6))])
static fpu_unpacked_t sin_poly(fpu_state_t *fpu, fpu_unpacked_t r, bool negate, uint32_t saved_fpcr) {
    // R' = negate ? -r : r
    fpu_unpacked_t rp = r;
    if (negate)
        rp.sign = !rp.sign;

    fpu_unpacked_t s = fp_rnd64(fpu_op_mul(fpu, r, r)); // S = R*R

    // Coefficients (A7..A3 are doubles, A2..A1 are extended)
    fpu_unpacked_t a7 = fpsp_dbl(0xBD6AAA77, 0xCCC994F5);
    fpu_unpacked_t a6 = fpsp_dbl(0x3DE61209, 0x7AAE8DA1);
    fpu_unpacked_t a5 = fpsp_dbl(0xBE5AE645, 0x2A118AE4);
    fpu_unpacked_t a4 = fpsp_dbl(0x3EC71DE3, 0xA5341531);
    fpu_unpacked_t a3 = fpsp_dbl(0xBF2A01A0, 0x1A018B59);
    fpu_unpacked_t a2 = fpsp_ext(0x3FF80000, 0x88888888, 0x888859AF);
    fpu_unpacked_t a1 = fpsp_ext(0xBFFC0000, 0xAAAAAAAA, 0xAAAAAA99);

    fpu_unpacked_t t = fp_rnd64(fpu_op_mul(fpu, s, s)); // T = S*S

    // Odd path: A7, A5, A3 via T
    fpu_unpacked_t fp3 = fp_rnd64(fpu_op_mul(fpu, t, a7)); // TA7
    fp3 = fp_rnd64(fpu_op_add(fpu, a5, fp3)); // A5+TA7
    fp3 = fp_rnd64(fpu_op_mul(fpu, t, fp3)); // T(A5+TA7)
    fp3 = fp_rnd64(fpu_op_add(fpu, a3, fp3)); // A3+T(A5+TA7)

    // Even path: A6, A4, A2 via T
    fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, t, a6)); // TA6
    fp2 = fp_rnd64(fpu_op_add(fpu, a4, fp2)); // A4+TA6
    fp2 = fp_rnd64(fpu_op_mul(fpu, t, fp2)); // T(A4+TA6)
    fp2 = fp_rnd64(fpu_op_add(fpu, a2, fp2)); // A2+T(A4+TA6)

    fpu_unpacked_t fp1 = fp_rnd64(fpu_op_mul(fpu, fp3, t)); // T(A3+T(A5+TA7))
    fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S(A2+T(A4+TA6))
    fp1 = fp_rnd64(fpu_op_add(fpu, a1, fp1)); // A1+T(A3+T(A5+TA7))
    fpu_unpacked_t rs = fp_rnd64(fpu_op_mul(fpu, rp, s)); // R'*S
    fp1 = fp_rnd64(fpu_op_add(fpu, fp2, fp1)); // [A1+...]+[S(A2+...)]
    fpu_unpacked_t fp0 = fp_rnd64(fpu_op_mul(fpu, fp1, rs)); // R'*S*(poly)

    // Final add R' with user FPCR
    fpu->fpcr = saved_fpcr;
    fp0 = fpu_op_add(fpu, rp, fp0); // R' + R'*S*(poly)
    return fp0;
}

// Evaluate cos polynomial (standalone SIN/COS form, T=S*S split)
// Computes SGN*cos(r) where SGN is +-1 determined by negate flag
// Returns: SGN + S'*([B1+T(B3+T(B5+TB7))] + [S(B2+T(B4+T(B6+TB8)))])
static fpu_unpacked_t cos_poly(fpu_state_t *fpu, fpu_unpacked_t r, bool negate, uint32_t saved_fpcr) {
    fpu_unpacked_t s = fp_rnd64(fpu_op_mul(fpu, r, r)); // S = R*R

    // Coefficients (B8..B4 are doubles, B3..B2 are ext, B1 = -0.5 single)
    fpu_unpacked_t b8 = fpsp_dbl(0x3D2AC4D0, 0xD6011EE3);
    fpu_unpacked_t b7 = fpsp_dbl(0xBDA9396F, 0x9F45AC19);
    fpu_unpacked_t b6 = fpsp_dbl(0x3E21EED9, 0x0612C972);
    fpu_unpacked_t b5 = fpsp_dbl(0xBE927E4F, 0xB79D9FCF);
    fpu_unpacked_t b4 = fpsp_dbl(0x3EFA01A0, 0x1A01D423);
    fpu_unpacked_t b3 = fpsp_ext(0xBFF50000, 0xB60B60B6, 0x0B61D438);
    fpu_unpacked_t b2_ext = fpsp_ext(0x3FFA0000, 0xAAAAAAAA, 0xAAAAAB5E);

    fpu_unpacked_t t = fp_rnd64(fpu_op_mul(fpu, s, s)); // T = S*S

    // S' = negate ? -S : S
    fpu_unpacked_t sp = s;
    if (negate)
        sp.sign = !sp.sign;

    // SGN as single: +1.0 or -1.0
    fpu_unpacked_t sgn = fpsp_sgl(negate ? 0xBF800000 : 0x3F800000);

    // Odd path: B7, B5, B3 via T
    fpu_unpacked_t fp3 = fp_rnd64(fpu_op_mul(fpu, t, b8)); // TB8
    fpu_unpacked_t fp3b = fp_rnd64(fpu_op_mul(fpu, t, b7)); // TB7
    fp3 = fp_rnd64(fpu_op_add(fpu, b6, fp3)); // B6+TB8
    fp3b = fp_rnd64(fpu_op_add(fpu, b5, fp3b)); // B5+TB7
    fp3 = fp_rnd64(fpu_op_mul(fpu, t, fp3)); // T(B6+TB8)
    fp3b = fp_rnd64(fpu_op_mul(fpu, t, fp3b)); // T(B5+TB7)
    fp3 = fp_rnd64(fpu_op_add(fpu, b4, fp3)); // B4+T(B6+TB8)
    fp3b = fp_rnd64(fpu_op_add(fpu, b3, fp3b)); // B3+T(B5+TB7)
    fp3 = fp_rnd64(fpu_op_mul(fpu, t, fp3)); // T(B4+T(B6+TB8))
    fpu_unpacked_t fp1 = fp_rnd64(fpu_op_mul(fpu, fp3b, t)); // T(B3+T(B5+TB7))
    fpu_unpacked_t fp2 = fp_rnd64(fpu_op_add(fpu, b2_ext, fp3)); // B2+T(B4+...)
    // B1 = -0.5 as single
    fpu_unpacked_t b1 = fpsp_sgl(0xBF000000);
    fp1 = fp_rnd64(fpu_op_add(fpu, b1, fp1)); // B1+T(B3+T(B5+TB7))

    fpu_unpacked_t fp0 = fp_rnd64(fpu_op_mul(fpu, fp2, s)); // S(B2+T(B4+...))
    fp0 = fp_rnd64(fpu_op_add(fpu, fp1, fp0)); // [B1+...]+[S(B2+...)]
    fp0 = fp_rnd64(fpu_op_mul(fpu, sp, fp0)); // S'*(poly)

    // Final add SGN with user FPCR
    fpu->fpcr = saved_fpcr;
    fp0 = fpu_op_add(fpu, sgn, fp0); // SGN + S'*(poly)
    return fp0;
}

// FSIN: sine (opcode 0x0E)
fpu_unpacked_t fpu_op_sin(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // Special cases: zero, NaN, infinity
    if (src.exponent == FPU_EXP_ZERO) {
        return src; // sin(+-0) = +-0
    }
    if (src.exponent == FPU_EXP_INF) {
        if (src.mantissa_hi != 0) {
            // NaN
            if (fp80_is_snan(raw))
                fpu->fpsr |= FPEXC_SNAN;
            fpu_unpacked_t nan = src;
            nan.mantissa_hi |= 0x4000000000000000ULL;
            return nan;
        }
        // sin(inf) = OPERR
        fpu->fpsr |= FPEXC_OPERR;
        return (fpu_unpacked_t){0, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
    }

    // Save and clear FPCR
    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    // Compute compact form
    uint16_t biased = (uint16_t)(src.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)biased << 16) | (uint32_t)(src.mantissa_hi >> 48);

    // Tiny: |X| < 2^(-40)
    if (compact < 0x3FD78000) {
        fpu->fpcr = saved_fpcr;
        fpu->fpsr |= FPEXC_INEX2;
        return src; // sin(X) ≈ X
    }

    // Argument reduction
    trig_reduced_t red;
    if (compact < 0x4004BC7E) {
        red = trig_reduce_fast(fpu, src); // |X| < 15π
    } else {
        red = trig_reduce_general(fpu, src); // |X| >= 15π
    }

    // k = (N + AdjN) where AdjN=0 for sin
    int32_t k = red.n;
    int32_t k_mod4 = ((k % 4) + 4) % 4; // ensure positive
    bool is_odd = (k_mod4 & 1) != 0;

    fpu_unpacked_t result;
    if (!is_odd) {
        // k even: result = (-1)^(k/2) * sin(r)
        bool negate = (k_mod4 == 2);
        result = sin_poly(fpu, red.r, negate, saved_fpcr);
    } else {
        // k odd: result = (-1)^((k-1)/2) * cos(r)
        bool negate = (k_mod4 == 3);
        result = cos_poly(fpu, red.r, negate, saved_fpcr);
    }

    fpu->fpsr |= FPEXC_INEX2;
    return result;
}

// FCOS: cosine (opcode 0x1D)
fpu_unpacked_t fpu_op_cos(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // Special cases
    if (src.exponent == FPU_EXP_ZERO) {
        // cos(+-0) = 1.0
        return fpsp_sgl(0x3F800000);
    }
    if (src.exponent == FPU_EXP_INF) {
        if (src.mantissa_hi != 0) {
            if (fp80_is_snan(raw))
                fpu->fpsr |= FPEXC_SNAN;
            fpu_unpacked_t nan = src;
            nan.mantissa_hi |= 0x4000000000000000ULL;
            return nan;
        }
        // cos(inf) = OPERR
        fpu->fpsr |= FPEXC_OPERR;
        return (fpu_unpacked_t){0, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
    }

    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    uint16_t biased = (uint16_t)(src.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)biased << 16) | (uint32_t)(src.mantissa_hi >> 48);

    // Tiny: |X| < 2^(-40), cos(X) = 1 - tiny
    if (compact < 0x3FD78000) {
        fpu_unpacked_t one = fpsp_sgl(0x3F800000);
        fpu_unpacked_t tiny = fpsp_sgl(0x00800000);
        fpu->fpcr = saved_fpcr;
        fpu_unpacked_t result = fpu_op_sub(fpu, one, tiny);
        fpu->fpsr |= FPEXC_INEX2;
        return result;
    }

    trig_reduced_t red;
    if (compact < 0x4004BC7E) {
        red = trig_reduce_fast(fpu, src);
    } else {
        red = trig_reduce_general(fpu, src);
    }

    // k = (N + AdjN) where AdjN=1 for cos
    int32_t k = red.n + 1;
    int32_t k_mod4 = ((k % 4) + 4) % 4;
    bool is_odd = (k_mod4 & 1) != 0;

    fpu_unpacked_t result;
    if (!is_odd) {
        bool negate = (k_mod4 == 2);
        result = sin_poly(fpu, red.r, negate, saved_fpcr);
    } else {
        bool negate = (k_mod4 == 3);
        result = cos_poly(fpu, red.r, negate, saved_fpcr);
    }

    fpu->fpsr |= FPEXC_INEX2;
    return result;
}

// FSINCOS: sin and cos computed together (opcodes 0x30-0x37)
// Returns sin(X). Stores cos(X) into fpu->fp[cos_reg].
fpu_unpacked_t fpu_op_sincos(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw, int cos_reg) {
    // Special cases
    if (src.exponent == FPU_EXP_ZERO) {
        // sin(0) = +-0, cos(0) = 1
        fpu->fp[cos_reg] = fpu_pack(fpu, fpsp_sgl(0x3F800000));
        return src;
    }
    if (src.exponent == FPU_EXP_INF) {
        if (src.mantissa_hi != 0) {
            // NaN
            if (fp80_is_snan(raw))
                fpu->fpsr |= FPEXC_SNAN;
            fpu_unpacked_t nan = src;
            nan.mantissa_hi |= 0x4000000000000000ULL;
            fpu->fp[cos_reg] = fpu_pack(fpu, nan);
            return nan;
        }
        // sincos(inf) = OPERR, both NaN
        fpu->fpsr |= FPEXC_OPERR;
        fpu_unpacked_t nan = {0, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
        fpu->fp[cos_reg] = fpu_pack(fpu, nan);
        return nan;
    }

    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    uint16_t biased = (uint16_t)(src.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)biased << 16) | (uint32_t)(src.mantissa_hi >> 48);

    // Tiny: |X| < 2^(-40)
    if (compact < 0x3FD78000) {
        // cos(X) = 1 - tiny, sin(X) = X
        fpu_unpacked_t one = fpsp_sgl(0x3F800000);
        fpu_unpacked_t tiny = fpsp_sgl(0x00800000);
        fpu->fpcr = saved_fpcr;
        fpu_unpacked_t cos_result = fpu_op_sub(fpu, one, tiny);
        fpu->fp[cos_reg] = fpu_pack(fpu, cos_result);
        fpu->fpsr |= FPEXC_INEX2;
        return src;
    }

    trig_reduced_t red;
    if (compact < 0x4004BC7E) {
        red = trig_reduce_fast(fpu, src);
    } else {
        red = trig_reduce_general(fpu, src);
    }

    int32_t n = red.n;
    fpu_unpacked_t r = red.r;

    // SINCOS uses straight Horner evaluation (NOT T=S*S split)
    // and computes both sin and cos polynomials interleaved
    fpu_unpacked_t s = fp_rnd64(fpu_op_mul(fpu, r, r)); // S = R*R

    // Coefficients
    fpu_unpacked_t a7 = fpsp_dbl(0xBD6AAA77, 0xCCC994F5);
    fpu_unpacked_t a6 = fpsp_dbl(0x3DE61209, 0x7AAE8DA1);
    fpu_unpacked_t a5 = fpsp_dbl(0xBE5AE645, 0x2A118AE4);
    fpu_unpacked_t a4 = fpsp_dbl(0x3EC71DE3, 0xA5341531);
    fpu_unpacked_t a3 = fpsp_dbl(0xBF2A01A0, 0x1A018B59);
    fpu_unpacked_t a2 = fpsp_ext(0x3FF80000, 0x88888888, 0x888859AF);
    fpu_unpacked_t a1 = fpsp_ext(0xBFFC0000, 0xAAAAAAAA, 0xAAAAAA99);
    fpu_unpacked_t b8 = fpsp_dbl(0x3D2AC4D0, 0xD6011EE3);
    fpu_unpacked_t b7 = fpsp_dbl(0xBDA9396F, 0x9F45AC19);
    fpu_unpacked_t b6 = fpsp_dbl(0x3E21EED9, 0x0612C972);
    fpu_unpacked_t b5 = fpsp_dbl(0xBE927E4F, 0xB79D9FCF);
    fpu_unpacked_t b4 = fpsp_dbl(0x3EFA01A0, 0x1A01D423);
    fpu_unpacked_t b3 = fpsp_ext(0xBFF50000, 0xB60B60B6, 0x0B61D438);
    fpu_unpacked_t b2_ext = fpsp_ext(0x3FFA0000, 0xAAAAAAAA, 0xAAAAAB5E);
    fpu_unpacked_t b1 = fpsp_sgl(0xBF000000);

    int32_t n_mod4 = ((n % 4) + 4) % 4;
    bool n_is_odd = (n_mod4 & 1) != 0;

    fpu_unpacked_t sin_result, cos_result;

    if (n_is_odd) {
        // NODD: sin(X) = sgn1*cos(r), cos(X) = sgn2*sin(r)
        // j1 = (k-1)/2 for sin, j2 = j1 XOR (k mod 2) for cos
        // sgn1 = (-1)^j1, sgn2 = (-1)^j2 where k = N mod 4
        // For N odd: bit1 of N determines j1, bit0 XOR bit1 determines j2
        bool sin_cos_negate; // negate cos poly result for sin output
        bool cos_sin_negate; // negate sin poly result for cos output

        // Following the FPSP NODD sign logic:
        // d0 = N ROR 1 (bit31 = bit0 of N = 1 since N odd)
        // d2 = d0 ← N ROR 1
        // d2 ROR 1 → N ROR 2: bit31 = bit1 of N
        // d2 AND 0x80000000 → bit1 of N in sign pos
        // d2 XOR d0 → (bit1 of N) XOR (N ROR 1's bit31 = bit0 of N = 1)
        //           = bit1 XOR 1 = NOT bit1
        // d2 AND 0x80000000 → NOT bit1 in sign pos → negate cos_sin

        // cos result sign (from RPRIME modification):
        // cos_sin_negate = (bit1 of N) XOR 1 = NOT bit1
        // For N mod 4 = 1: bit1=0, cos_sin_negate = true
        // For N mod 4 = 3: bit1=1, cos_sin_negate = false
        cos_sin_negate = (n_mod4 == 1);

        // d0 after second ROR: d0 ROR 1 again → N ROR 2: bit31 = bit1 of N
        // AND 0x80000000 → bit1 in sign pos
        // SGN for sin = POSNEG1 = -(bit1 of N as sign)
        // Actually: movel #0x3F800000,POSNEG1; eorl %d0,POSNEG1
        // d0 has bit1 of N in bit31. EOR with 0x3F800000 → if bit1=0: +1.0, bit1=1: -1.0
        sin_cos_negate = (n_mod4 == 3);

        // Interleaved Horner computation (NODD):
        // sin(X) uses cos poly (fp0 path → POSNEG1 + S'*(B1+...))
        // cos(X) uses sin poly (fp1 path → R' + R'*S*(A1+...))

        // Sin polynomial (straight Horner): for cos(X) output
        fpu_unpacked_t fp_sin = a7; // A7
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin)); // SA7
        fp_sin = fp_rnd64(fpu_op_add(fpu, a6, fp_sin)); // A6+SA7
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin)); // S(A6+SA7)
        fp_sin = fp_rnd64(fpu_op_add(fpu, a5, fp_sin)); // A5+...
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin)); // S(A5+...)
        fp_sin = fp_rnd64(fpu_op_add(fpu, a4, fp_sin)); // A4+...
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin)); // S(A4+...)
        fp_sin = fp_rnd64(fpu_op_add(fpu, a3, fp_sin)); // A3+...
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin)); // S(A3+...)
        fp_sin = fp_rnd64(fpu_op_add(fpu, a2, fp_sin)); // A2+...
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin)); // S(A2+...)
        fp_sin = fp_rnd64(fpu_op_add(fpu, a1, fp_sin)); // A1+...
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin)); // S(A1+...)

        // Cos polynomial (straight Horner): for sin(X) output
        fpu_unpacked_t fp_cos = b8; // B8
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos)); // SB8
        fp_cos = fp_rnd64(fpu_op_add(fpu, b7, fp_cos)); // B7+SB8
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos)); // S(B7+SB8)
        fp_cos = fp_rnd64(fpu_op_add(fpu, b6, fp_cos)); // B6+...
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos)); // S(B6+...)
        fp_cos = fp_rnd64(fpu_op_add(fpu, b5, fp_cos)); // B5+...
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos)); // S(B5+...)
        fp_cos = fp_rnd64(fpu_op_add(fpu, b4, fp_cos)); // B4+...
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos)); // S(B4+...)
        fp_cos = fp_rnd64(fpu_op_add(fpu, b3, fp_cos)); // B3+...
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos)); // S(B3+...)
        fp_cos = fp_rnd64(fpu_op_add(fpu, b2_ext, fp_cos)); // B2+...
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos)); // S(B2+...)

        // Finish sin polynomial: R'*S(A1+...) for cos output
        fpu_unpacked_t rp = r;
        if (cos_sin_negate)
            rp.sign = !rp.sign; // R' = sgn*R
        fp_sin = fp_rnd64(fpu_op_mul(fpu, rp, fp_sin)); // R'*S(A1+...)

        // Finish cos polynomial: B1+S(B2+...) for sin output
        fp_cos = fp_rnd64(fpu_op_add(fpu, b1, fp_cos)); // B1+S(B2+...)
        fpu_unpacked_t sp = s;
        if (sin_cos_negate)
            sp.sign = !sp.sign; // S' for sin
        fp_cos = fp_rnd64(fpu_op_mul(fpu, sp, fp_cos)); // S'*(B1+...)

        // cos(X) = R' + R'*S(A1+...)
        fpu->fpcr = saved_fpcr;
        cos_result = fpu_op_add(fpu, rp, fp_sin);
        fpu->fp[cos_reg] = fpu_pack(fpu, cos_result);

        // sin(X) = SGN + S'*(B1+...)
        fpu_unpacked_t sgn_sin = fpsp_sgl(sin_cos_negate ? 0xBF800000 : 0x3F800000);
        sin_result = fpu_op_add(fpu, sgn_sin, fp_cos);

    } else {
        // NEVEN: sin(X) = sgn*sin(r), cos(X) = sgn*cos(r)
        // j = N/2 mod 2, sgn = (-1)^j
        // Same sgn for both sin and cos
        // d0 = N ROR 1: bit31 = bit0 of N = 0 since even
        // d0 ROR 1: bit31 = bit1 of N (from position 0 of ROR'd value)
        // Actually: after first ROR (at SCCONT), d0[31]=bit0=0, d0[0]=bit1
        // At NEVEN: d0 ROR 1 again: d0[31] = former d0[0] = bit1 of N
        // AND 0x80000000: bit1 of N in sign position
        bool negate = ((n_mod4 == 2) || (n_mod4 == 6));
        // Actually, n_mod4 can only be 0 or 2 since N is even
        negate = (n_mod4 == 2);

        // Both use straight Horner interleaved
        fpu_unpacked_t rp = r;
        if (negate)
            rp.sign = !rp.sign;

        fpu_unpacked_t sp = s;
        if (negate)
            sp.sign = !sp.sign;

        fpu_unpacked_t sgn_val = fpsp_sgl(negate ? 0xBF800000 : 0x3F800000);

        // Cos polynomial path (fp1 in FPSP): straight Horner
        fpu_unpacked_t fp_cos = b8;
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos));
        fp_cos = fp_rnd64(fpu_op_add(fpu, b7, fp_cos));
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos));
        fp_cos = fp_rnd64(fpu_op_add(fpu, b6, fp_cos));
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos));
        fp_cos = fp_rnd64(fpu_op_add(fpu, b5, fp_cos));
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos));
        fp_cos = fp_rnd64(fpu_op_add(fpu, b4, fp_cos));
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos));
        fp_cos = fp_rnd64(fpu_op_add(fpu, b3, fp_cos));
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos));
        fp_cos = fp_rnd64(fpu_op_add(fpu, b2_ext, fp_cos));
        fp_cos = fp_rnd64(fpu_op_mul(fpu, s, fp_cos));

        // Sin polynomial path (fp2 in FPSP): straight Horner
        fpu_unpacked_t fp_sin = a7;
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin));
        fp_sin = fp_rnd64(fpu_op_add(fpu, a6, fp_sin));
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin));
        fp_sin = fp_rnd64(fpu_op_add(fpu, a5, fp_sin));
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin));
        fp_sin = fp_rnd64(fpu_op_add(fpu, a4, fp_sin));
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin));
        fp_sin = fp_rnd64(fpu_op_add(fpu, a3, fp_sin));
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin));
        fp_sin = fp_rnd64(fpu_op_add(fpu, a2, fp_sin));
        fp_sin = fp_rnd64(fpu_op_mul(fpu, s, fp_sin));
        fp_sin = fp_rnd64(fpu_op_add(fpu, a1, fp_sin));
        fp_sin = fp_rnd64(fpu_op_mul(fpu, fp_sin, s));

        // Finish: cos = B1+... then S'*(...)
        fp_cos = fp_rnd64(fpu_op_add(fpu, b1, fp_cos)); // B1+S(B2+...)
        fp_sin = fp_rnd64(fpu_op_mul(fpu, rp, fp_sin)); // R'*S(A1+...)
        fp_cos = fp_rnd64(fpu_op_mul(fpu, sp, fp_cos)); // S'*(B1+...)

        // cos(X) = SGN + S'*(B1+...)
        fpu->fpcr = saved_fpcr;
        cos_result = fpu_op_add(fpu, sgn_val, fp_cos);
        fpu->fp[cos_reg] = fpu_pack(fpu, cos_result);

        // sin(X) = R' + R'*S(A1+...)
        sin_result = fpu_op_add(fpu, rp, fp_sin);
    }

    fpu->fpsr |= FPEXC_INEX2;
    return sin_result;
}

// FTAN: tangent (opcode 0x0F)
fpu_unpacked_t fpu_op_tan(fpu_state_t *fpu, fpu_unpacked_t src, float80_reg_t raw) {
    // Special cases
    if (src.exponent == FPU_EXP_ZERO) {
        return src; // tan(+-0) = +-0
    }
    if (src.exponent == FPU_EXP_INF) {
        if (src.mantissa_hi != 0) {
            if (fp80_is_snan(raw))
                fpu->fpsr |= FPEXC_SNAN;
            fpu_unpacked_t nan = src;
            nan.mantissa_hi |= 0x4000000000000000ULL;
            return nan;
        }
        // tan(inf) = OPERR
        fpu->fpsr |= FPEXC_OPERR;
        return (fpu_unpacked_t){0, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
    }

    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    uint16_t biased = (uint16_t)(src.exponent + FPU_EXP_BIAS);
    uint32_t compact = ((uint32_t)biased << 16) | (uint32_t)(src.mantissa_hi >> 48);

    // Tiny: |X| < 2^(-40)
    if (compact < 0x3FD78000) {
        fpu->fpcr = saved_fpcr;
        fpu->fpsr |= FPEXC_INEX2;
        return src;
    }

    trig_reduced_t red;
    if (compact < 0x4004BC7E) {
        red = trig_reduce_fast(fpu, src);
    } else {
        red = trig_reduce_general(fpu, src);
    }

    fpu_unpacked_t r = red.r;
    // For tan: k = N mod 2, where d0 = N. After rorl #5, andil #0x80000000:
    // bit31 = bit 4 of N... wait, that's not right.
    // FPSP computes N*16 for table indexing, then uses rorl #5 + andil
    // to extract bit 0 of N (the parity bit) into the sign position.
    // So: N is odd iff the extracted bit is set.
    bool n_is_odd = (red.n & 1) != 0;

    // Rational function: U = r + r*s*(P1 + s*(P2 + s*P3))
    //                    V = 1 + s*(Q1 + s*(Q2 + s*(Q3 + s*Q4)))
    fpu_unpacked_t p3 = fpsp_dbl(0xBEF2BAA5, 0xA8924F04);
    fpu_unpacked_t p2 = fpsp_ext(0x3FF60000, 0xE073D3FC, 0x199C4A00);
    fpu_unpacked_t p1 = fpsp_ext(0xBFFC0000, 0x8895A6C5, 0xFB423BCA);
    fpu_unpacked_t q4 = fpsp_dbl(0x3EA0B759, 0xF50F8688);
    fpu_unpacked_t q3 = fpsp_ext(0xBF346F59, 0xB39BA65F, 0x00000000);
    fpu_unpacked_t q2 = fpsp_ext(0x3FF90000, 0xD23CD684, 0x15D95FA1);
    fpu_unpacked_t q1 = fpsp_ext(0xBFFD0000, 0xEEF57E0D, 0xA84BC8CE);

    fpu_unpacked_t s = fp_rnd64(fpu_op_mul(fpu, r, r)); // S = R*R

    if (!n_is_odd) {
        // Even: tan(r) = U/V
        // Following FPSP TANMAIN (even case):
        fpu_unpacked_t fp1 = r; // save R

        fpu_unpacked_t fp3 = fp_rnd64(fpu_op_mul(fpu, s, q4)); // SQ4
        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, s, p3)); // SP3
        fp3 = fp_rnd64(fpu_op_add(fpu, q3, fp3)); // Q3+SQ4
        fp2 = fp_rnd64(fpu_op_add(fpu, p2, fp2)); // P2+SP3
        fp3 = fp_rnd64(fpu_op_mul(fpu, s, fp3)); // S(Q3+SQ4)
        fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S(P2+SP3)
        fp3 = fp_rnd64(fpu_op_add(fpu, q2, fp3)); // Q2+S(Q3+SQ4)
        fp2 = fp_rnd64(fpu_op_add(fpu, p1, fp2)); // P1+S(P2+SP3)
        fp3 = fp_rnd64(fpu_op_mul(fpu, s, fp3)); // S(Q2+S(Q3+SQ4))
        fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2)); // S(P1+S(P2+SP3))
        fp3 = fp_rnd64(fpu_op_add(fpu, q1, fp3)); // Q1+S(Q2+...)
        fp2 = fp_rnd64(fpu_op_mul(fpu, r, fp2)); // RS(P1+...)
        fp1 = fp_rnd64(fpu_op_mul(fpu, fp3, s)); // S(Q1+...)
        fpu_unpacked_t fp0 = fp_rnd64(fpu_op_add(fpu, r, fp2)); // R+RS(P1+...)
        fpu_unpacked_t one = fpsp_sgl(0x3F800000);
        fp1 = fp_rnd64(fpu_op_add(fpu, one, fp1)); // 1+S(Q1+...)

        // Final divide with user FPCR
        fpu->fpcr = saved_fpcr;
        fp0 = fpu_op_div(fpu, fp0, fp1);
        fpu->fpsr |= FPEXC_INEX2;
        return fp0;
    } else {
        // Odd: tan = -V/U
        fpu_unpacked_t fp1 = r; // save R

        fpu_unpacked_t fp3 = fp_rnd64(fpu_op_mul(fpu, s, q4));
        fpu_unpacked_t fp2 = fp_rnd64(fpu_op_mul(fpu, s, p3));
        fp3 = fp_rnd64(fpu_op_add(fpu, q3, fp3));
        fp2 = fp_rnd64(fpu_op_add(fpu, p2, fp2));
        fp3 = fp_rnd64(fpu_op_mul(fpu, s, fp3));
        fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2));
        fp3 = fp_rnd64(fpu_op_add(fpu, q2, fp3));
        fp2 = fp_rnd64(fpu_op_add(fpu, p1, fp2));
        fp3 = fp_rnd64(fpu_op_mul(fpu, s, fp3));
        fp2 = fp_rnd64(fpu_op_mul(fpu, s, fp2));
        fp3 = fp_rnd64(fpu_op_add(fpu, q1, fp3));
        fp2 = fp_rnd64(fpu_op_mul(fpu, fp1, fp2)); // RS(P1+...)
        fpu_unpacked_t fp0 = fp_rnd64(fpu_op_mul(fpu, fp3, s)); // S(Q1+...)
        fp1 = fp_rnd64(fpu_op_add(fpu, fp1, fp2)); // R+RS(P1+...)
        fpu_unpacked_t one = fpsp_sgl(0x3F800000);
        fp0 = fp_rnd64(fpu_op_add(fpu, one, fp0)); // 1+S(Q1+...)

        // Negate U: -U
        fp1.sign = !fp1.sign;

        // Final divide: -V/U → fp0/fp1... wait, we have V in fp0, -U in fp1
        // Result = fp0 / fp1 = V / (-U) = -V/U ✓
        fpu->fpcr = saved_fpcr;
        fp0 = fpu_op_div(fpu, fp0, fp1);
        fpu->fpsr |= FPEXC_INEX2;
        return fp0;
    }
}
