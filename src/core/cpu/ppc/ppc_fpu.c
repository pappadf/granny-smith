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

// stfs/stfsx: double to single.  Values representable convert per IEEE
// round-to-nearest (host conversion, deterministic); NaN keeps the top 23
// payload bits, with the quiet bit forced if truncation would produce an
// infinity pattern.
uint32_t ppc_f64_to_f32(uint64_t d) {
    uint32_t sign = (uint32_t)(d >> 63) << 31;
    uint32_t exp = (uint32_t)(d >> 52) & 0x7FFu;
    if (exp == 0x7FFu) {
        uint64_t frac = d & 0xFFFFFFFFFFFFFull;
        if (frac == 0)
            return sign | 0x7F800000u; // infinity
        uint32_t frac23 = (uint32_t)(frac >> 29) & 0x7FFFFFu;
        if (frac23 == 0)
            frac23 = 0x400000u; // payload lived in the low bits: keep it NaN
        return sign | 0x7F800000u | frac23;
    }
    double v;
    float f;
    memcpy(&v, &d, 8);
    f = (float)v; // IEEE round-to-nearest-even, deterministic
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return bits;
}

// === FP compares ============================================================

// Classify a double bit pattern for compare purposes without host FP.
static inline bool f64_is_nan(uint64_t d) {
    return ((d >> 52) & 0x7FFu) == 0x7FFu && (d & 0xFFFFFFFFFFFFFull) != 0;
}

static inline bool f64_is_snan(uint64_t d) {
    return f64_is_nan(d) && !(d & 0x0008000000000000ull); // quiet bit clear
}

// Ordered compare of two non-NaN doubles by bit pattern: flip the ordering
// of negative values (sign-magnitude → two's-complement trick), treating
// -0 == +0.
static int f64_compare(uint64_t a, uint64_t b) {
    // Normalize both zeroes to +0 so -0 == +0.
    if ((a & 0x7FFFFFFFFFFFFFFFull) == 0)
        a = 0;
    if ((b & 0x7FFFFFFFFFFFFFFFull) == 0)
        b = 0;
    int64_t ka = (int64_t)((a >> 63) ? (0x8000000000000000ull - a) : (a | 0x8000000000000000ull));
    int64_t kb = (int64_t)((b >> 63) ? (0x8000000000000000ull - b) : (b | 0x8000000000000000ull));
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

void ppc_do_mtfsb(ppc_t *p, uint32_t iw, bool set) {
    uint32_t bit = 0x80000000u >> PPC_RT(iw);
    if (!(bit & PPC_FPSCR_UNWRITABLE))
        p->fpscr = set ? (p->fpscr | bit) : (p->fpscr & ~bit);
    p->fpscr = ppc_fpscr_derive(p->fpscr);
    if (PPC_RC(iw))
        ppc_set_cr_field(p, 1, p->fpscr >> 28);
    ppc_fp_trap_check(p);
}
