// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_fpu.c
// PPC (MPC601) floating-point bodies, Phase-B scope: the FPR file's
// load/store format conversions, compares, and the mcrfs field move.  The
// register-move and FPSCR-access instructions are one-liners in the OP_
// table (ppc_ops.h); the arithmetic datapath (fadd/fmul/fmadd/frsp/
// fctiw[z] with full FPSCR status modeling) lands in Phase E per the
// proposal §3.6 — until then ppc_fpu_unimpl() makes reaching it loud,
// never silently wrong.
//
// Determinism rule (§3.6): NaN bit patterns never pass through host
// floating-point arithmetic — WASM does not guarantee NaN payload/sign
// propagation, and checkpoints must be byte-identical across hosts.  The
// single<->double conversions below therefore handle NaN in integer code
// and use host conversion (exact or correctly-rounded IEEE, deterministic
// on both hosts) only for numeric values.

#include "ppc_ops.h"

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

// FPSCR bits used by the Phase-B surface
#define FPSCR_FX     0x80000000u
#define FPSCR_VXSNAN 0x01000000u
#define FPSCR_VXVC   0x00080000u
#define FPSCR_FPCC   0x0000F000u // FL/FG/FE/FU, bits 16-19

// fcmpu/fcmpo shared body (601UM fcmpu/fcmpo pages)
void ppc_fcmp(ppc_t *p, uint32_t iw, bool ordered) {
    uint64_t a = p->fpr[PPC_RA(iw)], b = p->fpr[PPC_RB(iw)];
    uint32_t c;
    if (f64_is_nan(a) || f64_is_nan(b)) {
        c = 1u; // unordered (FU)
        if (f64_is_snan(a) || f64_is_snan(b)) {
            p->fpscr |= FPSCR_FX | FPSCR_VXSNAN;
            if (ordered)
                p->fpscr |= FPSCR_VXVC;
        } else if (ordered) {
            p->fpscr |= FPSCR_FX | FPSCR_VXVC;
        }
    } else {
        int cmp = f64_compare(a, b);
        c = (cmp < 0) ? 8u : (cmp > 0) ? 4u : 2u;
    }
    p->fpscr = (p->fpscr & ~FPSCR_FPCC) | (c << 12); // FPCC mirrors the field
    ppc_set_cr_field(p, PPC_CRFD(iw), c);
}

// mcrfs: FPSCR field to CR field; the copied exception bits clear on read
// (FEX/VX are derived, FPCC is not).
void ppc_do_mcrfs(ppc_t *p, uint32_t iw) {
    uint32_t crfs = PPC_CRFS(iw);
    uint32_t field = (p->fpscr >> (28 - 4 * crfs)) & 0xFu;
    ppc_set_cr_field(p, PPC_CRFD(iw), field);
    if (crfs == 0)
        p->fpscr &= ~0x90000000u; // FX, OX
    else if (crfs < 4)
        p->fpscr &= ~(0xFu << (28 - 4 * crfs));
}

// Phase-E backstop: arithmetic reaching here is a loud illegal, not a
// silent wrong answer (the detectors-fail-loudly rule).
void ppc_fpu_unimpl(ppc_t *p, uint32_t iw) {
    (void)iw; // referenced only when the log category is compiled in
    LOG(0, "unimplemented FP arithmetic $%08X at $%08X — lands in Phase E", iw, p->instruction_pc);
    ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_ILLEGAL, p->instruction_pc);
}
