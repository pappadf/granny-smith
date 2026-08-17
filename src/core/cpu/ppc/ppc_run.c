// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_run.c
// The PPC (MPC601) interpreter: primary-opcode switch with extended-opcode
// switches for opcodes 19 and 31 (the FP groups 59/63 dispatch to ppc_fpu.c).
// Fixed-width 32-bit fetch through the global fast-path memory accessors —
// this core is a MAIN CPU (cores.md main-vs-aux rule).
//
// Instruction semantics follow the MPC601 User's Manual chapter 10 pages,
// including the POWER-architecture holdovers the 601 retains (abs, clcs,
// div, divs, doz, dozi, lscbx, maskg, maskir, mul, nabs, rlmi, rrib, the
// sle/sre shift-with-MQ family) and the MQ register they imply.

#include "ppc_ops.h"

#include "log.h"

LOG_USE_CATEGORY_NAME("ppc");

// Illegal-instruction program exception (also the documented path for
// PowerPC instructions the 601 does not implement: mftb, tlbia, the
// 64-bit set — 601UM §10.3 Tables 10-6/10-8).
static void ppc_illegal(ppc_t *p, uint32_t iw) {
    LOG(5, "illegal instruction $%08X at $%08X", iw, p->instruction_pc);
    ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_ILLEGAL, p->instruction_pc);
}

// Privileged-instruction program exception from user mode; returns true if
// the fault was taken (caller abandons the instruction body).
static bool ppc_priv_check(ppc_t *p) {
    if (p->msr & PPC_MSR_PR) {
        ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_PRIV, p->instruction_pc);
        return true;
    }
    return false;
}

// FP-availability gate for every FP opcode (601UM §5.4.8): MSR[FP]=0 makes
// any floating-point instruction take the FP-unavailable exception.
static bool ppc_fp_check(ppc_t *p) {
    if (!(p->msr & PPC_MSR_FP)) {
        ppc_exception(p, PPC_VEC_FPUNAVAIL, 0, p->instruction_pc);
        return true;
    }
    return false;
}

// === String transfers (lswi/lswx/stswi/stswx/lscbx) =========================

// Load n bytes at ea into registers rt.. (wrapping, left-to-right per
// register, partial last register zero-filled).  Registers named by the
// EA-forming fields are skipped (601UM lswx page).  skip_a/skip_b are -1
// when not applicable.
static void ppc_load_string(ppc_t *p, uint32_t ea, uint32_t rt, uint32_t n, int skip_a, int skip_b) {
    uint32_t r = rt;
    while (n > 0) {
        uint32_t word = 0;
        uint32_t take = n < 4 ? n : 4;
        for (uint32_t i = 0; i < take; i++)
            word |= (uint32_t)memory_read_uint8(ea + i) << (24 - 8 * i);
        if ((int)r != skip_a && (int)r != skip_b)
            p->gpr[r] = word;
        ea += take;
        n -= take;
        r = (r + 1) & 31;
    }
}

static void ppc_store_string(ppc_t *p, uint32_t ea, uint32_t rs, uint32_t n) {
    uint32_t r = rs;
    while (n > 0) {
        uint32_t word = p->gpr[r];
        uint32_t take = n < 4 ? n : 4;
        for (uint32_t i = 0; i < take; i++)
            memory_write_uint8(ea + i, (uint8_t)(word >> (24 - 8 * i)));
        ea += take;
        n -= take;
        r = (r + 1) & 31;
    }
}

// === Extended opcode 19 (branch/CR-logical group) ===========================

static void ppc_op19(ppc_t *p, uint32_t iw) {
    uint32_t bo = PPC_RT(iw), bi = PPC_RA(iw);
    switch (PPC_XO10(iw)) {
    case 0: { // mcrf crfD,crfS
        uint32_t crfd = (iw >> 23) & 7, crfs = (iw >> 18) & 7;
        ppc_set_cr_field(p, crfd, ppc_get_cr_field(p, crfs));
        break;
    }
    case 16: { // bclr[l]
        bool taken = ppc_branch_taken(p, bo, bi, true);
        uint32_t target = p->lr & ~3u;
        if (PPC_RC(iw))
            p->lr = p->pc;
        if (taken)
            p->pc = target;
        break;
    }
    case 33: // crnor
    case 129: // crandc
    case 193: // crxor
    case 225: // crnand
    case 257: // crand
    case 289: // creqv
    case 417: // crorc
    case 449: { // cror
        uint32_t a = ppc_cr_bit(p, PPC_RA(iw)), b = ppc_cr_bit(p, PPC_RB(iw)), r;
        switch (PPC_XO10(iw)) {
        case 33:
            r = ~(a | b);
            break;
        case 129:
            r = a & ~b;
            break;
        case 193:
            r = a ^ b;
            break;
        case 225:
            r = ~(a & b);
            break;
        case 257:
            r = a & b;
            break;
        case 289:
            r = ~(a ^ b);
            break;
        case 417:
            r = a | ~b;
            break;
        default:
            r = a | b;
            break;
        }
        ppc_set_cr_bit(p, PPC_RT(iw), r & 1u);
        break;
    }
    case 50: // rfi (601UM: MSR[16-31] ← SRR1[16-31]; NIA = SRR0 & ~3)
        if (ppc_priv_check(p))
            return;
        p->msr = (p->srr1 & 0x0000FFFFu) & PPC_MSR_MASK;
        ppc_update_active_maps(p);
        p->pc = p->srr0 & ~3u;
        break;
    case 150: // isync — context synchronize; no pipeline to flush here
        break;
    case 528: { // bcctr[l] (CTR-decrement forms are invalid; no decrement)
        bool taken = ppc_branch_taken(p, bo, bi, false);
        uint32_t target = p->ctr & ~3u;
        if (PPC_RC(iw))
            p->lr = p->pc;
        if (taken)
            p->pc = target;
        break;
    }
    default:
        ppc_illegal(p, iw);
        break;
    }
}

// === Extended opcode 31 =====================================================

static void ppc_op31(ppc_t *p, uint32_t iw) {
    uint32_t rt = PPC_RT(iw), ra = PPC_RA(iw), rb = PPC_RB(iw);
    uint32_t a = p->gpr[ra], b = p->gpr[rb], s = p->gpr[rt];
    bool oe = PPC_OE(iw) != 0, rc = PPC_RC(iw) != 0;
    uint32_t r;
    uint32_t ea;

    switch (PPC_XO10(iw)) {

    // --- compares / traps ---
    case 0: // cmp
        ppc_cmp_signed(p, (iw >> 23) & 7, (int32_t)a, (int32_t)b);
        return;
    case 32: // cmpl
        ppc_cmp_unsigned(p, (iw >> 23) & 7, a, b);
        return;
    case 4: // tw
        if (ppc_trap_cond(rt, a, b))
            ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_TRAP, p->instruction_pc);
        return;

    // --- add/subtract family (XO-form: OE variant is xo+512) ---
    case 266:
    case 266 + 512: // add
        r = ppc_add_body(p, a, b, 0, false, oe);
        break;
    case 10:
    case 10 + 512: // addc
        r = ppc_add_body(p, a, b, 0, true, oe);
        break;
    case 138:
    case 138 + 512: // adde
        r = ppc_add_body(p, a, b, (p->xer & PPC_XER_CA) ? 1 : 0, true, oe);
        break;
    case 234:
    case 234 + 512: // addme
        r = ppc_add_body(p, a, 0xFFFFFFFFu, (p->xer & PPC_XER_CA) ? 1 : 0, true, oe);
        break;
    case 202:
    case 202 + 512: // addze
        r = ppc_add_body(p, a, 0, (p->xer & PPC_XER_CA) ? 1 : 0, true, oe);
        break;
    case 40:
    case 40 + 512: // subf
        r = ppc_subf_body(p, a, b, 1, false, oe);
        break;
    case 8:
    case 8 + 512: // subfc
        r = ppc_subf_body(p, a, b, 1, true, oe);
        break;
    case 136:
    case 136 + 512: // subfe
        r = ppc_subf_body(p, a, b, (p->xer & PPC_XER_CA) ? 1 : 0, true, oe);
        break;
    case 232:
    case 232 + 512: // subfme
        r = ppc_subf_body(p, a, 0xFFFFFFFFu, (p->xer & PPC_XER_CA) ? 1 : 0, true, oe);
        break;
    case 200:
    case 200 + 512: // subfze
        r = ppc_subf_body(p, a, 0, (p->xer & PPC_XER_CA) ? 1 : 0, true, oe);
        break;
    case 104:
    case 104 + 512: // neg
        r = 0u - a;
        if (oe)
            ppc_set_ov(p, a == 0x80000000u);
        break;

    // --- multiply / divide ---
    case 75: // mulhw (no OE variant)
        r = (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32);
        break;
    case 11: // mulhwu
        r = (uint32_t)(((uint64_t)a * b) >> 32);
        break;
    case 235:
    case 235 + 512: { // mullw
        int64_t prod = (int64_t)(int32_t)a * (int32_t)b;
        r = (uint32_t)prod;
        if (oe)
            ppc_set_ov(p, prod != (int64_t)(int32_t)r);
        break;
    }
    case 491:
    case 491 + 512: // divw
        if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) {
            r = 0; // architecturally undefined; fixed for determinism
            if (oe)
                ppc_set_ov(p, 1);
        } else {
            r = (uint32_t)((int32_t)a / (int32_t)b);
            if (oe)
                ppc_set_ov(p, 0);
        }
        break;
    case 459:
    case 459 + 512: // divwu
        if (b == 0) {
            r = 0;
            if (oe)
                ppc_set_ov(p, 1);
        } else {
            r = a / b;
            if (oe)
                ppc_set_ov(p, 0);
        }
        break;

    // --- POWER arithmetic holdovers ---
    case 360:
    case 360 + 512: // abs
        r = ((int32_t)a < 0) ? 0u - a : a;
        if (oe)
            ppc_set_ov(p, a == 0x80000000u);
        break;
    case 488:
    case 488 + 512: // nabs (never overflows; OE=1 clears OV, SO unchanged)
        r = ((int32_t)a < 0) ? a : 0u - a;
        if (oe)
            p->xer &= ~PPC_XER_OV;
        break;
    case 264:
    case 264 + 512: // doz
        if ((int32_t)a > (int32_t)b) {
            r = 0;
            if (oe)
                ppc_set_ov(p, 0);
        } else {
            r = b - a;
            if (oe) // positive overflows only (601UM doz page)
                ppc_set_ov(p, (int)(((a ^ b) & (r ^ b)) >> 31));
        }
        break;
    case 107:
    case 107 + 512: { // mul: rD = high 32, MQ = low 32; CR0 reflects MQ
        int64_t prod = (int64_t)(int32_t)a * (int32_t)b;
        p->mq = (uint32_t)prod;
        r = (uint32_t)((uint64_t)prod >> 32);
        if (oe)
            ppc_set_ov(p, prod != (int64_t)(int32_t)p->mq);
        p->gpr[rt] = r;
        if (rc)
            ppc_record_cr0(p, p->mq);
        return;
    }
    case 331:
    case 331 + 512: { // div: (rA||MQ) / rB → rD, remainder → MQ
        int64_t dd = ((int64_t)a << 32) | p->mq;
        if (b == 0) {
            r = 0;
            p->mq = 0;
            if (oe)
                ppc_set_ov(p, 1);
        } else {
            int64_t q = dd / (int32_t)b; // C99: truncation toward zero,
            int64_t rem = dd % (int32_t)b; // remainder sign follows dividend
            if (q != (int64_t)(int32_t)q) {
                // Quotient unrepresentable: the documented -2^31/-1 case
                // yields rD=$80000000/MQ=0; others are undefined — same
                // deterministic values chosen.
                r = 0x80000000u;
                p->mq = 0;
                if (oe)
                    ppc_set_ov(p, 1);
            } else {
                r = (uint32_t)q;
                p->mq = (uint32_t)rem;
                if (oe)
                    ppc_set_ov(p, 0);
            }
        }
        p->gpr[rt] = r;
        if (rc)
            ppc_record_cr0(p, p->mq); // CR0 reflects the remainder
        return;
    }
    case 363:
    case 363 + 512: { // divs: rA / rB → rD, remainder → MQ
        if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) {
            r = (b != 0) ? 0x80000000u : 0u;
            p->mq = 0;
            if (oe)
                ppc_set_ov(p, 1);
        } else {
            r = (uint32_t)((int32_t)a / (int32_t)b);
            p->mq = (uint32_t)((int32_t)a % (int32_t)b);
            if (oe)
                ppc_set_ov(p, 0);
        }
        p->gpr[rt] = r;
        if (rc)
            ppc_record_cr0(p, p->mq); // CR0 reflects the remainder
        return;
    }

    // --- logical ---
    case 28: // and
        r = s & b;
        p->gpr[ra] = r;
        goto record_ra;
    case 60: // andc
        r = s & ~b;
        p->gpr[ra] = r;
        goto record_ra;
    case 444: // or
        r = s | b;
        p->gpr[ra] = r;
        goto record_ra;
    case 412: // orc
        r = s | ~b;
        p->gpr[ra] = r;
        goto record_ra;
    case 316: // xor
        r = s ^ b;
        p->gpr[ra] = r;
        goto record_ra;
    case 476: // nand
        r = ~(s & b);
        p->gpr[ra] = r;
        goto record_ra;
    case 124: // nor
        r = ~(s | b);
        p->gpr[ra] = r;
        goto record_ra;
    case 284: // eqv
        r = ~(s ^ b);
        p->gpr[ra] = r;
        goto record_ra;
    case 954: // extsb
        r = (uint32_t)(int32_t)(int8_t)s;
        p->gpr[ra] = r;
        goto record_ra;
    case 922: // extsh
        r = (uint32_t)(int32_t)(int16_t)s;
        p->gpr[ra] = r;
        goto record_ra;
    case 26: // cntlzw
        r = s ? (uint32_t)__builtin_clz(s) : 32u;
        p->gpr[ra] = r;
        goto record_ra;

    // --- architectural shifts ---
    case 24: // slw
        r = (b & 0x20u) ? 0 : (s << (b & 31u));
        p->gpr[ra] = r;
        goto record_ra;
    case 536: // srw
        r = (b & 0x20u) ? 0 : (s >> (b & 31u));
        p->gpr[ra] = r;
        goto record_ra;
    case 792: { // sraw
        uint32_t n = b & 31u;
        if (b & 0x20u) {
            r = (uint32_t)((int32_t)s >> 31);
            ppc_set_ca(p, ((int32_t)s < 0) && s != r);
        } else {
            r = (uint32_t)((int32_t)s >> n);
            ppc_set_ca(p, ((int32_t)s < 0) && n != 0 && (s & ((1u << n) - 1u)) != 0);
        }
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 824: { // srawi
        uint32_t n = rb; // SH field
        r = (uint32_t)((int32_t)s >> n);
        ppc_set_ca(p, ((int32_t)s < 0) && n != 0 && (s & ((1u << n) - 1u)) != 0);
        p->gpr[ra] = r;
        goto record_ra;
    }

    // --- POWER shift-with-MQ family (601UM chapter-10 POWER pages) ---
    case 29: // maskg
        r = ppc_mask(s & 31u, b & 31u);
        p->gpr[ra] = r;
        goto record_ra;
    case 541: // maskir: insert rS into rA under mask rB
        r = (p->gpr[ra] & ~b) | (s & b);
        p->gpr[ra] = r;
        goto record_ra;
    case 537: // rrib: bit 0 of rS rotated right rB[27-31], inserted into rA
        r = (p->gpr[ra] & ~(0x80000000u >> (b & 31u))) | ((s & 0x80000000u) >> (b & 31u));
        p->gpr[ra] = r;
        goto record_ra;
    case 153: { // sle
        uint32_t n = b & 31u, rot = ppc_rotl(s, n);
        p->mq = rot;
        r = rot & (0xFFFFFFFFu << n);
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 217: { // sleq
        uint32_t n = b & 31u, rot = ppc_rotl(s, n), m = 0xFFFFFFFFu << n;
        r = (rot & m) | (p->mq & ~m);
        p->mq = rot;
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 184: { // sliq
        uint32_t n = rb, rot = ppc_rotl(s, n);
        p->mq = rot;
        r = rot & (0xFFFFFFFFu << n);
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 248: { // slliq
        uint32_t n = rb, rot = ppc_rotl(s, n), m = 0xFFFFFFFFu << n;
        r = (rot & m) | (p->mq & ~m);
        p->mq = rot;
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 216: { // sllq (MQ not altered)
        uint32_t n = b & 31u, m = 0xFFFFFFFFu << n;
        r = (b & 0x20u) ? (p->mq & m) : ((ppc_rotl(s, n) & m) | (p->mq & ~m));
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 152: { // slq
        uint32_t n = b & 31u, rot = ppc_rotl(s, n);
        p->mq = rot;
        r = (b & 0x20u) ? 0 : (rot & (0xFFFFFFFFu << n));
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 664: { // srq
        uint32_t n = b & 31u, rot = ppc_rotl(s, 32u - n);
        p->mq = rot;
        r = (b & 0x20u) ? 0 : (rot & (0xFFFFFFFFu >> n));
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 665: { // sre
        uint32_t n = b & 31u, rot = ppc_rotl(s, 32u - n);
        p->mq = rot;
        r = rot & (0xFFFFFFFFu >> n);
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 696: { // sriq
        uint32_t n = rb, rot = ppc_rotl(s, 32u - n);
        p->mq = rot;
        r = rot & (0xFFFFFFFFu >> n);
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 728: { // srlq (MQ not altered)
        uint32_t n = b & 31u, m = 0xFFFFFFFFu >> n;
        r = (b & 0x20u) ? (p->mq & m) : ((ppc_rotl(s, 32u - n) & m) | (p->mq & ~m));
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 729: { // sreq
        uint32_t n = b & 31u, rot = ppc_rotl(s, 32u - n), m = 0xFFFFFFFFu >> n;
        r = (rot & m) | (p->mq & ~m);
        p->mq = rot;
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 760: { // srliq
        uint32_t n = rb, rot = ppc_rotl(s, 32u - n), m = 0xFFFFFFFFu >> n;
        r = (rot & m) | (p->mq & ~m);
        p->mq = rot;
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 920: { // sraq
        uint32_t n = b & 31u, rot = ppc_rotl(s, 32u - n);
        uint32_t m = (b & 0x20u) ? 0u : (0xFFFFFFFFu >> n);
        uint32_t sign = (uint32_t)((int32_t)s >> 31);
        p->mq = rot;
        ppc_sra_mq_ca(p, rot, m, s);
        r = (rot & m) | (sign & ~m);
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 952: { // sraiq
        uint32_t n = rb, rot = ppc_rotl(s, 32u - n), m = 0xFFFFFFFFu >> n;
        uint32_t sign = (uint32_t)((int32_t)s >> 31);
        p->mq = rot;
        ppc_sra_mq_ca(p, rot, m, s);
        r = (rot & m) | (sign & ~m);
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 921: { // srea
        uint32_t n = b & 31u, rot = ppc_rotl(s, 32u - n), m = 0xFFFFFFFFu >> n;
        uint32_t sign = (uint32_t)((int32_t)s >> 31);
        p->mq = rot;
        ppc_sra_mq_ca(p, rot, m, s);
        r = (rot & m) | (sign & ~m);
        p->gpr[ra] = r;
        goto record_ra;
    }
    case 531: // clcs: line size for the valid rA codes is 64 (601UM page);
        r = 64; // undefined codes get the same deterministic value
        p->gpr[rt] = r;
        if (rc)
            ppc_record_cr0(p, r);
        return;

    // --- CR / MSR / SPR / SR moves ---
    case 19: // mfcr
        p->gpr[rt] = p->cr;
        return;
    case 144: { // mtcrf
        uint32_t crm = (iw >> 12) & 0xFFu, mask = 0;
        for (int i = 0; i < 8; i++)
            if (crm & (0x80u >> i))
                mask |= 0xFu << (28 - 4 * i);
        p->cr = (s & mask) | (p->cr & ~mask);
        return;
    }
    case 512: { // mcrxr
        ppc_set_cr_field(p, (iw >> 23) & 7, p->xer >> 28);
        p->xer &= 0x0FFFFFFFu;
        return;
    }
    case 339: // mfspr
        ppc_mfspr(p, iw);
        return;
    case 467: // mtspr
        ppc_mtspr(p, iw);
        return;
    case 83: // mfmsr
        if (ppc_priv_check(p))
            return;
        p->gpr[rt] = p->msr;
        return;
    case 146: // mtmsr
        if (ppc_priv_check(p))
            return;
        p->msr = s & PPC_MSR_MASK;
        ppc_update_active_maps(p);
        return;
    case 595: // mfsr
        if (ppc_priv_check(p))
            return;
        p->gpr[rt] = p->sr[(iw >> 16) & 0xFu];
        return;
    case 210: // mtsr
        if (ppc_priv_check(p))
            return;
        p->sr[(iw >> 16) & 0xFu] = s;
        return;
    case 659: // mfsrin
        if (ppc_priv_check(p))
            return;
        p->gpr[rt] = p->sr[b >> 28];
        return;
    case 242: // mtsrin
        if (ppc_priv_check(p))
            return;
        p->sr[b >> 28] = s;
        return;
    case 306: // tlbie — invalidation entry points arrive with the MMU front
        if (ppc_priv_check(p)) // end (Phase D); translation is off until then
            return;
        return;
    case 371: // mftb: NOT implemented on the 601 (RTC instead of timebase)
        ppc_illegal(p, iw);
        return;

    // --- storage control (no cache model; semantics per proposal §3.8) ---
    case 598: // sync
    case 854: // eieio
    case 982: // icbi
    case 278: // dcbt
    case 246: // dcbtst
    case 54: // dcbst
    case 86: // dcbf
        return;
    case 470: // dcbi is supervisor-only
        if (ppc_priv_check(p))
            return;
        return;
    case 1014: { // dcbz really zeroes the 32-byte block (§3.8)
        ea = ppc_ra0(p, iw) + b;
        ea &= ~31u;
        for (int i = 0; i < 8; i++)
            memory_write_uint32(ea + 4u * (uint32_t)i, 0);
        return;
    }

    // --- indexed loads/stores ---
    case 23: // lwzx
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 4))
            return;
        p->gpr[rt] = memory_read_uint32(ea);
        return;
    case 55: // lwzux
        ea = a + b;
        if (ppc_check_align_scalar(p, iw, ea, 4))
            return;
        p->gpr[ra] = ea;
        p->gpr[rt] = memory_read_uint32(ea);
        return;
    case 87: // lbzx
        p->gpr[rt] = memory_read_uint8(ppc_ra0(p, iw) + b);
        return;
    case 119: // lbzux
        ea = a + b;
        p->gpr[ra] = ea;
        p->gpr[rt] = memory_read_uint8(ea);
        return;
    case 279: // lhzx
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 2))
            return;
        p->gpr[rt] = memory_read_uint16(ea);
        return;
    case 311: // lhzux
        ea = a + b;
        if (ppc_check_align_scalar(p, iw, ea, 2))
            return;
        p->gpr[ra] = ea;
        p->gpr[rt] = memory_read_uint16(ea);
        return;
    case 343: // lhax
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 2))
            return;
        p->gpr[rt] = (uint32_t)(int32_t)(int16_t)memory_read_uint16(ea);
        return;
    case 375: // lhaux
        ea = a + b;
        if (ppc_check_align_scalar(p, iw, ea, 2))
            return;
        p->gpr[ra] = ea;
        p->gpr[rt] = (uint32_t)(int32_t)(int16_t)memory_read_uint16(ea);
        return;
    case 151: // stwx
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 4))
            return;
        memory_write_uint32(ea, s);
        return;
    case 183: // stwux
        ea = a + b;
        if (ppc_check_align_scalar(p, iw, ea, 4))
            return;
        p->gpr[ra] = ea;
        memory_write_uint32(ea, s);
        return;
    case 215: // stbx
        memory_write_uint8(ppc_ra0(p, iw) + b, (uint8_t)s);
        return;
    case 247: // stbux
        ea = a + b;
        p->gpr[ra] = ea;
        memory_write_uint8(ea, (uint8_t)s);
        return;
    case 407: // sthx
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 2))
            return;
        memory_write_uint16(ea, (uint16_t)s);
        return;
    case 439: // sthux
        ea = a + b;
        if (ppc_check_align_scalar(p, iw, ea, 2))
            return;
        p->gpr[ra] = ea;
        memory_write_uint16(ea, (uint16_t)s);
        return;

    // --- byte-reversed ---
    case 534: // lwbrx
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 4))
            return;
        p->gpr[rt] = __builtin_bswap32(memory_read_uint32(ea));
        return;
    case 790: // lhbrx
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 2))
            return;
        p->gpr[rt] = __builtin_bswap16(memory_read_uint16(ea));
        return;
    case 662: // stwbrx
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 4))
            return;
        memory_write_uint32(ea, __builtin_bswap32(s));
        return;
    case 918: // sthbrx
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 2))
            return;
        memory_write_uint16(ea, (uint16_t)__builtin_bswap16((uint16_t)s));
        return;

    // --- atomics ---
    case 20: // lwarx (word-aligned only → alignment exception)
        ea = ppc_ra0(p, iw) + b;
        if (ea & 3u) {
            ppc_align_exception(p, iw, ea);
            return;
        }
        p->reserve = 1;
        p->reserve_addr = ea;
        p->gpr[rt] = memory_read_uint32(ea);
        return;
    case 150: // stwcx.
        ea = ppc_ra0(p, iw) + b;
        if (ea & 3u) {
            ppc_align_exception(p, iw, ea);
            return;
        }
        if (p->reserve) {
            memory_write_uint32(ea, s);
            ppc_set_cr_field(p, 0, 2u | ((p->xer & PPC_XER_SO) ? 1u : 0u));
        } else {
            ppc_set_cr_field(p, 0, (p->xer & PPC_XER_SO) ? 1u : 0u);
        }
        p->reserve = 0;
        return;

    // --- external control (EAR-gated, 601UM eciwx/ecowx pages) ---
    case 310: // eciwx
        ea = ppc_ra0(p, iw) + b;
        if (!(p->ear & 0x80000000u)) {
            p->dar = ea;
            p->dsisr = 0x00100000u; // DSISR[11]: EAR[E]=0
            ppc_exception(p, PPC_VEC_DSI, 0, p->instruction_pc);
            return;
        }
        p->gpr[rt] = memory_read_uint32(ea);
        return;
    case 438: // ecowx
        ea = ppc_ra0(p, iw) + b;
        if (!(p->ear & 0x80000000u)) {
            p->dar = ea;
            p->dsisr = 0x00100000u | PPC_DSISR_STORE;
            ppc_exception(p, PPC_VEC_DSI, 0, p->instruction_pc);
            return;
        }
        memory_write_uint32(ea, s);
        return;

    // --- strings ---
    case 597: { // lswi
        uint32_t n = rb ? rb : 32u;
        ea = ppc_ra0(p, iw);
        if (ppc_check_align_string(p, iw, ea, n, false))
            return;
        ppc_load_string(p, ea, rt, n, ra ? (int)ra : -1, -1);
        return;
    }
    case 533: { // lswx
        uint32_t n = p->xer & PPC_XER_BYTES;
        ea = ppc_ra0(p, iw) + b;
        if (n == 0)
            return;
        if (ppc_check_align_string(p, iw, ea, n, false))
            return;
        ppc_load_string(p, ea, rt, n, ra ? (int)ra : -1, (int)rb);
        return;
    }
    case 725: { // stswi
        uint32_t n = rb ? rb : 32u;
        ea = ppc_ra0(p, iw);
        if (ppc_check_align_string(p, iw, ea, n, false))
            return;
        ppc_store_string(p, ea, rt, n);
        return;
    }
    case 661: { // stswx
        uint32_t n = p->xer & PPC_XER_BYTES;
        ea = ppc_ra0(p, iw) + b;
        if (n == 0)
            return;
        if (ppc_check_align_string(p, iw, ea, n, false))
            return;
        ppc_store_string(p, ea, rt, n);
        return;
    }
    case 277: { // lscbx: load string, stop at the XER compare byte
        uint32_t n = p->xer & PPC_XER_BYTES;
        uint32_t match_byte = (p->xer >> 8) & 0xFFu;
        ea = ppc_ra0(p, iw) + b;
        if (n == 0)
            return; // rD undefined; leave untouched (deterministic)
        if (ppc_check_align_string(p, iw, ea, n, true))
            return;
        uint32_t reg = rt, word = 0, loaded = 0;
        int shift = 24;
        bool matched = false;
        for (uint32_t i = 0; i < n && !matched; i++) {
            uint8_t byte = memory_read_uint8(ea + i);
            word |= (uint32_t)byte << shift;
            loaded++;
            matched = (byte == match_byte);
            shift -= 8;
            if (shift < 0 || matched || i + 1 == n) {
                bool skip = ((int)reg == (ra ? (int)ra : -1)) || (reg == rb);
                if (!skip)
                    p->gpr[reg] = word;
                reg = (reg + 1) & 31;
                word = 0;
                shift = 24;
            }
        }
        if (matched) // count of bytes loaded incl. the match (else unchanged)
            p->xer = (p->xer & ~PPC_XER_BYTES) | loaded;
        if (rc)
            ppc_set_cr_field(p, 0, (matched ? 2u : 0u) | ((p->xer & PPC_XER_SO) ? 1u : 0u));
        return;
    }

    // --- FP indexed loads/stores (FPR file lives; datapath in ppc_fpu.c) ---
    case 535: // lfsx
        if (ppc_fp_check(p))
            return;
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 4))
            return;
        p->fpr[rt] = ppc_f32_to_f64(memory_read_uint32(ea));
        return;
    case 567: // lfsux
        if (ppc_fp_check(p))
            return;
        ea = a + b;
        if (ppc_check_align_scalar(p, iw, ea, 4))
            return;
        p->gpr[ra] = ea;
        p->fpr[rt] = ppc_f32_to_f64(memory_read_uint32(ea));
        return;
    case 599: // lfdx
        if (ppc_fp_check(p))
            return;
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 8))
            return;
        p->fpr[rt] = ((uint64_t)memory_read_uint32(ea) << 32) | memory_read_uint32(ea + 4);
        return;
    case 631: // lfdux
        if (ppc_fp_check(p))
            return;
        ea = a + b;
        if (ppc_check_align_scalar(p, iw, ea, 8))
            return;
        p->gpr[ra] = ea;
        p->fpr[rt] = ((uint64_t)memory_read_uint32(ea) << 32) | memory_read_uint32(ea + 4);
        return;
    case 663: // stfsx
        if (ppc_fp_check(p))
            return;
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 4))
            return;
        memory_write_uint32(ea, ppc_f64_to_f32(p->fpr[rt]));
        return;
    case 695: // stfsux
        if (ppc_fp_check(p))
            return;
        ea = a + b;
        if (ppc_check_align_scalar(p, iw, ea, 4))
            return;
        p->gpr[ra] = ea;
        memory_write_uint32(ea, ppc_f64_to_f32(p->fpr[rt]));
        return;
    case 727: // stfdx
        if (ppc_fp_check(p))
            return;
        ea = ppc_ra0(p, iw) + b;
        if (ppc_check_align_scalar(p, iw, ea, 8))
            return;
        memory_write_uint32(ea, (uint32_t)(p->fpr[rt] >> 32));
        memory_write_uint32(ea + 4, (uint32_t)p->fpr[rt]);
        return;
    case 759: // stfdux
        if (ppc_fp_check(p))
            return;
        ea = a + b;
        if (ppc_check_align_scalar(p, iw, ea, 8))
            return;
        p->gpr[ra] = ea;
        memory_write_uint32(ea, (uint32_t)(p->fpr[rt] >> 32));
        memory_write_uint32(ea + 4, (uint32_t)p->fpr[rt]);
        return;

    default:
        ppc_illegal(p, iw);
        return;
    }

    // XO-form arithmetic falls through to here: write rD, record CR0 on Rc.
    p->gpr[rt] = r;
    if (rc)
        ppc_record_cr0(p, r);
    return;

record_ra: // X-form logical/shift bodies: result already in rA
    if (rc)
        ppc_record_cr0(p, r);
}

// === One instruction ========================================================

void ppc_execute(ppc_t *restrict p, uint32_t iw) {
    uint32_t rt = PPC_RT(iw), ra = PPC_RA(iw);
    uint32_t ea, r;

    switch (PPC_OPCD(iw)) {
    case 3: // twi
        if (ppc_trap_cond(rt, p->gpr[ra], (uint32_t)PPC_SIMM(iw)))
            ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_TRAP, p->instruction_pc);
        break;
    case 7: // mulli
        p->gpr[rt] = (uint32_t)((int64_t)(int32_t)p->gpr[ra] * PPC_SIMM(iw));
        break;
    case 8: { // subfic
        uint64_t wide = (uint64_t)(uint32_t)~p->gpr[ra] + (uint32_t)PPC_SIMM(iw) + 1u;
        ppc_set_ca(p, (int)(wide >> 32));
        p->gpr[rt] = (uint32_t)wide;
        break;
    }
    case 9: { // dozi (POWER)
        int32_t a = (int32_t)p->gpr[ra], simm = PPC_SIMM(iw);
        p->gpr[rt] = (a > simm) ? 0u : (uint32_t)simm - (uint32_t)a;
        break;
    }
    case 10: // cmpli
        ppc_cmp_unsigned(p, (iw >> 23) & 7, p->gpr[ra], PPC_UIMM(iw));
        break;
    case 11: // cmpi
        ppc_cmp_signed(p, (iw >> 23) & 7, (int32_t)p->gpr[ra], PPC_SIMM(iw));
        break;
    case 12: // addic
        p->gpr[rt] = ppc_add_body(p, p->gpr[ra], (uint32_t)PPC_SIMM(iw), 0, true, false);
        break;
    case 13: // addic.
        r = ppc_add_body(p, p->gpr[ra], (uint32_t)PPC_SIMM(iw), 0, true, false);
        p->gpr[rt] = r;
        ppc_record_cr0(p, r);
        break;
    case 14: // addi
        p->gpr[rt] = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        break;
    case 15: // addis
        p->gpr[rt] = (ra ? p->gpr[ra] : 0u) + (PPC_UIMM(iw) << 16);
        break;
    case 16: { // bc[l][a]
        bool taken = ppc_branch_taken(p, rt, ra, true);
        int32_t bd = (int32_t)(int16_t)(iw & 0xFFFCu);
        uint32_t target = (iw & 2u) ? (uint32_t)bd : p->instruction_pc + (uint32_t)bd;
        if (iw & 1u)
            p->lr = p->pc;
        if (taken)
            p->pc = target;
        break;
    }
    case 17: // sc (bit 30 distinguishes it; SRR0 = next instruction)
        if (iw & 2u)
            ppc_exception(p, PPC_VEC_SYSCALL, 0, p->pc);
        else
            ppc_illegal(p, iw);
        break;
    case 18: { // b[l][a]
        int32_t li = (int32_t)(iw << 6) >> 6; // sign-extend the 26-bit field
        li &= ~3;
        uint32_t target = (iw & 2u) ? (uint32_t)li : p->instruction_pc + (uint32_t)li;
        if (iw & 1u)
            p->lr = p->pc;
        p->pc = target;
        break;
    }
    case 19:
        ppc_op19(p, iw);
        break;
    case 20: { // rlwimi
        uint32_t m = ppc_mask(PPC_MB(iw), PPC_ME(iw));
        r = (ppc_rotl(p->gpr[rt], PPC_RB(iw)) & m) | (p->gpr[ra] & ~m);
        p->gpr[ra] = r;
        if (PPC_RC(iw))
            ppc_record_cr0(p, r);
        break;
    }
    case 21: { // rlwinm
        r = ppc_rotl(p->gpr[rt], PPC_RB(iw)) & ppc_mask(PPC_MB(iw), PPC_ME(iw));
        p->gpr[ra] = r;
        if (PPC_RC(iw))
            ppc_record_cr0(p, r);
        break;
    }
    case 22: { // rlmi (POWER)
        uint32_t m = ppc_mask(PPC_MB(iw), PPC_ME(iw));
        r = (ppc_rotl(p->gpr[rt], p->gpr[PPC_RB(iw)] & 31u) & m) | (p->gpr[ra] & ~m);
        p->gpr[ra] = r;
        if (PPC_RC(iw))
            ppc_record_cr0(p, r);
        break;
    }
    case 23: { // rlwnm
        r = ppc_rotl(p->gpr[rt], p->gpr[PPC_RB(iw)] & 31u) & ppc_mask(PPC_MB(iw), PPC_ME(iw));
        p->gpr[ra] = r;
        if (PPC_RC(iw))
            ppc_record_cr0(p, r);
        break;
    }
    case 24: // ori
        p->gpr[ra] = p->gpr[rt] | PPC_UIMM(iw);
        break;
    case 25: // oris
        p->gpr[ra] = p->gpr[rt] | (PPC_UIMM(iw) << 16);
        break;
    case 26: // xori
        p->gpr[ra] = p->gpr[rt] ^ PPC_UIMM(iw);
        break;
    case 27: // xoris
        p->gpr[ra] = p->gpr[rt] ^ (PPC_UIMM(iw) << 16);
        break;
    case 28: // andi.
        r = p->gpr[rt] & PPC_UIMM(iw);
        p->gpr[ra] = r;
        ppc_record_cr0(p, r);
        break;
    case 29: // andis.
        r = p->gpr[rt] & (PPC_UIMM(iw) << 16);
        p->gpr[ra] = r;
        ppc_record_cr0(p, r);
        break;
    case 31:
        ppc_op31(p, iw);
        break;

    // --- D-form loads/stores ---
    case 32: // lwz
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 4))
            break;
        p->gpr[rt] = memory_read_uint32(ea);
        break;
    case 33: // lwzu
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 4))
            break;
        p->gpr[ra] = ea;
        p->gpr[rt] = memory_read_uint32(ea);
        break;
    case 34: // lbz
        p->gpr[rt] = memory_read_uint8((ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw));
        break;
    case 35: // lbzu
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        p->gpr[ra] = ea;
        p->gpr[rt] = memory_read_uint8(ea);
        break;
    case 36: // stw
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 4))
            break;
        memory_write_uint32(ea, p->gpr[rt]);
        break;
    case 37: // stwu
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 4))
            break;
        p->gpr[ra] = ea;
        memory_write_uint32(ea, p->gpr[rt]);
        break;
    case 38: // stb
        memory_write_uint8((ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw), (uint8_t)p->gpr[rt]);
        break;
    case 39: // stbu
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        p->gpr[ra] = ea;
        memory_write_uint8(ea, (uint8_t)p->gpr[rt]);
        break;
    case 40: // lhz
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 2))
            break;
        p->gpr[rt] = memory_read_uint16(ea);
        break;
    case 41: // lhzu
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 2))
            break;
        p->gpr[ra] = ea;
        p->gpr[rt] = memory_read_uint16(ea);
        break;
    case 42: // lha
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 2))
            break;
        p->gpr[rt] = (uint32_t)(int32_t)(int16_t)memory_read_uint16(ea);
        break;
    case 43: // lhau
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 2))
            break;
        p->gpr[ra] = ea;
        p->gpr[rt] = (uint32_t)(int32_t)(int16_t)memory_read_uint16(ea);
        break;
    case 44: // sth
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 2))
            break;
        memory_write_uint16(ea, (uint16_t)p->gpr[rt]);
        break;
    case 45: // sthu
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 2))
            break;
        p->gpr[ra] = ea;
        memory_write_uint16(ea, (uint16_t)p->gpr[rt]);
        break;
    case 46: { // lmw
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_string(p, iw, ea, 4u * (32u - rt), false))
            break;
        for (uint32_t reg = rt; reg < 32; reg++, ea += 4)
            if (reg != ra || ra == 0) // rA in range is skipped (kept as base)
                p->gpr[reg] = memory_read_uint32(ea);
        break;
    }
    case 47: { // stmw
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_string(p, iw, ea, 4u * (32u - rt), false))
            break;
        for (uint32_t reg = rt; reg < 32; reg++, ea += 4)
            memory_write_uint32(ea, p->gpr[reg]);
        break;
    }

    // --- D-form FP loads/stores ---
    case 48: // lfs
        if (ppc_fp_check(p))
            break;
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 4))
            break;
        p->fpr[rt] = ppc_f32_to_f64(memory_read_uint32(ea));
        break;
    case 49: // lfsu
        if (ppc_fp_check(p))
            break;
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 4))
            break;
        p->gpr[ra] = ea;
        p->fpr[rt] = ppc_f32_to_f64(memory_read_uint32(ea));
        break;
    case 50: // lfd
        if (ppc_fp_check(p))
            break;
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 8))
            break;
        p->fpr[rt] = ((uint64_t)memory_read_uint32(ea) << 32) | memory_read_uint32(ea + 4);
        break;
    case 51: // lfdu
        if (ppc_fp_check(p))
            break;
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 8))
            break;
        p->gpr[ra] = ea;
        p->fpr[rt] = ((uint64_t)memory_read_uint32(ea) << 32) | memory_read_uint32(ea + 4);
        break;
    case 52: // stfs
        if (ppc_fp_check(p))
            break;
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 4))
            break;
        memory_write_uint32(ea, ppc_f64_to_f32(p->fpr[rt]));
        break;
    case 53: // stfsu
        if (ppc_fp_check(p))
            break;
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 4))
            break;
        p->gpr[ra] = ea;
        memory_write_uint32(ea, ppc_f64_to_f32(p->fpr[rt]));
        break;
    case 54: // stfd
        if (ppc_fp_check(p))
            break;
        ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 8))
            break;
        memory_write_uint32(ea, (uint32_t)(p->fpr[rt] >> 32));
        memory_write_uint32(ea + 4, (uint32_t)p->fpr[rt]);
        break;
    case 55: // stfdu
        if (ppc_fp_check(p))
            break;
        ea = p->gpr[ra] + (uint32_t)PPC_SIMM(iw);
        if (ppc_check_align_scalar(p, iw, ea, 8))
            break;
        p->gpr[ra] = ea;
        memory_write_uint32(ea, (uint32_t)(p->fpr[rt] >> 32));
        memory_write_uint32(ea + 4, (uint32_t)p->fpr[rt]);
        break;

    case 59: // FP single group
        if (ppc_fp_check(p))
            break;
        ppc_fpu_op59(p, iw);
        break;
    case 63: // FP double group
        if (ppc_fp_check(p))
            break;
        ppc_fpu_op63(p, iw);
        break;

    default:
        ppc_illegal(p, iw);
        break;
    }
}

// === The sprint loop (main-CPU seam ABI) ====================================

void ppc_run(ppc_t *restrict p, uint32_t *instructions) {
    // A memory-layer fault zeroes the burndown through this pointer so the
    // sprint exits and the epilogue delivers the machine check (the 68030
    // decoder precedent in cpu_68000.c/cpu_68030.c).
    g_bus_error_instr_ptr = instructions;
    ppc_poll_interrupt(p);
    while (*instructions > 0) {
        // Level-sensitive interrupt inputs re-checked at every boundary —
        // this is what makes the "loop until all flags clear" dispatch and
        // post-rfi redelivery work (proposal §4.6).
        if ((p->ext_irq | p->dec_pending) && (p->msr & PPC_MSR_EE))
            ppc_poll_interrupt(p);
        p->instruction_pc = p->pc;
        uint32_t iw = memory_read_uint32(p->pc);
        if (__builtin_expect(g_bus_error_pending, 0))
            break; // fetch faulted; delivered below
        p->pc += 4;
        if (*instructions > 0) // saturating (I/O penalty may have zeroed it)
            (*instructions)--;
        ppc_execute(p, iw);
    }
    // Deferred data/fetch fault → machine check (601UM §5.4.2: the TEA
    // path; the PDM family's AMIC/BART bus errors arrive this way).
    if (__builtin_expect(g_bus_error_pending, 0)) {
        g_bus_error_pending = false;
        p->dar = g_bus_error_address;
        ppc_exception(p, PPC_VEC_MCHECK, 0, p->instruction_pc);
    }
    *instructions = 0;
}
