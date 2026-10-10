// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fpu_packed.c
// Motorola 68882 FPU packed decimal (FMOVE.P) conversions: exact
// arithmetic rounded once by the soft-float core (fpu_pack), with ILOG from
// the FMOVECR powers of ten.

#include "fpu.h"
#include "fpu_internal.h"

#include <string.h>

// ============================================================================
// Packed decimal (BCD) format — 12-byte packed BCD ↔ float80_reg_t
// ============================================================================
//
// 68882 packed decimal memory layout (3 longwords, 12 bytes):
//   Word 0: [31]=SM [30]=SE [29:28]=YY [27:16]=3 BCD exponent digits
//           [15:12]=4th exponent digit (thousands; written by a store whose
//           exponent needs it, ignored on load) [11:4]=zero
//           [3:0]=d16 (integer digit, MSD)
//   Word 1: [31:0]=digits d15..d8 (8 BCD digits, 4 bits each)
//   Word 2: [31:0]=digits d7..d0 (8 BCD digits, 4 bits each)
// Total: 17 significant mantissa digits (d16..d0), 3 exponent digits (4 on
// a store overflowing them, which is an operand error).
//
// SM = mantissa sign, SE = exponent sign
// YY: 0=normal; non-zero + all-zero mantissa → infinity; non-zero + non-zero mantissa → NaN
//
// Reference: Motorola, MC68881/MC68882 Floating-Point Coprocessor User's
// Manual, 2nd ed. (1989), §3.3, §3.6 (Figure 3-11), §4.3.3, §6.1.8; Motorola
// M68040 Floating-Point Software Package (FPSP), bindec.sa and decbin.sa

// Extract a 4-bit BCD digit from a 32-bit word at nibble position (0=MSN, 7=LSN)
static inline unsigned bcd_nibble(uint32_t word, int pos) {
    return (word >> (28 - pos * 4)) & 0xF;
}

// 10^4096 to 128 bits (truncated).  The FMOVECR table holds only the 64
// bits the 68882 ROM returns, a value just below 10^4096 that the
// floor(log10) comparison would otherwise count as reaching it.
static const fpu_unpacked_t pow10_4096_wide = {false, 13606, 0xC46052028A20979AULL, 0xC94C153F804A4A92ULL};

// Compute 10^|n| as fpu_unpacked_t using the FMOVECR power-of-10 table.
// Decomposes n into sum of powers of 2, multiplying corresponding table
// entries; `wide` swaps in the 128-bit 10^4096.
static fpu_unpacked_t fpu_power_of_10_wide(fpu_state_t *fpu, int32_t n, bool wide) {
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
        fpu_unpacked_t pw = (wide && bit == 12) ? pow10_4096_wide : fpu_rom_constant(0x33 + bit);
        if (first) {
            result = pw;
            first = false;
        } else {
            result = fpu_op_mul(fpu, result, pw);
        }
    }
    return result;
}

// ============================================================================
// Exact arithmetic for the conversions
// ============================================================================
//
// Both directions are computed exactly and rounded once, in the user's
// rounding mode: a decimal string M * 10^E and a binary value m * 2^e are
// both ratios of integers, and the conversions only need the integer part
// of such a ratio and where its remainder lies against one half.  The
// numbers are held as natural numbers in 32-bit limbs; 10^n is split as
// 5^n * 2^n so the power of two becomes a shift.
//
// Width: the largest operand is a store of the smallest denormal at LEN 17,
// m * 5^4967 (64 + 11534 bits), or of the largest value, m * 2^11404; the
// division shifts the divisor up to 63 bits past its own width, never past
// the dividend's.  12288 bits cover both with room to spare.

#define BIG_LIMBS 384

// Natural number, little-endian 32-bit limbs; n limbs in use (0 = zero)
typedef struct {
    int n;
    uint32_t w[BIG_LIMBS];
} big_t;

// x = v
static void big_set_u64(big_t *x, uint64_t v) {
    x->w[0] = (uint32_t)v;
    x->w[1] = (uint32_t)(v >> 32);
    x->n = x->w[1] ? 2 : (x->w[0] ? 1 : 0);
}

// Number of significant bits in x (0 for zero)
static int big_bitlen(const big_t *x) {
    if (x->n == 0)
        return 0;
    return (x->n - 1) * 32 + (64 - clz64(x->w[x->n - 1])); // clz64 of a limb counts 32 extra
}

// x *= f (f < 2^32)
static void big_mul_small(big_t *x, uint32_t f) {
    uint64_t carry = 0;
    for (int i = 0; i < x->n; i++) {
        uint64_t t = (uint64_t)x->w[i] * f + carry;
        x->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && x->n < BIG_LIMBS)
        x->w[x->n++] = (uint32_t)carry;
}

// x *= 5^n, in steps of 5^13 (the largest power of five below 2^32)
static void big_mul_pow5(big_t *x, int n) {
    static const uint32_t pow5[14] = {1,     5,      25,      125,     625,      3125,      15625,
                                      78125, 390625, 1953125, 9765625, 48828125, 244140625, 1220703125};
    for (; n >= 13; n -= 13)
        big_mul_small(x, pow5[13]);
    if (n > 0)
        big_mul_small(x, pow5[n]);
}

// x <<= bits
static void big_shl(big_t *x, int bits) {
    if (x->n == 0 || bits <= 0)
        return;
    int limbs = bits / 32, sh = bits % 32;
    int n = x->n + limbs + 1;
    if (n > BIG_LIMBS)
        n = BIG_LIMBS; // cannot happen within the conversions' ranges (see above)
    for (int i = n - 1; i >= 0; i--) {
        int src = i - limbs;
        uint32_t hi = (src >= 0 && src < x->n) ? x->w[src] : 0;
        uint32_t lo = (src - 1 >= 0 && src - 1 < x->n) ? x->w[src - 1] : 0;
        x->w[i] = sh ? (hi << sh) | (lo >> (32 - sh)) : hi;
    }
    x->n = n;
    while (x->n > 0 && x->w[x->n - 1] == 0)
        x->n--;
}

// x >>= 1
static void big_shr1(big_t *x) {
    for (int i = 0; i < x->n; i++)
        x->w[i] = (x->w[i] >> 1) | (i + 1 < x->n ? x->w[i + 1] << 31 : 0);
    while (x->n > 0 && x->w[x->n - 1] == 0)
        x->n--;
}

// Three-way compare: -1, 0 or +1 as a <, ==, > b
static int big_cmp(const big_t *a, const big_t *b) {
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i])
            return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

// a -= b, for a >= b
static void big_sub(big_t *a, const big_t *b) {
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->w[i] - (i < b->n ? b->w[i] : 0) - borrow;
        borrow = t < 0;
        a->w[i] = (uint32_t)t;
    }
    while (a->n > 0 && a->w[a->n - 1] == 0)
        a->n--;
}

// Where a division's remainder lies against half the divisor
typedef enum { REM_ZERO, REM_BELOW_HALF, REM_HALF, REM_ABOVE_HALF } rem_class_t;

// floor(num / den) for a quotient below 2^64, by restoring division; num
// is consumed (it ends as the doubled remainder), den is left as it was.
static uint64_t big_div(big_t *num, big_t *den, rem_class_t *rem) {
    uint64_t q = 0;
    int k = big_bitlen(num) - big_bitlen(den); // highest quotient bit that can be set
    if (k >= 0) {
        big_shl(den, k);
        for (int i = k; i >= 0; i--) {
            if (big_cmp(num, den) >= 0) {
                big_sub(num, den);
                if (i < 64) // every caller bounds the quotient below 2^64
                    q |= 1ULL << i;
            }
            if (i > 0)
                big_shr1(den);
        }
    }
    if (num->n == 0) {
        *rem = REM_ZERO;
    } else {
        big_shl(num, 1);
        int c = big_cmp(num, den);
        *rem = c < 0 ? REM_BELOW_HALF : (c == 0 ? REM_HALF : REM_ABOVE_HALF);
    }
    return q;
}

// Round an integer part q with remainder class `rem` to an integer in FPCR
// rounding mode `rmode`, for a value of sign `neg` (MC68881/MC68882 UM
// §6.1.7, Figure 6-3, with the remainder as guard and sticky)
static uint64_t round_quotient(uint64_t q, rem_class_t rem, bool neg, unsigned rmode) {
    if (rem == REM_ZERO)
        return q;
    switch (rmode) {
    case 0: // nearest, ties to even
        return (rem == REM_ABOVE_HALF || (rem == REM_HALF && (q & 1))) ? q + 1 : q;
    case 2: // toward minus infinity: away from zero when negative
        return neg ? q + 1 : q;
    case 3: // toward plus infinity: away from zero when positive
        return neg ? q : q + 1;
    default: // toward zero
        return q;
    }
}

// floor(log10(x)) for a finite, normalized, non-zero x (sign ignored).
// With x in [2^e, 2^(e+1)), log10(x) lies in [e*log10(2), (e+1)*log10(2)),
// so the answer is floor(e*log10(2)) or one more; a comparison against the
// powers of ten picks between them.  log10(2) is taken as the 32-bit
// fixed-point 0x4D104D42 / 2^32, whose error is far below the closest
// approach of e*log10(2) to an integer over the extended exponent range.
// The powers are the 128-bit table products (10^4096 at full width): their
// truncation error is far below the gap between any 64-bit value and the
// nearest power of ten, so the comparison is exact (checked against exact
// arithmetic at both 64-bit neighbours of every 10^m in range).
// Leaves FPSR untouched.
int32_t fpu_floor_log10(fpu_state_t *fpu, fpu_unpacked_t x) {
    int32_t ilog = (int32_t)(((int64_t)x.exponent * 0x4D104D42LL) >> 32); // floor(e*log10(2))
    int32_t m = ilog + 1;
    uint32_t saved_fpsr = fpu->fpsr;
    bool ge; // |x| >= 10^m ?
    x.sign = false;
    if (m >= 0) {
        fpu_unpacked_t p = fpu_power_of_10_wide(fpu, m, true);
        if (x.exponent != p.exponent)
            ge = x.exponent > p.exponent;
        else if (x.mantissa_hi != p.mantissa_hi)
            ge = x.mantissa_hi > p.mantissa_hi;
        else
            ge = x.mantissa_lo >= p.mantissa_lo;
    } else {
        // |x| >= 10^m  <=>  |x| * 10^-m >= 1
        fpu_unpacked_t y = fpu_op_mul(fpu, x, fpu_power_of_10_wide(fpu, -m, true));
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

    // The exact value mant * 10^adj_exp as num / den * 2^adj_exp
    // (10^n = 5^n * 2^n), so a quotient in [2^63, 2^64) of num * 2^s / den
    // is the significand with the remainder as guard and sticky.
    big_t num, den;
    big_set_u64(&num, mant);
    big_set_u64(&den, 1);
    if (adj_exp >= 0)
        big_mul_pow5(&num, adj_exp);
    else
        big_mul_pow5(&den, -adj_exp);
    int s = 63 - (big_bitlen(&num) - big_bitlen(&den)); // ratio * 2^s in (2^62, 2^64)
    if (s >= 0)
        big_shl(&num, s);
    else
        big_shl(&den, -s);
    // One bit more when the ratio sits below 2^63: num < den * 2^63
    big_t den63;
    den63 = den;
    big_shl(&den63, 63);
    if (big_cmp(&num, &den63) < 0) {
        big_shl(&num, 1);
        s++;
    }
    rem_class_t rem;
    uint64_t q = big_div(&num, &den, &rem);

    // Significand with the remainder folded into guard (bit 63 of the low
    // word) and sticky (bit 0)
    fpu_unpacked_t val;
    val.sign = (sm != 0);
    val.exponent = 63 - s + adj_exp;
    val.mantissa_hi = q;
    val.mantissa_lo = (rem >= REM_HALF ? 0x8000000000000000ULL : 0) | (rem != REM_ZERO && rem != REM_HALF ? 1 : 0);

    // Round once to extended precision in the user's rounding mode,
    // whatever the rounding precision (MC68881/MC68882 UM §6.1.8; FPSP
    // decbin rounds its final scaling in the user's mode, extended).  The
    // FMOVE or arithmetic that consumes the operand then rounds to the
    // selected precision itself, signalling INEX2.  An inexact conversion
    // is reported as INEX1, never INEX2.
    uint32_t saved_fpcr = fpu->fpcr;
    uint32_t pre_fpsr = fpu->fpsr;
    fpu->fpcr &= 0x30u; // rounding mode only: extended precision
    float80_reg_t packed = fpu_pack(fpu, val);
    bool pack_inexact = (fpu->fpsr & FPEXC_INEX2) != 0;
    fpu->fpcr = saved_fpcr;
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

    // ILOG = floor(log10(|val|)), exact (FPSP bindec A3 estimates it and
    // corrects the estimate in A13; the exact value needs no correction)
    fpu_unpacked_t uv = fpu_unpack(val); // unnormals normalized, J-bit set
    if (uv.exponent == FPU_EXP_ZERO) {
        // An unnormal zero is a zero (MC68881/MC68882 UM §3.5.1)
        *w0 = (uint32_t)sm << 31;
        *w1 = 0;
        *w2 = 0;
        return;
    }
    int32_t ilog = fpu_floor_log10(fpu, uv);
    unsigned rmode = (fpu->fpcr >> 4) & 3;

    // A6: LEN, the number of digits: k for k > 0 (E format), else the
    // digits up to the k-th place right of the decimal point (F format).
    // LEN above 17 is cut to 17, an operand error only when k asked for it
    // (MC68881/MC68882 UM FMOVE: k > +17 sets OPERR).
    int32_t len = (k_factor > 0) ? k_factor : ilog + 1 - k_factor;
    if (len < 1) {
        len = 1;
    } else if (len > 17) {
        len = 17;
        if (k_factor > 0)
            fpu->fpsr |= FPEXC_OPERR;
    }
    // A7: in F format a value below 10^k is rounded at the 10^k digit:
    // ILOG becomes k, the one digit then being 0 or 1.  Below 10^(k-1) the
    // value is under half that digit, which needs no arithmetic (and would
    // otherwise need a divisor as wide as the whole exponent range).
    bool below_half_digit = false;
    if (k_factor <= 0 && ilog < k_factor) {
        below_half_digit = (ilog < k_factor - 1);
        ilog = k_factor;
    }

    // A9-A12: YINT = |val| / 10^ISCALE rounded to an integer in the user's
    // rounding mode for the value's sign, at extended precision whatever the
    // rounding precision (FPSP bindec A11/A12, FINT of +-Y).  Computed
    // exactly: |val| = m * 2^(e-63), 10^ISCALE = 5^ISCALE * 2^ISCALE.
    int32_t iscale = ilog + 1 - len;
    rem_class_t rem = REM_BELOW_HALF;
    uint64_t y_int = 0;
    if (!below_half_digit) {
        big_t num, den;
        big_set_u64(&num, uv.mantissa_hi);
        big_set_u64(&den, 1);
        if (iscale >= 0)
            big_mul_pow5(&den, iscale);
        else
            big_mul_pow5(&num, -iscale);
        int32_t p2 = uv.exponent - 63 - iscale;
        if (p2 >= 0)
            big_shl(&num, p2);
        else
            big_shl(&den, -p2);
        y_int = big_div(&num, &den, &rem);
    }
    y_int = round_quotient(y_int, rem, sm != 0, rmode);
    if (rem != REM_ZERO)
        fpu->fpsr |= FPEXC_INEX2; // digits beyond LEN were discarded

    // A13: rounding carried into a new digit (YINT = 10^LEN): one digit
    // fewer, one decade up.  With an exact ILOG, YINT never falls outside
    // [10^(LEN-1), 10^LEN] otherwise, except as 0 under the A7 clamp, where
    // the FPSP's retry with ILOG - 1 clamps back to the same result.
    uint64_t hi_bound = 1;
    for (int i = 0; i < len; i++)
        hi_bound *= 10;
    if (y_int == hi_bound) {
        y_int /= 10;
        ilog++;
    }

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

    // A15: the decimal exponent, |ILOG| (1 when every digit is zero, the
    // F-format result of a value below half of 10^k), in up to four digits.
    // A fourth digit (exponent 1000 or more, values beyond the IEEE double
    // range) is written in bits 15:12 and is an operand error
    // (MC68881/MC68882 UM §4.3.3 and Figure 3-11; FPSP bindec A15).
    int se = (ilog < 0); // A16: SE is the sign of ILOG
    uint32_t out_exp = (y_int == 0) ? 1u : (uint32_t)(ilog < 0 ? -ilog : ilog);
    if (out_exp > 999)
        fpu->fpsr |= FPEXC_OPERR;
    unsigned exp_e4 = (unsigned)(out_exp / 1000); // thousands
    unsigned exp_e1 = (unsigned)((out_exp / 100) % 10); // hundreds
    unsigned exp_e2 = (unsigned)((out_exp / 10) % 10); // tens
    unsigned exp_e3 = (unsigned)(out_exp % 10); // units

    // Pack word 0: SM|SE|YY=00|exponent(3 digits)|4th exponent digit|zeros|d16
    *w0 = ((uint32_t)sm << 31) | ((uint32_t)se << 30) | (exp_e1 << 24) | (exp_e2 << 20) | (exp_e3 << 16) |
          (exp_e4 << 12) | ((uint32_t)digits[0] & 0xF);

    // Pack word 1: d15..d8 (8 BCD digits)
    *w1 = ((uint32_t)digits[1] << 28) | ((uint32_t)digits[2] << 24) | ((uint32_t)digits[3] << 20) |
          ((uint32_t)digits[4] << 16) | ((uint32_t)digits[5] << 12) | ((uint32_t)digits[6] << 8) |
          ((uint32_t)digits[7] << 4) | (uint32_t)digits[8];

    // Pack word 2: d7..d0 (8 BCD digits)
    *w2 = ((uint32_t)digits[9] << 28) | ((uint32_t)digits[10] << 24) | ((uint32_t)digits[11] << 20) |
          ((uint32_t)digits[12] << 16) | ((uint32_t)digits[13] << 12) | ((uint32_t)digits[14] << 8) |
          ((uint32_t)digits[15] << 4) | (uint32_t)digits[16];
}
