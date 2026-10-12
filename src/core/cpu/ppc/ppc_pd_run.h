// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_pd_run.h
// The PowerPC predecoded sprint loop: the same page-keyed
// block pool as the 68K, one entry per instruction word, specialized (T0)
// handlers for the register-only shapes, and the flattened decode tree
// (T1) for everything else with the raw word in the entry.  The fetch
// window (g_ppc_fetch) still decides where the PC executes from; a block
// hangs off the window's page.  Included by ppc_run.c after its executor
// and helpers, so the T1 cases run ppc_ops.h's bodies and the branch
// handlers use the same folding helpers as the switch loop.

#include "ppc_pd_ids.h"
#include "predecode.h"

// Forward: the classifier is instantiated after the loop.
static uint16_t PPC_PD_CLASSIFY_NAME(uint32_t iw, uint32_t ipc, uint32_t page_lo, pd_entry_t *e);

// Decode one entry at first execution.
static __attribute__((noinline)) void ppc_pd_decode(pd_block_t *blk, uint32_t idx, uint32_t page_lo) {
    uint32_t iw = LOAD_BE32(blk->host + (idx << 2));
    pd_entry_t e;
    e.id = PPC_PD_CLASSIFY_NAME(iw, page_lo + (idx << 2), page_lo, &e);
    blk->raw32[idx] = iw;
    blk->e[idx] = e;
    predecode_count_decode(PD_ARCH_PPC, e.id);
}

// Register access by byte offset.
#define PPD_R(off) (*(uint32_t *)((uint8_t *)p + (off)))

// Sequential advance / in-page branch / re-derive from p->pc, all through
// the retire tail (fold rule and budget) the switch loop applies.
#define PPD_NEXT()                                                                                                     \
    do {                                                                                                               \
        cur++;                                                                                                         \
        goto retire;                                                                                                   \
    } while (0)
// The same for an entry that is not a branch: nothing it does can fold, so
// it retires through retire_plain (no fold test).
#define PPD_ADVANCE()                                                                                                  \
    do {                                                                                                               \
        cur++;                                                                                                         \
        goto retire_plain;                                                                                             \
    } while (0)
#define PPD_JUMP_IN(index, fold_)                                                                                      \
    do {                                                                                                               \
        p->fold = (fold_);                                                                                             \
        cur = blk->e + (index);                                                                                        \
        goto retire;                                                                                                   \
    } while (0)
#define PPD_JUMP_PC(target)                                                                                            \
    do {                                                                                                               \
        p->fold = 1; /* out of the page: never the instruction itself */                                               \
        p->pc = (target);                                                                                              \
        goto retire_relookup;                                                                                          \
    } while (0)

// Result writeback with the optional CR0 record (the RECT/RECA shape).
#define PPD_REC(off, v, rc)                                                                                            \
    do {                                                                                                               \
        uint32_t _v = (v);                                                                                             \
        PPD_R(off) = _v;                                                                                               \
        if (rc)                                                                                                        \
            ppc_record_cr0(p, _v);                                                                                     \
    } while (0)

// One T0 case: audit, body, advance.  RC is 0/1 for the record twin.
#define PPD_CASE(ID, BODY)                                                                                             \
    case ID: {                                                                                                         \
        PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));                                                                   \
        BODY;                                                                                                          \
        PPD_ADVANCE();                                                                                                 \
    }
#define PPD_PAIR(ID, BODY)                                                                                             \
    PPD_CASE(ID, BODY(0))                                                                                              \
    PPD_CASE(ID##_RC, BODY(1))

// --- bodies (ppc_ops.h semantics, operands by entry field) ---
#define PPD_B_ADD(rc)    PPD_REC(e.a, ppc_add_body(p, PPD_R(e.b), PPD_R(e.c), 0, false, false), rc)
#define PPD_B_SUBF(rc)   PPD_REC(e.a, ppc_subf_body(p, PPD_R(e.b), PPD_R(e.c), 1, false, false), rc)
#define PPD_B_MULLW(rc)  PPD_REC(e.a, (uint32_t)((int64_t)(int32_t)PPD_R(e.b) * (int32_t)PPD_R(e.c)), rc)
#define PPD_B_NEG(rc)    PPD_REC(e.a, 0u - PPD_R(e.b), rc)
#define PPD_B_AND(rc)    PPD_REC(e.a, PPD_R(e.b) & PPD_R(e.c), rc)
#define PPD_B_ANDC(rc)   PPD_REC(e.a, PPD_R(e.b) & ~PPD_R(e.c), rc)
#define PPD_B_OR(rc)     PPD_REC(e.a, PPD_R(e.b) | PPD_R(e.c), rc)
#define PPD_B_NOR(rc)    PPD_REC(e.a, ~(PPD_R(e.b) | PPD_R(e.c)), rc)
#define PPD_B_XOR(rc)    PPD_REC(e.a, PPD_R(e.b) ^ PPD_R(e.c), rc)
#define PPD_B_SLW(rc)    PPD_REC(e.a, (PPD_R(e.c) & 0x20u) ? 0 : (PPD_R(e.b) << (PPD_R(e.c) & 31u)), rc)
#define PPD_B_SRW(rc)    PPD_REC(e.a, (PPD_R(e.c) & 0x20u) ? 0 : (PPD_R(e.b) >> (PPD_R(e.c) & 31u)), rc)
#define PPD_B_EXTSB(rc)  PPD_REC(e.a, (uint32_t)(int32_t)(int8_t)PPD_R(e.b), rc)
#define PPD_B_EXTSH(rc)  PPD_REC(e.a, (uint32_t)(int32_t)(int16_t)PPD_R(e.b), rc)
#define PPD_B_CNTLZW(rc) PPD_REC(e.a, PPD_R(e.b) ? (uint32_t)__builtin_clz(PPD_R(e.b)) : 32u, rc)
#define PPD_B_SRAWI(rc)                                                                                                \
    uint32_t n_ = e.c, s_ = PPD_R(e.b);                                                                                \
    ppc_set_ca(p, ((int32_t)s_ < 0) && n_ != 0 && (s_ & ((1u << n_) - 1u)) != 0);                                      \
    PPD_REC(e.a, (uint32_t)((int32_t)s_ >> n_), rc)
// The rotates re-derive their fields from the raw word (§6.2).
#define PPD_B_RLWINM(rc)                                                                                               \
    uint32_t iw = e.c;                                                                                                 \
    PPD_REC(PPD_GOFF(PPC_RA(iw)), ppc_rotl(p->gpr[PPC_RT(iw)], PPC_RB(iw)) & ppc_mask(PPC_MB(iw), PPC_ME(iw)), rc)
#define PPD_B_RLWIMI(rc)                                                                                               \
    uint32_t iw = e.c;                                                                                                 \
    uint32_t m_ = ppc_mask(PPC_MB(iw), PPC_ME(iw));                                                                    \
    PPD_REC(PPD_GOFF(PPC_RA(iw)), (ppc_rotl(p->gpr[PPC_RT(iw)], PPC_RB(iw)) & m_) | (p->gpr[PPC_RA(iw)] & ~m_), rc)
#define PPD_B_RLWNM(rc)                                                                                                \
    uint32_t iw = e.c;                                                                                                 \
    PPD_REC(PPD_GOFF(PPC_RA(iw)),                                                                                      \
            ppc_rotl(p->gpr[PPC_RT(iw)], p->gpr[PPC_RB(iw)] & 31u) & ppc_mask(PPC_MB(iw), PPC_ME(iw)), rc)

// CR logical: a = bT, b = bA, c = bB.
#define PPD_CROP(ID, EXPR)                                                                                             \
    PPD_CASE(ID, uint32_t a = ppc_cr_bit(p, e.b); uint32_t b = ppc_cr_bit(p, e.c); (void)a; (void)b;                   \
             ppc_set_cr_bit(p, e.a, (EXPR) & 1u))

// T0 loads and stores: the access, then the cursor advance (or the
// relookup a fault or a flushed window needs).
#define PPD_LS_BEGIN(ID)                                                                                               \
    case ID: {                                                                                                         \
        PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));                                                                   \
        uint32_t ea = PPD_R(e.b) + e.c;                                                                                \
        uint32_t xa = ea;                                                                                              \
        uint32_t iw = blk->raw32[cur - blk->e];                                                                        \
        p->instruction_pc = ipc;                                                                                       \
        p->pc = ipc + 4;
#define PPD_LS_END()                                                                                                   \
    if (__builtin_expect(g_ppc_fetch.blk != blk || g_ppc_fetch.span != MEM_PAGE_SIZE, 0))                              \
        goto retire_relookup;                                                                                          \
    PPD_ADVANCE();                                                                                                     \
    }
#define PPD_LOAD(ID, SZ, BITS, EXT, UPD)                                                                               \
    PPD_LS_BEGIN(ID)                                                                                                   \
    uint64_t lv_ = 0;                                                                                                  \
    int lg_ = ppc_scalar_gate_inl(p, iw, &xa, SZ, false, &lv_);                                                        \
    if (lg_ < 0)                                                                                                       \
        goto retire_relookup;                                                                                          \
    uint32_t v_ = EXT(lg_ ? (uint##BITS##_t)lv_ : memory_read_uint##BITS(xa));                                         \
    if (UPD)                                                                                                           \
        PPD_R(e.b) = ea;                                                                                               \
    PPD_R(e.a) = v_;                                                                                                   \
    PPD_LS_END()
#define PPD_LOAD_BYTE(ID, UPD)                                                                                         \
    PPD_LS_BEGIN(ID)                                                                                                   \
    xa = ppc_le_ea(p, ea, 1);                                                                                          \
    if (ppc_dxlate(p, iw, &xa, false))                                                                                 \
        goto retire_relookup;                                                                                          \
    uint32_t v_ = memory_read_uint8(xa);                                                                               \
    if (UPD)                                                                                                           \
        PPD_R(e.b) = ea;                                                                                               \
    PPD_R(e.a) = v_;                                                                                                   \
    PPD_LS_END()
#define PPD_STORE(ID, SZ, BITS, UPD)                                                                                   \
    PPD_LS_BEGIN(ID)                                                                                                   \
    uint64_t sv_ = PPD_R(e.a);                                                                                         \
    int sg_ = ppc_scalar_gate_inl(p, iw, &xa, SZ, true, &sv_);                                                         \
    if (sg_ < 0)                                                                                                       \
        goto retire_relookup;                                                                                          \
    if (!sg_)                                                                                                          \
        memory_write_uint##BITS(xa, (uint##BITS##_t)sv_);                                                              \
    if (UPD)                                                                                                           \
        PPD_R(e.b) = ea; /* store before update: rS == rA stores the pre-update value */                               \
    PPD_LS_END()
#define PPD_STORE_BYTE(ID, UPD)                                                                                        \
    PPD_LS_BEGIN(ID)                                                                                                   \
    xa = ppc_le_ea(p, ea, 1);                                                                                          \
    if (ppc_dxlate(p, iw, &xa, true))                                                                                  \
        goto retire_relookup;                                                                                          \
    memory_write_uint8(xa, (uint8_t)PPD_R(e.a));                                                                       \
    if (UPD)                                                                                                           \
        PPD_R(e.b) = ea;                                                                                               \
    PPD_LS_END()

// The T1 case body: bind the word, materialize PC, run the leaf.
#define PD_T1_BODY(OPX)                                                                                                \
    do {                                                                                                               \
        uint32_t iw = e.c;                                                                                             \
        (void)iw;                                                                                                      \
        PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));                                                                   \
        p->instruction_pc = ipc;                                                                                       \
        p->pc = ipc + 4;                                                                                               \
        OPX;                                                                                                           \
    } while (0)

static void PPC_PD_RUN_NAME(ppc_t *restrict p, uint32_t *instructions) {
    g_bus_error_instr_ptr = instructions; // let memory slow paths force exit
    p->fold = 0;
    // The switch loop's bound on branches folded without a budget slot (two
    // branches folding to each other must not spin a sprint forever).
    uint64_t folds_left = (uint64_t)*instructions * 4u;
    ppc_poll_interrupt(p);
    pd_block_t *blk = NULL; // block of the page being executed (NULL: generic tier)
    pd_entry_t *cur = NULL; // the entry to dispatch next
    uint32_t page_lo = 1; // guest address of that page (odd: none yet)
    uint32_t ipc = p->instruction_pc; // address of the instruction being dispatched
    // ipc from the cursor in one shift and add: entries are 8 bytes and
    // instructions 4, so ipc = page_lo + (cur - blk->e) * 4
    // = ((uintptr_t)cur >> 1) + ipc_bias, the bias set with the block.
    _Static_assert(sizeof(pd_entry_t) == 8, "ipc_bias assumes 8-byte entries");
    uint32_t ipc_bias = 0;
    // The page the pool last declined (demoted and held, no region): while
    // the PC stays on it under the same window, every instruction goes
    // straight to the generic step instead of asking the pool again (the
    // 68K loop's pd_held).  Odd: none.
    uint32_t held_lo = 1;
    g_ppc_fetch.blk = NULL; // re-validate the window's block once per sprint (demotions, logpoints)
    goto relookup;

top:
    if (*instructions == 0)
        goto done;
dispatch:
    // Level-sensitive interrupt inputs re-checked at every boundary (as the
    // switch loop does); taking one redirects the PC.
    if (__builtin_expect((p->ext_irq | p->dec_pending) && (p->msr & PPC_MSR_EE), 0)) {
        p->pc = page_lo + ((uint32_t)(cur - blk->e) << 2);
        ppc_poll_interrupt(p);
        goto relookup;
    }
    ipc = (uint32_t)((uintptr_t)cur >> 1) + ipc_bias;
    {
        pd_entry_t e = *cur;
        uint16_t id = e.id;
    redispatch:
        switch (id) {
        case PD_UNDECODED:
            ppc_pd_decode(blk, (uint32_t)(cur - blk->e), page_lo);
            e = *cur;
            id = e.id;
            goto redispatch;

        case PD_CROSS:
            g_pd_stats.generic_cross++;
            p->pc = ipc;
            goto t2_step;
        case PD_GENERIC:
            g_pd_stats.generic_declined++;
            p->pc = ipc;
            goto t2_step;

        case PD_PAGE_END:
            // Fell off the page: no instruction ran; continue on the next page.
            p->pc = ipc;
            goto relookup;

            // ---- T0: register arithmetic and logic ----
            PPD_PAIR(PPD_ADD, PPD_B_ADD)
            PPD_PAIR(PPD_SUBF, PPD_B_SUBF)
            PPD_PAIR(PPD_MULLW, PPD_B_MULLW)
            PPD_PAIR(PPD_NEG, PPD_B_NEG)
            PPD_PAIR(PPD_AND, PPD_B_AND)
            PPD_PAIR(PPD_ANDC, PPD_B_ANDC)
            PPD_PAIR(PPD_OR, PPD_B_OR)
            PPD_PAIR(PPD_NOR, PPD_B_NOR)
            PPD_PAIR(PPD_XOR, PPD_B_XOR)
            PPD_PAIR(PPD_SLW, PPD_B_SLW)
            PPD_PAIR(PPD_SRW, PPD_B_SRW)
            PPD_PAIR(PPD_EXTSB, PPD_B_EXTSB)
            PPD_PAIR(PPD_EXTSH, PPD_B_EXTSH)
            PPD_PAIR(PPD_CNTLZW, PPD_B_CNTLZW)
            PPD_PAIR(PPD_SRAWI, PPD_B_SRAWI)
            PPD_PAIR(PPD_RLWINM, PPD_B_RLWINM)
            PPD_PAIR(PPD_RLWIMI, PPD_B_RLWIMI)
            PPD_PAIR(PPD_RLWNM, PPD_B_RLWNM)

            // ---- T0: immediates ----
            PPD_CASE(PPD_ADDI, PPD_R(e.a) = PPD_R(e.b) + e.c)
            PPD_CASE(PPD_LI, PPD_R(e.a) = e.c)
            PPD_CASE(PPD_MULLI, PPD_R(e.a) = (uint32_t)((int64_t)(int32_t)PPD_R(e.b) * (int32_t)e.c))
            PPD_CASE(PPD_ORI, PPD_R(e.a) = PPD_R(e.b) | e.c)
            PPD_CASE(PPD_XORI, PPD_R(e.a) = PPD_R(e.b) ^ e.c)
            PPD_CASE(PPD_ANDI_RC, PPD_REC(e.a, PPD_R(e.b) & e.c, 1))
            PPD_CASE(PPD_ADDIC, PPD_R(e.a) = ppc_add_body(p, PPD_R(e.b), e.c, 0, true, false))
            PPD_CASE(PPD_ADDIC_RC, PPD_REC(e.a, ppc_add_body(p, PPD_R(e.b), e.c, 0, true, false), 1))
            PPD_CASE(PPD_SUBFIC, PPD_R(e.a) = ppc_subf_body(p, PPD_R(e.b), e.c, 1, true, false))

            // ---- T0: compares ----
            PPD_CASE(PPD_CMPWI, ppc_cmp_signed(p, e.a, (int32_t)PPD_R(e.b), (int32_t)e.c))
            PPD_CASE(PPD_CMPLWI, ppc_cmp_unsigned(p, e.a, PPD_R(e.b), e.c))
            PPD_CASE(PPD_CMPW, ppc_cmp_signed(p, e.a, (int32_t)PPD_R(e.b), (int32_t)PPD_R(e.c)))
            PPD_CASE(PPD_CMPLW, ppc_cmp_unsigned(p, e.a, PPD_R(e.b), PPD_R(e.c)))

            // ---- T0: SPR / CR moves ----
            PPD_CASE(PPD_MFLR, PPD_R(e.a) = p->lr)
            PPD_CASE(PPD_MTLR, p->lr = PPD_R(e.a))
            PPD_CASE(PPD_MFCTR, PPD_R(e.a) = p->ctr)
            PPD_CASE(PPD_MTCTR, p->ctr = PPD_R(e.a))
            PPD_CASE(PPD_MFXER, PPD_R(e.a) = p->xer)
            // The 601's XER reserved bits (3-15, 24) read as zero (MPC601UM
            // 2.2.5.2); the 604 keeps what was written -- as ppc_mtspr.
            PPD_CASE(PPD_MTXER, p->xer = ppc_is_604(p) ? PPD_R(e.a)
                                                       : (PPD_R(e.a) & (PPC_XER_SO | PPC_XER_OV | PPC_XER_CA |
                                                                        PPC_XER_CMPBYTE | PPC_XER_BYTES)))
            PPD_CASE(PPD_MFCR, PPD_R(e.a) = p->cr)
            PPD_CASE(PPD_MTCRF, p->cr = (PPD_R(e.a) & e.c) | (p->cr & ~e.c))
            PPD_CROP(PPD_CRAND, a & b)
            PPD_CROP(PPD_CRANDC, a & ~b)
            PPD_CROP(PPD_CROR, a | b)
            PPD_CROP(PPD_CRORC, a | ~b)
            PPD_CROP(PPD_CRXOR, a ^ b)
            PPD_CROP(PPD_CREQV, ~(a ^ b))
            PPD_CROP(PPD_CRNAND, ~(a & b))
            PPD_CROP(PPD_CRNOR, ~(a | b))
            PPD_CASE(PPD_MCRF, ppc_set_cr_field(p, e.a, ppc_get_cr_field(p, e.b)))
            PPD_CASE(PPD_NOP, (void)0)
            PPD_CASE(PPD_ISYNC, ppc_context_sync(p))

            // ---- T0: D-form loads and stores (rA != 0) ----
            // The T1 bodies (CHKA gate, XLT for bytes, update order) with the
            // fields pre-extracted.  instruction_pc and pc are set as T1 does,
            // for the exceptions and slow paths the access may take; the raw
            // word (the DSISR and alignment images need it) comes from the
            // shadow.  A raised exception moved the PC: retire_relookup, as
            // T1.  An access with side effects (a device store remapping
            // memory) may have flushed the fetch window: then relookup too.
            PPD_LOAD(PPD_LWZ, 4, 32, (uint32_t), 0)
            PPD_LOAD(PPD_LWZU, 4, 32, (uint32_t), 1)
            PPD_LOAD(PPD_LHZ, 2, 16, (uint32_t), 0)
            PPD_LOAD(PPD_LHZU, 2, 16, (uint32_t), 1)
            PPD_LOAD(PPD_LHA, 2, 16, (uint32_t)(int32_t)(int16_t), 0)
            PPD_LOAD(PPD_LHAU, 2, 16, (uint32_t)(int32_t)(int16_t), 1)
            PPD_LOAD_BYTE(PPD_LBZ, 0)
            PPD_LOAD_BYTE(PPD_LBZU, 1)
            PPD_STORE(PPD_STW, 4, 32, 0)
            PPD_STORE(PPD_STWU, 4, 32, 1)
            PPD_STORE(PPD_STH, 2, 16, 0)
            PPD_STORE(PPD_STHU, 2, 16, 1)
            PPD_STORE_BYTE(PPD_STB, 0)
            PPD_STORE_BYTE(PPD_STBU, 1)

            // ---- T0: branches (the 601 fold rule: a taken branch to itself burns a slot) ----
        case PPD_B_IN: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            PPD_JUMP_IN(e.c, e.c != (uint32_t)(cur - blk->e));
        }
        case PPD_B_OUT: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            PPD_JUMP_PC(e.c);
        }
        case PPD_BL_IN: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            p->lr = ipc + 4;
            PPD_JUMP_IN(e.c, e.c != (uint32_t)(cur - blk->e));
        }
        case PPD_BL_OUT: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            p->lr = ipc + 4;
            PPD_JUMP_PC(e.c);
        }
        case PPD_BDNZ_IN: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            p->ctr -= 1;
            if (p->ctr != 0)
                PPD_JUMP_IN(e.c, e.c != (uint32_t)(cur - blk->e));
            p->fold = 1; // not taken: falls through, still folds
            PPD_NEXT();
        }
        case PPD_BDNZ_OUT: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            p->ctr -= 1;
            if (p->ctr != 0)
                PPD_JUMP_PC(e.c);
            p->fold = 1;
            PPD_NEXT();
        }
        case PPD_BC_T_IN: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            if ((p->cr >> e.a) & 1u)
                PPD_JUMP_IN(e.c, e.c != (uint32_t)(cur - blk->e));
            p->fold = 1;
            PPD_NEXT();
        }
        case PPD_BC_T_OUT: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            if ((p->cr >> e.a) & 1u)
                PPD_JUMP_PC(e.c);
            p->fold = 1;
            PPD_NEXT();
        }
        case PPD_BC_F_IN: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            if (!((p->cr >> e.a) & 1u))
                PPD_JUMP_IN(e.c, e.c != (uint32_t)(cur - blk->e));
            p->fold = 1;
            PPD_NEXT();
        }
        case PPD_BC_F_OUT: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            if (!((p->cr >> e.a) & 1u))
                PPD_JUMP_PC(e.c);
            p->fold = 1;
            PPD_NEXT();
        }
        case PPD_BLR: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            uint32_t target = p->lr & ~3u;
            p->fold = (target != ipc);
            p->pc = target;
            goto retire_relookup;
        }
        case PPD_BCTR: {
            PD_AUDIT_PPC(blk, (uint32_t)(cur - blk->e));
            uint32_t target = p->ctr & ~3u;
            p->fold = (target != ipc);
            p->pc = target;
            goto retire_relookup;
        }

        // ---- T1: every leaf of the decode tree, flattened ----
#include "ppc_pd_t1_cases.h"

        default:
            p->pc = ipc;
            goto t2_step;
        }
    }
    // T1 leaves: the body set p->pc (sequentially or by branching).
    goto retire_relookup;

retire_plain:
    // A non-branch entry: one read of the budget for the saturating
    // decrement and the zero test, then straight past top's own test.
    {
        uint32_t n_ = *instructions;
        if (__builtin_expect(n_ > 1, 1)) {
            *instructions = n_ - 1;
            goto dispatch;
        }
        *instructions = 0;
        goto done;
    }

retire:
    // The switch loop's tail: a folded branch keeps its slot unless it is
    // the last one; the budget decrement saturates (an I/O penalty may have
    // zeroed it).
    if (__builtin_expect(p->fold != 0, 0)) {
        p->fold = 0;
        if (*instructions > 1 && folds_left != 0) {
            folds_left--;
            goto top;
        }
    }
    if (*instructions > 0)
        (*instructions)--;
    goto top;

retire_relookup:
    if (__builtin_expect(p->fold != 0, 0)) {
        p->fold = 0;
        if (*instructions > 1 && folds_left != 0) {
            folds_left--;
            goto relookup;
        }
    }
    if (*instructions > 0)
        (*instructions)--;
    goto relookup;

t2_step:
    // Generic tier: the switch loop's iteration, verbatim.
    g_pd_stats.generic_steps++;
    {
        p->instruction_pc = p->pc;
        ipc = p->pc;
        uint32_t iw;
        if (!ppc_fetch(p, &iw))
            goto relookup; // ISI (pc at the vector), or a fetch fault (burndown zeroed)
        p->pc += 4;
        ppc_execute(p, iw);
    }
    goto retire_relookup;

relookup:
    // The only place a block pointer is derived from an address: the
    // fetch window says where the PC executes from; the pool says
    // whether that page is decoded.
    {
        uint32_t pc = p->pc;
        // Same logical page: reuse the block only while the fetch window
        // still stands behind it.  An rfi or mtmsr that changes IR/PR, or a
        // TLB invalidation, flushes the window (ppc_mmu_flush_fetch) because
        // the same logical page may now fetch from another physical page —
        // the switch loop re-resolves on every fetch; this is its equivalent.
        if (blk && (pc - page_lo) < MEM_PAGE_SIZE && !(pc & 3u) && g_ppc_fetch.span == MEM_PAGE_SIZE &&
            g_ppc_fetch.lo == page_lo && g_ppc_fetch.blk == blk) {
            cur = blk->e + ((pc - page_lo) >> 2);
            goto top;
        }
        if (!blk && (pc - held_lo) < MEM_PAGE_SIZE && g_ppc_fetch.lo == held_lo && g_ppc_fetch.span == MEM_PAGE_SIZE) {
            // Still on the declined page: generic, with the boundary's
            // budget and interrupt checks.
            if (*instructions == 0)
                goto done;
            if ((p->ext_irq | p->dec_pending) && (p->msr & PPC_MSR_EE))
                ppc_poll_interrupt(p);
            goto t2_step;
        }
        // Leaving the block: from here p->pc is authoritative (the exit
        // materializes from the cursor only while a block is current).
        blk = NULL;
        page_lo = pc & ~(uint32_t)MEM_PAGE_MASK;
        predecode_enter(NULL, *instructions); // charge the page being left
        if (*instructions == 0)
            goto done;
        // Little-endian mode munges every fetch (XOR 4) and is rare: it runs
        // on the generic tier, whose fetch applies the munge.  Setting MSR[LE]
        // flushes the fetch window, so a block never outlives the switch.
        if (!(pc & 3u) && !(p->msr & PPC_MSR_LE)) {
            bool refilled = false;
            if ((pc - g_ppc_fetch.lo) >= g_ppc_fetch.span) {
                // The window belongs to the page being left: refill it here
                // rather than through a generic step — the transition is the
                // common case.  A page seen before refills inline from the
                // fetch TLB, which also hands over the page's last block.
                g_pd_stats.relookup_nomap++;
                refilled = true;
                if (!ppc_fetch_window_hit(p->msr, pc)) {
                    uint32_t iw;
                    p->instruction_pc = pc;
                    if (!ppc_fetch_fill(p, pc, &iw)) {
                        // ISI (pc now at the vector), or a fetch fault: the burndown
                        // was zeroed, and the next relookup reaches the epilogue.
                        ipc = pc;
                        goto relookup;
                    }
                }
            }
            if ((pc - g_ppc_fetch.lo) < g_ppc_fetch.span && g_ppc_fetch.span == MEM_PAGE_SIZE) {
                // The window's block (kept in the fetch TLB across sprints)
                // may have been recycled, demoted or reset since: trust it
                // only while predecode_block_ok holds.  Taking it on a
                // refill ticks the lookup clock as the lookup it replaces.
                uint8_t *host_page = (uint8_t *)(g_ppc_fetch.host_adjust + g_ppc_fetch.lo);
                if (!predecode_block_ok(g_ppc_fetch.blk, host_page, g_ppc_fetch.lo)) {
                    g_ppc_fetch.blk = predecode_find(host_page, g_ppc_fetch.lo, PD_ARCH_PPC);
                    if (g_ppc_fetch.fe)
                        g_ppc_fetch.fe->blk = g_ppc_fetch.blk;
                } else if (refilled) {
                    g_pd_stats.lookups++;
                }
                blk = g_ppc_fetch.blk;
                page_lo = g_ppc_fetch.lo;
                if (blk) {
                    predecode_enter(blk, *instructions);
                    ipc_bias = page_lo - (uint32_t)((uintptr_t)blk->e >> 1);
                    cur = blk->e + ((pc - page_lo) >> 2);
                    held_lo = 1;
                    goto top;
                }
                g_pd_stats.relookup_nopool++;
                held_lo = g_ppc_fetch.lo;
            }
        }
        // No window for this page yet, a device window, a logpointed or
        // demoted page, or an unaligned PC: one generic instruction (whose
        // fetch fills the window), then look again.
    }
    if ((p->ext_irq | p->dec_pending) && (p->msr & PPC_MSR_EE))
        ppc_poll_interrupt(p);
    goto t2_step;

done:
    if (blk)
        p->pc = page_lo + ((uint32_t)(cur - blk->e) << 2);
    p->instruction_pc = ipc;
    predecode_enter(NULL, *instructions);
    // Deferred data/fetch fault → machine check (the switch loop's epilogue).
    if (__builtin_expect(g_bus_error_pending, 0)) {
        g_bus_error_pending = false;
        p->dar = g_bus_error_address;
        LOG(3, "machine check: addr $%08X pc $%08X r24 $%08X", g_bus_error_address, p->instruction_pc, p->gpr[24]);
        if (ppc_is_604(p)) {
            ppc_exception(p, PPC_VEC_MCHECK, 0x00040000u, p->instruction_pc);
            p->srr1 &= ~PPC_MSR_RI;
            p->msr &= ~PPC_MSR_ME;
        } else {
            ppc_exception(p, PPC_VEC_MCHECK, 0, p->instruction_pc);
        }
    }
    *instructions = 0;
}

// The classifier (rebinds every OP_ name: nothing below may execute ops).
#include "ppc_pd_classify.h"
