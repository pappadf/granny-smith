// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_fpu.c
// PPC (MPC601) floating-point bodies: the FPR file's load/store format
// conversions, compares, the FPSCR-instruction semantics, and the Phase-E
// arithmetic surface — thin wrappers over the integer-only kernel in
// ppc_softfp.c (values, rounding, and every status bit computed in
// integer code; see ppc_softfp.h for the determinism rationale).
//
// Determinism rule (§3.6): NaN bit patterns never pass through host
// floating-point arithmetic — WASM does not guarantee NaN payload/sign
// propagation, and checkpoints must be byte-identical across hosts.  The
// single<->double conversions below therefore handle NaN in integer code
// and use host conversion (exact or correctly-rounded IEEE, deterministic
// on both hosts) only for numeric values.

#include "ppc_ops.h"

#include "ppc_softfp.h"

#include "log.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("ppc");

// === single <-> double conversion ==========================================

// lfs/lfsx: 32-bit single to the FPR's double format.  Numeric values
// convert exactly (float→double is lossless); NaN payloads are widened in
// integer code (top 23 fraction bits become the top of the 52-bit field).
uint64_t ppc_f32_to_f64(uint32_t s) {
    uint32_t exp = (s >> 23) & 0xFFu;
    uint64_t sign = (uint64_t)(s >> 31) << 63;
    if (exp == 0xFFu) { // inf / NaN: rebuild the pattern bitwise
        return sign | 0x7FF0000000000000ull | ((uint64_t)(s & 0x7FFFFFu) << 29);
    }
    float f;
    double d;
    memcpy(&f, &s, 4);
    d = (double)f; // exact for every finite single incl. denormals
    uint64_t bits;
    memcpy(&bits, &d, 8);
    return bits;
}

// stfs/stfsu/stfsx/stfsux: the 601UM §3.5.10.1 store conversion, verbatim.
// This is NOT a rounding conversion and must not be written as one (the
// host (float) cast rounds to nearest, which is wrong at the last bit for
// any value the single format cannot hold exactly — powerpc-test's
// stfsx/stfsux vectors are what caught it).  The manual's algorithm:
//
//   no denormalization (frS[1-11] > 896, or the value is +-0):
//       WORD[0-1] <- frS[0-1];  WORD[2-31] <- frS[5-34]
//   denormalization (874 <= frS[1-11] <= 896):
//       shift the significand right until the exponent reaches -126, then
//       WORD[0] <- sign;  WORD[1-8] <- 0;  WORD[9-31] <- frac[1-23]
//
// The first case is a pure bit extraction, which is also why inf and NaN
// need no special case: exponent 2047 > 896, so the pattern and the top 23
// payload bits carry across unchanged.  The store raises no exceptions
// (§2.5.5), so no FPSCR argument.
uint32_t ppc_f64_to_f32_store(uint64_t d) {
    uint32_t expfield = (uint32_t)(d >> 52) & 0x7FFu;

    if (expfield > 896u || (d & 0x7FFFFFFFFFFFFFFFull) == 0)
        return (uint32_t)((d >> 32) & 0xC0000000u) | (uint32_t)((d >> 29) & 0x3FFFFFFFu);

    // Denormalize: frac carries the hidden bit at 52, and each step of the
    // manual's loop is one right shift.  Below the 874 floor the manual
    // specifies nothing; shifting out is the natural continuation and
    // lands on a signed zero, which is the value the single format has.
    uint64_t frac = (1ull << 52) | (d & 0x000FFFFFFFFFFFFFull);
    int32_t exp = (int32_t)expfield - 1023;
    uint32_t shift = (exp < -126) ? (uint32_t)(-126 - exp) : 0u;
    frac = (shift > 52u) ? 0u : (frac >> shift);

    return (uint32_t)((d >> 32) & 0x80000000u) | (uint32_t)((frac >> 29) & 0x007FFFFFu);
}

// === FP compares ============================================================

// Classify a double bit pattern for compare purposes without host FP.
static inline bool f64_is_nan(uint64_t d) {
    return ((d >> 52) & 0x7FFu) == 0x7FFu && (d & 0xFFFFFFFFFFFFFull) != 0;
}

static inline bool f64_is_snan(uint64_t d) {
    return f64_is_nan(d) && !(d & 0x0008000000000000ull); // quiet bit clear
}

// Map an IEEE double's bit pattern onto a signed key that orders the same
// way the values do: a positive pattern is already magnitude-ordered, and a
// negative one mirrors below zero.  Both operands are non-NaN here, and
// zeroes arrive normalized, so |x| never exceeds INT64_MAX.
static int64_t f64_order_key(uint64_t x) {
    return (x >> 63) ? -(int64_t)(x & 0x7FFFFFFFFFFFFFFFull) : (int64_t)x;
}

static int f64_compare(uint64_t a, uint64_t b) {
    // Normalize both zeroes to +0 so -0 == +0.
    if ((a & 0x7FFFFFFFFFFFFFFFull) == 0)
        a = 0;
    if ((b & 0x7FFFFFFFFFFFFFFFull) == 0)
        b = 0;
    int64_t ka = f64_order_key(a), kb = f64_order_key(b);
    return (ka < kb) ? -1 : (ka > kb) ? 1 : 0;
}

// Enabled-exception delivery (601UM §5.4.7): FEX & MSR[FE0|FE1] takes the
// program exception PRECISELY — SRR0 = the causing instruction, SRR1 bit
// 11 set (the 601 ORs FE0/FE1; either bit means precise mode).  Called
// after every FPSCR-updating instruction completes its register effects.
void ppc_fp_trap_check(ppc_t *p) {
    if ((p->fpscr & PPC_FPSCR_FEX) && (p->msr & (PPC_MSR_FE0 | PPC_MSR_FE1)))
        ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_FPENABLED, p->instruction_pc);
}

// The A-form arithmetic surface: kernel call + writeback + CR1 + trap.
void ppc_fp_arith(ppc_t *p, uint32_t iw, int op, bool single) {
    uint64_t frt;
    if (ppc_sf_arith((ppc_sf_op_t)op, p->fpr[PPC_RA(iw)], p->fpr[PPC_RB(iw)], p->fpr[PPC_FRC(iw)], single, &p->fpscr,
                     &frt))
        p->fpr[PPC_RT(iw)] = frt;
    if (PPC_RC(iw))
        ppc_set_cr_field(p, 1, p->fpscr >> 28);
    ppc_fp_trap_check(p);
}

void ppc_do_frsp(ppc_t *p, uint32_t iw) {
    uint64_t frt;
    if (ppc_sf_frsp(p->fpr[PPC_RB(iw)], &p->fpscr, &frt))
        p->fpr[PPC_RT(iw)] = frt;
    if (PPC_RC(iw))
        ppc_set_cr_field(p, 1, p->fpscr >> 28);
    ppc_fp_trap_check(p);
}

void ppc_do_fctiw(ppc_t *p, uint32_t iw, bool round_to_zero) {
    uint64_t frt;
    if (ppc_sf_fctiw(p->fpr[PPC_RB(iw)], round_to_zero, &p->fpscr, &frt))
        p->fpr[PPC_RT(iw)] = frt;
    if (PPC_RC(iw))
        ppc_set_cr_field(p, 1, p->fpscr >> 28);
    ppc_fp_trap_check(p);
}

// fcmpu/fcmpo shared body (601UM folios 10-61/10-62): SNaN raises VXSNAN
// always, plus VXVC for the ordered compare only when VE=0; a QNaN with
// no SNaN raises VXVC for the ordered compare unconditionally.  FPCC is
// set to reflect the (un)ordered outcome in every case; FR/FI/C are
// untouched.
void ppc_fcmp(ppc_t *p, uint32_t iw, bool ordered) {
    uint64_t a = p->fpr[PPC_RA(iw)], b = p->fpr[PPC_RB(iw)];
    uint32_t f = p->fpscr;
    uint32_t c;
    if (f64_is_nan(a) || f64_is_nan(b)) {
        c = 1u; // unordered (FU)
        if (f64_is_snan(a) || f64_is_snan(b)) {
            uint32_t vx = PPC_FPSCR_VXSNAN;
            if (ordered && !(f & PPC_FPSCR_VE))
                vx |= PPC_FPSCR_VXVC;
            f = ppc_fpscr_raise(f, vx);
        } else if (ordered) {
            f = ppc_fpscr_raise(f, PPC_FPSCR_VXVC);
        }
    } else {
        int cmp = f64_compare(a, b);
        c = (cmp < 0) ? 8u : (cmp > 0) ? 4u : 2u;
    }
    p->fpscr = ppc_fpscr_derive((f & ~PPC_FPSCR_FPCC) | (c << 12));
    ppc_set_cr_field(p, PPC_CRFD(iw), c);
    ppc_fp_trap_check(p);
}

// mcrfs (601UM folio 10-120): copy FPSCR field crfS to CR crfD, then
// clear the EXCEPTION bits that were copied — and only those (field 3
// carries VXVC beside FR/FI/C, which are not exception bits; FEX/VX are
// derived and re-derived after the clear).
void ppc_do_mcrfs(ppc_t *p, uint32_t iw) {
    static const uint32_t clear_mask[8] = {
        PPC_FPSCR_FX | PPC_FPSCR_OX, // field 0 (FEX/VX re-derive)
        PPC_FPSCR_UX | PPC_FPSCR_ZX | PPC_FPSCR_XX | PPC_FPSCR_VXSNAN, // 1
        PPC_FPSCR_VXISI | PPC_FPSCR_VXIDI | PPC_FPSCR_VXZDZ | PPC_FPSCR_VXIMZ, // 2
        PPC_FPSCR_VXVC, // 3 (FR/FI/C copied, not cleared)
        0, // 4: FPCC
        PPC_FPSCR_VXSOFT | PPC_FPSCR_VXSQRT | PPC_FPSCR_VXCVI, // 5
        0, // 6: enables
        0, // 7: NI/RN
    };
    uint32_t crfs = PPC_CRFS(iw);
    uint32_t field = (p->fpscr >> (28 - 4 * crfs)) & 0xFu;
    ppc_set_cr_field(p, PPC_CRFD(iw), field);
    p->fpscr = ppc_fpscr_derive(p->fpscr & ~clear_mask[crfs]);
}

// mtfsf / mtfsfi (601UM folios 10-133/10-134): FX and OX come verbatim
// from the source (the 0->1 transition rule does NOT apply); FEX and VX
// are never writable and re-derive.  mtfsb0/mtfsb1 (folios 10-131/132):
// bits 1 and 2 cannot be explicitly written.
void ppc_do_mtfsf(ppc_t *p, uint32_t iw) {
    uint32_t m = ppc_crm_mask((iw >> 17) & 0xFFu) & ~PPC_FPSCR_UNWRITABLE;
    p->fpscr = ppc_fpscr_derive(((uint32_t)p->fpr[PPC_RB(iw)] & m) | (p->fpscr & ~m));
    if (PPC_RC(iw))
        ppc_set_cr_field(p, 1, p->fpscr >> 28);
    ppc_fp_trap_check(p);
}

void ppc_do_mtfsfi(ppc_t *p, uint32_t iw) {
    uint32_t sh = 28 - 4 * PPC_CRFD(iw);
    uint32_t m = (0xFu << sh) & ~PPC_FPSCR_UNWRITABLE;
    p->fpscr = ppc_fpscr_derive(((((iw >> 12) & 0xFu) << sh) & m) | (p->fpscr & ~m));
    if (PPC_RC(iw))
        ppc_set_cr_field(p, 1, p->fpscr >> 28);
    ppc_fp_trap_check(p);
}

// mtfsb1 of an exception condition bit also sets FX: Table 2-1 bit 0 says
// "every floating-point instruction implicitly sets FPSCR[FX] if that
// instruction causes any of the floating-point exception bits to transition
// from 0 to 1", and mtfsb1 is not carved out of it.  (Folio 10-132's "other
// registers altered" line names only FPSCR[crbD], but that list also omits
// the derived VX, so it reads as a summary rather than an exhaustive action
// list — unlike §5.4.7.4.1, which IS one and does override the same table
// for FR/FI on disabled overflow.  powerpc-test's model agrees.)
void ppc_do_mtfsb(ppc_t *p, uint32_t iw, bool set) {
    uint32_t bit = 0x80000000u >> PPC_RT(iw);
    if (!(bit & PPC_FPSCR_UNWRITABLE)) {
        if (!set)
            p->fpscr &= ~bit;
        else if (bit & PPC_FPSCR_EXCEPTIONS)
            p->fpscr = ppc_fpscr_raise(p->fpscr, bit);
        else
            p->fpscr |= bit;
    }
    p->fpscr = ppc_fpscr_derive(p->fpscr);
    if (PPC_RC(iw))
        ppc_set_cr_field(p, 1, p->fpscr >> 28);
    ppc_fp_trap_check(p);
}
