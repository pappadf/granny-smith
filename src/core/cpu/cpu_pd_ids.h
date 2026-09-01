// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_pd_ids.h
// The 68K predecoded-executor id space (proposal §4.1-§4.3) and the
// per-id property table the flag-liveness pass consults (§5.1).
//
// Layout of the 16-bit id space:
//   0..15         control ids (predecode.h)
//   16..T1_END-1  flattened-generic ids, one per decode-tree leaf
//                 (generated: build/gen/cpu_pd_t1_ids.h)
//   T1_END..      specialized (T0) ids, allocated in FAMILIES below
//
// A family is one operation at one size over a fixed list of operand
// shapes; each shape occupies one id (single) or an adjacent PAIR (full-
// flags id, no-flags twin at id + 1) when the operation's condition-code
// result can be elided.  The executor stamps its handlers from the same
// family list, so ids and cases cannot drift.

#ifndef CPU_PD_IDS_H
#define CPU_PD_IDS_H

#include "predecode.h"

#include "cpu_pd_t1_ids.h" // generated: T1_<leaf> ids and T1_END

// Operand shapes.  A family's ids are laid out in this order; the classifier
// computes `family + PD_SHAPE_STRIDE * shape` (+1 for the no-flags twin).
enum {
    PD_SH_D = 0, // Dn or An by register offset (the value, not an address)
    PD_SH_IND, // (An)
    PD_SH_INC, // (An)+
    PD_SH_DEC, // -(An)
    PD_SH_D16, // (d16,An)
    PD_SH_ABS, // (xxx).L or (d16,PC): the resolved absolute address in c
    PD_SH_IMM, // #imm in c
    PD_SH_COUNT
};

// Family kinds: what the shape list and the id stride are.
//   S7P  seven source shapes (D..IMM), pairs        — ea,Dn operations
//   S7S  seven source shapes, singles                — MOVEA/ADDA/SUBA/DIV
//   D6P  six destination shapes (D..ABS), pairs      — Dn,ea / #imm,ea / CLR / ADDQ
//   D6S  six destination shapes, singles             — (unused today)
//   P1   one pair                                    — register-only ops
//   S1   one single
//   N(n) n singles                                   — branches, misc
#define PD_S7P_SLOTS 14
#define PD_S7S_SLOTS 7
#define PD_D6P_SLOTS 12
#define PD_D6S_SLOTS 6

// The family list: X(name, slots).  Order is the id order; keep the
// executor's stamping in sync (cpu_pd_run.h uses the same names).
// clang-format off
#define PD_FAMILIES(X) \
    /* MOVE.sz src→dst: one family per destination shape (14 source slots each) */ \
    X(MOVE_B_D, PD_S7P_SLOTS) X(MOVE_B_IND, PD_S7P_SLOTS) X(MOVE_B_INC, PD_S7P_SLOTS) \
    X(MOVE_B_DEC, PD_S7P_SLOTS) X(MOVE_B_D16, PD_S7P_SLOTS) X(MOVE_B_ABS, PD_S7P_SLOTS) \
    X(MOVE_W_D, PD_S7P_SLOTS) X(MOVE_W_IND, PD_S7P_SLOTS) X(MOVE_W_INC, PD_S7P_SLOTS) \
    X(MOVE_W_DEC, PD_S7P_SLOTS) X(MOVE_W_D16, PD_S7P_SLOTS) X(MOVE_W_ABS, PD_S7P_SLOTS) \
    X(MOVE_L_D, PD_S7P_SLOTS) X(MOVE_L_IND, PD_S7P_SLOTS) X(MOVE_L_INC, PD_S7P_SLOTS) \
    X(MOVE_L_DEC, PD_S7P_SLOTS) X(MOVE_L_D16, PD_S7P_SLOTS) X(MOVE_L_ABS, PD_S7P_SLOTS) \
    /* MOVEA.W/L src→An */ \
    X(MOVEA_W, PD_S7S_SLOTS) X(MOVEA_L, PD_S7S_SLOTS) \
    /* ea,Dn arithmetic/logic/compare (pairs) */ \
    X(ADD_B_EA_DN, PD_S7P_SLOTS) X(ADD_W_EA_DN, PD_S7P_SLOTS) X(ADD_L_EA_DN, PD_S7P_SLOTS) \
    X(SUB_B_EA_DN, PD_S7P_SLOTS) X(SUB_W_EA_DN, PD_S7P_SLOTS) X(SUB_L_EA_DN, PD_S7P_SLOTS) \
    X(AND_B_EA_DN, PD_S7P_SLOTS) X(AND_W_EA_DN, PD_S7P_SLOTS) X(AND_L_EA_DN, PD_S7P_SLOTS) \
    X(OR_B_EA_DN, PD_S7P_SLOTS)  X(OR_W_EA_DN, PD_S7P_SLOTS)  X(OR_L_EA_DN, PD_S7P_SLOTS) \
    X(CMP_B_EA_DN, PD_S7P_SLOTS) X(CMP_W_EA_DN, PD_S7P_SLOTS) X(CMP_L_EA_DN, PD_S7P_SLOTS) \
    /* Dn,ea read-modify-write (pairs; shape D is EOR Dn,Dn) */ \
    X(ADD_B_DN_EA, PD_D6P_SLOTS) X(ADD_W_DN_EA, PD_D6P_SLOTS) X(ADD_L_DN_EA, PD_D6P_SLOTS) \
    X(SUB_B_DN_EA, PD_D6P_SLOTS) X(SUB_W_DN_EA, PD_D6P_SLOTS) X(SUB_L_DN_EA, PD_D6P_SLOTS) \
    X(AND_B_DN_EA, PD_D6P_SLOTS) X(AND_W_DN_EA, PD_D6P_SLOTS) X(AND_L_DN_EA, PD_D6P_SLOTS) \
    X(OR_B_DN_EA, PD_D6P_SLOTS)  X(OR_W_DN_EA, PD_D6P_SLOTS)  X(OR_L_DN_EA, PD_D6P_SLOTS) \
    X(EOR_B_DN_EA, PD_D6P_SLOTS) X(EOR_W_DN_EA, PD_D6P_SLOTS) X(EOR_L_DN_EA, PD_D6P_SLOTS) \
    /* #imm,ea (pairs) */ \
    X(ADDI_B, PD_D6P_SLOTS) X(ADDI_W, PD_D6P_SLOTS) X(ADDI_L, PD_D6P_SLOTS) \
    X(SUBI_B, PD_D6P_SLOTS) X(SUBI_W, PD_D6P_SLOTS) X(SUBI_L, PD_D6P_SLOTS) \
    X(ANDI_B, PD_D6P_SLOTS) X(ANDI_W, PD_D6P_SLOTS) X(ANDI_L, PD_D6P_SLOTS) \
    X(ORI_B, PD_D6P_SLOTS)  X(ORI_W, PD_D6P_SLOTS)  X(ORI_L, PD_D6P_SLOTS) \
    X(EORI_B, PD_D6P_SLOTS) X(EORI_W, PD_D6P_SLOTS) X(EORI_L, PD_D6P_SLOTS) \
    X(CMPI_B, PD_D6P_SLOTS) X(CMPI_W, PD_D6P_SLOTS) X(CMPI_L, PD_D6P_SLOTS) \
    /* ADDQ/SUBQ #q,ea (pairs) and #q,An (singles, no flags) */ \
    X(ADDQ_B, PD_D6P_SLOTS) X(ADDQ_W, PD_D6P_SLOTS) X(ADDQ_L, PD_D6P_SLOTS) \
    X(SUBQ_B, PD_D6P_SLOTS) X(SUBQ_W, PD_D6P_SLOTS) X(SUBQ_L, PD_D6P_SLOTS) \
    X(ADDQ_AN, 1) X(SUBQ_AN, 1) \
    /* ADDA/SUBA (singles) and CMPA (pairs) */ \
    X(ADDA_W, PD_S7S_SLOTS) X(ADDA_L, PD_S7S_SLOTS) X(SUBA_W, PD_S7S_SLOTS) X(SUBA_L, PD_S7S_SLOTS) \
    X(CMPA_W, PD_S7P_SLOTS) X(CMPA_L, PD_S7P_SLOTS) \
    /* TST (pairs, pure flag), CLR (pairs) */ \
    X(TST_B, PD_S7P_SLOTS) X(TST_W, PD_S7P_SLOTS) X(TST_L, PD_S7P_SLOTS) \
    X(CLR_B, PD_D6P_SLOTS) X(CLR_W, PD_D6P_SLOTS) X(CLR_L, PD_D6P_SLOTS) \
    /* register-only unaries (pairs) */ \
    X(NEG_B_D, 2) X(NEG_W_D, 2) X(NEG_L_D, 2) X(NOT_B_D, 2) X(NOT_W_D, 2) X(NOT_L_D, 2) \
    X(EXT_W, 2) X(EXT_L, 2) X(EXTB_L, 2) X(SWAP, 2) X(MOVEQ, 2) \
    /* shifts and rotates by immediate (pairs) and by register (pairs) */ \
    X(ASL_B_I, 2) X(ASL_W_I, 2) X(ASL_L_I, 2) X(ASR_B_I, 2) X(ASR_W_I, 2) X(ASR_L_I, 2) \
    X(LSL_B_I, 2) X(LSL_W_I, 2) X(LSL_L_I, 2) X(LSR_B_I, 2) X(LSR_W_I, 2) X(LSR_L_I, 2) \
    X(ROL_B_I, 2) X(ROL_W_I, 2) X(ROL_L_I, 2) X(ROR_B_I, 2) X(ROR_W_I, 2) X(ROR_L_I, 2) \
    X(ASL_B_R, 2) X(ASL_W_R, 2) X(ASL_L_R, 2) X(ASR_B_R, 2) X(ASR_W_R, 2) X(ASR_L_R, 2) \
    X(LSL_B_R, 2) X(LSL_W_R, 2) X(LSL_L_R, 2) X(LSR_B_R, 2) X(LSR_W_R, 2) X(LSR_L_R, 2) \
    X(ROL_B_R, 2) X(ROL_W_R, 2) X(ROL_L_R, 2) X(ROR_B_R, 2) X(ROR_W_R, 2) X(ROR_L_R, 2) \
    /* multiply (pairs) and divide (singles) */ \
    X(MULU_W, PD_S7P_SLOTS) X(MULS_W, PD_S7P_SLOTS) X(DIVU_W, PD_S7S_SLOTS) X(DIVS_W, PD_S7S_SLOTS) \
    /* bit operations on Dn (pairs) */ \
    X(BTST_I_D, 2) X(BCHG_I_D, 2) X(BCLR_I_D, 2) X(BSET_I_D, 2) \
    X(BTST_D_D, 2) X(BCHG_D_D, 2) X(BCLR_D_D, 2) X(BSET_D_D, 2) \
    /* CMPM (pairs), EXG / ABCD / SBCD (singles) */ \
    X(CMPM_B, 2) X(CMPM_W, 2) X(CMPM_L, 2) X(EXG_DD, 1) X(EXG_AA, 1) X(EXG_DA, 1) X(ABCD_DD, 1) X(SBCD_DD, 1) \
    /* branches: Bcc.B/W by condition (0 = BRA; 2..15), each _IN and _OUT */ \
    X(BCC_B_IN, 16) X(BCC_B_OUT, 16) X(BCC_W_IN, 16) X(BCC_W_OUT, 16) \
    X(BSR_B_IN, 1) X(BSR_B_OUT, 1) X(BSR_W_IN, 1) X(BSR_W_OUT, 1) \
    X(DBF_IN, 1) X(DBF_OUT, 1) X(DBCC_IN, 1) X(DBCC_OUT, 1) \
    X(JMP_IND, 1) X(JMP_D16, 1) X(JMP_ABS, 1) X(JSR_IND, 1) X(JSR_D16, 1) X(JSR_ABS, 1) \
    X(RTS, 1) X(RTD, 1) X(NOP, 1) X(UNLK, 1) X(LINK_W, 1) X(LINK_L, 1) \
    X(PEA_IND, 1) X(PEA_D16, 1) X(PEA_ABS, 1) X(LEA_IND, 1) X(LEA_D16, 1) X(LEA_ABS, 1) \
    /* MOVEM.W/L: registers→memory (predec, (An), (d16,An)) and memory→registers (postinc, (An), (d16,An)) */ \
    X(MOVEM_W_R_DEC, 1) X(MOVEM_W_R_IND, 1) X(MOVEM_W_R_D16, 1) X(MOVEM_W_INC_R, 1) X(MOVEM_W_IND_R, 1) X(MOVEM_W_D16_R, 1) \
    X(MOVEM_L_R_DEC, 1) X(MOVEM_L_R_IND, 1) X(MOVEM_L_R_D16, 1) X(MOVEM_L_INC_R, 1) X(MOVEM_L_IND_R, 1) X(MOVEM_L_D16_R, 1) \
    /* Scc Dn (condition in b), A-line trap, TRAP #n */ \
    X(SCC_D, 1) X(ATRAP, 1) X(TRAP, 1)
// clang-format on

// The id enum: PDF_<family> is the family's first id; PDF_<family>_END its last.
enum {
    PD_T0_FIRST = T1_END - 1, // so the first family starts at T1_END
#define X(name, slots) PDF_##name, PDF_##name##_END = PDF_##name + (slots) - 1,
    PD_FAMILIES(X)
#undef X
        PD_ID_END, // one past the last id
};

_Static_assert(PD_ID_END < 65536, "68K predecode id space overflow");
#define PD_ID_COUNT ((uint32_t)PD_ID_END)

// Shape → id within a paired seven/six-shape family.
#define PD_ID_S7P(fam, shape) ((uint16_t)(PDF_##fam + 2 * (shape)))
#define PD_ID_D6P(fam, shape) ((uint16_t)(PDF_##fam + 2 * (shape)))
#define PD_ID_S7S(fam, shape) ((uint16_t)(PDF_##fam + (shape)))

// === Per-id properties (the elision pass, §5.1) ===
// Declared next to the handlers by kind (cpu.c builds the table from the
// family list); T1 and control ids carry the conservative bits.
enum {
    PD_P_WNZVC = 1 << 0, // writes N,Z,V,C unconditionally and reads none of them (an overwriter)
    PD_P_CANFAULT = 1 << 1, // may reach memory, raise or trap before its flags are written
    PD_P_ELIDABLE = 1 << 2, // has a no-flags twin at id + 1
    PD_P_TWIN = 1 << 3, // IS the no-flags twin of id - 1
    PD_P_MEMDEF = 1 << 4, // the twin touches memory: E2 only (slow-path guard)
};
extern uint8_t g_cpu_pd_prop[];

// Build g_cpu_pd_prop from the family list (idempotent; cpu_init calls it).
void cpu_pd_prop_init(void);

#endif // CPU_PD_IDS_H
