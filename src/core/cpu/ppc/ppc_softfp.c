// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_softfp.c
// The 601 FPU arithmetic kernel — integer-only IEEE 754 with the PowerPC
// FPSCR status model.  See ppc_softfp.h for the determinism rationale and
// the 601UM citation map.  Structure:
//
//   unpack        double bit pattern -> {class, sign, exponent, significand}
//   round_pack    exact intermediate -> rounded double + FR/FI/OX/UX/XX,
//                 handling denormalization, the per-mode overflow results,
//                 and the ±1536/±192 trap-enabled exponent wraps
//   sf_add/mul/div/madd   exact significand arithmetic (the madd product
//                 is the architecture's full 106-bit fused intermediate)
//   ppc_sf_arith / ppc_sf_frsp / ppc_sf_fctiw   the instruction surface:
//                 specials, NaN selection, exception-action lists
//
// The invariant threaded through everything: an operation's numeric path
// produces (sign, exp, sig, sticky) describing the infinitely precise
// result exactly — sig holds the top 63 bits, sticky ORs the rest — so
// round_pack's decisions (including ties) are exact, not approximations.

#include "ppc_softfp.h"

// ============================================================================
// Unpacking
// ============================================================================

typedef enum { SF_ZERO, SF_FINITE, SF_INF, SF_QNAN, SF_SNAN } sf_class_t;

typedef struct {
    sf_class_t cls;
    int sign; // 0 / 1
    int32_t exp; // value = sig * 2^(exp - 62), sig in [2^62, 2^63)
    uint64_t sig; // normalized significand, MSB at bit 62 (finite nonzero)
} sf_val_t;

static sf_val_t sf_unpack(uint64_t x) {
    sf_val_t v;
    uint32_t e = (uint32_t)(x >> 52) & 0x7FFu;
    uint64_t frac = x & 0x000FFFFFFFFFFFFFull;
    v.sign = (int)(x >> 63);
    if (e == 0x7FFu) {
        if (frac == 0) {
            v.cls = SF_INF;
        } else {
            // Quiet bit = high-order fraction bit (601UM §2.5.2.7)
            v.cls = (frac & 0x0008000000000000ull) ? SF_QNAN : SF_SNAN;
        }
        v.exp = 0;
        v.sig = 0;
        return v;
    }
    if (e == 0) {
        if (frac == 0) {
            v.cls = SF_ZERO;
            v.exp = 0;
            v.sig = 0;
            return v;
        }
        // Denormal: value = frac * 2^-1074; normalize to bit 62
        int shift = __builtin_clzll(frac) - 1; // move MSB to bit 62
        v.cls = SF_FINITE;
        v.sig = frac << shift;
        // frac's bit 52 at value-exponent -1022 would be sig bit 62;
        // frac's actual MSB is at bit (63 - clz) - 1 = 62 - shift.
        v.exp = -1022 - (shift - 10);
        return v;
    }
    v.cls = SF_FINITE;
    v.exp = (int32_t)e - 1023;
    v.sig = (frac | 0x0010000000000000ull) << 10; // 53 bits -> bit 62
    return v;
}

static inline int sf_is_nan(const sf_val_t *v) {
    return v->cls == SF_QNAN || v->cls == SF_SNAN;
}

// Quiet a NaN operand for delivery (set the high-order fraction bit;
// payload and sign preserved — 601UM folio 2-67).
static inline uint64_t sf_quiet(uint64_t x) {
    return x | 0x0008000000000000ull;
}

#define SF_DEFAULT_QNAN 0x7FF8000000000000ull // sign 0, frac MSB 1 (folio 2-67)

// ============================================================================
// FPRF result classification (601UM Table 2-2)
// ============================================================================

static uint32_t sf_fprf_of(uint64_t x) {
    uint32_t e = (uint32_t)(x >> 52) & 0x7FFu;
    uint64_t frac = x & 0x000FFFFFFFFFFFFFull;
    int neg = (int)(x >> 63);
    uint32_t code;
    if (e == 0x7FFu)
        code = frac ? 0x11u : (neg ? 0x09u : 0x05u); // QNaN / -inf / +inf
    else if (e == 0 && frac == 0)
        code = neg ? 0x12u : 0x02u; // ±zero
    else if (e == 0)
        code = neg ? 0x18u : 0x14u; // ±denormal
    else
        code = neg ? 0x08u : 0x04u; // ±normal
    return code << 12; // FPRF field at bits 15-19
}

static inline uint32_t sf_set_fprf(uint32_t fpscr, uint64_t result) {
    return (fpscr & ~PPC_FPSCR_FPRF) | sf_fprf_of(result);
}

// ============================================================================
// Rounding and packing
// ============================================================================

// Right-shift with sticky collection ("jamming"): the OR of shifted-out
// bits lands in *sticky.
static inline uint64_t sf_shr_sticky(uint64_t sig, int shift, int *sticky) {
    if (shift <= 0)
        return sig;
    if (shift > 63) {
        if (sig)
            *sticky = 1;
        return 0;
    }
    if (sig & ((1ull << shift) - 1u))
        *sticky = 1;
    return sig >> shift;
}

// Right-shift with the shifted-out bits ORed into bit 0 of the result
// (the Berkeley "jamming" form, used where the value participates in a
// subsequent subtraction).  Sound because the significand carries >= 10
// guard bits below its LSB: a shift small enough to lose real bits
// implies an exponent gap large enough that cancellation cannot lift the
// jam bit anywhere near the round position.
static inline uint64_t sf_shr_jam(uint64_t sig, int shift) {
    if (shift <= 0)
        return sig;
    if (shift > 63)
        return sig != 0;
    return (sig >> shift) | ((sig & ((1ull << shift) - 1u)) != 0);
}

// 128-bit jamming shift for the fused multiply-add accumulator.
static inline unsigned __int128 jam128(unsigned __int128 v, int32_t shift) {
    if (shift <= 0)
        return v;
    if (shift > 127)
        return v != 0;
    unsigned __int128 lost = v & (((unsigned __int128)1 << shift) - 1u);
    return (v >> shift) | (lost != 0);
}

// Pack a finite nonzero exact intermediate (sign, exp, sig at bit 62,
// sticky) into double format, rounding to `single ? 24 : 53` bits with the
// single/double exponent range, applying the 601UM §5.4.7 overflow /
// underflow action lists.  Sets FR/FI (direct), OX/UX/XX (sticky via
// ppc_fpscr_raise) and FPRF.  Never sees NaN/inf inputs; sig == 0 (an
// exact zero from cancellation) packs a signed zero.
static uint64_t sf_round_pack(int sign, int32_t exp, uint64_t sig, int sticky, int single, uint32_t *fpscr) {
    uint32_t f = *fpscr & ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
    uint32_t rmode = f & PPC_FPSCR_RN;
    const int P = single ? 24 : 53;
    const int32_t emin = single ? -126 : -1022;
    const int32_t emax = single ? 127 : 1023;
    const int32_t wrap = single ? 192 : 1536;
    uint64_t result;

    if (sig == 0) { // exact zero (caller determined the sign rule)
        *fpscr = sf_set_fprf(f, (uint64_t)sign << 63);
        return (uint64_t)sign << 63;
    }

    // Tininess is detected BEFORE rounding (601UM §5.4.7.5): the
    // unbounded-range value is below 2^emin.
    int tiny = exp < emin;
    if (tiny && (f & PPC_FPSCR_UE)) {
        // Enabled underflow: UX on tininess alone; exponent wrapped up,
        // then rounded as a normal-range value (folio 5-43).
        f = ppc_fpscr_raise(f, PPC_FPSCR_UX);
        exp += wrap;
        tiny = 0;
    } else if (tiny) {
        // Disabled underflow: denormalize to the format's minimum
        // exponent, collecting shifted-out bits as sticky (§2.5.4).
        sig = sf_shr_sticky(sig, (int)(emin - exp), &sticky);
        exp = emin;
    }

    // Round at precision P: kept bits are 62..(63-P) of sig.
    const uint64_t ulp = 1ull << (63 - P);
    const uint64_t round_bit = ulp >> 1;
    const uint64_t below = round_bit - 1u;
    int inexact = ((sig & (ulp - 1u)) != 0) || sticky;
    int increment = 0;
    switch (rmode) {
    case PPC_RN_NEAREST:
        increment = (sig & round_bit) && ((sig & below) || sticky || (sig & ulp));
        break;
    case PPC_RN_ZERO:
        increment = 0;
        break;
    case PPC_RN_PLUS:
        increment = !sign && inexact;
        break;
    case PPC_RN_MINUS:
        increment = sign && inexact;
        break;
    }
    sig &= ~(ulp - 1u);
    if (increment) {
        sig += ulp;
        if (sig == 0) { // carry out of bit 63: 1.111.. rounded up
            sig = 1ull << 62;
            exp += 1;
        } else if ((sig & (1ull << 63)) != 0) {
            sig >>= 1; // low bit is zero (sig was ulp-aligned)
            exp += 1;
        }
    }
    if (inexact) {
        f |= PPC_FPSCR_FI;
        if (increment)
            f |= PPC_FPSCR_FR;
        f = ppc_fpscr_raise(f, PPC_FPSCR_XX);
    }

    // A denormalized result that rounded up to 2^emin became normal; a
    // tiny inexact delivery is an underflow (tiny AND loss of accuracy,
    // 601UM §5.4.7.5 disabled case).
    if (tiny && inexact)
        f = ppc_fpscr_raise(f, PPC_FPSCR_UX);

    if (sig == 0) { // denormalization shifted everything out, no round-up
        result = (uint64_t)sign << 63;
        *fpscr = sf_set_fprf(f, result);
        return result;
    }

    // Overflow: the rounded unbounded-range result exceeds the format
    // maximum (601UM §5.4.7.4; can only happen on the non-tiny path).
    if (exp > emax) {
        if (f & PPC_FPSCR_OE) {
            f = ppc_fpscr_raise(f, PPC_FPSCR_OX);
            exp -= wrap; // wrapped result delivered; FR/FI per rounding
        } else {
            // Disabled overflow: per-mode ±inf / ±max, XX set, FR/FI
            // CLEARED (§5.4.7.4.1 is the operative rule over the Table
            // 2-1 "FI on disabled overflow" boilerplate).
            f = ppc_fpscr_raise(f, PPC_FPSCR_OX | PPC_FPSCR_XX);
            f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
            int to_inf = 0;
            switch (rmode) {
            case PPC_RN_NEAREST:
                to_inf = 1;
                break;
            case PPC_RN_ZERO:
                to_inf = 0;
                break;
            case PPC_RN_PLUS:
                to_inf = !sign;
                break;
            case PPC_RN_MINUS:
                to_inf = sign;
                break;
            }
            if (to_inf)
                result = ((uint64_t)sign << 63) | 0x7FF0000000000000ull;
            else if (single) // single max finite, in double format
                result = ((uint64_t)sign << 63) | 0x47EFFFFFE0000000ull;
            else
                result = ((uint64_t)sign << 63) | 0x7FEFFFFFFFFFFFFFull;
            *fpscr = sf_set_fprf(f, result);
            return result;
        }
    }

    // Encode.  A denormalized result (top bit clear; exp == emin by
    // construction) encodes as a double denormal for double targets; a
    // single-target denormal is a double NORMAL value and renormalizes
    // exactly (P=24 leaves ample low zeros).
    if ((sig & (1ull << 62)) == 0) {
        if (single) {
            int shift = __builtin_clzll(sig) - 1;
            sig <<= shift;
            exp -= shift;
            result = ((uint64_t)sign << 63) | ((uint64_t)(uint32_t)(exp + 1023) << 52) |
                     ((sig >> 10) & 0x000FFFFFFFFFFFFFull);
        } else {
            result = ((uint64_t)sign << 63) | (sig >> 10); // biased exp 0
        }
    } else {
        result =
            ((uint64_t)sign << 63) | ((uint64_t)(uint32_t)(exp + 1023) << 52) | ((sig >> 10) & 0x000FFFFFFFFFFFFFull);
    }
    *fpscr = sf_set_fprf(f, result);
    return result;
}

// ============================================================================
// Exact significand arithmetic
// ============================================================================
// Each helper produces the infinitely precise result as (sign, exp, sig at
// bit 62, sticky) and hands it to sf_round_pack.  Operands are finite and
// nonzero unless stated.

// Magnitude addition/subtraction of two unpacked finite nonzero values.
// eff_sign_b is b's sign after any operation-level inversion (fsub/fmsub).
// The jam discipline (Berkeley softfloat): significands sit at bit 62 with
// >= 10 guard bits below their LSB, and alignment shifts OR their lost
// bits into bit 0.  A shift small enough to lose real bits implies an
// exponent gap > 10, which bounds post-subtraction normalization to one
// bit — so the jam can never rise near the round position.
static uint64_t sf_add(const sf_val_t *a, const sf_val_t *b, int eff_sign_b, int single, uint32_t *fpscr) {
    uint64_t sa = a->sig, sb = b->sig;
    int32_t exp;
    int sign;
    uint64_t sig;

    if (a->sign == eff_sign_b) {
        // Same sign: align with a plain sticky (no borrow to worry
        // about), add, renormalize the possible carry.
        int sticky = 0;
        if (a->exp >= b->exp) {
            sb = sf_shr_sticky(sb, (int)(a->exp - b->exp), &sticky);
            exp = a->exp;
        } else {
            sa = sf_shr_sticky(sa, (int)(b->exp - a->exp), &sticky);
            exp = b->exp;
        }
        sign = a->sign;
        sig = sa + sb; // may carry into bit 63
        if (sig & (1ull << 63)) {
            sticky |= (int)(sig & 1u);
            sig >>= 1;
            exp += 1;
        }
        return sf_round_pack(sign, exp, sig, sticky, single, fpscr);
    }

    // Opposite signs: magnitude subtraction.
    if (a->exp == b->exp) {
        if (sa == sb) {
            // Exact zero: +0, or -0 in round-toward-minus (601UM §2.5.3)
            int zsign = ((*fpscr & PPC_FPSCR_RN) == PPC_RN_MINUS) ? 1 : 0;
            return sf_round_pack(zsign, 0, 0, 0, single, fpscr);
        }
        if (sa > sb) {
            sign = a->sign;
            sig = sa - sb;
        } else {
            sign = eff_sign_b;
            sig = sb - sa;
        }
        exp = a->exp;
    } else if (a->exp > b->exp) {
        // b aligned down with jamming; a's magnitude strictly dominates.
        sb = sf_shr_jam(sb, (int)(a->exp - b->exp));
        sign = a->sign;
        sig = sa - sb;
        exp = a->exp;
    } else {
        sa = sf_shr_jam(sa, (int)(b->exp - a->exp));
        sign = eff_sign_b;
        sig = sb - sa;
        exp = b->exp;
    }
    // Renormalize to bit 62 (sig != 0 here).
    int shift = __builtin_clzll(sig) - 1;
    sig <<= shift;
    exp -= shift;
    return sf_round_pack(sign, exp, sig, 0, single, fpscr);
}

static uint64_t sf_mul(const sf_val_t *a, const sf_val_t *c, int single, uint32_t *fpscr) {
    // 53-bit significands (drop the bit-62 padding) -> exact 105/106-bit
    // product.
    uint64_t ma = a->sig >> 10, mc = c->sig >> 10;
    unsigned __int128 p = (unsigned __int128)ma * mc;
    int sign = a->sign ^ c->sign;
    int32_t exp = a->exp + c->exp;
    // p has its MSB at bit 105 (2 <= m*m' < 4) or 104.  Normalize to a
    // 63-bit sig + sticky.
    int sticky = 0;
    uint64_t hi;
    if (p >> 105) {
        hi = (uint64_t)(p >> 43);
        if ((uint64_t)p & ((1ull << 43) - 1u))
            sticky = 1;
        exp += 1;
    } else {
        hi = (uint64_t)(p >> 42);
        if ((uint64_t)p & ((1ull << 42) - 1u))
            sticky = 1;
    }
    return sf_round_pack(sign, exp, hi, sticky, single, fpscr);
}

static uint64_t sf_div(const sf_val_t *a, const sf_val_t *b, int single, uint32_t *fpscr) {
    uint64_t ma = a->sig >> 10, mb = b->sig >> 10; // 53-bit significands
    int sign = a->sign ^ b->sign;
    int32_t exp = a->exp - b->exp;
    // Scale the dividend so the quotient lands with its MSB at bit 62:
    // ma/mb is in (1/2, 2); shift ma up by 62 bits and adjust below.
    unsigned __int128 n = (unsigned __int128)ma << 62;
    uint64_t q = (uint64_t)(n / mb);
    uint64_t r = (uint64_t)(n % mb);
    // q has its MSB at bit 62 (ma >= mb) or bit 61 (ma < mb).
    int sticky = r != 0;
    if ((q & (1ull << 62)) == 0) {
        // Shift up one and refine: q,r describe ma*2^62 = q*mb + r, so
        // ma*2^63 = 2q*mb + 2r; propagate the extra quotient bit.
        q <<= 1;
        r <<= 1;
        if (r >= mb) {
            q += 1;
            r -= mb;
        }
        sticky = r != 0;
        exp -= 1;
    }
    return sf_round_pack(sign, exp, q, sticky, single, fpscr);
}

// Fused multiply-add: the exact 106-bit product plus the aligned addend
// (601UM §2.5.1.1: all product bits take part in the add; one X' sticky
// participates).  eff_sign_b is the addend sign after msub inversion.
static uint64_t sf_madd(const sf_val_t *a, const sf_val_t *c, const sf_val_t *b, int eff_sign_b, int single,
                        uint32_t *fpscr) {
    // Exact product: 106 bits, value = p * 2^(pexp - 105)
    uint64_t ma = a->sig >> 10, mc = c->sig >> 10;
    unsigned __int128 p = (unsigned __int128)ma * mc;
    int psign = a->sign ^ c->sign;
    int32_t pexp = a->exp + c->exp + ((p >> 105) ? 1 : 0);
    int pmsb = (p >> 105) ? 105 : 104;

    if (b->cls == SF_ZERO) {
        // Addend zero: the result is just the product (plus the add-rule
        // sign if the product is also exactly zero — impossible here
        // since operands are nonzero).
        int sticky = 0;
        uint64_t hi;
        if (pmsb == 105) {
            hi = (uint64_t)(p >> 43);
            sticky = ((uint64_t)p & ((1ull << 43) - 1u)) != 0;
        } else {
            hi = (uint64_t)(p >> 42);
            sticky = ((uint64_t)p & ((1ull << 42) - 1u)) != 0;
        }
        return sf_round_pack(psign, pexp, hi, sticky, single, fpscr);
    }

    // Work in a 128-bit accumulator: product placed with MSB at bit 125,
    // giving 20+ guard bits under the round position — the same jam
    // argument as sf_add: a lossy alignment shift implies an exponent gap
    // that bounds cancellation to one bit of normalization.
    unsigned __int128 acc_p = p << (125 - pmsb);
    unsigned __int128 acc_b = (unsigned __int128)(b->sig >> 10) << (125 - 52);
    int32_t bexp = b->exp;
    int32_t exp;
    if (pexp >= bexp) {
        acc_b = jam128(acc_b, pexp - bexp);
        exp = pexp;
    } else {
        acc_p = jam128(acc_p, bexp - pexp);
        exp = bexp;
    }

    int sign;
    unsigned __int128 acc;
    if (psign == eff_sign_b) {
        sign = psign;
        acc = acc_p + acc_b; // MSB at 125 or 126
    } else {
        if (acc_p == acc_b) {
            // Exact zero (equal jammed values imply equal exact values:
            // a jam bit only exists alongside an exponent gap, which
            // makes the magnitudes strictly unequal).
            int zsign = ((*fpscr & PPC_FPSCR_RN) == PPC_RN_MINUS) ? 1 : 0;
            return sf_round_pack(zsign, 0, 0, 0, single, fpscr);
        }
        if (acc_p > acc_b) {
            sign = psign;
            acc = acc_p - acc_b;
        } else {
            sign = eff_sign_b;
            acc = acc_b - acc_p;
        }
    }
    int sticky = 0;

    // Normalize the accumulator to MSB at bit 125, then narrow to a
    // 63-bit sig + sticky for round_pack.
    int msb = 127;
    while (msb >= 0 && !(acc >> msb))
        msb--; // acc != 0 here
    exp += msb - 125;
    // Narrow: keep top 63 bits.
    int drop = msb - 62;
    uint64_t sig;
    if (drop > 0) {
        if (acc & (((unsigned __int128)1 << drop) - 1u))
            sticky = 1;
        sig = (uint64_t)(acc >> drop);
    } else {
        sig = (uint64_t)acc << -drop;
    }
    return sf_round_pack(sign, exp, sig, sticky, single, fpscr);
}

// ============================================================================
// The instruction surface
// ============================================================================

// Truncate a NaN result of a single-precision operation to single payload
// (low 29 fraction bits cleared — 601UM folio 2-70: single results carry
// zero there; AUTHORITY-PENDING for NaNs, Appendix F absent).
static inline uint64_t sf_nan_single(uint64_t nan) {
    uint64_t r = nan & ~0x1FFFFFFFull;
    // Keep it a NaN if the payload lived entirely in the low bits.
    if ((r & 0x000FFFFFFFFFFFFFull) == 0)
        r |= 0x0008000000000000ull;
    return r;
}

int ppc_sf_arith(ppc_sf_op_t op, uint64_t a, uint64_t b, uint64_t c, int single, uint32_t *fpscr, uint64_t *frt) {
    sf_val_t va = sf_unpack(a);
    sf_val_t vb = sf_unpack(b);
    sf_val_t vc = sf_unpack(c);
    uint32_t f = *fpscr;
    int is_madd = (op == PPC_SF_MADD || op == PPC_SF_MSUB || op == PPC_SF_NMADD || op == PPC_SF_NMSUB);
    int is_mul = (op == PPC_SF_MUL);
    int is_div = (op == PPC_SF_DIV);
    int is_addsub = (op == PPC_SF_ADD || op == PPC_SF_SUB);
    // Effective addend-sign inversion (fsub / fmsub / fnmsub subtract frB)
    int inv_b = (op == PPC_SF_SUB || op == PPC_SF_MSUB || op == PPC_SF_NMSUB);
    // Final negation (fnmadd / fnmsub), applied after rounding, never to
    // NaN results (601UM folio 10-76).
    int negate = (op == PPC_SF_NMADD || op == PPC_SF_NMSUB);

    // --- collect invalid-operation conditions (601UM §5.4.7.2.1) ---
    uint32_t vx = 0;
    if (va.cls == SF_SNAN || (is_addsub || is_div ? vb.cls == SF_SNAN : 0) ||
        (is_mul || is_madd ? vc.cls == SF_SNAN : 0) || (is_madd && vb.cls == SF_SNAN))
        vx |= PPC_FPSCR_VXSNAN;
    if (is_addsub && va.cls == SF_INF && vb.cls == SF_INF && (va.sign != (vb.sign ^ inv_b)))
        vx |= PPC_FPSCR_VXISI;
    if ((is_mul || is_madd) && ((va.cls == SF_INF && vc.cls == SF_ZERO) || (va.cls == SF_ZERO && vc.cls == SF_INF)))
        vx |= PPC_FPSCR_VXIMZ;
    if (is_madd && !(vx & PPC_FPSCR_VXIMZ) && !sf_is_nan(&va) && !sf_is_nan(&vc)) {
        // Product is a well-defined ±inf iff either factor is inf; the
        // add step then checks magnitude subtraction of infinities.
        if ((va.cls == SF_INF || vc.cls == SF_INF) && vb.cls == SF_INF) {
            int psign = va.sign ^ vc.sign;
            if (psign != (vb.sign ^ inv_b))
                vx |= PPC_FPSCR_VXISI;
        }
    }
    if (is_div && va.cls == SF_INF && vb.cls == SF_INF)
        vx |= PPC_FPSCR_VXIDI;
    if (is_div && va.cls == SF_ZERO && vb.cls == SF_ZERO)
        vx |= PPC_FPSCR_VXZDZ;

    if (vx) {
        f = ppc_fpscr_raise(f, vx);
        if (f & PPC_FPSCR_VE) {
            // Enabled invalid: suppressed — frD and FPRF unchanged,
            // FR/FI cleared (601UM folio 5-40).
            f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
            *fpscr = ppc_fpscr_derive(f);
            return 0;
        }
    }

    // --- NaN result selection (601UM §2.5.2.7: frA, then frB, then frC) ---
    if (sf_is_nan(&va) || sf_is_nan(&vb) || sf_is_nan(&vc)) {
        int b_participates = is_addsub || is_div || is_madd;
        int c_participates = is_mul || is_madd;
        uint64_t nan;
        if (sf_is_nan(&va))
            nan = sf_quiet(a);
        else if (b_participates && sf_is_nan(&vb))
            nan = sf_quiet(b);
        else if (c_participates && sf_is_nan(&vc))
            nan = sf_quiet(c);
        else if (vx)
            nan = SF_DEFAULT_QNAN; // generated by the disabled invalid op
        else
            goto numeric; // NaN only in a non-participating operand slot
        if (single)
            nan = sf_nan_single(nan);
        f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
        f = sf_set_fprf(f, nan);
        *fpscr = ppc_fpscr_derive(f);
        *frt = nan;
        return 1;
    }
    if (vx) {
        // Invalid without NaN operands (∞-∞, ∞*0, ∞/∞, 0/0): the
        // generated default QNaN (601UM folio 2-67).
        uint64_t nan = SF_DEFAULT_QNAN;
        f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
        f = sf_set_fprf(f, nan);
        *fpscr = ppc_fpscr_derive(f);
        *frt = nan;
        return 1;
    }

numeric:;
    uint64_t result;
    // --- zero divide (601UM §5.4.7.3: finite nonzero / zero) ---
    if (is_div && vb.cls == SF_ZERO) { // va is finite nonzero here
        f = ppc_fpscr_raise(f, PPC_FPSCR_ZX);
        if (f & PPC_FPSCR_ZE) {
            f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
            *fpscr = ppc_fpscr_derive(f);
            return 0;
        }
        result = ((uint64_t)(va.sign ^ vb.sign) << 63) | 0x7FF0000000000000ull;
        f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
        f = sf_set_fprf(f, result);
        *fpscr = ppc_fpscr_derive(f);
        *frt = result;
        return 1;
    }

    // --- infinities (valid combinations are exact; §2.5.2.6) ---
    {
        int inf_sign = 0, is_inf = 0, is_zero = 0, zero_sign = 0;
        if (is_addsub) {
            if (va.cls == SF_INF) {
                is_inf = 1;
                inf_sign = va.sign;
            } else if (vb.cls == SF_INF) {
                is_inf = 1;
                inf_sign = vb.sign ^ inv_b;
            }
        } else if (is_mul) {
            if (va.cls == SF_INF || vc.cls == SF_INF) {
                is_inf = 1;
                inf_sign = va.sign ^ vc.sign;
            }
        } else if (is_div) {
            if (va.cls == SF_INF) {
                is_inf = 1;
                inf_sign = va.sign ^ vb.sign;
            } else if (vb.cls == SF_INF) {
                is_zero = 1;
                zero_sign = va.sign ^ vb.sign;
            }
        } else { // madd family
            int psign = va.sign ^ vc.sign;
            int prod_inf = (va.cls == SF_INF || vc.cls == SF_INF); // ∞*0 was VXIMZ
            if (prod_inf) {
                is_inf = 1;
                inf_sign = psign; // same-sign inf addend (else VXISI)
            } else if (vb.cls == SF_INF) {
                is_inf = 1;
                inf_sign = vb.sign ^ inv_b;
            }
        }
        if (is_inf) {
            result = ((uint64_t)inf_sign << 63) | 0x7FF0000000000000ull;
            if (negate)
                result ^= 0x8000000000000000ull;
            f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
            f = sf_set_fprf(f, result);
            *fpscr = ppc_fpscr_derive(f);
            *frt = result;
            return 1;
        }
        if (is_zero) { // finite / inf
            result = (uint64_t)zero_sign << 63;
            f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
            f = sf_set_fprf(f, result);
            *fpscr = ppc_fpscr_derive(f);
            *frt = result;
            return 1;
        }
    }

    // --- zeros in finite arithmetic ---
    if (is_addsub) {
        int sb = vb.sign ^ inv_b;
        if (va.cls == SF_ZERO && vb.cls == SF_ZERO) {
            int sign = (va.sign == sb) ? va.sign : (((f & PPC_FPSCR_RN) == PPC_RN_MINUS) ? 1 : 0);
            result = sf_round_pack(sign, 0, 0, 0, single, &f);
        } else if (va.cls == SF_ZERO) {
            sf_val_t vbe = vb;
            vbe.sign = sb;
            result = sf_round_pack(vbe.sign, vbe.exp, vbe.sig, 0, single, &f);
        } else if (vb.cls == SF_ZERO) {
            result = sf_round_pack(va.sign, va.exp, va.sig, 0, single, &f);
        } else {
            result = sf_add(&va, &vb, sb, single, &f);
        }
    } else if (is_mul) {
        if (va.cls == SF_ZERO || vc.cls == SF_ZERO) {
            result = sf_round_pack(va.sign ^ vc.sign, 0, 0, 0, single, &f);
        } else {
            result = sf_mul(&va, &vc, single, &f);
        }
    } else if (is_div) {
        if (va.cls == SF_ZERO) { // 0 / finite-nonzero
            result = sf_round_pack(va.sign ^ vb.sign, 0, 0, 0, single, &f);
        } else {
            result = sf_div(&va, &vb, single, &f);
        }
    } else { // madd family, all finite
        int sb = vb.sign ^ inv_b;
        if (va.cls == SF_ZERO || vc.cls == SF_ZERO) {
            // Product is an exact ±0; result is the addend (rounded to
            // the target precision), or the signed-zero add rule.
            int psign = va.sign ^ vc.sign;
            if (vb.cls == SF_ZERO) {
                int sign = (psign == sb) ? psign : (((f & PPC_FPSCR_RN) == PPC_RN_MINUS) ? 1 : 0);
                result = sf_round_pack(sign, 0, 0, 0, single, &f);
            } else {
                result = sf_round_pack(sb, vb.exp, vb.sig, 0, single, &f);
            }
        } else {
            result = sf_madd(&va, &vc, &vb, sb, single, &f);
        }
    }

    if (negate)
        result ^= 0x8000000000000000ull;
    // FPRF was set from the pre-negation value; fix the sign-dependent
    // class for the nm forms.
    if (negate)
        f = sf_set_fprf(f, result);

    *fpscr = ppc_fpscr_derive(f);
    *frt = result;
    return 1;
}

int ppc_sf_frsp(uint64_t b, uint32_t *fpscr, uint64_t *frt) {
    sf_val_t vb = sf_unpack(b);
    uint32_t f = *fpscr;

    if (sf_is_nan(&vb)) {
        if (vb.cls == SF_SNAN) {
            f = ppc_fpscr_raise(f, PPC_FPSCR_VXSNAN);
            if (f & PPC_FPSCR_VE) {
                f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
                *fpscr = ppc_fpscr_derive(f);
                return 0;
            }
        }
        // Quiet, truncate the payload to single precision (low 29
        // fraction bits cleared; AUTHORITY-PENDING, Appendix F absent).
        uint64_t nan = sf_nan_single(sf_quiet(b));
        f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
        f = sf_set_fprf(f, nan);
        *fpscr = ppc_fpscr_derive(f);
        *frt = nan;
        return 1;
    }
    if (vb.cls == SF_INF || vb.cls == SF_ZERO) {
        f &= ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
        f = sf_set_fprf(f, b);
        *fpscr = ppc_fpscr_derive(f);
        *frt = b;
        return 1;
    }
    *frt = sf_round_pack(vb.sign, vb.exp, vb.sig, 0, 1, &f);
    *fpscr = ppc_fpscr_derive(f);
    return 1;
}

// fres (604; PEM fresx page): single-precision reciprocal estimate,
// architected envelope 2^-8 relative, value implementation-defined.  This
// model's constant choice: the CORRECTLY-ROUNDED single-precision quotient
// 1.0/frB from the division kernel — deterministic and well inside the
// envelope.  Architected FPSCR effects only: FPRF, FX, OX, UX, ZX, VXSNAN;
// FR/FI are "undefined" and read cleared; XX is NOT an fres effect and the
// quotient's inexactness is masked off.
int ppc_sf_fres(uint64_t b, uint32_t *fpscr, uint64_t *frt) {
    uint32_t f0 = *fpscr;
    uint32_t f = f0;
    uint64_t r;
    int wrote = ppc_sf_arith(PPC_SF_DIV, 0x3FF0000000000000ull /* 1.0 */, b, 0, 1, &f, &r);
    // Rebuild the FPSCR from the pre-instruction image plus the allowed
    // newly-raised stickies; FX follows only those (the raise rule).
    const uint32_t allowed = PPC_FPSCR_OX | PPC_FPSCR_UX | PPC_FPSCR_ZX | PPC_FPSCR_VXSNAN;
    uint32_t newly = f & ~f0 & allowed;
    uint32_t nf = (f0 & ~(PPC_FPSCR_FPRF | PPC_FPSCR_FR | PPC_FPSCR_FI)) | newly;
    nf |= (wrote ? f : f0) & PPC_FPSCR_FPRF; // suppressed delivery leaves FPRF unchanged
    if (newly)
        nf |= PPC_FPSCR_FX;
    *fpscr = ppc_fpscr_derive(nf);
    if (wrote)
        *frt = r;
    return wrote;
}

// Integer square root: floor(sqrt(a)) for a < 2^126 (the frsqrte scaling
// keeps the root's MSB at bit 62).  Classic bit-pair method, maintaining
// rem = prefix(a) - root^2.
static uint64_t sf_isqrt128(unsigned __int128 a) {
    unsigned __int128 rem = 0, root = 0;
    for (int i = 62; i >= 0; i--) {
        rem = (rem << 2) | ((a >> (2 * i)) & 3u);
        unsigned __int128 cand = (root << 2) | 1u;
        root <<= 1;
        if (rem >= cand) {
            rem -= cand;
            root |= 1u;
        }
    }
    return (uint64_t)root;
}

// frsqrte (604; PEM frsqrtex page): double-precision reciprocal square
// root estimate, architected envelope 2^-5 relative, value implementation-
// defined.  This model's constant choice: divide 1.0 by the 62-bit
// truncated integer square root of the significand and round to nearest —
// relative error < 2^-61.  Architected FPSCR effects only: FPRF, FX, ZX,
// VXSNAN, VXSQRT; FR/FI read cleared; no OX/UX/XX.
int ppc_sf_frsqrte(uint64_t b, uint32_t *fpscr, uint64_t *frt) {
    sf_val_t vb = sf_unpack(b);
    uint32_t f = *fpscr & ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
    uint64_t result;

    if (sf_is_nan(&vb)) {
        if (vb.cls == SF_SNAN) {
            f = ppc_fpscr_raise(f, PPC_FPSCR_VXSNAN);
            if (f & PPC_FPSCR_VE) {
                *fpscr = ppc_fpscr_derive(f);
                return 0;
            }
        }
        result = sf_quiet(b);
    } else if (vb.cls == SF_ZERO) {
        // ±0 → ±inf with ZX (no result when ZE enabled).
        f = ppc_fpscr_raise(f, PPC_FPSCR_ZX);
        if (f & PPC_FPSCR_ZE) {
            *fpscr = ppc_fpscr_derive(f);
            return 0;
        }
        result = ((uint64_t)vb.sign << 63) | 0x7FF0000000000000ull;
    } else if (vb.sign) {
        // Negative (incl. -inf) → default QNaN with VXSQRT.
        f = ppc_fpscr_raise(f, PPC_FPSCR_VXSQRT);
        if (f & PPC_FPSCR_VE) {
            *fpscr = ppc_fpscr_derive(f);
            return 0;
        }
        result = SF_DEFAULT_QNAN;
    } else if (vb.cls == SF_INF) {
        result = 0; // +inf → +0
    } else {
        // Positive finite: value = m2 * 2^e2 with e2 even and the
        // significand at bit 62 (bit 63 for the odd-exponent double).
        int32_t e2 = vb.exp;
        uint64_t sig_a = vb.sig;
        if (e2 & 1) {
            sig_a <<= 1; // m2 = 2m in [2,4)
            e2 -= 1;
        }
        // S = floor(sqrt(m2) * 2^62), in [2^62, 2^63).
        uint64_t s = sf_isqrt128((unsigned __int128)sig_a << 62);
        // Q = floor(2^124 / S) ≈ (1/sqrt(m2)) * 2^62, in (2^61, 2^62].
        unsigned __int128 n = (unsigned __int128)1 << 124;
        uint64_t q = (uint64_t)(n / s);
        uint64_t rem = (uint64_t)(n % s);
        int32_t exp = -(e2 >> 1);
        if ((q & (1ull << 62)) == 0) {
            // MSB at 61: shift up one quotient bit (the sf_div refinement).
            q <<= 1;
            unsigned __int128 r2 = (unsigned __int128)rem << 1;
            if (r2 >= s) {
                q += 1;
                r2 -= s;
            }
            rem = (uint64_t)r2;
            exp -= 1;
        }
        int sticky = rem != 0 || ((unsigned __int128)s * s != (unsigned __int128)sig_a << 62);
        // Round through the shared packer, then strip the non-architected
        // status its rounding raised (frsqrte reports no OX/UX/XX and its
        // FR/FI are "undefined" → cleared); only FPRF survives from it.
        uint32_t fr = f;
        result = sf_round_pack(0, exp, q, sticky, 0, &fr);
        f = (f & ~PPC_FPSCR_FPRF) | (fr & PPC_FPSCR_FPRF);
        *fpscr = ppc_fpscr_derive(f);
        *frt = result;
        return 1;
    }

    f = sf_set_fprf(f, result);
    *fpscr = ppc_fpscr_derive(f);
    *frt = result;
    return 1;
}

int ppc_sf_fctiw(uint64_t b, int round_to_zero, uint32_t *fpscr, uint64_t *frt) {
    sf_val_t vb = sf_unpack(b);
    uint32_t f = *fpscr & ~(PPC_FPSCR_FR | PPC_FPSCR_FI);
    uint32_t rmode = round_to_zero ? PPC_RN_ZERO : (f & PPC_FPSCR_RN);
    // The 601 stores $FFF80000 in the undefined high word (folio 3-38);
    // FPSCR[FPRF] is undefined for fctiw — we leave it unchanged
    // (deterministic choice, documented in ppc.md).
    const uint64_t hi = 0xFFF8000000000000ull;

    if (sf_is_nan(&vb) || vb.cls == SF_INF) {
        uint32_t vx = PPC_FPSCR_VXCVI | (vb.cls == SF_SNAN ? PPC_FPSCR_VXSNAN : 0);
        f = ppc_fpscr_raise(f, vx);
        if (f & PPC_FPSCR_VE) {
            *fpscr = ppc_fpscr_derive(f);
            return 0;
        }
        // NaN and -inf saturate to the most negative integer; +inf to the
        // most positive (the instruction-page rule; folio 10-63).
        uint32_t sat = (vb.cls == SF_INF && !vb.sign) ? 0x7FFFFFFFu : 0x80000000u;
        *fpscr = ppc_fpscr_derive(f);
        *frt = hi | sat;
        return 1;
    }

    int64_t ival = 0;
    int inexact = 0, increment = 0;
    if (vb.cls != SF_ZERO) {
        if (vb.exp > 62) {
            ival = vb.sign ? INT64_MIN : INT64_MAX; // far out of range
        } else if (vb.exp < -1) {
            // |value| < 1/2: integer part 0, fraction nonzero.  Nearest
            // stays 0 (below the half-way point; |value| in [1/2, 1)
            // takes the general branch below); only the directed
            // away-from-zero modes reach 1.
            inexact = 1;
            uint64_t mag = 0;
            if ((rmode == PPC_RN_PLUS && !vb.sign) || (rmode == PPC_RN_MINUS && vb.sign))
                mag = 1, increment = 1;
            ival = vb.sign ? -(int64_t)mag : (int64_t)mag;
        } else {
            // Integer part: sig holds 63 bits at exponent vb.exp
            int shift = 62 - vb.exp; // bits below the integer point
            uint64_t mag = vb.sig >> shift;
            uint64_t frac_mask = (shift > 0) ? ((1ull << shift) - 1u) : 0;
            uint64_t frac = vb.sig & frac_mask;
            if (frac) {
                inexact = 1;
                uint64_t half = 1ull << (shift - 1);
                switch (rmode) {
                case PPC_RN_NEAREST:
                    if (frac > half || (frac == half && (mag & 1u)))
                        increment = 1;
                    break;
                case PPC_RN_ZERO:
                    break;
                case PPC_RN_PLUS:
                    increment = !vb.sign;
                    break;
                case PPC_RN_MINUS:
                    increment = vb.sign;
                    break;
                }
                mag += (uint64_t)increment;
            }
            ival = vb.sign ? -(int64_t)mag : (int64_t)mag;
        }
    }

    if (ival > INT32_MAX || ival < INT32_MIN) {
        // The ROUNDED result is unrepresentable: invalid integer convert
        // with the instruction-page saturation (AUTHORITY-PENDING for
        // the rounded-vs-unrounded boundary, Appendix F absent).
        f = ppc_fpscr_raise(f, PPC_FPSCR_VXCVI);
        if (f & PPC_FPSCR_VE) {
            *fpscr = ppc_fpscr_derive(f);
            return 0;
        }
        *fpscr = ppc_fpscr_derive(f);
        *frt = hi | (ival > 0 ? 0x7FFFFFFFu : 0x80000000u);
        return 1;
    }

    if (inexact) {
        f |= PPC_FPSCR_FI;
        if (increment)
            f |= PPC_FPSCR_FR; // magnitude incremented (AUTHORITY-PENDING)
        f = ppc_fpscr_raise(f, PPC_FPSCR_XX);
    }
    *fpscr = ppc_fpscr_derive(f);
    *frt = hi | (uint64_t)(uint32_t)(int32_t)ival;
    return 1;
}
