// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_pd_classify.h
// ppc_decode.h in its third role: the predecode CLASSIFIER (proposal §6.2).
// Included by ppc_run.c after the executors, with every OP_ leaf redefined
// to return an id: the generated defaults return the leaf's T1 id (the raw
// word travels in c), and the overrides below return a specialized id for
// the shapes the predecoded loop handles directly.  Validity — reserved
// fields, invalid BO encodings — is the tree's, unchanged; model gating
// (M601/M604) stays in the handlers, so ids are model-independent and the
// pool is simply reset when the model changes.
//
// Template contract: defines
//     static uint16_t PPC_PD_CLASSIFY_NAME(ppc_t *p, uint32_t iw, uint32_t ipc,
//                                          uint32_t page_lo, pd_entry_t *e);

#include "ppc_pd_ids.h"

// Classification context for the tree's OP_ overrides.
typedef struct ppc_cls {
    uint32_t ipc; // address of the instruction
    uint32_t page_lo; // its page (in-page branch targets become indices)
    pd_entry_t *e; // out: fields
} ppc_cls_t;

_Static_assert(offsetof(ppc_t, gpr) == 0, "gpr[] must be the first ppc_t member");

static inline uint16_t ppd_emit(const ppc_cls_t *c, uint16_t id, uint8_t a, uint8_t b, uint32_t cval) {
    c->e->a = a;
    c->e->b = b;
    c->e->c = cval;
    return id;
}

// XO-form rT,rA,rB with an Rc twin; the OE forms stay generic.
static inline uint16_t ppd_cls_xo(const ppc_cls_t *c, uint32_t iw, uint16_t id, uint16_t t1) {
    if (PPC_OE(iw))
        return t1;
    return ppd_emit(c, (uint16_t)(id + PPC_RC(iw)), PPD_GOFF(PPC_RT(iw)), PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RB(iw)));
}

// X-form rA,rS,rB with an Rc twin.
static inline uint16_t ppd_cls_x(const ppc_cls_t *c, uint32_t iw, uint16_t id) {
    return ppd_emit(c, (uint16_t)(id + PPC_RC(iw)), PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RT(iw)), PPD_GOFF(PPC_RB(iw)));
}

// In-page target → entry index; false when the target leaves the page.
static inline bool ppd_in_page(const ppc_cls_t *c, uint32_t target, uint32_t *index) {
    if ((target & ~(uint32_t)PAGE_MASK) != c->page_lo)
        return false;
    *index = (target & PAGE_MASK) >> 2;
    return true;
}

static inline uint16_t ppd_cls_b(const ppc_cls_t *c, uint32_t iw) {
    int32_t li = (int32_t)(iw << 6) >> 6;
    li &= ~3;
    uint32_t target = (iw & 2u) ? (uint32_t)li : c->ipc + (uint32_t)li;
    uint32_t index;
    bool in = ppd_in_page(c, target, &index);
    uint16_t id = (iw & 1u) ? (in ? PPD_BL_IN : PPD_BL_OUT) : (in ? PPD_B_IN : PPD_B_OUT);
    return ppd_emit(c, id, 0, 0, in ? index : target);
}

// bc with the three common BO shapes (bdnz, branch-if-true, branch-if-
// false) and no link; everything else runs the generic body.
static inline uint16_t ppd_cls_bc(const ppc_cls_t *c, uint32_t iw, uint16_t t1) {
    uint32_t bo = PPC_RT(iw), bi = PPC_RA(iw);
    if (iw & 1u)
        return t1; // bcl: link stays generic
    int32_t bd = (int32_t)(int16_t)(iw & 0xFFFCu);
    uint32_t target = (iw & 2u) ? (uint32_t)bd : c->ipc + (uint32_t)bd;
    uint32_t index;
    bool in = ppd_in_page(c, target, &index);
    uint16_t id;
    if ((bo & 0x1Eu) == 0x10u) // 1z00y with z = 0: decrement CTR, branch if CTR != 0
        id = in ? PPD_BDNZ_IN : PPD_BDNZ_OUT;
    else if ((bo & 0x1Eu) == 0x0Cu) // 011zy: branch if CR[BI] set
        id = in ? PPD_BC_T_IN : PPD_BC_T_OUT;
    else if ((bo & 0x1Eu) == 0x04u) // 001zy: branch if CR[BI] clear
        id = in ? PPD_BC_F_IN : PPD_BC_F_OUT;
    else
        return t1;
    return ppd_emit(c, id, (uint8_t)(31u - bi), 0, in ? index : target);
}

// bclr / bcctr: only the unconditional, non-linking forms (BO = 20) — blr
// and bctr — are specialized; the rest keep ppc_do_bclr/bcctr's semantics
// (the 601's decrement-then-fetch-the-old-CTR quirk included).
static inline uint16_t ppd_cls_bclr(const ppc_cls_t *c, uint32_t iw, uint16_t id, uint16_t t1) {
    if (PPC_RT(iw) != 20u || (iw & 1u))
        return t1;
    return ppd_emit(c, id, 0, 0, 0);
}

// mfspr/mtspr: LR, CTR and XER are plain user-level moves (ppc.c).
static inline uint16_t ppd_cls_spr(const ppc_cls_t *c, uint32_t iw, bool to_spr, uint16_t t1) {
    uint32_t n = ((iw >> 16) & 0x1Fu) | (((iw >> 11) & 0x1Fu) << 5);
    uint16_t id;
    switch (n) {
    case 1:
        id = to_spr ? PPD_MTXER : PPD_MFXER;
        break;
    case 8:
        id = to_spr ? PPD_MTLR : PPD_MFLR;
        break;
    case 9:
        id = to_spr ? PPD_MTCTR : PPD_MFCTR;
        break;
    default:
        return t1;
    }
    return ppd_emit(c, id, PPD_GOFF(PPC_RT(iw)), 0, 0);
}

#include "ppc_pd_t1_classify.h" // generated: every OP_ → return T1_<leaf>

// clang-format off
#undef OP_ADD
#define OP_ADD    return ppd_cls_xo(ctx, iw, PPD_ADD, T1_ADD)
#undef OP_SUBF
#define OP_SUBF   return ppd_cls_xo(ctx, iw, PPD_SUBF, T1_SUBF)
#undef OP_MULLW
#define OP_MULLW  return ppd_cls_xo(ctx, iw, PPD_MULLW, T1_MULLW)
#undef OP_NEG
#define OP_NEG    return ppd_cls_xo(ctx, iw, PPD_NEG, T1_NEG)
#undef OP_AND
#define OP_AND    return ppd_cls_x(ctx, iw, PPD_AND)
#undef OP_ANDC
#define OP_ANDC   return ppd_cls_x(ctx, iw, PPD_ANDC)
#undef OP_OR
#define OP_OR     return ppd_cls_x(ctx, iw, PPD_OR)
#undef OP_NOR
#define OP_NOR    return ppd_cls_x(ctx, iw, PPD_NOR)
#undef OP_XOR
#define OP_XOR    return ppd_cls_x(ctx, iw, PPD_XOR)
#undef OP_SLW
#define OP_SLW    return ppd_cls_x(ctx, iw, PPD_SLW)
#undef OP_SRW
#define OP_SRW    return ppd_cls_x(ctx, iw, PPD_SRW)
#undef OP_EXTSB
#define OP_EXTSB  return ppd_cls_x(ctx, iw, PPD_EXTSB)
#undef OP_EXTSH
#define OP_EXTSH  return ppd_cls_x(ctx, iw, PPD_EXTSH)
#undef OP_CNTLZW
#define OP_CNTLZW return ppd_cls_x(ctx, iw, PPD_CNTLZW)
#undef OP_SRAWI
#define OP_SRAWI  return ppd_emit(ctx, (uint16_t)(PPD_SRAWI + PPC_RC(iw)), PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RT(iw)), PPC_RB(iw))
// D-form immediates (addis/lis, oris, xoris, andis. fold into their base id with the immediate pre-shifted)
#undef OP_ADDI
#define OP_ADDI   return PPC_RA(iw) ? ppd_emit(ctx, PPD_ADDI, PPD_GOFF(PPC_RT(iw)), PPD_GOFF(PPC_RA(iw)), (uint32_t)PPC_SIMM(iw)) : ppd_emit(ctx, PPD_LI, PPD_GOFF(PPC_RT(iw)), 0, (uint32_t)PPC_SIMM(iw))
#undef OP_ADDIS
#define OP_ADDIS  return PPC_RA(iw) ? ppd_emit(ctx, PPD_ADDI, PPD_GOFF(PPC_RT(iw)), PPD_GOFF(PPC_RA(iw)), PPC_UIMM(iw) << 16) : ppd_emit(ctx, PPD_LI, PPD_GOFF(PPC_RT(iw)), 0, PPC_UIMM(iw) << 16)
#undef OP_MULLI
#define OP_MULLI  return ppd_emit(ctx, PPD_MULLI, PPD_GOFF(PPC_RT(iw)), PPD_GOFF(PPC_RA(iw)), (uint32_t)PPC_SIMM(iw))
#undef OP_ORI
#define OP_ORI    return ppd_emit(ctx, PPD_ORI, PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RT(iw)), PPC_UIMM(iw))
#undef OP_ORIS
#define OP_ORIS   return ppd_emit(ctx, PPD_ORI, PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RT(iw)), PPC_UIMM(iw) << 16)
#undef OP_XORI
#define OP_XORI   return ppd_emit(ctx, PPD_XORI, PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RT(iw)), PPC_UIMM(iw))
#undef OP_XORIS
#define OP_XORIS  return ppd_emit(ctx, PPD_XORI, PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RT(iw)), PPC_UIMM(iw) << 16)
#undef OP_ANDI_DOT
#define OP_ANDI_DOT  return ppd_emit(ctx, PPD_ANDI_RC, PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RT(iw)), PPC_UIMM(iw))
#undef OP_ANDIS_DOT
#define OP_ANDIS_DOT return ppd_emit(ctx, PPD_ANDI_RC, PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RT(iw)), PPC_UIMM(iw) << 16)
#undef OP_ADDIC
#define OP_ADDIC     return ppd_emit(ctx, PPD_ADDIC, PPD_GOFF(PPC_RT(iw)), PPD_GOFF(PPC_RA(iw)), (uint32_t)PPC_SIMM(iw))
#undef OP_ADDIC_DOT
#define OP_ADDIC_DOT return ppd_emit(ctx, PPD_ADDIC_RC, PPD_GOFF(PPC_RT(iw)), PPD_GOFF(PPC_RA(iw)), (uint32_t)PPC_SIMM(iw))
#undef OP_SUBFIC
#define OP_SUBFIC    return ppd_emit(ctx, PPD_SUBFIC, PPD_GOFF(PPC_RT(iw)), PPD_GOFF(PPC_RA(iw)), (uint32_t)PPC_SIMM(iw))
// rotates: raw word
#undef OP_RLWINM
#define OP_RLWINM return ppd_emit(ctx, (uint16_t)(PPD_RLWINM + PPC_RC(iw)), 0, 0, iw)
#undef OP_RLWIMI
#define OP_RLWIMI return ppd_emit(ctx, (uint16_t)(PPD_RLWIMI + PPC_RC(iw)), 0, 0, iw)
#undef OP_RLWNM
#define OP_RLWNM  return ppd_emit(ctx, (uint16_t)(PPD_RLWNM + PPC_RC(iw)), 0, 0, iw)
// compares
#undef OP_CMPI
#define OP_CMPI   return ppd_emit(ctx, PPD_CMPWI, (uint8_t)PPC_CRFD(iw), PPD_GOFF(PPC_RA(iw)), (uint32_t)PPC_SIMM(iw))
#undef OP_CMPLI
#define OP_CMPLI  return ppd_emit(ctx, PPD_CMPLWI, (uint8_t)PPC_CRFD(iw), PPD_GOFF(PPC_RA(iw)), PPC_UIMM(iw))
#undef OP_CMP
#define OP_CMP    return ppd_emit(ctx, PPD_CMPW, (uint8_t)PPC_CRFD(iw), PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RB(iw)))
#undef OP_CMPL
#define OP_CMPL   return ppd_emit(ctx, PPD_CMPLW, (uint8_t)PPC_CRFD(iw), PPD_GOFF(PPC_RA(iw)), PPD_GOFF(PPC_RB(iw)))
// branches
#undef OP_B
#define OP_B      return ppd_cls_b(ctx, iw)
#undef OP_BC
#define OP_BC     return ppd_cls_bc(ctx, iw, T1_BC)
#undef OP_BCLR
#define OP_BCLR   return ppd_cls_bclr(ctx, iw, PPD_BLR, T1_BCLR)
#undef OP_BCCTR
#define OP_BCCTR  return ppd_cls_bclr(ctx, iw, PPD_BCTR, T1_BCCTR)
// SPR / CR moves
#undef OP_MFSPR
#define OP_MFSPR  return ppd_cls_spr(ctx, iw, false, T1_MFSPR)
#undef OP_MTSPR
#define OP_MTSPR  return ppd_cls_spr(ctx, iw, true, T1_MTSPR)
#undef OP_MFCR
#define OP_MFCR   return ppd_emit(ctx, PPD_MFCR, PPD_GOFF(PPC_RT(iw)), 0, 0)
#undef OP_MTCRF
#define OP_MTCRF  return ppd_emit(ctx, PPD_MTCRF, PPD_GOFF(PPC_RT(iw)), 0, ppc_crm_mask((iw >> 12) & 0xFFu))
#undef OP_CRAND
#define OP_CRAND  return ppd_emit(ctx, PPD_CRAND, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), PPC_RB(iw))
#undef OP_CRANDC
#define OP_CRANDC return ppd_emit(ctx, PPD_CRANDC, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), PPC_RB(iw))
#undef OP_CROR
#define OP_CROR   return ppd_emit(ctx, PPD_CROR, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), PPC_RB(iw))
#undef OP_CRORC
#define OP_CRORC  return ppd_emit(ctx, PPD_CRORC, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), PPC_RB(iw))
#undef OP_CRXOR
#define OP_CRXOR  return ppd_emit(ctx, PPD_CRXOR, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), PPC_RB(iw))
#undef OP_CREQV
#define OP_CREQV  return ppd_emit(ctx, PPD_CREQV, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), PPC_RB(iw))
#undef OP_CRNAND
#define OP_CRNAND return ppd_emit(ctx, PPD_CRNAND, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), PPC_RB(iw))
#undef OP_CRNOR
#define OP_CRNOR  return ppd_emit(ctx, PPD_CRNOR, (uint8_t)PPC_RT(iw), (uint8_t)PPC_RA(iw), PPC_RB(iw))
#undef OP_MCRF
#define OP_MCRF   return ppd_emit(ctx, PPD_MCRF, (uint8_t)PPC_CRFD(iw), (uint8_t)PPC_CRFS(iw), 0)
// no-ops
#undef OP_SYNC
#define OP_SYNC   return PPD_NOP
#undef OP_EIEIO
#define OP_EIEIO  return PPD_NOP
#undef OP_ICBI
#define OP_ICBI   return PPD_NOP
#undef OP_DCBT
#define OP_DCBT   return PPD_NOP
#undef OP_DCBTST
#define OP_DCBTST return PPD_NOP
#undef OP_DCBST
#define OP_DCBST  return PPD_NOP
#undef OP_DCBF
#define OP_DCBF   return PPD_NOP
#undef OP_ISYNC
#define OP_ISYNC  return PPD_ISYNC
// clang-format on

#define PPC_DECODER_NAME        PPC_PD_TREE_NAME
#define PPC_DECODER_RETURN_TYPE static uint16_t
#define PPC_DECODER_ARGS        uint32_t iw, ppc_cls_t *ctx
#define PPC_DECODER_PROLOGUE    (void)ctx
#define PPC_DECODER_EPILOGUE    return PD_GENERIC
#include "ppc_decode.h"
#undef PPC_DECODER_NAME
#undef PPC_DECODER_RETURN_TYPE
#undef PPC_DECODER_ARGS
#undef PPC_DECODER_PROLOGUE
#undef PPC_DECODER_EPILOGUE

// Classify one instruction word.  T1 entries carry the word in c.
static uint16_t PPC_PD_CLASSIFY_NAME(uint32_t iw, uint32_t ipc, uint32_t page_lo, pd_entry_t *e) {
    e->a = 0;
    e->b = 0;
    e->c = iw;
    ppc_cls_t ctx = {ipc, page_lo, e};
    return PPC_PD_TREE_NAME(iw, &ctx);
}
