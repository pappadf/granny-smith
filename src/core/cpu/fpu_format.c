// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fpu_format.c
// Motorola 68882 FPU data format conversions between the register format
// (float80_reg_t) and the memory formats: 68882 extended, IEEE single and
// double, and byte/word/long integers.  Packed decimal is in fpu_packed.c.

#include "fpu.h"
#include "fpu_internal.h"

// ============================================================================
// Data format conversions: memory -> float80_reg_t
// ============================================================================

// Convert 68882 extended (96-bit in memory) to float80_reg_t (trivial)
float80_reg_t fpu_from_extended(uint32_t exp_sign_pad, uint32_t mant_hi, uint32_t mant_lo) {
    uint16_t exp_sign = (uint16_t)(exp_sign_pad >> 16);
    uint64_t mantissa = ((uint64_t)mant_hi << 32) | mant_lo;
    float80_reg_t r;
    r.exponent = exp_sign;
    r.mantissa = mantissa;
    return r;
}

// Convert float80_reg_t to 68882 extended (96-bit): writes 3 x 32-bit words
void fpu_to_extended(float80_reg_t val, uint32_t *word0, uint32_t *word1, uint32_t *word2) {
    *word0 = (uint32_t)val.exponent << 16; // exp+sign in upper 16, pad in lower 16
    *word1 = (uint32_t)(val.mantissa >> 32);
    *word2 = (uint32_t)(val.mantissa & 0xFFFFFFFF);
}

// Convert IEEE 754 single (32-bit) to float80_reg_t
float80_reg_t fpu_from_single(uint32_t bits) {
    int sign = (bits >> 31) & 1;
    int exp = (bits >> 23) & 0xFF;
    uint32_t frac = bits & 0x7FFFFF;

    if (exp == 0 && frac == 0) {
        return fp80_make(sign, 0, 0);
    }
    if (exp == 0xFF) {
        if (frac == 0)
            return fp80_make(sign, 0x7FFF, 0);
        // NaN: place fraction bits into extended mantissa (J-bit stays 0)
        uint64_t mant = (uint64_t)frac << 40;
        return fp80_make(sign, 0x7FFF, mant);
    }
    if (exp == 0) {
        // Denormal: normalize
        int shift = clz64((uint64_t)frac) - 40;
        uint64_t mant = (uint64_t)frac << (40 + shift);
        uint16_t biased = (uint16_t)(1 - 127 + FPU_EXP_BIAS - shift);
        return fp80_make(sign, biased, mant);
    }
    // Normal: add implicit bit, convert to extended
    uint64_t mant = ((uint64_t)(frac | 0x800000)) << 40;
    uint16_t biased = (uint16_t)(exp - 127 + FPU_EXP_BIAS);
    return fp80_make(sign, biased, mant);
}

// Convert float80_reg_t to an IEEE 754 binary format with `mant_bits`
// significand bits (implicit bit included: 24 single, 53 double), exponent
// bias `bias` and all-ones exponent field `exp_max` (255 / 2047).  Returns
// the encoding in the low bits.  Rounds once, per the FPCR mode, straight
// to the final (normal or denormal) width, so denormal results are not
// double-rounded.  Unnormal inputs are normalized first.
static uint64_t fpu_to_ieee(fpu_state_t *fpu, float80_reg_t val, int mant_bits, int32_t bias, int32_t exp_max) {
    int frac_n = mant_bits - 1; // stored fraction bits
    int exp_width = 64 - clz64((uint64_t)exp_max); // 8 / 11
    bool sign = fp80_sign(val) != 0;
    uint64_t sign_bit = (uint64_t)sign << (frac_n + exp_width);
    uint64_t exp_field_inf = (uint64_t)exp_max << frac_n;
    uint64_t frac_mask = (1ULL << frac_n) - 1;
    uint16_t exp = fp80_exp(val);

    if (val.mantissa == 0 && exp != 0x7FFF)
        return sign_bit; // zero (including an unnormal zero)
    if (exp == 0x7FFF) {
        if (val.mantissa == 0)
            return sign_bit | exp_field_inf;
        // NaN: signal SNaN and quiet it, keeping the top payload bits
        if (!(val.mantissa & 0x4000000000000000ULL))
            fpu->fpsr |= FPEXC_SNAN;
        uint64_t frac = ((val.mantissa | 0x4000000000000000ULL) >> (64 - mant_bits)) & frac_mask;
        if (frac == 0)
            frac = 1; // preserve NaN-ness
        return sign_bit | exp_field_inf | frac;
    }

    // Normalize: exponent field 0 means 2^-16383 with explicit J-bit
    uint64_t m = val.mantissa;
    int32_t true_exp = (exp == 0 ? 0 : (int32_t)exp) - FPU_EXP_BIAS;
    int lz = clz64(m);
    m <<= lz;
    true_exp -= lz;

    unsigned rmode = (fpu->fpcr >> 4) & 3;
    int32_t e = true_exp + bias; // target biased exponent before rounding
    bool denormal = (e <= 0);
    // Bits to drop from the 64-bit mantissa: the normal discard, plus the
    // denormal shift.  Beyond 64 every bit is below the result's LSB.
    int32_t shift = (64 - mant_bits) + (denormal ? 1 - e : 0);
    uint64_t q, rem;
    bool above_half, at_half;
    if (shift < 64) {
        q = m >> shift;
        rem = m & ((1ULL << shift) - 1);
        uint64_t half = 1ULL << (shift - 1);
        above_half = rem > half;
        at_half = rem == half;
    } else {
        q = 0;
        rem = m;
        above_half = (shift == 64) && m > 0x8000000000000000ULL;
        at_half = (shift == 64) && m == 0x8000000000000000ULL;
    }
    if (rem != 0) {
        fpu->fpsr |= FPEXC_INEX2;
        bool round_up = false;
        switch (rmode) {
        case 0: // nearest (ties to even)
            round_up = above_half || (at_half && (q & 1));
            break;
        case 1: // toward zero
            break;
        case 2: // toward -inf
            round_up = sign;
            break;
        case 3: // toward +inf
            round_up = !sign;
            break;
        }
        if (round_up)
            q++;
    }

    if (denormal) {
        // A carry out of the denormal range lands in the exponent field's
        // LSB, which is exactly the minimum normal encoding.
        fpu->fpsr |= FPEXC_UNFL | FPEXC_INEX2;
        return sign_bit | q;
    }
    if (q >> mant_bits) {
        // Carry out of the significand: renormalize
        q >>= 1;
        e++;
    }
    if (e >= exp_max) {
        fpu->fpsr |= FPEXC_OVFL;
        bool to_inf = (rmode == 0) || (rmode == 2 && sign) || (rmode == 3 && !sign);
        if (to_inf)
            return sign_bit | exp_field_inf;
        return sign_bit | (exp_field_inf - 1); // max finite
    }
    return sign_bit | ((uint64_t)e << frac_n) | (q & frac_mask);
}

// Convert float80_reg_t to IEEE 754 single (32-bit)
uint32_t fpu_to_single(fpu_state_t *fpu, float80_reg_t val) {
    return (uint32_t)fpu_to_ieee(fpu, val, 24, 127, 0xFF);
}

// Convert IEEE 754 double (64-bit) to float80_reg_t
float80_reg_t fpu_from_double(uint64_t bits) {
    int sign = (int)(bits >> 63) & 1;
    int exp = (int)((bits >> 52) & 0x7FF);
    uint64_t frac = bits & 0x000FFFFFFFFFFFFFULL;

    if (exp == 0 && frac == 0) {
        return fp80_make(sign, 0, 0);
    }
    if (exp == 0x7FF) {
        if (frac == 0)
            return fp80_make(sign, 0x7FFF, 0);
        // NaN: place fraction bits into extended mantissa (J-bit stays 0)
        uint64_t mant = frac << 11;
        return fp80_make(sign, 0x7FFF, mant);
    }
    if (exp == 0) {
        // Denormal
        int shift = clz64(frac) - 11;
        uint64_t mant = frac << (11 + shift);
        uint16_t biased = (uint16_t)(1 - 1023 + FPU_EXP_BIAS - shift);
        return fp80_make(sign, biased, mant);
    }
    // Normal
    uint64_t mant = (frac | 0x0010000000000000ULL) << 11;
    uint16_t biased = (uint16_t)(exp - 1023 + FPU_EXP_BIAS);
    return fp80_make(sign, biased, mant);
}

// Convert float80_reg_t to IEEE 754 double (64-bit)
uint64_t fpu_to_double(fpu_state_t *fpu, float80_reg_t val) {
    return fpu_to_ieee(fpu, val, 53, 1023, 0x7FF);
}

// Convert int32 to float80_reg_t
float80_reg_t fpu_from_int32(int32_t v) {
    if (v == 0)
        return FP80_ZERO;
    int sign = 0;
    uint32_t abs_v;
    if (v < 0) {
        sign = 1;
        abs_v = (uint32_t)(-(int64_t)v);
    } else {
        abs_v = (uint32_t)v;
    }
    int shift = clz64((uint64_t)abs_v);
    uint64_t mant = (uint64_t)abs_v << shift;
    int32_t true_exp = 63 - shift;
    uint16_t biased = (uint16_t)(true_exp + FPU_EXP_BIAS);
    return fp80_make(sign, biased, mant);
}

// Convert int16 to float80_reg_t
float80_reg_t fpu_from_int16(int16_t v) {
    return fpu_from_int32((int32_t)v);
}

// Convert int8 to float80_reg_t
float80_reg_t fpu_from_int8(int8_t v) {
    return fpu_from_int32((int32_t)v);
}

// Convert float80_reg_t to int32, clamping on overflow
int32_t fpu_to_int32(fpu_state_t *fpu, float80_reg_t val) {
    if (fp80_is_nan(val)) {
        fpu->fpsr |= FPEXC_OPERR;
        return 0;
    }
    if (fp80_is_zero(val))
        return 0;
    int sign = fp80_sign(val);
    uint16_t exp = fp80_exp(val);
    int32_t true_exp = (int32_t)exp - FPU_EXP_BIAS;

    if (fp80_is_inf(val) || true_exp > 30) {
        fpu->fpsr |= FPEXC_OPERR;
        return sign ? INT32_MIN : INT32_MAX;
    }
    if (true_exp < 0) {
        // Value is between -1 and 1 exclusive; apply rounding mode
        fpu->fpsr |= FPEXC_INEX2;
        unsigned rmode = (fpu->fpcr >> 4) & 3;
        switch (rmode) {
        case 0: { // round to nearest: round to ±1 if |value| > 0.5
            // true_exp == -1 means 0.5 <= |value| < 1.0
            if (true_exp == -1) {
                // Check if value > 0.5: mantissa must have bits beyond MSB
                uint64_t frac_bits = val.mantissa & 0x7FFFFFFFFFFFFFFFULL;
                if (frac_bits > 0)
                    return sign ? -1 : 1;
                // Exactly 0.5: round to nearest even = 0
            }
            return 0;
        }
        case 1: // round to zero
            return 0;
        case 2: // round toward -inf
            return sign ? -1 : 0;
        case 3: // round toward +inf
            return sign ? 0 : 1;
        }
        return 0;
    }

    // Shift mantissa right to get integer part
    int shift = 63 - true_exp;
    uint64_t abs_val;
    bool has_frac = false;
    if (shift >= 64) {
        abs_val = 0;
        has_frac = true; // entire mantissa is fractional
    } else {
        abs_val = val.mantissa >> shift;
        // Check if fractional bits were discarded
        if (shift > 0 && (val.mantissa & ((1ULL << shift) - 1)))
            has_frac = true;
    }

    // Apply rounding per FPCR rounding mode
    if (has_frac) {
        fpu->fpsr |= FPEXC_INEX2;
        unsigned rmode = (fpu->fpcr >> 4) & 3;
        bool round_up = false;
        switch (rmode) {
        case 0: { // round to nearest
            // Check if fraction > 0.5 or == 0.5 with odd integer
            if (shift > 0 && shift < 64) {
                uint64_t half = 1ULL << (shift - 1);
                uint64_t frac = val.mantissa & ((1ULL << shift) - 1);
                if (frac > half || (frac == half && (abs_val & 1)))
                    round_up = true;
            }
            break;
        }
        case 1: // round to zero — truncation, no adjustment
            break;
        case 2: // round toward -inf
            if (sign)
                round_up = true;
            break;
        case 3: // round toward +inf
            if (!sign)
                round_up = true;
            break;
        }
        if (round_up)
            abs_val++;
    }

    if (sign) {
        if (abs_val > (uint64_t)INT32_MAX + 1) {
            fpu->fpsr = (fpu->fpsr & ~FPEXC_INEX2) | FPEXC_OPERR;
            return INT32_MIN;
        }
        return -(int32_t)abs_val;
    }
    if (abs_val > (uint64_t)INT32_MAX) {
        fpu->fpsr = (fpu->fpsr & ~FPEXC_INEX2) | FPEXC_OPERR;
        return INT32_MAX;
    }
    return (int32_t)abs_val;
}

// Convert float80_reg_t to int16, clamping on overflow
int16_t fpu_to_int16(fpu_state_t *fpu, float80_reg_t val) {
    int32_t v = fpu_to_int32(fpu, val);
    if (v > INT16_MAX) {
        fpu->fpsr = (fpu->fpsr & ~FPEXC_INEX2) | FPEXC_OPERR;
        return INT16_MAX;
    }
    if (v < INT16_MIN) {
        fpu->fpsr = (fpu->fpsr & ~FPEXC_INEX2) | FPEXC_OPERR;
        return INT16_MIN;
    }
    return (int16_t)v;
}

// Convert float80_reg_t to int8, clamping on overflow
int8_t fpu_to_int8(fpu_state_t *fpu, float80_reg_t val) {
    int32_t v = fpu_to_int32(fpu, val);
    if (v > INT8_MAX) {
        fpu->fpsr = (fpu->fpsr & ~FPEXC_INEX2) | FPEXC_OPERR;
        return INT8_MAX;
    }
    if (v < INT8_MIN) {
        fpu->fpsr = (fpu->fpsr & ~FPEXC_INEX2) | FPEXC_OPERR;
        return INT8_MIN;
    }
    return (int8_t)v;
}
