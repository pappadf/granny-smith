// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fpu_packed.c
// Motorola 68882 FPU packed decimal (FMOVE.P) conversions, built on the
// soft-float core (fpu_op_mul/div, fpu_pack) and the FMOVECR powers of ten.

#include "fpu.h"
#include "fpu_internal.h"

#include <string.h>

// ============================================================================
// Packed decimal (BCD) format — 12-byte packed BCD ↔ float80_reg_t
// ============================================================================
//
// 68882 packed decimal memory layout (3 longwords, 12 bytes):
//   Word 0: [31]=SM [30]=SE [29:28]=YY [27:16]=3 BCD exponent digits
//           [15:4]=zero [3:0]=d16 (integer digit, MSD)
//   Word 1: [31:0]=digits d15..d8 (8 BCD digits, 4 bits each)
//   Word 2: [31:0]=digits d7..d0 (8 BCD digits, 4 bits each)
// Total: 17 significant mantissa digits (d16..d0), 3 exponent digits.
//
// SM = mantissa sign, SE = exponent sign
// YY: 0=normal; non-zero + all-zero mantissa → infinity; non-zero + non-zero mantissa → NaN
//
// Reference: MC68882 User's Manual, Motorola FPSP

// Extract a 4-bit BCD digit from a 32-bit word at nibble position (0=MSN, 7=LSN)
static inline unsigned bcd_nibble(uint32_t word, int pos) {
    return (word >> (28 - pos * 4)) & 0xF;
}

// Compute 10^|n| as fpu_unpacked_t using the FMOVECR power-of-10 table.
// Decomposes n into sum of powers of 2, multiplying corresponding table entries.
static fpu_unpacked_t fpu_power_of_10(fpu_state_t *fpu, int32_t n) {
    if (n == 0) {
        fpu_unpacked_t one = {false, 0, 0x8000000000000000ULL, 0};
        return one;
    }
    if (n < 0)
        n = -n;

    // FMOVECR offsets 0x33..0x3F = 10^1, 10^2, 10^4, ..., 10^4096
    fpu_unpacked_t result = {false, 0, 0x8000000000000000ULL, 0}; // 1.0
    bool first = true;
    for (int bit = 0; bit < 13; bit++) {
        if (!(n & (1 << bit)))
            continue;
        fpu_unpacked_t pw = fpu_rom_constant(0x33 + bit);
        if (first) {
            result = pw;
            first = false;
        } else {
            result = fpu_op_mul(fpu, result, pw);
        }
    }
    return result;
}

// floor(log10(x)) for a finite, normalized, non-zero x (sign ignored).
// With x in [2^e, 2^(e+1)), log10(x) lies in [e*log10(2), (e+1)*log10(2)),
// so the answer is floor(e*log10(2)) or one more; a comparison against the
// FMOVECR powers of ten picks between them.  log10(2) is taken as the 32-bit
// fixed-point 0x4D104D42 / 2^32, whose error is far below the closest
// approach of e*log10(2) to an integer over the extended exponent range.
// Leaves FPSR untouched.
static int32_t fpu_floor_log10(fpu_state_t *fpu, fpu_unpacked_t x) {
    int32_t ilog = (int32_t)(((int64_t)x.exponent * 0x4D104D42LL) >> 32); // floor(e*log10(2))
    int32_t m = ilog + 1;
    uint32_t saved_fpsr = fpu->fpsr;
    bool ge; // |x| >= 10^m ?
    x.sign = false;
    if (m >= 0) {
        fpu_unpacked_t p = fpu_power_of_10(fpu, m);
        if (x.exponent != p.exponent)
            ge = x.exponent > p.exponent;
        else if (x.mantissa_hi != p.mantissa_hi)
            ge = x.mantissa_hi > p.mantissa_hi;
        else
            ge = x.mantissa_lo >= p.mantissa_lo;
    } else {
        // |x| >= 10^m  <=>  |x| * 10^-m >= 1
        fpu_unpacked_t y = fpu_op_mul(fpu, x, fpu_power_of_10(fpu, -m));
        ge = y.exponent >= 0;
    }
    fpu->fpsr = saved_fpsr;
    return ge ? m : ilog;
}

// Convert 12-byte packed BCD from memory to float80_reg_t
float80_reg_t fpu_from_packed(fpu_state_t *fpu, uint32_t w0, uint32_t w1, uint32_t w2) {
    int sm = (w0 >> 31) & 1; // mantissa sign
    int se = (w0 >> 30) & 1; // exponent sign
    int yy = (w0 >> 28) & 3; // special encoding

    // Special values: YY != 0
    if (yy != 0) {
        if (w1 == 0 && w2 == 0)
            return fp80_make(sm, 0x7FFF, 0); // infinity
        // NaN: place mantissa bits as payload, set J-bit and quiet bit.
        // The payload is non-zero here (the all-zero case is infinity above),
        // so it never needs a substitute to stay a NaN.
        uint64_t nan_mant = ((uint64_t)w1 << 32) | w2;
        nan_mant |= 0xC000000000000000ULL;
        return fp80_make(sm, 0x7FFF, nan_mant);
    }

    // Extract 3 BCD exponent digits from w0 bits 27:16
    unsigned e1 = (w0 >> 24) & 0xF; // hundreds
    unsigned e2 = (w0 >> 20) & 0xF; // tens
    unsigned e3 = (w0 >> 16) & 0xF; // units
    int32_t bcd_exp = (int32_t)(e1 * 100 + e2 * 10 + e3);
    if (se)
        bcd_exp = -bcd_exp;

    // Subtract 16: mantissa is 17 integer digits, value = mant * 10^(exp-16)
    int32_t adj_exp = bcd_exp - 16;

    // Extract 17 BCD mantissa digits → uint64_t.
    // d16 from w0[3:0], d15..d8 from w1, d7..d0 from w2.
    // Per MC68882UM, any nibble > 9 is an invalid BCD operand and sets OPERR.
    uint64_t mant = w0 & 0xF; // d16 (MSD)
    bool bcd_invalid = (mant > 9);
    for (int i = 0; i < 8; i++) { // d15..d8 from w1
        unsigned n = bcd_nibble(w1, i);
        if (n > 9)
            bcd_invalid = true;
        mant = mant * 10 + n;
    }
    for (int i = 0; i < 8; i++) { // d7..d0 from w2
        unsigned n = bcd_nibble(w2, i);
        if (n > 9)
            bcd_invalid = true;
        mant = mant * 10 + n;
    }
    if (bcd_invalid)
        fpu->fpsr |= FPEXC_OPERR;

    // Zero mantissa → signed zero
    if (mant == 0)
        return fp80_make(sm, 0, 0);

    // Convert integer mantissa to fpu_unpacked_t
    fpu_unpacked_t val;
    val.sign = (sm != 0);
    val.mantissa_lo = 0;
    int lz = clz64(mant);
    val.mantissa_hi = mant << lz;
    val.exponent = 63 - lz; // true binary exponent for integer value

    // Scale by 10^adj_exp
    if (adj_exp != 0) {
        // Use extended precision, round-to-nearest for intermediate computation
        uint32_t saved_fpcr = fpu->fpcr;
        fpu->fpcr = 0;

        fpu_unpacked_t pw = fpu_power_of_10(fpu, adj_exp < 0 ? -adj_exp : adj_exp);
        if (adj_exp > 0)
            val = fpu_op_mul(fpu, val, pw);
        else
            val = fpu_op_div(fpu, val, pw);

        fpu->fpcr = saved_fpcr;
    }

    // Decimal input conversion: convert any INEX2 from packing to INEX1
    // The 68882 signals input conversion inexactness as INEX1, not INEX2
    uint32_t pre_fpsr = fpu->fpsr;
    float80_reg_t packed = fpu_pack(fpu, val);
    bool pack_inexact = (fpu->fpsr & FPEXC_INEX2) != 0;
    fpu->fpsr = pre_fpsr;
    if (pack_inexact)
        fpu->fpsr |= FPEXC_INEX1;
    return packed;
}

// Convert float80_reg_t to 12-byte packed BCD with k-factor
void fpu_to_packed(fpu_state_t *fpu, float80_reg_t val, int k_factor, uint32_t *w0, uint32_t *w1, uint32_t *w2) {
    int sm = fp80_sign(val);

    // Zero
    if (fp80_is_zero(val)) {
        *w0 = (uint32_t)sm << 31;
        *w1 = 0;
        *w2 = 0;
        return;
    }

    // Infinity (YY=01)
    if (fp80_is_inf(val)) {
        *w0 = ((uint32_t)sm << 31) | (1u << 28);
        *w1 = 0;
        *w2 = 0;
        return;
    }

    // NaN (YY=11, mantissa preserved). Per MC68882UM, FMOVE.P FPn,<ea> must
    // signal SNaN on a signaling NaN source and quiet the stored value, just
    // like other FMOVE forms. The packed-decimal store path bypasses
    // fpu_execute_op's SNAN check so handle it inline.
    if (fp80_is_nan(val)) {
        if (fp80_is_snan(val)) {
            fpu->fpsr |= FPEXC_SNAN;
            val.mantissa |= 0x4000000000000000ULL; // quiet the NaN
        }
        *w0 = ((uint32_t)sm << 31) | (3u << 28);
        *w1 = (uint32_t)(val.mantissa >> 32);
        *w2 = (uint32_t)(val.mantissa & 0xFFFFFFFF);
        return;
    }

    // Compute ILOG = floor(log10(|val|)) in soft-float (no host libm)
    fpu_unpacked_t uv = fpu_unpack(val);
    fpu_normalize(&uv); // unnormal inputs: same value, J-bit set
    int32_t ilog = (uv.exponent == FPU_EXP_ZERO) ? 0 : fpu_floor_log10(fpu, uv);

    // Determine LEN (number of significant digits)
    int32_t len;
    if (k_factor > 0) {
        len = k_factor;
    } else if (k_factor == 0) {
        len = ilog + 1;
    } else {
        len = ilog + 1 - k_factor;
    }
    if (len < 1)
        len = 1;
    if (len > 17) {
        len = 17;
        if (k_factor > 0)
            fpu->fpsr |= FPEXC_OPERR;
    }

    // Scale: Y = |val| * 10^(LEN-1-ILOG) to produce LEN-digit integer
    int32_t iscale = ilog + 1 - len;

    // Save FPCR, use extended/RN for intermediate math
    uint32_t saved_fpcr = fpu->fpcr;
    fpu->fpcr = 0;

    fpu_unpacked_t abs_val = uv;
    abs_val.sign = false;

    fpu_unpacked_t y;
    if (iscale != 0) {
        fpu_unpacked_t pw = fpu_power_of_10(fpu, iscale < 0 ? -iscale : iscale);
        if (iscale > 0)
            y = fpu_op_div(fpu, abs_val, pw);
        else
            y = fpu_op_mul(fpu, abs_val, pw);
    } else {
        y = abs_val;
    }

// Extract integer part of y with rounding (round-to-nearest)
#define EXTRACT_Y_INT(yval, out)                                                                                       \
    do {                                                                                                               \
        if ((yval).exponent == FPU_EXP_ZERO) {                                                                         \
            (out) = 0;                                                                                                 \
        } else if ((yval).exponent >= 63) {                                                                            \
            (out) = (yval).mantissa_hi;                                                                                \
        } else if ((yval).exponent < 0) {                                                                              \
            (out) = 0;                                                                                                 \
        } else {                                                                                                       \
            int shift = 63 - (yval).exponent;                                                                          \
            (out) = (yval).mantissa_hi >> shift;                                                                       \
            if (shift > 0 && ((yval).mantissa_hi >> (shift - 1)) & 1)                                                  \
                (out)++;                                                                                               \
        }                                                                                                              \
    } while (0)

    uint64_t y_int;
    EXTRACT_Y_INT(y, y_int);

    // Validate and correct ILOG if digit count is wrong
    uint64_t lo_bound = 1;
    for (int i = 0; i < len - 1; i++)
        lo_bound *= 10;
    uint64_t hi_bound = lo_bound * 10;

    if (y_int < lo_bound && ilog > -999) {
        // ILOG too high — decrement and rescale
        ilog--;
        iscale = ilog + 1 - len;
        if (iscale != 0) {
            fpu_unpacked_t pw = fpu_power_of_10(fpu, iscale < 0 ? -iscale : iscale);
            if (iscale > 0)
                y = fpu_op_div(fpu, abs_val, pw);
            else
                y = fpu_op_mul(fpu, abs_val, pw);
        } else {
            y = abs_val;
        }
        EXTRACT_Y_INT(y, y_int);
    } else if (y_int >= hi_bound && ilog < 999) {
        // ILOG too low — increment and rescale
        ilog++;
        iscale = ilog + 1 - len;
        if (iscale != 0) {
            fpu_unpacked_t pw = fpu_power_of_10(fpu, iscale < 0 ? -iscale : iscale);
            if (iscale > 0)
                y = fpu_op_div(fpu, abs_val, pw);
            else
                y = fpu_op_mul(fpu, abs_val, pw);
        } else {
            y = abs_val;
        }
        EXTRACT_Y_INT(y, y_int);
    }

#undef EXTRACT_Y_INT

    // If still at hi_bound, divide by 10 and increment ilog
    if (y_int >= hi_bound) {
        y_int /= 10;
        ilog++;
    }

    fpu->fpcr = saved_fpcr;

    // Check for inexact result
    if (y.exponent != FPU_EXP_ZERO && y.exponent >= 0 && y.exponent < 63 &&
        (y.mantissa_hi & ((1ULL << (63 - y.exponent)) - 1)) != 0)
        fpu->fpsr |= FPEXC_INEX2;
    if (y.mantissa_lo != 0)
        fpu->fpsr |= FPEXC_INEX2;

    // Convert y_int to 17 BCD digits, left-justified: digits[0]=d16 (MSD)
    // The 68882 always places significant digits starting at d16 (the MSD),
    // with trailing zeros filling the remaining positions.
    uint8_t digits[17];
    memset(digits, 0, sizeof(digits));
    uint64_t tmp = y_int;
    for (int i = len - 1; i >= 0; i--) {
        digits[i] = (uint8_t)(tmp % 10);
        tmp /= 10;
    }

    // Compute output exponent and sign
    int32_t out_exp = ilog;
    int se = 0;
    if (out_exp < 0) {
        se = 1;
        out_exp = -out_exp;
    }
    if (out_exp > 999) {
        out_exp = 999;
        fpu->fpsr |= FPEXC_OPERR;
    }
    unsigned exp_e1 = (unsigned)(out_exp / 100); // hundreds
    unsigned exp_e2 = (unsigned)((out_exp / 10) % 10); // tens
    unsigned exp_e3 = (unsigned)(out_exp % 10); // units

    // Pack word 0: SM|SE|YY=00|exponent(3 digits)|zeros|d16
    *w0 = ((uint32_t)sm << 31) | ((uint32_t)se << 30) | (exp_e1 << 24) | (exp_e2 << 20) | (exp_e3 << 16) |
          ((uint32_t)digits[0] & 0xF);

    // Pack word 1: d15..d8 (8 BCD digits)
    *w1 = ((uint32_t)digits[1] << 28) | ((uint32_t)digits[2] << 24) | ((uint32_t)digits[3] << 20) |
          ((uint32_t)digits[4] << 16) | ((uint32_t)digits[5] << 12) | ((uint32_t)digits[6] << 8) |
          ((uint32_t)digits[7] << 4) | (uint32_t)digits[8];

    // Pack word 2: d7..d0 (8 BCD digits)
    *w2 = ((uint32_t)digits[9] << 28) | ((uint32_t)digits[10] << 24) | ((uint32_t)digits[11] << 20) |
          ((uint32_t)digits[12] << 16) | ((uint32_t)digits[13] << 12) | ((uint32_t)digits[14] << 8) |
          ((uint32_t)digits[15] << 4) | (uint32_t)digits[16];
}
