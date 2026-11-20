// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_jit_classify.h
// ppc_decode.h in its fourth role: the translation tier's CLASSIFIER.
// The translator (ppc_jit.c) treats as translatable the shapes the
// predecoded loop specializes (T0, ppc_pd_classify.h) plus shapes of its
// own, which the predecoded loop leaves generic (T1).  Those are the
// leaves overridden below: each returns a translator id (JX_*) with the
// fields the emitter reads, or the leaf's T1 id when the translator
// leaves that form to the predecoded loop.  Validity (reserved fields,
// invalid BO encodings) is the tree's, so an encoding the interpreter
// rejects is never translated; the translator's model rule (the 601's
// floating point and dcbz stay in C) is the `is601` argument.
//
// Template contract: included once per translation unit, after
// ppc_internal.h (ppc_t, for the SPR field offsets); defines
//     static uint16_t ppc_jit_classify(uint32_t iw, uint32_t ipc, uint32_t page_lo,
//                                      bool is601, pd_entry_t *e);
// over the predecoded classifier `ppc_pd_classify_word` (ppc_run.c).

#ifndef PPC_JIT_CLASSIFY_H
#define PPC_JIT_CLASSIFY_H

#include "ppc_pd_ids.h"
#include "ppc_softfp.h" // the PPC_SF_ op enum the JX_FP kinds extend
#include "ppc_spr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The predecoded tier's classifier (ppc_run.c).
uint16_t ppc_pd_classify_word(uint32_t iw, uint32_t ipc, uint32_t page_lo, pd_entry_t *e);

// Shapes the predecoded loop leaves generic (T1) but the translator handles
// from the raw word, with ids of its own above the T0 range:
//   JX_BC               every bc form the T0 ids are not (linking,
//                       decrementing with a condition): a = BO, b = BI,
//                       c = the raw word
//   JX_BCLR, JX_BCCTR   every bclr/bcctr form the T0 BLR/BCTR are not
//                       (conditional, decrementing, linking): a = BO, b = BI,
//                       c = LK
//   JX_MFMSR .. JX_ARITH  the MSR and time base moves, lwarx/stwcx., and
//                       the divides, high multiplies, eqv and sraw (the
//                       Mac OS X kernel's untranslated core; fields at
//                       their enum entries)
//   JX_LMW, JX_STMW     load/store multiple word: a = rT/rS (the number),
//                       b = rA offset (JX_RA0: rA = 0), c = the displacement
//   JX_LWZX ..          the X-form loads and stores: a = rT/rS offset,
//                       b = rA offset (JX_RA0: rA = 0, the non-update forms
//                       only), c = rB offset
//   JX_ADDX             the XO-form add/subtract family the T0 ids leave
//                       out (carrying, extended, OE): a = rT offset, b = rA
//                       offset, c = rB offset | kind << 8 | OE << 16 | Rc << 17
// and the D-form loads and stores with rA = 0 keep their T0 ids with
// b = JX_RA0.
#define JX_RA0 0xFFu
enum { // JX_ADDX kinds: rT = (subf ? ~rA : rA) + operand + carry-in
    ADDK_ADD, // rB, 0
    ADDK_ADDC, // rB, 0, sets CA
    ADDK_ADDE, // rB, CA, sets CA
    ADDK_ADDME, // -1, CA, sets CA
    ADDK_ADDZE, // 0, CA, sets CA
    ADDK_SUBF, // rB, 1
    ADDK_SUBFC, // rB, 1, sets CA
    ADDK_SUBFE, // rB, CA, sets CA
    ADDK_SUBFME, // -1, CA, sets CA
    ADDK_SUBFZE, // 0, CA, sets CA
    ADDK_NEG, // 0 - rA
};
enum { // JX_ARITH kinds (OE clear)
    XK_DIVW, // rT = rA / rB (signed; 0 on division by zero or overflow, ppc_do_divw)
    XK_DIVWU, // rT = rA / rB (unsigned; 0 on division by zero)
    XK_MULHW, // rT = high word of rA * rB (signed)
    XK_MULHWU, // the same, unsigned
    XK_EQV, // rA = ~(rS ^ rB)
    XK_SRAW, // rA = rS >> rB (algebraic), sets CA (ppc_do_sraw)
};
// JX_FP kinds (a): the arithmetic forms are their softfp op (ppc_sf_op_t,
// single and double alike: the primary opcode tells them apart), the
// others follow it
enum {
    JF_FMR = PPC_SF_NMSUB + 1,
    JF_FNEG,
    JF_FABS,
    JF_FNABS,
    JF_FSEL, // (604)
    JF_FRSP,
    JF_FCTIW,
    JF_FCTIWZ,
    JF_FRES, // (604)
    JF_FRSQRTE, // (604)
};
enum {
    JX_BCLR = PPD_END,
    JX_BCCTR,
    JX_BC,
    JX_MCRXR, // a = crfD
    JX_MFSPR, // a = rT offset, c = the ppc_t field (a plain supervisor SPR, ppc_spr.h)
    JX_MTSPR, // a = rS offset, c = the ppc_t field
    JX_MFMSR, // a = rT offset
    JX_MTMSR, // a = rS offset
    JX_MFTB, // a = rT offset, c = 1 for TBU
    JX_LWARX, // a = rT offset, b = rA offset (JX_RA0: rA = 0), c = rB offset
    JX_STWCX, // a = rS offset, b and c as lwarx
    JX_ARITH, // a = dest offset, b and c & 0xFF the sources' offsets, c >> 8 the kind, c >> 16 Rc
    JX_LMW,
    JX_STMW,
    JX_LWZX,
    JX_LWZUX,
    JX_LHZX,
    JX_LHZUX,
    JX_LHAX,
    JX_LHAUX,
    JX_LBZX,
    JX_LBZUX,
    JX_STWX,
    JX_STWUX,
    JX_STHX,
    JX_STHUX,
    JX_STBX,
    JX_STBUX,
    JX_ADDX,
    // Floating point (not the 601's: T=1 segments and its alignment rules
    // stay with the predecoded loop).  The loads and stores: a = frT/frS
    // (the number), b = rA offset (JX_RA0: rA = 0), c = the displacement
    // (D-form) or rB offset (X-form).
    JX_LFS,
    JX_LFSU,
    JX_LFD,
    JX_LFDU,
    JX_STFS,
    JX_STFSU,
    JX_STFD,
    JX_LFSX,
    JX_LFSUX,
    JX_LFDX,
    JX_LFDUX,
    JX_STFSX,
    JX_STFSUX,
    JX_STFDX,
    JX_LWBRX, // a = rT offset, b = rA offset (JX_RA0), c = rB offset
    JX_STWBRX, // a = rS offset, b and c as lwbrx
    JX_FP, // FP arithmetic and moves, Rc clear, through ppc_jit_fp: a = the JF_ / PPC_SF_ kind, c = the word
    JX_FCMP, // fcmpu / fcmpo through ppc_jit_fp: a = crfD, c = the word
    JX_DCBZ, // dcbz through ppc_jit_dcbz: b = rA offset (JX_RA0: rA = 0), c = rB offset
    JX_END
};
_Static_assert(JX_END < 65536, "translator id space overflow");

// Classification context for the tree's OP_ overrides
typedef struct jit_cls {
    bool is601; // the unit runs on a 601
    pd_entry_t *e; // out: fields
} jit_cls_t;

static inline uint16_t jx_emit(const jit_cls_t *c, uint16_t id, uint8_t a, uint8_t b, uint32_t cval) {
    *c->e = (pd_entry_t){.a = a, .b = b, .c = cval};
    return id;
}

// rA as a base: its offset, or JX_RA0 for rA = 0
static inline uint8_t jx_base(uint32_t iw) {
    return PPC_RA(iw) ? PPD_GOFF(PPC_RA(iw)) : (uint8_t)JX_RA0;
}

// mfspr / mtspr of a plain supervisor SPR the translator moves: the
// PPC_SPR_JIT rows of the SPR table (ppc_spr.h), fields of ppc_t with no
// side effect (in a user-mode region they raise the privilege exception,
// so translatable() keeps them in C there)
static inline uint16_t jx_cls_spr(const jit_cls_t *c, uint32_t iw, uint16_t id, uint16_t t1) {
    uint32_t off;
    switch (PPC_SPR_NUMBER(iw)) {
#define JX_SPR_PLAIN(n_, field_, flags_)                                                                               \
    case n_:                                                                                                           \
        if (!((flags_) & PPC_SPR_JIT))                                                                                 \
            return t1;                                                                                                 \
        off = offsetof(ppc_t, field_);                                                                                 \
        break;
#define JX_SPR_OWN(n_, field_, flags_)
#define JX_SPR_NO(n_, field_, flags_)
// (a row the translator moves is PLAIN both ways: its mtspr kind must be too)
#define X(n_, name_, mf_, mt_, field_, flags_, models_) JX_SPR_##mf_(n_, field_, flags_)
        PPC_SPRS(X)
#undef X
#undef JX_SPR_PLAIN
#undef JX_SPR_OWN
#undef JX_SPR_NO
    default:
        return t1;
    }
    return jx_emit(c, id, PPD_GOFF(PPC_RT(iw)), 0, off);
}

// The table's translator rows: supervisor-only, every model, a plain field
// both ways (mfspr and mtspr then raise nothing but the privilege
// exception, which a supervisor region cannot take)
#define JX_SPR_KIND_PLAIN 1
#define JX_SPR_KIND_OWN   0
#define JX_SPR_KIND_NO    0
#define X(n_, name_, mf_, mt_, field_, flags_, models_)                                                                \
    _Static_assert(!((flags_) & PPC_SPR_JIT) || (((flags_) & PPC_SPR_PRIV) && (models_) == PPC_SPR_MALL &&             \
                                                 JX_SPR_KIND_##mf_ && JX_SPR_KIND_##mt_),                              \
                   "SPR " #n_ ": the translator moves only plain supervisor SPRs every model has");
PPC_SPRS(X)
#undef X
#undef JX_SPR_KIND_PLAIN
#undef JX_SPR_KIND_OWN
#undef JX_SPR_KIND_NO

// D-form integer access with rA = 0 (the T0 ids take rA != 0)
#define JX_D_RA0(T0, T1)                                                                                               \
    return PPC_RA(iw) ? T1 : jx_emit(ctx, T0, PPD_GOFF(PPC_RT(iw)), (uint8_t)JX_RA0, (uint32_t)PPC_SIMM(iw))
// XO-form add/subtract family
#define JX_ADD(K)                                                                                                      \
    return jx_emit(ctx, JX_ADDX, PPD_GOFF(PPC_RT(iw)), PPD_GOFF(PPC_RA(iw)),                                           \
                   PPD_GOFF(PPC_RB(iw)) | (uint32_t)(K) << 8 | PPC_OE(iw) << 16 | PPC_RC(iw) << 17)
// divides and high multiplies, OE clear: rT, rA, rB
#define JX_ARITH_T(K, T1)                                                                                              \
    return PPC_OE(iw) ? T1                                                                                             \
                      : jx_emit(ctx, JX_ARITH, PPD_GOFF(PPC_RT(iw)), PPD_GOFF(PPC_RA(iw)),                             \
                                PPD_GOFF(PPC_RB(iw)) | (uint32_t)(K) << 8 | PPC_RC(iw) << 16)
// eqv, sraw: rA, rS, rB
#define JX_ARITH_A(K)                                                                                                  \
    return jx_emit(ctx, JX_ARITH, PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RT(iw)),                                          \
                   PPD_GOFF(PPC_RB(iw)) | (uint32_t)(K) << 8 | PPC_RC(iw) << 16)
// X-form integer access (an update form with rA = 0 -- the 601's
// r0-inhibited update -- stays with the predecoded loop)
#define JX_X(ID, UPD, T1)                                                                                              \
    return ((UPD) && !PPC_RA(iw)) ? T1 : jx_emit(ctx, ID, PPD_GOFF(PPC_RT(iw)), jx_base(iw), PPD_GOFF(PPC_RB(iw)))
// FP access, not on the 601 (an update form with rA = 0 is invalid: left to
// the predecoded loop): a = the FPR number, c = the displacement (D) or rB
// offset (X)
#define JX_FD(ID, UPD, T1)                                                                                             \
    return (ctx->is601 || ((UPD) && !PPC_RA(iw)))                                                                      \
               ? T1                                                                                                    \
               : jx_emit(ctx, ID, (uint8_t)PPC_RT(iw), jx_base(iw), (uint32_t)PPC_SIMM(iw))
#define JX_FX(ID, UPD, T1)                                                                                             \
    return (ctx->is601 || ((UPD) && !PPC_RA(iw)))                                                                      \
               ? T1                                                                                                    \
               : jx_emit(ctx, ID, (uint8_t)PPC_RT(iw), jx_base(iw), PPD_GOFF(PPC_RB(iw)))
// FP arithmetic and moves, Rc clear, not on the 601 (record forms write
// CR1: the predecoded loop's)
#define JX_F(K, T1) return (ctx->is601 || PPC_RC(iw)) ? T1 : jx_emit(ctx, JX_FP, (uint8_t)(K), 0, iw)

#include "ppc_pd_t1_classify.h" // generated: every OP_ → return T1_<leaf>

// clang-format off
// branches
#undef OP_BC
#define OP_BC     return jx_emit(ctx, JX_BC, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), iw)
#undef OP_BCLR
#define OP_BCLR   return jx_emit(ctx, JX_BCLR, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), PPC_RC(iw))
#undef OP_BCCTR
#define OP_BCCTR  return jx_emit(ctx, JX_BCCTR, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), PPC_RC(iw))
// D-form loads and stores with rA = 0
#undef OP_LWZ
#define OP_LWZ    JX_D_RA0(PPD_LWZ, T1_LWZ)
#undef OP_LBZ
#define OP_LBZ    JX_D_RA0(PPD_LBZ, T1_LBZ)
#undef OP_LHZ
#define OP_LHZ    JX_D_RA0(PPD_LHZ, T1_LHZ)
#undef OP_LHA
#define OP_LHA    JX_D_RA0(PPD_LHA, T1_LHA)
#undef OP_STW
#define OP_STW    JX_D_RA0(PPD_STW, T1_STW)
#undef OP_STB
#define OP_STB    JX_D_RA0(PPD_STB, T1_STB)
#undef OP_STH
#define OP_STH    JX_D_RA0(PPD_STH, T1_STH)
// the XO-form add/subtract family (the T0 ids keep add, subf and neg with OE clear)
#undef OP_ADD
#define OP_ADD    JX_ADD(ADDK_ADD)
#undef OP_ADDC
#define OP_ADDC   JX_ADD(ADDK_ADDC)
#undef OP_ADDE
#define OP_ADDE   JX_ADD(ADDK_ADDE)
#undef OP_ADDME
#define OP_ADDME  JX_ADD(ADDK_ADDME)
#undef OP_ADDZE
#define OP_ADDZE  JX_ADD(ADDK_ADDZE)
#undef OP_SUBF
#define OP_SUBF   JX_ADD(ADDK_SUBF)
#undef OP_SUBFC
#define OP_SUBFC  JX_ADD(ADDK_SUBFC)
#undef OP_SUBFE
#define OP_SUBFE  JX_ADD(ADDK_SUBFE)
#undef OP_SUBFME
#define OP_SUBFME JX_ADD(ADDK_SUBFME)
#undef OP_SUBFZE
#define OP_SUBFZE JX_ADD(ADDK_SUBFZE)
#undef OP_NEG
#define OP_NEG    JX_ADD(ADDK_NEG)
// divides, high multiplies, eqv, sraw
#undef OP_DIVW
#define OP_DIVW   JX_ARITH_T(XK_DIVW, T1_DIVW)
#undef OP_DIVWU
#define OP_DIVWU  JX_ARITH_T(XK_DIVWU, T1_DIVWU)
#undef OP_MULHW
#define OP_MULHW  JX_ARITH_T(XK_MULHW, T1_MULHW)
#undef OP_MULHWU
#define OP_MULHWU JX_ARITH_T(XK_MULHWU, T1_MULHWU)
#undef OP_EQV
#define OP_EQV    JX_ARITH_A(XK_EQV)
#undef OP_SRAW
#define OP_SRAW   JX_ARITH_A(XK_SRAW)
// mcrxr with crfD's low bits, rA and rB clear
#undef OP_MCRXR
#define OP_MCRXR  return (iw & 0x007FF800u) ? T1_MCRXR : jx_emit(ctx, JX_MCRXR, (uint8_t)PPC_CRFD(iw), 0, 0)
// SPR, MSR and time base moves
#undef OP_MFSPR
#define OP_MFSPR  return jx_cls_spr(ctx, iw, JX_MFSPR, T1_MFSPR)
#undef OP_MTSPR
#define OP_MTSPR  return jx_cls_spr(ctx, iw, JX_MTSPR, T1_MTSPR)
#undef OP_MFMSR
#define OP_MFMSR  return jx_emit(ctx, JX_MFMSR, PPD_GOFF(PPC_RT(iw)), 0, 0)
#undef OP_MTMSR
#define OP_MTMSR  return jx_emit(ctx, JX_MTMSR, PPD_GOFF(PPC_RT(iw)), 0, 0)
// mftb: TBL or TBU (any other number is illegal: left to C)
#undef OP_MFTB
#define OP_MFTB   return (PPC_SPR_NUMBER(iw) == 268 || PPC_SPR_NUMBER(iw) == 269) ? jx_emit(ctx, JX_MFTB, PPD_GOFF(PPC_RT(iw)), 0, PPC_SPR_NUMBER(iw) - 268) : T1_MFTB
// the reservation pair
#undef OP_LWARX
#define OP_LWARX  return jx_emit(ctx, JX_LWARX, PPD_GOFF(PPC_RT(iw)), jx_base(iw), PPD_GOFF(PPC_RB(iw)))
#undef OP_STWCX_DOT
#define OP_STWCX_DOT return jx_emit(ctx, JX_STWCX, PPD_GOFF(PPC_RT(iw)), jx_base(iw), PPD_GOFF(PPC_RB(iw)))
// lmw / stmw: a = the first register's number
#undef OP_LMW
#define OP_LMW    return jx_emit(ctx, JX_LMW, (uint8_t)PPC_RT(iw), jx_base(iw), (uint32_t)PPC_SIMM(iw))
#undef OP_STMW
#define OP_STMW   return jx_emit(ctx, JX_STMW, (uint8_t)PPC_RT(iw), jx_base(iw), (uint32_t)PPC_SIMM(iw))
// X-form integer loads and stores
#undef OP_LWZX
#define OP_LWZX   JX_X(JX_LWZX, false, T1_LWZX)
#undef OP_LWZUX
#define OP_LWZUX  JX_X(JX_LWZUX, true, T1_LWZUX)
#undef OP_LHZX
#define OP_LHZX   JX_X(JX_LHZX, false, T1_LHZX)
#undef OP_LHZUX
#define OP_LHZUX  JX_X(JX_LHZUX, true, T1_LHZUX)
#undef OP_LHAX
#define OP_LHAX   JX_X(JX_LHAX, false, T1_LHAX)
#undef OP_LHAUX
#define OP_LHAUX  JX_X(JX_LHAUX, true, T1_LHAUX)
#undef OP_LBZX
#define OP_LBZX   JX_X(JX_LBZX, false, T1_LBZX)
#undef OP_LBZUX
#define OP_LBZUX  JX_X(JX_LBZUX, true, T1_LBZUX)
#undef OP_STWX
#define OP_STWX   JX_X(JX_STWX, false, T1_STWX)
#undef OP_STWUX
#define OP_STWUX  JX_X(JX_STWUX, true, T1_STWUX)
#undef OP_STHX
#define OP_STHX   JX_X(JX_STHX, false, T1_STHX)
#undef OP_STHUX
#define OP_STHUX  JX_X(JX_STHUX, true, T1_STHUX)
#undef OP_STBX
#define OP_STBX   JX_X(JX_STBX, false, T1_STBX)
#undef OP_STBUX
#define OP_STBUX  JX_X(JX_STBUX, true, T1_STBUX)
#undef OP_LWBRX
#define OP_LWBRX  JX_X(JX_LWBRX, false, T1_LWBRX)
#undef OP_STWBRX
#define OP_STWBRX JX_X(JX_STWBRX, false, T1_STWBRX)
// dcbz, not on the 601
#undef OP_DCBZ
#define OP_DCBZ   return ctx->is601 ? T1_DCBZ : jx_emit(ctx, JX_DCBZ, 0, jx_base(iw), PPD_GOFF(PPC_RB(iw)))
// FP loads and stores (stfdu and its X form stay in C)
#undef OP_LFS
#define OP_LFS    JX_FD(JX_LFS, false, T1_LFS)
#undef OP_LFSU
#define OP_LFSU   JX_FD(JX_LFSU, true, T1_LFSU)
#undef OP_LFD
#define OP_LFD    JX_FD(JX_LFD, false, T1_LFD)
#undef OP_LFDU
#define OP_LFDU   JX_FD(JX_LFDU, true, T1_LFDU)
#undef OP_STFS
#define OP_STFS   JX_FD(JX_STFS, false, T1_STFS)
#undef OP_STFSU
#define OP_STFSU  JX_FD(JX_STFSU, true, T1_STFSU)
#undef OP_STFD
#define OP_STFD   JX_FD(JX_STFD, false, T1_STFD)
#undef OP_LFSX
#define OP_LFSX   JX_FX(JX_LFSX, false, T1_LFSX)
#undef OP_LFSUX
#define OP_LFSUX  JX_FX(JX_LFSUX, true, T1_LFSUX)
#undef OP_LFDX
#define OP_LFDX   JX_FX(JX_LFDX, false, T1_LFDX)
#undef OP_LFDUX
#define OP_LFDUX  JX_FX(JX_LFDUX, true, T1_LFDUX)
#undef OP_STFSX
#define OP_STFSX  JX_FX(JX_STFSX, false, T1_STFSX)
#undef OP_STFSUX
#define OP_STFSUX JX_FX(JX_STFSUX, true, T1_STFSUX)
#undef OP_STFDX
#define OP_STFDX  JX_FX(JX_STFDX, false, T1_STFDX)
// FP compares (they write a CR field, which the helper returns), not on the 601
#undef OP_FCMPU
#define OP_FCMPU  return ctx->is601 ? T1_FCMPU : jx_emit(ctx, JX_FCMP, (uint8_t)PPC_CRFD(iw), 0, iw)
#undef OP_FCMPO
#define OP_FCMPO  return ctx->is601 ? T1_FCMPO : jx_emit(ctx, JX_FCMP, (uint8_t)PPC_CRFD(iw), 0, iw)
// FP arithmetic and moves; the FPSCR moves stay in C
#undef OP_FMR
#define OP_FMR    JX_F(JF_FMR, T1_FMR)
#undef OP_FNEG
#define OP_FNEG   JX_F(JF_FNEG, T1_FNEG)
#undef OP_FABS
#define OP_FABS   JX_F(JF_FABS, T1_FABS)
#undef OP_FNABS
#define OP_FNABS  JX_F(JF_FNABS, T1_FNABS)
#undef OP_FRSP
#define OP_FRSP   JX_F(JF_FRSP, T1_FRSP)
#undef OP_FCTIW
#define OP_FCTIW  JX_F(JF_FCTIW, T1_FCTIW)
#undef OP_FCTIWZ
#define OP_FCTIWZ JX_F(JF_FCTIWZ, T1_FCTIWZ)
#undef OP_FSEL
#define OP_FSEL   JX_F(JF_FSEL, T1_FSEL)
#undef OP_FRES
#define OP_FRES   JX_F(JF_FRES, T1_FRES)
#undef OP_FRSQRTE
#define OP_FRSQRTE JX_F(JF_FRSQRTE, T1_FRSQRTE)
#undef OP_FDIV
#define OP_FDIV   JX_F(PPC_SF_DIV, T1_FDIV)
#undef OP_FSUB
#define OP_FSUB   JX_F(PPC_SF_SUB, T1_FSUB)
#undef OP_FADD
#define OP_FADD   JX_F(PPC_SF_ADD, T1_FADD)
#undef OP_FMUL
#define OP_FMUL   JX_F(PPC_SF_MUL, T1_FMUL)
#undef OP_FMSUB
#define OP_FMSUB  JX_F(PPC_SF_MSUB, T1_FMSUB)
#undef OP_FMADD
#define OP_FMADD  JX_F(PPC_SF_MADD, T1_FMADD)
#undef OP_FNMSUB
#define OP_FNMSUB JX_F(PPC_SF_NMSUB, T1_FNMSUB)
#undef OP_FNMADD
#define OP_FNMADD JX_F(PPC_SF_NMADD, T1_FNMADD)
#undef OP_FDIVS
#define OP_FDIVS  JX_F(PPC_SF_DIV, T1_FDIVS)
#undef OP_FSUBS
#define OP_FSUBS  JX_F(PPC_SF_SUB, T1_FSUBS)
#undef OP_FADDS
#define OP_FADDS  JX_F(PPC_SF_ADD, T1_FADDS)
#undef OP_FMULS
#define OP_FMULS  JX_F(PPC_SF_MUL, T1_FMULS)
#undef OP_FMSUBS
#define OP_FMSUBS JX_F(PPC_SF_MSUB, T1_FMSUBS)
#undef OP_FMADDS
#define OP_FMADDS JX_F(PPC_SF_MADD, T1_FMADDS)
#undef OP_FNMSUBS
#define OP_FNMSUBS JX_F(PPC_SF_NMSUB, T1_FNMSUBS)
#undef OP_FNMADDS
#define OP_FNMADDS JX_F(PPC_SF_NMADD, T1_FNMADDS)
// clang-format on

#define PPC_DECODER_NAME        ppc_jit_tree
#define PPC_DECODER_RETURN_TYPE static uint16_t
#define PPC_DECODER_ARGS        uint32_t iw, const jit_cls_t *ctx
#define PPC_DECODER_PROLOGUE    (void)ctx
#define PPC_DECODER_EPILOGUE    return PD_GENERIC
#include "ppc_decode.h"
#undef PPC_DECODER_NAME
#undef PPC_DECODER_RETURN_TYPE
#undef PPC_DECODER_ARGS
#undef PPC_DECODER_PROLOGUE
#undef PPC_DECODER_EPILOGUE

#undef JX_D_RA0
#undef JX_ADD
#undef JX_ARITH_T
#undef JX_ARITH_A
#undef JX_X
#undef JX_FD
#undef JX_FX
#undef JX_F

// Classify one word: the predecoded classifier's T0 shapes, else the
// translator's own; a word neither handles keeps its T1 id (and the
// predecoded classifier's fields).
static uint16_t ppc_jit_classify(uint32_t iw, uint32_t ipc, uint32_t page_lo, bool is601, pd_entry_t *e) {
    uint16_t id = ppc_pd_classify_word(iw, ipc, page_lo, e);
    if (id > PPD_FIRST && id < PPD_END)
        return id;
    jit_cls_t ctx = {is601, e};
    return ppc_jit_tree(iw, &ctx);
}

#endif // PPC_JIT_CLASSIFY_H
