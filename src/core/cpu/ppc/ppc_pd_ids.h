// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_pd_ids.h
// The PowerPC predecoded-executor id space (proposal §6.1-§6.2):
//   0..15         control ids (predecode.h)
//   16..T1_END-1  flattened-generic ids, one per ppc_decode.h leaf
//                 (generated: build/gen/ppc_pd_t1_ids.h; c = the raw word)
//   T1_END..      specialized (T0) ids, one per shape below
//
// T0 entry conventions: a = destination register byte offset (gpr[n] is at
// 4n) or a CR field / CR bit, b = source register byte offset, c = the
// second source's offset, a sign-extended or pre-shifted immediate, an
// in-page target index, an absolute target, or the raw word where the
// handler re-derives its fields (the rotates).  Loads, stores and every
// leaf that needs the raw word for its exception paths stay T1.

#ifndef PPC_PD_IDS_H
#define PPC_PD_IDS_H

#include "predecode.h"

#include "ppc_pd_t1_ids.h" // generated: T1_<leaf> ids and T1_END

// clang-format off
#define PPC_PD_T0(X) \
    /* XO-form register arithmetic (OE forms stay T1): a = rT, b = rA, c = rB offset */ \
    X(ADD) X(ADD_RC) X(SUBF) X(SUBF_RC) X(MULLW) X(MULLW_RC) X(NEG) X(NEG_RC) \
    /* X-form logical/shift: a = rA (dest), b = rS, c = rB offset */ \
    X(AND) X(AND_RC) X(ANDC) X(ANDC_RC) X(OR) X(OR_RC) X(NOR) X(NOR_RC) X(XOR) X(XOR_RC) \
    X(SLW) X(SLW_RC) X(SRW) X(SRW_RC) X(EXTSB) X(EXTSB_RC) X(EXTSH) X(EXTSH_RC) X(CNTLZW) X(CNTLZW_RC) \
    /* srawi: a = rA, b = rS, c = shift */ \
    X(SRAWI) X(SRAWI_RC) \
    /* D-form immediates: a = rT/rA, b = rA/rS, c = immediate (addis/oris/xoris/andis pre-shifted) */ \
    X(ADDI) X(LI) X(MULLI) X(ORI) X(XORI) X(ANDI_RC) X(ADDIC) X(ADDIC_RC) X(SUBFIC) \
    /* rotates keep the raw word in c (mask and shift re-derived at run time) */ \
    X(RLWINM) X(RLWINM_RC) X(RLWIMI) X(RLWIMI_RC) X(RLWNM) X(RLWNM_RC) \
    /* compares: a = crfD, b = rA offset, c = immediate or rB offset */ \
    X(CMPWI) X(CMPLWI) X(CMPW) X(CMPLW) \
    /* branches: c = in-page entry index (_IN) or absolute target (_OUT); a = 31 - BI for bc */ \
    X(B_IN) X(B_OUT) X(BL_IN) X(BL_OUT) X(BDNZ_IN) X(BDNZ_OUT) X(BC_T_IN) X(BC_T_OUT) X(BC_F_IN) X(BC_F_OUT) \
    X(BLR) X(BCTR) \
    /* SPR / CR moves: a = rT or rS, c = the expanded mtcrf mask */ \
    X(MFLR) X(MTLR) X(MFCTR) X(MTCTR) X(MFXER) X(MTXER) X(MFCR) X(MTCRF) \
    /* CR logical: a = bT, b = bA, c = bB; mcrf: a = crfD, b = crfS */ \
    X(CRAND) X(CRANDC) X(CROR) X(CRORC) X(CRXOR) X(CREQV) X(CRNAND) X(CRNOR) X(MCRF) \
    /* cache/sync no-ops and isync */ \
    X(NOP) X(ISYNC)
// clang-format on

enum {
    PPD_FIRST = T1_END - 1,
#define X(name) PPD_##name,
    PPC_PD_T0(X)
#undef X
        PPD_END, // one past the last id
};

_Static_assert(PPD_END < 65536, "PPC predecode id space overflow");

// Register byte offsets stored in entries (gpr[] is the first ppc_t member).
#define PPD_GOFF(n) ((uint8_t)(4u * (uint32_t)(n)))

#endif // PPC_PD_IDS_H
