// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_ops.h
// The PPC (MPC601/MPC604) emulator's instruction bodies: factored helpers first,
// then the one-liner OP_ table that overloads the shared decode tree
// (ppc_decode.h) with execution content — the cpu_ops.h pattern
// (proposal-heterogeneous-multi-cpu.md §3.3.1).  Included by ppc_run.c
// only; the disassembler overloads the same OP_ names with printing.
//
// Alignment rules, DSISR encodings, and POWER-holdover semantics cite the
// MPC601 User's Manual (601UM) chapter 5 / chapter 10 instruction pages.

#ifndef GS_CPU_PPC_OPS_H
#define GS_CPU_PPC_OPS_H

#include "ppc_internal.h"
#include "ppc_softfp.h" // FPSCR bits + the PPC_SF_ op enum

// === Alignment exceptions (601UM §5.4.6; 604UM §4.5.6) ======================
//
// 601: with data translation ON (MSR[DT]=1) a scalar operand that spans a
// 4 KB page boundary takes the alignment exception; with translation OFF
// only a 256 MB boundary crossing does.  String/multiple accesses fault on
// any page crossing when unaligned, and on 256 MB crossings even when
// word-aligned; lscbx faults on any page crossing regardless of alignment.
// 604: only FP loads/stores, lmw/stmw, lwarx/stwcx. and eciwx/ecowx that
// are not word-aligned take the alignment exception — every other
// misaligned access is split by hardware, which ppc_scalar_gate models
// byte-wise when a translated access crosses a page.  The string rules are
// kept 601-shaped on both models (implementation-specific per the PEM;
// ladder-observable).
// (ppc_align_exception itself lives in ppc_internal.h — the MMU raises it
// for dcbz-to-W/I pages and FP accesses to I/O controller segments.)

// True if [ea, ea+size) crosses a 4 KB page boundary.
static inline bool ppc_crosses_page(uint32_t ea, uint32_t size) {
    return ((ea ^ (ea + size - 1)) & ~0xFFFu) != 0;
}

// True if [ea, ea+size) crosses a 256 MB segment boundary.
static inline bool ppc_crosses_segment(uint32_t ea, uint32_t size) {
    return ((ea ^ (ea + size - 1)) & 0xF0000000u) != 0;
}

// Scalar-access alignment check; raises and returns true on fault.
static inline bool ppc_check_align_scalar(ppc_t *p, uint32_t iw, uint32_t ea, uint32_t size) {
    if (size == 1)
        return false;
    if (p->msr & PPC_MSR_DT) {
        if (!ppc_crosses_page(ea, size))
            return false;
    } else {
        if (!ppc_crosses_segment(ea, size))
            return false;
    }
    ppc_align_exception(p, iw, ea);
    return true;
}

// String/multiple alignment check over the whole transfer (601UM §5.4.6.1.4).
static inline bool ppc_check_align_string(ppc_t *p, uint32_t iw, uint32_t ea, uint32_t nbytes, bool page_always) {
    if (nbytes == 0)
        return false;
    bool fault = ppc_crosses_segment(ea, nbytes);
    if (!fault && (p->msr & PPC_MSR_DT)) {
        // Unaligned strings/multiples fault on page crossings; word-aligned
        // ones only on the 256 MB case.  lscbx (page_always) faults on any
        // page crossing regardless of alignment.
        if ((page_always || (ea & 3)) && ppc_crosses_page(ea, nbytes))
            fault = true;
    }
    if (fault) {
        ppc_align_exception(p, iw, ea);
        return true;
    }
    return false;
}

// === Add/subtract carry & overflow ==========================================

// r = a + b (+carry); sets CA for the carrying forms, OV per OE.  Signed
// overflow: operands same sign, result different.
static inline uint32_t ppc_add_body(ppc_t *p, uint32_t a, uint32_t b, uint32_t cin, bool set_ca, bool oe) {
    uint64_t wide = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)wide;
    if (set_ca)
        ppc_set_ca(p, (int)(wide >> 32));
    if (oe)
        ppc_set_ov(p, (int)((~(a ^ b) & (a ^ r)) >> 31));
    return r;
}

// subf family: r = ~a + b + cin
static inline uint32_t ppc_subf_body(ppc_t *p, uint32_t a, uint32_t b, uint32_t cin, bool set_ca, bool oe) {
    return ppc_add_body(p, ~a, b, cin, set_ca, oe);
}

// === Compares ===============================================================

static inline void ppc_set_cr_field(ppc_t *p, uint32_t crf, uint32_t bits4) {
    uint32_t shift = 28 - 4 * crf;
    p->cr = (p->cr & ~(0xFu << shift)) | (bits4 << shift);
}

static inline uint32_t ppc_get_cr_field(ppc_t *p, uint32_t crf) {
    return (p->cr >> (28 - 4 * crf)) & 0xFu;
}

static inline void ppc_cmp_signed(ppc_t *p, uint32_t crf, int32_t a, int32_t b) {
    uint32_t f = (a < b) ? 8u : (a > b) ? 4u : 2u;
    if (p->xer & PPC_XER_SO)
        f |= 1u;
    ppc_set_cr_field(p, crf, f);
}

static inline void ppc_cmp_unsigned(ppc_t *p, uint32_t crf, uint32_t a, uint32_t b) {
    uint32_t f = (a < b) ? 8u : (a > b) ? 4u : 2u;
    if (p->xer & PPC_XER_SO)
        f |= 1u;
    ppc_set_cr_field(p, crf, f);
}

// === CR bit access ==========================================================

static inline uint32_t ppc_cr_bit(ppc_t *p, uint32_t n) {
    return (p->cr >> (31 - n)) & 1u;
}

static inline void ppc_set_cr_bit(ppc_t *p, uint32_t n, uint32_t v) {
    uint32_t m = 1u << (31 - n);
    p->cr = v ? (p->cr | m) : (p->cr & ~m);
}

// mtcrf: expand the 8-bit CRM field mask to 32 bits
static inline uint32_t ppc_crm_mask(uint32_t crm) {
    uint32_t mask = 0;
    for (int i = 0; i < 8; i++)
        if (crm & (0x80u >> i))
            mask |= 0xFu << (28 - 4 * i);
    return mask;
}

// === Branch condition evaluation (601UM §10, bc/bclr/bcctr) =================
// BO&16: ignore condition; BO&8: sense of the tested CR bit; BO&4: don't
// decrement CTR; BO&2: branch on CTR==0 (else CTR!=0).
static inline bool ppc_branch_taken(ppc_t *p, uint32_t bo, uint32_t bi, bool allow_ctr_dec) {
    bool ctr_ok = true;
    if (!(bo & 4u) && allow_ctr_dec) {
        p->ctr -= 1;
        ctr_ok = (bo & 2u) ? (p->ctr == 0) : (p->ctr != 0);
    }
    bool cond_ok = (bo & 16u) || (ppc_cr_bit(p, bi) == ((bo >> 3) & 1u));
    return ctr_ok && cond_ok;
}

// === Trap conditions (tw/twi) ===============================================
static inline bool ppc_trap_cond(uint32_t to, uint32_t a, uint32_t b) {
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    return ((to & 16u) && sa < sb) || ((to & 8u) && sa > sb) || ((to & 4u) && a == b) || ((to & 2u) && a < b) ||
           ((to & 1u) && a > b);
}

// CA rule shared by sraq/sraiq/srea (601UM sraq page): the bits shifted out
// (rotated word under the mask complement), OR-reduced, ANDed with the sign.
static inline void ppc_sra_mq_ca(ppc_t *p, uint32_t rot, uint32_t mask, uint32_t rs) {
    ppc_set_ca(p, ((rot & ~mask) != 0 && ((int32_t)rs < 0)) ? 1 : 0);
}

// ============================================================================
// The OP_ one-liner table (expanded inside ppc_decode.h by ppc_run.c).
// Locals `p` (the core) and `iw` (the instruction word) are bound by the
// decoder prologue.  A `break` inside OP() abandons the instruction body
// (used after raising an exception).
// ============================================================================

// clang-format off

#define OP(...) do { __VA_ARGS__; } while (0)

// Primitive accessors for the table
#define GPR(n)  p->gpr[n]
#define RT      PPC_RT(iw)
#define RA      PPC_RA(iw)
#define RB      PPC_RB(iw)
#define RA0     (RA ? GPR(RA) : 0u)          // the (rA|0) EA convention
#define SIMM32  ((uint32_t)PPC_SIMM(iw))
#define UIMM16  PPC_UIMM(iw)
#define READ(bits, ea)      memory_read_uint##bits(ea)
#define WRITE(bits, ea, v)  memory_write_uint##bits(ea, v)

// Result writeback + optional CR0 record (Rc)
#define RECT(...) do { uint32_t rect_v_ = (__VA_ARGS__); GPR(RT) = rect_v_; if (PPC_RC(iw)) ppc_record_cr0(p, rect_v_); } while (0)
#define RECA(...) do { uint32_t reca_v_ = (__VA_ARGS__); GPR(RA) = reca_v_; if (PPC_RC(iw)) ppc_record_cr0(p, reca_v_); } while (0)

// Guards: each aborts the instruction body on the raised exception.
// The access gates declare a local `xa` — the address the access must
// use (the EA itself on the SoA fast path, the physical address after a
// slow translation).  `ea` itself stays LOGICAL so the update forms
// write back the architected EA, and translation happens before any
// register writeback (a faulted access abandons with no side effects).
// M601/M604 reject the other model's encodings with the illegal program
// exception (TNT proposal §4.2 — decode-tree validity per model).
// CHKA_* route through ppc_scalar_gate (per-model alignment + the 604's
// hardware-split page crossings): loads read their value via LDV(bits) —
// the gate's byte-wise value when it split, a normal access at xa
// otherwise — and stores capture the value into `sv_` up front so the
// gate can consume it when it splits.
#define PRIV()  if (ppc_priv_check(p)) break
#define FP()    if (ppc_fp_check(p)) break
#define M601()  if (ppc_is_604(p)) { ppc_illegal_op(p, iw); break; }
#define M604()  if (!ppc_is_604(p)) { ppc_illegal_op(p, iw); break; }
#define CHKA_LD(ea, sz) uint32_t xa = ea; uint64_t lv_ = 0; int lg_ = ppc_scalar_gate(p, iw, &xa, sz, false, &lv_); if (lg_ < 0) break
#define CHKA_ST(ea, sz, vexpr) uint32_t xa = ea; uint64_t sv_ = (vexpr); int sg_ = ppc_scalar_gate(p, iw, &xa, sz, true, &sv_); if (sg_ < 0) break
#define LDV(bits)       (lg_ ? (uint##bits##_t)lv_ : READ(bits, xa))
#define STV(bits)       do { if (!sg_) WRITE(bits, xa, (uint##bits##_t)sv_); } while (0) // gate already stored byte-wise when it split — but the op body (update forms) must still run
#define XLT_LD(ea)      uint32_t xa = ea; if (ppc_dxlate(p, iw, &xa, false)) break
#define XLT_ST(ea)      uint32_t xa = ea; if (ppc_dxlate(p, iw, &xa, true)) break

// Effective addresses (D-form, D-form update, X-form, X-form update)
#define EA_D()  uint32_t ea = RA0 + SIMM32
#define EA_DU() uint32_t ea = GPR(RA) + SIMM32
#define EA_X()  uint32_t ea = RA0 + GPR(RB)
#define EA_XU() uint32_t ea = GPR(RA) + GPR(RB)
// Update-form writeback.  The PowerPC architecture calls rA = 0 an invalid
// form; the 601 keeps POWER compatibility instead and performs the access
// while INHIBITING the update of r0 (601UM §3.5.2 for the integer loads,
// §3.5.3 for the integer stores, §3.5.8 for the FP forms).  The
// rA = rD load case needs no guard: those forms update before the load
// writeback, so the loaded data wins, which is the same rule.
#define UPD()   do { if (RA) GPR(RA) = ea; } while (0)

// --- immediates, compares, traps ---
#define OP_TWI        OP(if (ppc_trap_cond(RT, GPR(RA), SIMM32)) ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_TRAP, p->instruction_pc))
#define OP_TW         OP(if (ppc_trap_cond(RT, GPR(RA), GPR(RB))) ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_TRAP, p->instruction_pc))
#define OP_MULLI      OP(GPR(RT) = (uint32_t)((int64_t)(int32_t)GPR(RA) * PPC_SIMM(iw)))
#define OP_SUBFIC     OP(GPR(RT) = ppc_subf_body(p, GPR(RA), SIMM32, 1, true, false))
#define OP_DOZI       OP(M601(); GPR(RT) = ((int32_t)GPR(RA) > PPC_SIMM(iw)) ? 0u : SIMM32 - GPR(RA))
#define OP_CMPLI      OP(ppc_cmp_unsigned(p, PPC_CRFD(iw), GPR(RA), UIMM16))
#define OP_CMPI       OP(ppc_cmp_signed(p, PPC_CRFD(iw), (int32_t)GPR(RA), PPC_SIMM(iw)))
#define OP_CMPL       OP(ppc_cmp_unsigned(p, PPC_CRFD(iw), GPR(RA), GPR(RB)))
#define OP_CMP        OP(ppc_cmp_signed(p, PPC_CRFD(iw), (int32_t)GPR(RA), (int32_t)GPR(RB)))
#define OP_ADDIC      OP(GPR(RT) = ppc_add_body(p, GPR(RA), SIMM32, 0, true, false))
#define OP_ADDIC_DOT  OP(uint32_t r_ = ppc_add_body(p, GPR(RA), SIMM32, 0, true, false); GPR(RT) = r_; ppc_record_cr0(p, r_))
#define OP_ADDI       OP(GPR(RT) = RA0 + SIMM32)
#define OP_ADDIS      OP(GPR(RT) = RA0 + (UIMM16 << 16))

// --- branches / system (complex bodies live in ppc_run.c helpers) ---
#define OP_BC         OP(ppc_do_bc(p, iw))
#define OP_B          OP(ppc_do_b(p, iw))
#define OP_BCLR       OP(ppc_do_bclr(p, iw))
#define OP_BCCTR      OP(ppc_do_bcctr(p, iw))
// sc: SRR1[0-15] is loaded from bits 16-31 of the instruction (601UM Table
// 5-22 — the POWER svc field, which §5.4.11's prose calls "undefined"; the
// table is the specific rule and costs nothing to honour).
#define OP_SC         OP(ppc_exception(p, PPC_VEC_SYSCALL, (iw & 0xFFFFu) << 16, p->pc))
// rfi restores MSR[16-31] from SRR1; bits outside that half (the 604's POW)
// are untouched — identical to the old whole-word form on the 601, whose
// implemented bits all live in the low half.
#define OP_RFI        OP(PRIV(); p->msr = ((p->msr & 0xFFFF0000u) | (p->srr1 & 0x0000FFFFu)) & ppc_msr_mask(p); ppc_update_active_maps(p); p->pc = p->srr0 & ~3u)
#define OP_ISYNC      OP((void)0) // context synchronize; no pipeline to flush

// --- CR logical (601UM XL-forms): a/b are the source CR bits ---
#define CROP(...)    OP(uint32_t a = ppc_cr_bit(p, RA), b = ppc_cr_bit(p, RB); (void)a; (void)b; ppc_set_cr_bit(p, RT, (__VA_ARGS__) & 1u))
#define OP_CRAND      CROP(a & b)
#define OP_CRANDC     CROP(a & ~b)
#define OP_CROR       CROP(a | b)
#define OP_CRORC      CROP(a | ~b)
#define OP_CRXOR      CROP(a ^ b)
#define OP_CREQV      CROP(~(a ^ b))
#define OP_CRNAND     CROP(~(a & b))
#define OP_CRNOR      CROP(~(a | b))
#define OP_MCRF       OP(ppc_set_cr_field(p, PPC_CRFD(iw), ppc_get_cr_field(p, PPC_CRFS(iw))))

// --- rotates ---
#define OP_RLWIMI     OP(uint32_t m_ = ppc_mask(PPC_MB(iw), PPC_ME(iw)); RECA((ppc_rotl(GPR(RT), RB) & m_) | (GPR(RA) & ~m_)))
#define OP_RLWINM     OP(RECA(ppc_rotl(GPR(RT), RB) & ppc_mask(PPC_MB(iw), PPC_ME(iw))))
#define OP_RLMI       OP(M601(); uint32_t m_ = ppc_mask(PPC_MB(iw), PPC_ME(iw)); RECA((ppc_rotl(GPR(RT), GPR(RB) & 31u) & m_) | (GPR(RA) & ~m_)))
#define OP_RLWNM      OP(RECA(ppc_rotl(GPR(RT), GPR(RB) & 31u) & ppc_mask(PPC_MB(iw), PPC_ME(iw))))

// --- logical immediates ---
#define OP_ORI        OP(GPR(RA) = GPR(RT) | UIMM16)
#define OP_ORIS       OP(GPR(RA) = GPR(RT) | (UIMM16 << 16))
#define OP_XORI       OP(GPR(RA) = GPR(RT) ^ UIMM16)
#define OP_XORIS      OP(GPR(RA) = GPR(RT) ^ (UIMM16 << 16))
#define OP_ANDI_DOT   OP(uint32_t r_ = GPR(RT) & UIMM16; GPR(RA) = r_; ppc_record_cr0(p, r_))
#define OP_ANDIS_DOT  OP(uint32_t r_ = GPR(RT) & (UIMM16 << 16); GPR(RA) = r_; ppc_record_cr0(p, r_))

// --- XO-form arithmetic (OE and Rc read from iw) ---
#define OP_ADD        OP(RECT(ppc_add_body(p, GPR(RA), GPR(RB), 0, false, PPC_OE(iw))))
#define OP_ADDC       OP(RECT(ppc_add_body(p, GPR(RA), GPR(RB), 0, true, PPC_OE(iw))))
#define OP_ADDE       OP(RECT(ppc_add_body(p, GPR(RA), GPR(RB), (p->xer & PPC_XER_CA) ? 1 : 0, true, PPC_OE(iw))))
#define OP_ADDME      OP(RECT(ppc_add_body(p, GPR(RA), 0xFFFFFFFFu, (p->xer & PPC_XER_CA) ? 1 : 0, true, PPC_OE(iw))))
#define OP_ADDZE      OP(RECT(ppc_add_body(p, GPR(RA), 0, (p->xer & PPC_XER_CA) ? 1 : 0, true, PPC_OE(iw))))
#define OP_SUBF       OP(RECT(ppc_subf_body(p, GPR(RA), GPR(RB), 1, false, PPC_OE(iw))))
#define OP_SUBFC      OP(RECT(ppc_subf_body(p, GPR(RA), GPR(RB), 1, true, PPC_OE(iw))))
#define OP_SUBFE      OP(RECT(ppc_subf_body(p, GPR(RA), GPR(RB), (p->xer & PPC_XER_CA) ? 1 : 0, true, PPC_OE(iw))))
#define OP_SUBFME     OP(RECT(ppc_subf_body(p, GPR(RA), 0xFFFFFFFFu, (p->xer & PPC_XER_CA) ? 1 : 0, true, PPC_OE(iw))))
#define OP_SUBFZE     OP(RECT(ppc_subf_body(p, GPR(RA), 0, (p->xer & PPC_XER_CA) ? 1 : 0, true, PPC_OE(iw))))
#define OP_NEG        OP(if (PPC_OE(iw)) ppc_set_ov(p, GPR(RA) == 0x80000000u); RECT(0u - GPR(RA)))
#define OP_MULHW      OP(RECT((uint32_t)(((int64_t)(int32_t)GPR(RA) * (int32_t)GPR(RB)) >> 32)))
#define OP_MULHWU     OP(RECT((uint32_t)(((uint64_t)GPR(RA) * GPR(RB)) >> 32)))
#define OP_MULLW      OP(int64_t pr_ = (int64_t)(int32_t)GPR(RA) * (int32_t)GPR(RB); if (PPC_OE(iw)) ppc_set_ov(p, pr_ != (int64_t)(int32_t)(uint32_t)pr_); RECT((uint32_t)pr_))
#define OP_DIVW       OP(ppc_do_divw(p, iw))
#define OP_DIVWU      OP(ppc_do_divwu(p, iw))

// --- POWER arithmetic holdovers ---
#define OP_ABS        OP(M601(); if (PPC_OE(iw)) ppc_set_ov(p, GPR(RA) == 0x80000000u); RECT(((int32_t)GPR(RA) < 0) ? 0u - GPR(RA) : GPR(RA)))
#define OP_NABS       OP(M601(); if (PPC_OE(iw)) p->xer &= ~PPC_XER_OV; RECT(((int32_t)GPR(RA) < 0) ? GPR(RA) : 0u - GPR(RA))) // never overflows
#define OP_DOZ        OP(M601(); ppc_do_doz(p, iw))
#define OP_MUL        OP(M601(); ppc_do_mul(p, iw))
#define OP_DIV        OP(M601(); ppc_do_div(p, iw))
#define OP_DIVS       OP(M601(); ppc_do_divs(p, iw))
#define OP_CLCS       OP(M601(); RECT(64)) // line size is 64 for every valid rA code

// --- logical ---
#define OP_AND        OP(RECA(GPR(RT) & GPR(RB)))
#define OP_ANDC       OP(RECA(GPR(RT) & ~GPR(RB)))
#define OP_OR         OP(RECA(GPR(RT) | GPR(RB)))
#define OP_ORC        OP(RECA(GPR(RT) | ~GPR(RB)))
#define OP_XOR        OP(RECA(GPR(RT) ^ GPR(RB)))
#define OP_NAND       OP(RECA(~(GPR(RT) & GPR(RB))))
#define OP_NOR        OP(RECA(~(GPR(RT) | GPR(RB))))
#define OP_EQV        OP(RECA(~(GPR(RT) ^ GPR(RB))))
#define OP_EXTSB      OP(RECA((uint32_t)(int32_t)(int8_t)GPR(RT)))
#define OP_EXTSH      OP(RECA((uint32_t)(int32_t)(int16_t)GPR(RT)))
#define OP_CNTLZW     OP(RECA(GPR(RT) ? (uint32_t)__builtin_clz(GPR(RT)) : 32u))

// --- architectural shifts ---
#define OP_SLW        OP(RECA((GPR(RB) & 0x20u) ? 0 : (GPR(RT) << (GPR(RB) & 31u))))
#define OP_SRW        OP(RECA((GPR(RB) & 0x20u) ? 0 : (GPR(RT) >> (GPR(RB) & 31u))))
#define OP_SRAW       OP(ppc_do_sraw(p, iw))
#define OP_SRAWI      OP(uint32_t n_ = RB, s_ = GPR(RT); ppc_set_ca(p, ((int32_t)s_ < 0) && n_ != 0 && (s_ & ((1u << n_) - 1u)) != 0); RECA((uint32_t)((int32_t)s_ >> n_)))

// --- POWER shift-with-MQ family (601UM chapter-10 POWER pages) ---
#define OP_MASKG      OP(M601(); RECA(ppc_mask(GPR(RT) & 31u, GPR(RB) & 31u)))
#define OP_MASKIR     OP(M601(); RECA((GPR(RA) & ~GPR(RB)) | (GPR(RT) & GPR(RB))))
#define OP_RRIB       OP(M601(); RECA((GPR(RA) & ~(0x80000000u >> (GPR(RB) & 31u))) | ((GPR(RT) & 0x80000000u) >> (GPR(RB) & 31u))))
#define OP_SLE        OP(M601(); uint32_t n_ = GPR(RB) & 31u, r_ = ppc_rotl(GPR(RT), n_); p->mq = r_; RECA(r_ & (0xFFFFFFFFu << n_)))
#define OP_SLEQ       OP(M601(); uint32_t n_ = GPR(RB) & 31u, r_ = ppc_rotl(GPR(RT), n_), m_ = 0xFFFFFFFFu << n_; uint32_t v_ = (r_ & m_) | (p->mq & ~m_); p->mq = r_; RECA(v_))
#define OP_SLIQ       OP(M601(); uint32_t n_ = RB, r_ = ppc_rotl(GPR(RT), n_); p->mq = r_; RECA(r_ & (0xFFFFFFFFu << n_)))
#define OP_SLLIQ      OP(M601(); uint32_t n_ = RB, r_ = ppc_rotl(GPR(RT), n_), m_ = 0xFFFFFFFFu << n_; uint32_t v_ = (r_ & m_) | (p->mq & ~m_); p->mq = r_; RECA(v_))
#define OP_SLLQ       OP(M601(); uint32_t n_ = GPR(RB) & 31u, m_ = 0xFFFFFFFFu << n_; RECA((GPR(RB) & 0x20u) ? (p->mq & m_) : ((ppc_rotl(GPR(RT), n_) & m_) | (p->mq & ~m_)))) // MQ unaltered
#define OP_SLQ        OP(M601(); uint32_t n_ = GPR(RB) & 31u, r_ = ppc_rotl(GPR(RT), n_); p->mq = r_; RECA((GPR(RB) & 0x20u) ? 0 : (r_ & (0xFFFFFFFFu << n_))))
#define OP_SRQ        OP(M601(); uint32_t n_ = GPR(RB) & 31u, r_ = ppc_rotl(GPR(RT), 32u - n_); p->mq = r_; RECA((GPR(RB) & 0x20u) ? 0 : (r_ & (0xFFFFFFFFu >> n_))))
#define OP_SRE        OP(M601(); uint32_t n_ = GPR(RB) & 31u, r_ = ppc_rotl(GPR(RT), 32u - n_); p->mq = r_; RECA(r_ & (0xFFFFFFFFu >> n_)))
#define OP_SRIQ       OP(M601(); uint32_t n_ = RB, r_ = ppc_rotl(GPR(RT), 32u - n_); p->mq = r_; RECA(r_ & (0xFFFFFFFFu >> n_)))
#define OP_SRLQ       OP(M601(); uint32_t n_ = GPR(RB) & 31u, m_ = 0xFFFFFFFFu >> n_; RECA((GPR(RB) & 0x20u) ? (p->mq & m_) : ((ppc_rotl(GPR(RT), 32u - n_) & m_) | (p->mq & ~m_)))) // MQ unaltered
#define OP_SREQ       OP(M601(); uint32_t n_ = GPR(RB) & 31u, r_ = ppc_rotl(GPR(RT), 32u - n_), m_ = 0xFFFFFFFFu >> n_; uint32_t v_ = (r_ & m_) | (p->mq & ~m_); p->mq = r_; RECA(v_))
#define OP_SRLIQ      OP(M601(); uint32_t n_ = RB, r_ = ppc_rotl(GPR(RT), 32u - n_), m_ = 0xFFFFFFFFu >> n_; uint32_t v_ = (r_ & m_) | (p->mq & ~m_); p->mq = r_; RECA(v_))
#define OP_SRAQ       OP(M601(); uint32_t n_ = GPR(RB) & 31u, r_ = ppc_rotl(GPR(RT), 32u - n_); uint32_t m_ = (GPR(RB) & 0x20u) ? 0u : (0xFFFFFFFFu >> n_); uint32_t s_ = GPR(RT); p->mq = r_; ppc_sra_mq_ca(p, r_, m_, s_); RECA((r_ & m_) | (((uint32_t)((int32_t)s_ >> 31)) & ~m_)))
#define OP_SRAIQ      OP(M601(); uint32_t n_ = RB, r_ = ppc_rotl(GPR(RT), 32u - n_), m_ = 0xFFFFFFFFu >> n_; uint32_t s_ = GPR(RT); p->mq = r_; ppc_sra_mq_ca(p, r_, m_, s_); RECA((r_ & m_) | (((uint32_t)((int32_t)s_ >> 31)) & ~m_)))
#define OP_SREA       OP(M601(); uint32_t n_ = GPR(RB) & 31u, r_ = ppc_rotl(GPR(RT), 32u - n_), m_ = 0xFFFFFFFFu >> n_; uint32_t s_ = GPR(RT); p->mq = r_; ppc_sra_mq_ca(p, r_, m_, s_); RECA((r_ & m_) | (((uint32_t)((int32_t)s_ >> 31)) & ~m_)))

// --- CR / MSR / SPR / SR moves ---
#define OP_MFCR       OP(GPR(RT) = p->cr)
#define OP_MTCRF      OP(uint32_t m_ = ppc_crm_mask((iw >> 12) & 0xFFu); p->cr = (GPR(RT) & m_) | (p->cr & ~m_))
#define OP_MCRXR      OP(ppc_set_cr_field(p, PPC_CRFD(iw), p->xer >> 28); p->xer &= 0x0FFFFFFFu)
#define OP_MFSPR      OP(ppc_mfspr(p, iw))
#define OP_MTSPR      OP(ppc_mtspr(p, iw))
#define OP_MFMSR      OP(PRIV(); GPR(RT) = p->msr)
#define OP_MTMSR      OP(PRIV(); p->msr = GPR(RT) & ppc_msr_mask(p); ppc_update_active_maps(p))
#define OP_MFSR       OP(PRIV(); GPR(RT) = p->sr[(iw >> 16) & 0xFu])
#define OP_MTSR       OP(PRIV(); ppc_set_sr(p, (iw >> 16) & 0xFu, GPR(RT)))
#define OP_MFSRIN     OP(PRIV(); GPR(RT) = p->sr[GPR(RB) >> 28])
#define OP_MTSRIN     OP(PRIV(); ppc_set_sr(p, GPR(RB) >> 28, GPR(RT)))
#define OP_TLBIE      OP(PRIV(); ppc_mmu_tlbie(p, GPR(RB)))
#define OP_MFTB       OP(M604(); ppc_do_mftb(p, iw)) // user-readable (PEM §2.2.1)
#define OP_TLBSYNC    OP(M604(); PRIV()) // ordering only: tlbie takes effect synchronously here (604UM §5.4.3.2)

// --- storage control (no cache model; semantics per proposal §3.8) ---
#define OP_SYNC       OP((void)0)
#define OP_EIEIO      OP((void)0)
#define OP_ICBI       OP((void)0)
#define OP_DCBT       OP((void)0)
#define OP_DCBTST     OP((void)0)
#define OP_DCBST      OP((void)0)
#define OP_DCBF       OP((void)0)
#define OP_DCBI       OP(PRIV()) // supervisor-only
#define OP_DCBZ       OP(EA_X(); ea &= ~31u; uint32_t xa = ea; int r_ = ppc_dxlate_dcbz(p, iw, &xa); if (r_) break; for (int i_ = 0; i_ < 8; i_++) WRITE(32, xa + 4u * (uint32_t)i_, 0)) // really zeroes the block (§3.8); W/I + T=1 rules in ppc_mmu.c

// --- D-form loads/stores ---
#define OP_LWZ        OP(EA_D();  CHKA_LD(ea, 4); GPR(RT) = LDV(32))
#define OP_LWZU       OP(EA_DU(); CHKA_LD(ea, 4); UPD(); GPR(RT) = LDV(32))
#define OP_LBZ        OP(EA_D();  XLT_LD(ea); GPR(RT) = READ(8, xa))
#define OP_LBZU       OP(EA_DU(); XLT_LD(ea); UPD(); GPR(RT) = READ(8, xa))
#define OP_STW        OP(EA_D();  CHKA_ST(ea, 4, GPR(RT)); STV(32))
#define OP_STWU       OP(EA_DU(); CHKA_ST(ea, 4, GPR(RT)); STV(32); UPD())  // store before update: rS==rA stores the pre-update value
#define OP_STB        OP(EA_D();  XLT_ST(ea); WRITE(8, xa, (uint8_t)GPR(RT)))
#define OP_STBU       OP(EA_DU(); XLT_ST(ea); WRITE(8, xa, (uint8_t)GPR(RT)); UPD())
#define OP_LHZ        OP(EA_D();  CHKA_LD(ea, 2); GPR(RT) = LDV(16))
#define OP_LHZU       OP(EA_DU(); CHKA_LD(ea, 2); UPD(); GPR(RT) = LDV(16))
#define OP_LHA        OP(EA_D();  CHKA_LD(ea, 2); GPR(RT) = (uint32_t)(int32_t)(int16_t)LDV(16))
#define OP_LHAU       OP(EA_DU(); CHKA_LD(ea, 2); UPD(); GPR(RT) = (uint32_t)(int32_t)(int16_t)LDV(16))
#define OP_STH        OP(EA_D();  CHKA_ST(ea, 2, GPR(RT)); STV(16))
#define OP_STHU       OP(EA_DU(); CHKA_ST(ea, 2, GPR(RT)); STV(16); UPD())
#define OP_LMW        OP(ppc_do_lmw(p, iw))
#define OP_STMW       OP(ppc_do_stmw(p, iw))

// --- indexed loads/stores ---
#define OP_LWZX       OP(EA_X();  CHKA_LD(ea, 4); GPR(RT) = LDV(32))
#define OP_LWZUX      OP(EA_XU(); CHKA_LD(ea, 4); UPD(); GPR(RT) = LDV(32))
#define OP_LBZX       OP(EA_X();  XLT_LD(ea); GPR(RT) = READ(8, xa))
#define OP_LBZUX      OP(EA_XU(); XLT_LD(ea); UPD(); GPR(RT) = READ(8, xa))
#define OP_LHZX       OP(EA_X();  CHKA_LD(ea, 2); GPR(RT) = LDV(16))
#define OP_LHZUX      OP(EA_XU(); CHKA_LD(ea, 2); UPD(); GPR(RT) = LDV(16))
#define OP_LHAX       OP(EA_X();  CHKA_LD(ea, 2); GPR(RT) = (uint32_t)(int32_t)(int16_t)LDV(16))
#define OP_LHAUX      OP(EA_XU(); CHKA_LD(ea, 2); UPD(); GPR(RT) = (uint32_t)(int32_t)(int16_t)LDV(16))
#define OP_STWX       OP(EA_X();  CHKA_ST(ea, 4, GPR(RT)); STV(32))
#define OP_STWUX      OP(EA_XU(); CHKA_ST(ea, 4, GPR(RT)); STV(32); UPD())  // store before update: rS==rA stores the pre-update value
#define OP_STBX       OP(EA_X();  XLT_ST(ea); WRITE(8, xa, (uint8_t)GPR(RT)))
#define OP_STBUX      OP(EA_XU(); XLT_ST(ea); WRITE(8, xa, (uint8_t)GPR(RT)); UPD())
#define OP_STHX       OP(EA_X();  CHKA_ST(ea, 2, GPR(RT)); STV(16))
#define OP_STHUX      OP(EA_XU(); CHKA_ST(ea, 2, GPR(RT)); STV(16); UPD())
#define OP_LWBRX      OP(EA_X();  CHKA_LD(ea, 4); GPR(RT) = __builtin_bswap32(LDV(32)))
#define OP_LHBRX      OP(EA_X();  CHKA_LD(ea, 2); GPR(RT) = __builtin_bswap16(LDV(16)))
#define OP_STWBRX     OP(EA_X();  CHKA_ST(ea, 4, __builtin_bswap32(GPR(RT))); STV(32))
#define OP_STHBRX     OP(EA_X();  CHKA_ST(ea, 2, (uint16_t)__builtin_bswap16((uint16_t)GPR(RT))); STV(16))

// --- atomics ---
// A misaligned EA is NOT an alignment exception by itself: §5.4.6.1.1 fires
// only on a 256 MB crossing with translation off (a page crossing with it
// on), and the chapter-3 lwarx/stwcx. pages say exactly that ("the
// alignment exception handler will be invoked if the word loaded crosses a
// page boundary, or the results may be undefined").  So these run the same
// scalar check as every other word access.
#define OP_LWARX      OP(EA_X(); CHKA_LD(ea, 4); p->reserve = 1; p->reserve_addr = xa; GPR(RT) = LDV(32))
#define OP_STWCX_DOT  OP(ppc_do_stwcx(p, iw))

// --- external control (EAR-gated, 601UM eciwx/ecowx pages; the 604 adds
//     the word-alignment requirement — 604UM §4.5.6) ---
#define ECX_ALIGN()   if (ppc_is_604(p) && (ea & 3u)) { ppc_align_exception(p, iw, ea); break; }
#define OP_ECIWX      OP(EA_X(); ECX_ALIGN(); if (!(p->ear & 0x80000000u)) { ppc_ecx_fault(p, ea, false); break; } XLT_LD(ea); GPR(RT) = READ(32, xa))
#define OP_ECOWX      OP(EA_X(); ECX_ALIGN(); if (!(p->ear & 0x80000000u)) { ppc_ecx_fault(p, ea, true); break; } XLT_ST(ea); WRITE(32, xa, GPR(RT)))

// --- strings ---
#define OP_LSWI       OP(ppc_do_lswi(p, iw))
#define OP_LSWX       OP(ppc_do_lswx(p, iw))
#define OP_STSWI      OP(ppc_do_stswi(p, iw))
#define OP_STSWX      OP(ppc_do_stswx(p, iw))
#define OP_LSCBX      OP(M601(); ppc_do_lscbx(p, iw))

// --- FP loads/stores (FPR file lives; conversions in ppc_fpu.c) ---
#define FPR_LOAD64()   (lg_ ? lv_ : (((uint64_t)READ(32, xa) << 32) | READ(32, xa + 4)))
#define FPR_STORE64()  do { if (!sg_) { WRITE(32, xa, (uint32_t)(sv_ >> 32)); WRITE(32, xa + 4, (uint32_t)sv_); } } while (0)
#define OP_LFS        OP(FP(); EA_D();  CHKA_LD(ea, 4); p->fpr[RT] = ppc_f32_to_f64(LDV(32)))
#define OP_LFSU       OP(FP(); EA_DU(); CHKA_LD(ea, 4); UPD(); p->fpr[RT] = ppc_f32_to_f64(LDV(32)))
#define OP_LFD        OP(FP(); EA_D();  CHKA_LD(ea, 8); p->fpr[RT] = FPR_LOAD64())
#define OP_LFDU       OP(FP(); EA_DU(); CHKA_LD(ea, 8); UPD(); p->fpr[RT] = FPR_LOAD64())
#define OP_STFS       OP(FP(); EA_D();  CHKA_ST(ea, 4, ppc_f64_to_f32_store(p->fpr[RT])); STV(32))
#define OP_STFSU      OP(FP(); EA_DU(); CHKA_ST(ea, 4, ppc_f64_to_f32_store(p->fpr[RT])); UPD(); STV(32))
#define OP_STFD       OP(FP(); EA_D();  CHKA_ST(ea, 8, p->fpr[RT]); FPR_STORE64())
#define OP_STFDU      OP(FP(); EA_DU(); CHKA_ST(ea, 8, p->fpr[RT]); UPD(); FPR_STORE64())
#define OP_LFSX       OP(FP(); EA_X();  CHKA_LD(ea, 4); p->fpr[RT] = ppc_f32_to_f64(LDV(32)))
#define OP_LFSUX      OP(FP(); EA_XU(); CHKA_LD(ea, 4); UPD(); p->fpr[RT] = ppc_f32_to_f64(LDV(32)))
#define OP_LFDX       OP(FP(); EA_X();  CHKA_LD(ea, 8); p->fpr[RT] = FPR_LOAD64())
#define OP_LFDUX      OP(FP(); EA_XU(); CHKA_LD(ea, 8); UPD(); p->fpr[RT] = FPR_LOAD64())
#define OP_STFSX      OP(FP(); EA_X();  CHKA_ST(ea, 4, ppc_f64_to_f32_store(p->fpr[RT])); STV(32))
#define OP_STFSUX     OP(FP(); EA_XU(); CHKA_ST(ea, 4, ppc_f64_to_f32_store(p->fpr[RT])); UPD(); STV(32))
#define OP_STFDX      OP(FP(); EA_X();  CHKA_ST(ea, 8, p->fpr[RT]); FPR_STORE64())
#define OP_STFDUX     OP(FP(); EA_XU(); CHKA_ST(ea, 8, p->fpr[RT]); UPD(); FPR_STORE64())
// stfiwx (604): store the FPR's low word untouched (PEM stfiwx page).
#define OP_STFIWX     OP(M604(); FP(); EA_X(); CHKA_ST(ea, 4, (uint32_t)p->fpr[RT]); STV(32))

// --- FP moves / FPSCR / compares (bodies in ppc_fpu.c); CR1 record on Rc ---
#define REC1()        if (PPC_RC(iw)) ppc_set_cr_field(p, 1, p->fpscr >> 28)
#define OP_FCMPU      OP(FP(); ppc_fcmp(p, iw, false))
#define OP_FCMPO      OP(FP(); ppc_fcmp(p, iw, true))
#define OP_FMR        OP(FP(); p->fpr[RT] = p->fpr[RB]; REC1())
#define OP_FNEG       OP(FP(); p->fpr[RT] = p->fpr[RB] ^ 0x8000000000000000ull; REC1())
#define OP_FABS       OP(FP(); p->fpr[RT] = p->fpr[RB] & 0x7FFFFFFFFFFFFFFFull; REC1())
#define OP_FNABS      OP(FP(); p->fpr[RT] = p->fpr[RB] | 0x8000000000000000ull; REC1())
#define OP_MFFS       OP(FP(); p->fpr[RT] = 0xFFFFFFFF00000000ull | p->fpscr; REC1()) // 601UM Table 3-15: frD[0-31] = $FFFFFFFF
#define OP_MTFSF      OP(FP(); ppc_do_mtfsf(p, iw))
#define OP_MTFSFI     OP(FP(); ppc_do_mtfsfi(p, iw))
#define OP_MTFSB0     OP(FP(); ppc_do_mtfsb(p, iw, false))
#define OP_MTFSB1     OP(FP(); ppc_do_mtfsb(p, iw, true))
#define OP_MCRFS      OP(FP(); ppc_do_mcrfs(p, iw))

// --- FP arithmetic: the Phase-E integer-kernel datapath (ppc_softfp.c) ---
#define OP_FRSP       OP(FP(); ppc_do_frsp(p, iw))
#define OP_FCTIW      OP(FP(); ppc_do_fctiw(p, iw, false))
#define OP_FCTIWZ     OP(FP(); ppc_do_fctiw(p, iw, true))
#define OP_FDIV       OP(FP(); ppc_fp_arith(p, iw, PPC_SF_DIV, false))
#define OP_FSUB       OP(FP(); ppc_fp_arith(p, iw, PPC_SF_SUB, false))
#define OP_FADD       OP(FP(); ppc_fp_arith(p, iw, PPC_SF_ADD, false))
#define OP_FMUL       OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MUL, false))
#define OP_FMSUB      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MSUB, false))
#define OP_FMADD      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MADD, false))
#define OP_FNMSUB     OP(FP(); ppc_fp_arith(p, iw, PPC_SF_NMSUB, false))
#define OP_FNMADD     OP(FP(); ppc_fp_arith(p, iw, PPC_SF_NMADD, false))
#define OP_FDIVS      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_DIV, true))
#define OP_FSUBS      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_SUB, true))
#define OP_FADDS      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_ADD, true))
#define OP_FMULS      OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MUL, true))
#define OP_FMSUBS     OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MSUB, true))
#define OP_FMADDS     OP(FP(); ppc_fp_arith(p, iw, PPC_SF_MADD, true))
#define OP_FNMSUBS    OP(FP(); ppc_fp_arith(p, iw, PPC_SF_NMSUB, true))
#define OP_FNMADDS    OP(FP(); ppc_fp_arith(p, iw, PPC_SF_NMADD, true))

// --- 604 optional-FP group (fsqrt stays illegal: not implemented on 604) ---
#define OP_FSEL       OP(M604(); FP(); ppc_do_fsel(p, iw))
#define OP_FRES       OP(M604(); FP(); ppc_do_fres(p, iw))
#define OP_FRSQRTE    OP(M604(); FP(); ppc_do_frsqrte(p, iw))

#define OP_ILLEGAL    OP(ppc_illegal_op(p, iw))

// clang-format on

#endif // GS_CPU_PPC_OPS_H
