// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_ops.h
// Shared instruction-body helpers for the PPC (MPC601) interpreter
// (ppc_run.c).  Alignment rules, DSISR encodings, and POWER-holdover
// semantics cite the MPC601 User's Manual (601UM) chapter 5 / chapter 10
// instruction pages.

#ifndef GS_CPU_PPC_OPS_H
#define GS_CPU_PPC_OPS_H

#include "ppc_internal.h"

// === Alignment exceptions (601UM §5.4.6) ====================================
//
// With data translation ON (MSR[DT]=1) a scalar operand that spans a 4 KB
// page boundary takes the alignment exception; with translation OFF only a
// 256 MB boundary crossing does.  String/multiple accesses fault on any page
// crossing when unaligned, and on 256 MB crossings even when word-aligned;
// lscbx faults on any page crossing regardless of alignment.

// True if [ea, ea+size) crosses a 4 KB page boundary.
static inline bool ppc_crosses_page(uint32_t ea, uint32_t size) {
    return ((ea ^ (ea + size - 1)) & ~0xFFFu) != 0;
}

// True if [ea, ea+size) crosses a 256 MB segment boundary.
static inline bool ppc_crosses_segment(uint32_t ea, uint32_t size) {
    return ((ea ^ (ea + size - 1)) & 0xF0000000u) != 0;
}

// DSISR image for an alignment exception (601UM Table 5-13): opcode fields
// repacked so the handler can emulate the access without re-reading the
// instruction.  X-form (opcode 31) and D-form encode differently.
static inline uint32_t ppc_align_dsisr(uint32_t iw) {
    uint32_t dsisr = 0;
    if (PPC_OPCD(iw) == 31) {
        dsisr |= ((iw >> 2) & 1u) << 16 | ((iw >> 1) & 1u) << 15; // DSISR[15-16] = instr bits 29-30
        dsisr |= ((iw >> 6) & 1u) << 14; // DSISR[17] = instr bit 25
        dsisr |= ((iw >> 7) & 0xFu) << 10; // DSISR[18-21] = instr bits 21-24
    } else {
        dsisr |= ((iw >> 26) & 1u) << 14; // DSISR[17] = instr bit 5
        dsisr |= ((iw >> 27) & 0xFu) << 10; // DSISR[18-21] = instr bits 1-4
    }
    dsisr |= ((iw >> 21) & 0x1Fu) << 5; // DSISR[22-26] = source/destination
    dsisr |= (iw >> 16) & 0x1Fu; // DSISR[27-31] = rA
    return dsisr;
}

// Raise the alignment exception for the access described by iw/ea.
static inline void ppc_align_exception(ppc_t *p, uint32_t iw, uint32_t ea) {
    p->dar = ea;
    p->dsisr = ppc_align_dsisr(iw);
    ppc_exception(p, PPC_VEC_ALIGNMENT, 0, p->instruction_pc);
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

// r = a + b (+carry); returns result, sets CA always for the -c forms,
// OV per OE.  Signed overflow: operands same sign, result different.
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

// === POWER-holdover shift helpers ===========================================
// All from the 601UM chapter-10 POWER pages: n is the 5-bit shift amount;
// the "bit 26" of rB (value 0x20) selects the >=32 half of a 64-bit shift.

// CA rule shared by sraq/sraiq/srea (601UM sraq page): the bits shifted out
// (rotated word under the mask complement), OR-reduced, ANDed with the sign.
static inline void ppc_sra_mq_ca(ppc_t *p, uint32_t rot, uint32_t mask, uint32_t rs) {
    ppc_set_ca(p, ((rot & ~mask) != 0 && ((int32_t)rs < 0)) ? 1 : 0);
}

#endif // GS_CPU_PPC_OPS_H
