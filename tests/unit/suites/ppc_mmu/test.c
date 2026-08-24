// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// test.c — directed unit tests for the 601 MMU front end
// (src/core/cpu/ppc/ppc_mmu.c), proposal-powerpc-601-pdm.md Phase D.
//
// Written from the MPC601 User's Manual Chapter 6 (translation) and the
// Chapter 5 DSI/ISI register-settings tables; the PTEG addresses are
// computed here independently from Figure 6-19 so an arithmetic slip in
// the implementation cannot silently agree with itself.  The §3.4 proof
// list covered: 601-format BATs with key/PP protection, T=1 memory-forced
// segments (SR5-toggle aliasing and the DT-off data path), primary and
// secondary hashed-table search with R/C write-back, exact DSISR/DAR/SRR1
// images, the (PR,DT)-keyed SoA discipline, tlbie congruence-class
// invalidation, mtsr change-triggered invalidation, and dcbz's W/I
// alignment rule.

#include "ppc_internal.h"

#include "harness.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(got, want)                                                                                            \
    do {                                                                                                               \
        checks++;                                                                                                      \
        uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);                                                          \
        if (g_ != w_) {                                                                                                \
            printf("FAIL %s:%d: %s = $%08X, want $%08X\n", __FILE__, __LINE__, #got, g_, w_);                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static ppc_t *P;

// === Encoders (601UM chapter-10 field layouts) ===
static uint32_t e_d(uint32_t op, uint32_t rt, uint32_t ra, uint32_t imm16) {
    return (op << 26) | (rt << 21) | (ra << 16) | (imm16 & 0xFFFFu);
}
static uint32_t e_x(uint32_t rt, uint32_t ra, uint32_t rb, uint32_t xo, uint32_t rc) {
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
}

// Execute exactly n instructions starting at addr (supervisor fetch is
// untranslated in every test — MSR[IT] stays 0 unless a test sets it).
static void run_at(uint32_t addr, int n) {
    P->pc = addr;
    uint32_t budget = (uint32_t)n;
    ppc_run(P, &budget);
}

// Reset to a clean supervisor state with EP cleared so exception vectors
// land at $00000xxx (mapped RAM), translation off.
static void fresh(void) {
    ppc_reset(P);
    P->msr = PPC_MSR_ME | PPC_MSR_FP;
    ppc_update_active_maps(P);
}

// === HTAB plumbing (Figure 6-19, computed independently) ====================

#define HTABORG  0x00100000u // 128 KB table at 1 MB (inside the 8 MB RAM)
#define SDR1_VAL (HTABORG | 0x001u) // HTABMASK = 1

// Physical address of the PTE slot for (vsid, ea) in the primary (h=0) or
// secondary (h=1) PTEG.
static uint32_t pteg_addr(uint32_t vsid, uint32_t ea, int h) {
    uint32_t hash = ((vsid & 0x7FFFFu) ^ ((ea >> 12) & 0xFFFFu));
    if (h)
        hash = ~hash & 0x7FFFFu;
    return HTABORG | ((((hash >> 10) & 0x1FFu) & 0x001u) << 16) | ((hash & 0x3FFu) << 6);
}

// Install a PTE for ea→pa in slot `slot` of the h-selected PTEG.
// wimg/pp go into word 1; R/C start clear.
static uint32_t put_pte(uint32_t vsid, uint32_t ea, uint32_t pa, int h, int slot, uint32_t wimg, uint32_t pp) {
    uint32_t addr = pteg_addr(vsid, ea, h) + (uint32_t)slot * 8u;
    uint32_t w0 = 0x80000000u | ((vsid & 0xFFFFFFu) << 7) | (h ? 0x40u : 0u) | ((ea >> 22) & 0x3Fu);
    uint32_t w1 = (pa & 0xFFFFF000u) | ((wimg & 0xFu) << 4) | (pp & 3u);
    memory_write_uint32(addr, w0);
    memory_write_uint32(addr + 4, w1);
    return addr;
}

static void wipe_htab(void) {
    for (uint32_t a = HTABORG; a < HTABORG + 0x20000u; a += 4)
        memory_write_uint32(a, 0);
}

// Enter supervisor translated-data mode with SDR1 loaded and all-zero T=0
// segments (VSID 0, Ks=0, Ku=1... note sr=0 means Ku=0 too: key is 0 in
// both modes unless a test sets the bits).
static void enter_dt(void) {
    P->sdr1 = SDR1_VAL;
    P->msr |= PPC_MSR_DT;
    ppc_update_active_maps(P);
    ppc_mmu_invalidate_all(P); // sdr1 was poked directly
}

// === Tests ==================================================================

// Primary-hash walk, R/C write-back, and the C-gated write fill.
static void test_htab_walk_rc(void) {
    fresh();
    wipe_htab();
    uint32_t pte = put_pte(0, 0x00200000u, 0x00300000u, 0, 0, 0, 2); // PP=10 rw
    memory_write_uint32(0x00300040u, 0xCAFEF00Du); // plant at the physical page
    enter_dt();

    // lwz r3, 0x40(r4) with r4 = EA base
    P->gpr[4] = 0x00200000u;
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0x40));
    // (the fetch at $1000 is untranslated; the DATA access translates)
    run_at(0x1000, 1);
    CHECK_EQ(P->gpr[3], 0xCAFEF00Du);
    // R set by the walk, C still clear after a load
    CHECK_EQ(memory_read_uint32(pte + 4) & 0x180u, 0x100u);

    // stw through the same page sets C
    P->gpr[3] = 0x11223344u;
    memory_write_uint32(0x1000, e_d(36, 3, 4, 0x44)); // stw r3, 0x44(r4)
    run_at(0x1000, 1);
    CHECK_EQ(memory_read_uint32(pte + 4) & 0x180u, 0x180u);
    P->msr &= (uint32_t)~PPC_MSR_DT;
    ppc_update_active_maps(P);
    CHECK_EQ(memory_read_uint32(0x00300044u), 0x11223344u);
}

// Secondary-hash search: PTE with H=1 in the rehashed PTEG.
static void test_htab_secondary(void) {
    fresh();
    wipe_htab();
    put_pte(0, 0x00204000u, 0x00304000u, 1, 3, 0, 2);
    memory_write_uint32(0x00304008u, 0x5EC04DA2u);
    enter_dt();
    P->gpr[4] = 0x00204000u;
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0x08));
    run_at(0x1000, 1);
    CHECK_EQ(P->gpr[3], 0x5EC04DA2u);
}

// HTAB miss → DSI with DSISR bit 1 (+bit 6 for stores), DAR = EA,
// SRR0 = the faulting instruction; update forms must NOT write rA back.
static void test_dsi_miss(void) {
    fresh();
    wipe_htab();
    enter_dt();
    P->gpr[4] = 0x00500000u; // no PTE anywhere
    memory_write_uint32(0x1000, e_d(33, 3, 4, 0x10)); // lwzu r3, 0x10(r4)
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x00000300u);
    CHECK_EQ(P->srr0, 0x1000u);
    CHECK_EQ(P->dar, 0x00500010u);
    CHECK_EQ(P->dsisr, PPC_DSISR_NOTFOUND);
    CHECK_EQ(P->gpr[4], 0x00500000u); // lwzu abandoned: rA NOT updated

    fresh();
    enter_dt();
    P->gpr[4] = 0x00500000u;
    memory_write_uint32(0x1000, e_d(37, 3, 4, 0x10)); // stwu r3, 0x10(r4)
    run_at(0x1000, 1);
    CHECK_EQ(P->dsisr, PPC_DSISR_NOTFOUND | PPC_DSISR_STORE);
    CHECK_EQ(P->gpr[4], 0x00500000u);
}

// Page protection: PP=11 is read-only under both keys; a denied store
// still sets R but never C (601UM §6.8.4).
static void test_page_protection(void) {
    fresh();
    wipe_htab();
    uint32_t pte = put_pte(0, 0x00200000u, 0x00300000u, 0, 0, 0, 3); // PP=11 ro
    enter_dt();
    P->gpr[4] = 0x00200000u;
    memory_write_uint32(0x1000, e_d(36, 3, 4, 0)); // stw
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x00000300u);
    CHECK_EQ(P->dsisr, PPC_DSISR_PROT | PPC_DSISR_STORE);
    CHECK_EQ(P->dar, 0x00200000u);
    CHECK_EQ(memory_read_uint32(pte + 4) & 0x180u, 0x100u); // R set, C clear

    // The load side of the same page still works
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0));
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x1004u);
}

// User-mode key: with SR[Ku]=1 and PP=00 a user access is denied; the
// (PR,DT) SoA discipline fills the user maps only for permitted pages.
static void test_user_key_and_soa(void) {
    fresh();
    wipe_htab();
    put_pte(0, 0x00200000u, 0x00300000u, 0, 0, 0, 2); // PP=10: rw both keys
    put_pte(0, 0x00201000u, 0x00301000u, 0, 0, 0, 0); // PP=00: no access key 1
    P->sdr1 = SDR1_VAL;
    ppc_set_sr(P, 0, 0x20000000u); // T=0, Ks=0, Ku=1, VSID 0
    P->msr |= PPC_MSR_DT | PPC_MSR_PR;
    ppc_update_active_maps(P);
    CHECK(g_active_read == g_user_read);

    // Load from the permitted page: works and fills the user READ map
    // only.  Test pokes go through the supervisor identity view (the
    // active maps in user+DT mode are the MMU's logical fills).
    P->gpr[4] = 0x00200000u;
    P->msr &= (uint32_t) ~(PPC_MSR_DT | PPC_MSR_PR);
    ppc_update_active_maps(P);
    memory_write_uint32(0x00300020u, 0xA5A5A5A5u);
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0x20)); // lwz r3, 0x20(r4)
    P->msr |= PPC_MSR_DT | PPC_MSR_PR;
    ppc_update_active_maps(P);
    run_at(0x1000, 1);
    CHECK_EQ(P->gpr[3], 0xA5A5A5A5u);
    CHECK(g_user_read[0x00200000u >> PAGE_SHIFT] != 0);
    CHECK(g_user_write[0x00200000u >> PAGE_SHIFT] == 0); // no store yet

    // Store fills the write map (C set on the walk)
    P->msr &= (uint32_t) ~(PPC_MSR_DT | PPC_MSR_PR);
    ppc_update_active_maps(P);
    memory_write_uint32(0x1000, e_d(36, 3, 4, 0x24));
    P->msr |= PPC_MSR_DT | PPC_MSR_PR;
    ppc_update_active_maps(P);
    run_at(0x1000, 1);
    CHECK(g_user_write[0x00200000u >> PAGE_SHIFT] != 0);

    // The PP=00 page is unreachable from user mode (key 1)
    P->gpr[4] = 0x00201000u;
    P->msr &= (uint32_t) ~(PPC_MSR_DT | PPC_MSR_PR);
    ppc_update_active_maps(P);
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0));
    P->msr |= PPC_MSR_DT | PPC_MSR_PR;
    ppc_update_active_maps(P);
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x00000300u);
    CHECK_EQ(P->dsisr, PPC_DSISR_PROT);
    // ...but supervisor (key 0) reaches it
    CHECK(g_active_read == g_supervisor_read); // exception entry restored
    P->msr |= PPC_MSR_DT;
    ppc_update_active_maps(P);
    P->gpr[4] = 0x00201000u;
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0)); // fetch path untranslated
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x1004u);
}

// 601 BATs: Ks/Ku/PP protection keys in the UPPER register, V+BSM in the
// LOWER; and a T=1 segment must PREVAIL over a matching BAT (Table 6-10).
static void test_bat(void) {
    fresh();
    wipe_htab();
    // BAT0: 128 KB block, EA $00600000 → PA $00700000, PP=10, keys 0
    P->batu[0] = 0x00600000u | 0x2u; // BLPI | PP=10
    P->batl[0] = 0x00700000u | 0x40u; // PBN | V
    ppc_mmu_invalidate_all(P);
    enter_dt();
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0));
    P->gpr[4] = 0x00612340u;
    P->msr &= (uint32_t)~PPC_MSR_DT;
    ppc_update_active_maps(P);
    memory_write_uint32(0x00712340u, 0xB0A7B0A7u);
    P->msr |= PPC_MSR_DT;
    ppc_update_active_maps(P);
    run_at(0x1000, 1);
    CHECK_EQ(P->gpr[3], 0xB0A7B0A7u);

    // PP=11 in BATU: store denied with the BAT protection DSISR
    P->batu[0] = 0x00600000u | 0x3u;
    ppc_mmu_invalidate_all(P);
    memory_write_uint32(0x1000, e_d(36, 3, 4, 0));
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x00000300u);
    CHECK_EQ(P->dsisr, PPC_DSISR_PROT | PPC_DSISR_STORE);

    // T=1 prevails: point segment 0 memory-forced at physical segment 0 —
    // the BAT above must be IGNORED and the access go straight through.
    ppc_set_sr(P, 0, 0x87F00000u);
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0));
    P->msr &= (uint32_t)~PPC_MSR_DT;
    ppc_update_active_maps(P);
    memory_write_uint32(0x00612340u, 0x600DF00Du); // identity target
    P->msr |= PPC_MSR_DT;
    ppc_update_active_maps(P);
    run_at(0x1000, 1);
    CHECK_EQ(P->gpr[3], 0x600DF00Du);
    ppc_set_sr(P, 0, 0);
}

// T=1 memory-forced segments translate data accesses even with MSR[DT]=0
// (601UM §6.5.2) — the SR5-toggle flash-probe aliasing depends on it.
static void test_t1_dt_off(void) {
    fresh();
    // SR2 memory-forced → physical segment 0: EA $2xxxxxxx aliases RAM.
    ppc_set_sr(P, 2, 0x87F00000u);
    memory_write_uint32(0x00004000u, 0);
    P->gpr[4] = 0x20004000u;
    P->gpr[3] = 0xF1A5F1A5u;
    memory_write_uint32(0x1000, e_d(36, 3, 4, 0)); // stw with DT OFF
    run_at(0x1000, 1);
    CHECK_EQ(memory_read_uint32(0x00004000u), 0xF1A5F1A5u);

    // Toggle the segment's low nibble: the same EA now hits segment 1.
    ppc_set_sr(P, 2, 0x87F00001u);
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0));
    run_at(0x1000, 1); // PA $10004000: unmapped space reads all-ones
    CHECK_EQ(P->gpr[3], 0xFFFFFFFFu);
    ppc_set_sr(P, 2, 0);
}

// tlbie invalidates by congruence class (EA[13-19] mod 128): a PTE edit
// is invisible until a tlbie whose EA lands in the same class.
static void test_tlbie_class(void) {
    fresh();
    wipe_htab();
    uint32_t pte = put_pte(0, 0x00200000u, 0x00300000u, 0, 0, 0, 2);
    enter_dt();
    P->msr &= (uint32_t)~PPC_MSR_DT;
    ppc_update_active_maps(P);
    memory_write_uint32(0x00300000u, 0x11111111u);
    memory_write_uint32(0x00301000u, 0x22222222u);
    P->msr |= PPC_MSR_DT;
    ppc_update_active_maps(P);

    P->gpr[4] = 0x00200000u;
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0));
    run_at(0x1000, 1);
    CHECK_EQ(P->gpr[3], 0x11111111u); // cached now (xtlb)

    // Repoint the PTE (via the untranslated view), no tlbie: stale is legal
    P->msr &= (uint32_t)~PPC_MSR_DT;
    ppc_update_active_maps(P);
    memory_write_uint32(pte + 4, 0x00301000u | 2u);
    P->msr |= PPC_MSR_DT;
    ppc_update_active_maps(P);

    // tlbie an EA in a DIFFERENT congruence class: must NOT be required to
    // drop the entry... but tlbie the matching class MUST drop it.
    memory_write_uint32(0x1000,
                        e_d(32, 3, 4, 0)); // (rewrite via active maps is fine: supervisor untranslated poke below)
    run_at(0x1000, 1);
    // (stale or fresh both architecturally fine here — no CHECK)

    // Same congruence class (page + 128 pages), different EA:
    ppc_mmu_tlbie(P, 0x00200000u + (128u << PAGE_SHIFT));
    run_at(0x1000, 1);
    CHECK_EQ(P->gpr[3], 0x22222222u); // new mapping visible after tlbie
}

// mtsr: rewriting the SAME value must keep the caches (the nanokernel
// reloads identical SRs wholesale); a changed value invalidates.
static void test_mtsr_invalidation(void) {
    fresh();
    wipe_htab();
    put_pte(0, 0x00200000u, 0x00300000u, 0, 0, 0, 2);
    P->sdr1 = SDR1_VAL;
    ppc_set_sr(P, 0, 0x20000000u);
    P->msr |= PPC_MSR_DT | PPC_MSR_PR;
    ppc_update_active_maps(P);
    P->msr &= (uint32_t) ~(PPC_MSR_DT | PPC_MSR_PR);
    ppc_update_active_maps(P);
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0));
    P->msr |= PPC_MSR_DT | PPC_MSR_PR;
    ppc_update_active_maps(P);
    P->gpr[4] = 0x00200000u;
    run_at(0x1000, 1);
    CHECK(g_user_read[0x00200000u >> PAGE_SHIFT] != 0);

    ppc_set_sr(P, 0, 0x20000000u); // same value: fills survive
    CHECK(g_user_read[0x00200000u >> PAGE_SHIFT] != 0);
    ppc_set_sr(P, 0, 0x20000001u); // new VSID: fills die
    CHECK(g_user_read[0x00200000u >> PAGE_SHIFT] == 0);
}

// dcbz to a write-through or cache-inhibited page takes the alignment
// exception (601UM Table 6-3); to a normal page it really zeroes.
static void test_dcbz_wimg(void) {
    fresh();
    wipe_htab();
    put_pte(0, 0x00200000u, 0x00300000u, 0, 0, 0x4u, 2); // WIM = 100: W set
    put_pte(0, 0x00201000u, 0x00301000u, 0, 0, 0, 2); // plain
    enter_dt();

    P->gpr[4] = 0x00200000u;
    memory_write_uint32(0x1000, e_x(0, 0, 4, 1014, 0)); // dcbz 0,r4
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x00000600u); // alignment exception
    CHECK_EQ(P->dar, 0x00200000u);

    fresh();
    enter_dt();
    P->msr &= (uint32_t)~PPC_MSR_DT;
    ppc_update_active_maps(P);
    for (int i = 0; i < 8; i++)
        memory_write_uint32(0x00301000u + 4u * (uint32_t)i, 0xFFFFFFFFu);
    P->msr |= PPC_MSR_DT;
    ppc_update_active_maps(P);
    P->gpr[4] = 0x00201000u;
    memory_write_uint32(0x1000, e_x(0, 0, 4, 1014, 0));
    run_at(0x1000, 1);
    P->msr &= (uint32_t)~PPC_MSR_DT;
    ppc_update_active_maps(P);
    for (int i = 0; i < 8; i++)
        CHECK_EQ(memory_read_uint32(0x00301000u + 4u * (uint32_t)i), 0);
}

// Instruction fetch: HTAB-translated fetch works with MSR[IT]; a missing
// PTE raises ISI with SRR1 bit 1 ONLY ($40000000 — the nanokernel's
// InstStorageInt masks $40200000 with andis./beq, so bit 1 satisfies it,
// and Copland reads bit 10 as a hard access error and would panic on an
// ordinary page fault); a non-memory-forced T=1 fetch sets NO SRR1 bits.
static void test_fetch_translation(void) {
    fresh();
    wipe_htab();
    // Map EA $00208000 → PA $00308000 and plant code there:
    // li r3, 0x77 ; (budget ends)
    put_pte(0, 0x00208000u, 0x00308000u, 0, 0, 0, 2);
    memory_write_uint32(0x00308000u, e_d(14, 3, 0, 0x77));
    P->sdr1 = SDR1_VAL;
    P->msr |= PPC_MSR_IT;
    ppc_update_active_maps(P);
    ppc_mmu_invalidate_all(P);
    run_at(0x00208000u, 1);
    CHECK_EQ(P->gpr[3], 0x77u);

    // Unmapped fetch → ISI, SRR1 = $40000000, and the vector itself is
    // fetched untranslated (IT cleared on entry).  A real instruction
    // sits at the vector so the budget completes without a second
    // exception clobbering SRR0/SRR1.
    memory_write_uint32(0x00000400u, e_d(14, 0, 0, 0)); // li r0,0
    run_at(0x00280000u, 1);
    CHECK_EQ(P->srr0, 0x00280000u);
    CHECK_EQ(P->srr1 & 0xFFFF0000u, 0x40000000u);

    // T=1 (non-memory-forced) fetch: ISI with NO status bits.
    fresh();
    P->sr[1] = 0x80000000u | (0x055u << 20); // T=1, BUID $055
    ppc_recompute_sr_t_mask(P);
    ppc_mmu_invalidate_all(P);
    P->msr |= PPC_MSR_IT;
    ppc_update_active_maps(P);
    memory_write_uint32(0x00000400u, e_d(14, 0, 0, 0)); // li r0,0
    run_at(0x10000000u, 1);
    CHECK_EQ(P->srr0, 0x10000000u);
    CHECK_EQ(P->srr1 & 0xFFFF0000u, 0);
}

// Non-memory-forced T=1 DATA access models the failed bus reply as the
// 601-only $00A00 exception: SRR0 = FOLLOWING instruction, DSISR
// unchanged (Table 5-21).
static void test_ioseg_error(void) {
    fresh();
    P->sr[1] = 0x80000000u | (0x055u << 20);
    ppc_recompute_sr_t_mask(P);
    ppc_mmu_invalidate_all(P);
    P->dsisr = 0x12345678u; // must survive
    P->gpr[4] = 0x10000000u;
    memory_write_uint32(0x1000, e_d(32, 3, 4, 0));
    run_at(0x1000, 1);
    CHECK_EQ(P->pc, 0x00000A00u);
    CHECK_EQ(P->srr0, 0x1004u); // following instruction
    CHECK_EQ(P->dsisr, 0x12345678u);
    CHECK_EQ(P->dar, 0x10000000u);
}

int main(void) {
    // 32-bit address space: 8 MB RAM at 0, 128 KB ROM at $40800000 (the
    // ppc-suite context shape).
    test_context_t *ctx = calloc(1, sizeof(test_context_t));
    ctx->memory = memory_map_init(32, 0x800000, 0x20000, NULL);
    if (!ctx->memory) {
        printf("FAIL: memory_map_init\n");
        return 1;
    }
    memory_populate_pages(ctx->memory, 0x40800000u, 0x40820000u);
    test_set_active_context(ctx);

    P = ppc_init(NULL); // reserves the user SoA arrays and wipes them
    if (!P) {
        printf("FAIL: ppc_init\n");
        return 1;
    }

    test_htab_walk_rc();
    test_htab_secondary();
    test_dsi_miss();
    test_page_protection();
    test_user_key_and_soa();
    test_bat();
    test_t1_dt_off();
    test_tlbie_class();
    test_mtsr_invalidation();
    test_dcbz_wimg();
    test_fetch_translation();
    test_ioseg_error();

    ppc_delete(P);
    printf("ppc_mmu: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
