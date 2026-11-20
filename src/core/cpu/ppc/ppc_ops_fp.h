// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_ops_fp.h
// The FP register-to-register leaves of the OP_ table (ppc_ops.h): moves,
// FPSCR moves, compares and arithmetic.  No include guard: ppc_ops.h
// includes it for the interpreter, and ppc_run.c includes it again over
// the classifier's leaves (ppc_pd_t1_classify.h) for ppc_execute_fp, the
// decode tree with only these leaves live -- the translation tier's FP
// helper.  Each leaf is undefined first so the second inclusion replaces
// whatever the includer had.  Needs ppc_ops.h's helper macros (OP, FP,
// M604, RT, RB, REC1).

// clang-format off

// --- FP moves / FPSCR / compares (bodies in ppc_fpu.c); CR1 record on Rc ---
#undef OP_FCMPU
#define OP_FCMPU      OP(FP(); ppc_fcmp(p, iw, false))
#undef OP_FCMPO
#define OP_FCMPO      OP(FP(); ppc_fcmp(p, iw, true))
#undef OP_FMR
#define OP_FMR        OP(FP(); p->fpr[RT] = p->fpr[RB]; REC1())
#undef OP_FNEG
#define OP_FNEG       OP(FP(); p->fpr[RT] = p->fpr[RB] ^ 0x8000000000000000ull; REC1())
#undef OP_FABS
#define OP_FABS       OP(FP(); p->fpr[RT] = p->fpr[RB] & 0x7FFFFFFFFFFFFFFFull; REC1())
#undef OP_FNABS
#define OP_FNABS      OP(FP(); p->fpr[RT] = p->fpr[RB] | 0x8000000000000000ull; REC1())
#undef OP_MFFS
#define OP_MFFS       OP(FP(); p->fpr[RT] = 0xFFFFFFFF00000000ull | p->fpscr; REC1()) // 601UM Table 3-15: frD[0-31] = $FFFFFFFF
#undef OP_MTFSF
#define OP_MTFSF      OP(FP(); ppc_do_mtfsf(p, iw))
#undef OP_MTFSFI
#define OP_MTFSFI     OP(FP(); ppc_do_mtfsfi(p, iw))
#undef OP_MTFSB0
#define OP_MTFSB0     OP(FP(); ppc_do_mtfsb(p, iw, false))
#undef OP_MTFSB1
#define OP_MTFSB1     OP(FP(); ppc_do_mtfsb(p, iw, true))
#undef OP_MCRFS
#define OP_MCRFS      OP(FP(); ppc_do_mcrfs(p, iw))

// --- FP arithmetic: the integer-kernel datapath (ppc_softfp.c) ---
#undef OP_FRSP
#define OP_FRSP       OP(FP(); ppc_do_frsp(p, iw))
#undef OP_FCTIW
#define OP_FCTIW      OP(FP(); ppc_do_fctiw(p, iw, false))
#undef OP_FCTIWZ
#define OP_FCTIWZ     OP(FP(); ppc_do_fctiw(p, iw, true))
#undef OP_FDIV
#define OP_FDIV       OP(FP(); ppc_fp_arith(p, iw, PPC_SF_DIV, false))
#undef OP_FSUB
#define OP_FSUB       OP(FP(); ppc_fp_arith(p, iw, PPC_SF_SUB, false))
#undef OP_FADD
#define OP_FADD       OP(FP(); ppc_fp_arith(p, iw, PPC_SF_ADD, false))
#undef OP_FMUL
#define OP_FMUL       OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MUL, false))
#undef OP_FMSUB
#define OP_FMSUB      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MSUB, false))
#undef OP_FMADD
#define OP_FMADD      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MADD, false))
#undef OP_FNMSUB
#define OP_FNMSUB     OP(FP(); ppc_fp_arith(p, iw, PPC_SF_NMSUB, false))
#undef OP_FNMADD
#define OP_FNMADD     OP(FP(); ppc_fp_arith(p, iw, PPC_SF_NMADD, false))
#undef OP_FDIVS
#define OP_FDIVS      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_DIV, true))
#undef OP_FSUBS
#define OP_FSUBS      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_SUB, true))
#undef OP_FADDS
#define OP_FADDS      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_ADD, true))
#undef OP_FMULS
#define OP_FMULS      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MUL, true))
#undef OP_FMSUBS
#define OP_FMSUBS     OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MSUB, true))
#undef OP_FMADDS
#define OP_FMADDS     OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MADD, true))
#undef OP_FNMSUBS
#define OP_FNMSUBS    OP(FP(); ppc_fp_arith(p, iw, PPC_SF_NMSUB, true))
#undef OP_FNMADDS
#define OP_FNMADDS    OP(FP(); ppc_fp_arith(p, iw, PPC_SF_NMADD, true))

// --- 604 optional-FP group (fsqrt stays illegal: not implemented on 604) ---
#undef OP_FSEL
#define OP_FSEL       OP(M604(); FP(); ppc_do_fsel(p, iw))
#undef OP_FRES
#define OP_FRES       OP(M604(); FP(); ppc_do_fres(p, iw))
#undef OP_FRSQRTE
#define OP_FRSQRTE    OP(M604(); FP(); ppc_do_frsqrte(p, iw))

// clang-format on
