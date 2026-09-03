// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_softfp.h
// The 601 FPU arithmetic kernel: pure functions of (operands, FPSCR) with
// no dependency beyond <stdint.h> — the ppc_disasm precedent.  Deliberately
// integer-only ("software floating point"): every significand operation,
// rounding decision, and status bit is computed in integer code, so results
// and FPSCR images are byte-identical on every host (native and WASM) by
// construction — the proposal §3.6 determinism requirement, delivered by
// construction instead of by auditing host-FP corner cases.  See §0.1
// "Phase-E findings" for the deviation note (host doubles remain the test
// oracle, not the implementation).
//
// All semantics cite Motorola/IBM, "PowerPC 601 RISC Microprocessor User's
// Manual", 1995 (601UM): FPSCR bits Table 2-1 (folio 2-9/2-10), exception
// actions §5.4.7 (folios 5-32..5-44), NaN rules §2.5.2.7 (folio 2-67),
// execution models §2.5 (folios 2-57..2-72), instruction pages Ch. 10.
// Appendix F (the full frsp/fctiw models) is absent from the scanned
// manual; the affected corner-case choices are marked AUTHORITY-PENDING
// here and in tests/unit/suites/ppc_fpu (proposal §11 item 1).

#ifndef GS_CPU_PPC_SOFTFP_H
#define GS_CPU_PPC_SOFTFP_H

#include <stdint.h>

// === FPSCR bits (601UM Table 2-1; BE bit n = mask 1<<(31-n)) ===============
#define PPC_FPSCR_FX     0x80000000u // bit 0: exception summary (sticky)
#define PPC_FPSCR_FEX    0x40000000u // bit 1: enabled-exception summary (derived)
#define PPC_FPSCR_VX     0x20000000u // bit 2: invalid-op summary (derived)
#define PPC_FPSCR_OX     0x10000000u // bit 3: overflow (sticky)
#define PPC_FPSCR_UX     0x08000000u // bit 4: underflow (sticky)
#define PPC_FPSCR_ZX     0x04000000u // bit 5: zero divide (sticky)
#define PPC_FPSCR_XX     0x02000000u // bit 6: inexact (sticky)
#define PPC_FPSCR_VXSNAN 0x01000000u // bit 7: invalid: SNaN
#define PPC_FPSCR_VXISI  0x00800000u // bit 8: invalid: inf - inf
#define PPC_FPSCR_VXIDI  0x00400000u // bit 9: invalid: inf / inf
#define PPC_FPSCR_VXZDZ  0x00200000u // bit 10: invalid: 0 / 0
#define PPC_FPSCR_VXIMZ  0x00100000u // bit 11: invalid: inf * 0
#define PPC_FPSCR_VXVC   0x00080000u // bit 12: invalid: invalid compare
#define PPC_FPSCR_FR     0x00040000u // bit 13: fraction rounded (not sticky)
#define PPC_FPSCR_FI     0x00020000u // bit 14: fraction inexact (not sticky)
#define PPC_FPSCR_C      0x00010000u // bit 15: result class descriptor
#define PPC_FPSCR_FPCC   0x0000F000u // bits 16-19: FL/FG/FE/FU
#define PPC_FPSCR_FPRF   0x0001F000u // bits 15-19: C + FPCC
#define PPC_FPSCR_VXSOFT 0x00000400u // bit 21: software-request invalid (601: storage only)
#define PPC_FPSCR_VXSQRT 0x00000200u // bit 22: invalid sqrt (601: storage only)
#define PPC_FPSCR_VXCVI  0x00000100u // bit 23: invalid integer convert
#define PPC_FPSCR_VE     0x00000080u // bit 24: invalid-op exception enable
#define PPC_FPSCR_OE     0x00000040u // bit 25: overflow exception enable
#define PPC_FPSCR_UE     0x00000020u // bit 26: underflow exception enable
#define PPC_FPSCR_ZE     0x00000010u // bit 27: zero-divide exception enable
#define PPC_FPSCR_XE     0x00000008u // bit 28: inexact exception enable
// bit 29 is reserved on the 601 (NO NI/non-IEEE mode — Table 2-1 note)
#define PPC_FPSCR_RN 0x00000003u // bits 30-31: rounding mode

// Rounding-mode encodings (FPSCR[RN])
#define PPC_RN_NEAREST 0u
#define PPC_RN_ZERO    1u
#define PPC_RN_PLUS    2u
#define PPC_RN_MINUS   3u

// All the individual invalid-operation condition bits (VX is their OR)
#define PPC_FPSCR_VX_ANY                                                                                               \
    (PPC_FPSCR_VXSNAN | PPC_FPSCR_VXISI | PPC_FPSCR_VXIDI | PPC_FPSCR_VXZDZ | PPC_FPSCR_VXIMZ | PPC_FPSCR_VXVC |       \
     PPC_FPSCR_VXSOFT | PPC_FPSCR_VXSQRT | PPC_FPSCR_VXCVI)

// The invalid-operation bits the active model implements.  The 601 has
// VXSOFT and VXSQRT as storage only ("not implemented in the 601", 601UM
// Table 2-1): writable and sticky, but neither summarized into VX nor an
// exception condition for FX.  Set by ppc_init per model.
extern uint32_t g_ppc_fpscr_vx_any;

// The exception condition bits: bits 3-12 and 21-23 (601UM §2.2.3 -- FEX
// and VX are summaries, not conditions).  A 0 -> 1 transition of any of
// these implicitly sets FX.
#define PPC_FPSCR_EXCEPTIONS (PPC_FPSCR_OX | PPC_FPSCR_UX | PPC_FPSCR_ZX | PPC_FPSCR_XX | g_ppc_fpscr_vx_any)

// FPSCR bits mtfsf/mtfsfi/mtfsb0/mtfsb1 can NOT write: FEX and VX are
// derived summaries ("cannot be explicitly set or reset", Table 2-1).
#define PPC_FPSCR_UNWRITABLE (PPC_FPSCR_FEX | PPC_FPSCR_VX)

// Recompute the derived FEX/VX summary bits (601UM §2.2.3):
//   VX  = OR of all invalid-operation condition bits
//   FEX = (VX & VE) | (OX & OE) | (UX & UE) | (ZX & ZE) | (XX & XE)
static inline uint32_t ppc_fpscr_derive(uint32_t f) {
    f &= ~(PPC_FPSCR_FEX | PPC_FPSCR_VX);
    if (f & g_ppc_fpscr_vx_any)
        f |= PPC_FPSCR_VX;
    if (((f & PPC_FPSCR_VX) && (f & PPC_FPSCR_VE)) || ((f & PPC_FPSCR_OX) && (f & PPC_FPSCR_OE)) ||
        ((f & PPC_FPSCR_UX) && (f & PPC_FPSCR_UE)) || ((f & PPC_FPSCR_ZX) && (f & PPC_FPSCR_ZE)) ||
        ((f & PPC_FPSCR_XX) && (f & PPC_FPSCR_XE)))
        f |= PPC_FPSCR_FEX;
    return f;
}

// OR exception-condition bits into the FPSCR with the FX transition rule:
// FX is set when any exception bit goes 0 -> 1 by instruction action
// (601UM Table 2-1 bit 0).  Does not derive FEX/VX — do that once at the
// end of the operation.
static inline uint32_t ppc_fpscr_raise(uint32_t f, uint32_t bits) {
    if (bits & ~f)
        f |= PPC_FPSCR_FX;
    return f | bits;
}

// === The arithmetic kernel ==================================================

typedef enum {
    PPC_SF_ADD, // frA + frB
    PPC_SF_SUB, // frA - frB
    PPC_SF_MUL, // frA * frC
    PPC_SF_DIV, // frA / frB
    PPC_SF_MADD, // (frA * frC) + frB
    PPC_SF_MSUB, // (frA * frC) - frB
    PPC_SF_NMADD, // -((frA * frC) + frB)
    PPC_SF_NMSUB, // -((frA * frC) - frB)
} ppc_sf_op_t;

// Perform one arithmetic operation on raw IEEE-double bit patterns.
// `single` selects the single-precision variants (round once to 24-bit
// precision and single range; result still stored in double format).
// Updates *fpscr (status + FX + derived FEX/VX) and stores the result via
// *frt.  Returns 0 when the target FPR must be left unchanged (the
// enabled-invalid / enabled-zero-divide suppression cases, 601UM folio
// 5-37), 1 otherwise.
int ppc_sf_arith(ppc_sf_op_t op, uint64_t a, uint64_t b, uint64_t c, int single, uint32_t *fpscr, uint64_t *frt);

// frsp: round a double to single precision (601UM folio 10-80).
int ppc_sf_frsp(uint64_t b, uint32_t *fpscr, uint64_t *frt);

// fctiw / fctiwz: convert to 32-bit signed integer (601UM folios 10-63/64).
// round_to_zero selects fctiwz.  The 601 stores $FFF80000 in the high word.
int ppc_sf_fctiw(uint64_t b, int round_to_zero, uint32_t *fpscr, uint64_t *frt);

// The 604's optional estimate instructions (PEM fresx/frsqrtex pages).
// Both deliver a value far inside the architected error envelope (2^-8 /
// 2^-5 relative) — see the implementation notes for the exact constants —
// and report only their architected FPSCR effects (no XX; FR/FI read
// cleared for "undefined").  Same return convention as ppc_sf_arith.
int ppc_sf_fres(uint64_t b, uint32_t *fpscr, uint64_t *frt);
int ppc_sf_frsqrte(uint64_t b, uint32_t *fpscr, uint64_t *frt);

#endif // GS_CPU_PPC_SOFTFP_H
