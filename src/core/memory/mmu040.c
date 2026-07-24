// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mmu040.c
// MC68040 MMU: register file + TLB invalidation (Phase A).  The fixed
// three-level table walk, transparent-translation matching, and the
// memory.c slow-path integration land in Phase B of the Quadra proposal.

#include "mmu040.h"

#include "log.h"
#include "memory.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mmu");

// Allocate a zeroed 040 MMU state (translation disabled).
mmu040_state_t *mmu040_init(void) {
    mmu040_state_t *mmu = (mmu040_state_t *)calloc(1, sizeof(mmu040_state_t));
    return mmu;
}

// Free the state.
void mmu040_delete(mmu040_state_t *mmu) {
    free(mmu);
}

// Invalidate the software TLB: zero all four SoA arrays so every access
// takes the slow path and re-resolves.  Correct but coarse — Phase B adds
// the populated-page tracking fast path the PMMU uses (mmu.c).
void mmu040_invalidate_tlb(mmu040_state_t *mmu) {
    (void)mmu;
    size_t sz = (size_t)g_page_count * sizeof(uintptr_t);
    if (g_supervisor_read)
        memset(g_supervisor_read, 0, sz);
    if (g_supervisor_write)
        memset(g_supervisor_write, 0, sz);
    if (g_user_read)
        memset(g_user_read, 0, sz);
    if (g_user_write)
        memset(g_user_write, 0, sz);
}

// MOVEC to TC: only E (bit 15) and P (bit 14) are implemented bits.
void mmu040_set_tc(mmu040_state_t *mmu, uint32_t value) {
    if (!mmu)
        return;
    mmu->tc = value & (TC040_E | TC040_P);
    mmu->enabled = (mmu->tc & TC040_E) != 0;
    mmu040_invalidate_tlb(mmu);
}

// MOVEC to ITT0/ITT1/DTT0/DTT1: mask to the writable bits, then flush.
void mmu040_set_ttr(mmu040_state_t *mmu, uint32_t *reg, uint32_t value) {
    if (!mmu || !reg)
        return;
    *reg = value & TT040_WRITE_MASK;
    mmu040_invalidate_tlb(mmu);
}

// MOVEC to URP/SRP: roots are 512-byte aligned (bits 8:0 read as zero).
void mmu040_set_root(mmu040_state_t *mmu, uint32_t *reg, uint32_t value) {
    if (!mmu || !reg)
        return;
    *reg = value & 0xFFFFFE00u;
    mmu040_invalidate_tlb(mmu);
}

// PFLUSH (An) / PFLUSHN (An): v1 flushes everything (correct, coarse).
void mmu040_pflush_page(mmu040_state_t *mmu, uint32_t addr, bool nonglobal_only) {
    (void)addr;
    (void)nonglobal_only;
    mmu040_invalidate_tlb(mmu);
}

// PFLUSHA / PFLUSHAN.
void mmu040_pflush_all(mmu040_state_t *mmu, bool nonglobal_only) {
    (void)nonglobal_only;
    mmu040_invalidate_tlb(mmu);
}

// PTESTR/PTESTW (An): load MMUSR with the translation result.
// Phase B implements the real walk against URP/SRP; with translation
// disabled the 040 reports the address back as a resident identity.
void mmu040_ptest(mmu040_state_t *mmu, uint32_t addr, bool write, uint32_t fc) {
    if (!mmu)
        return;
    (void)write;
    (void)fc;
    if (!mmu->enabled) {
        mmu->mmusr = (addr & MMUSR040_PA_MASK) | MMUSR040_R;
        return;
    }
    // Phase B: three-level walk.  Until then, log loudly rather than
    // fabricate a result (Trap 24: no silent zeros).
    LOG(1, "mmu040: PTEST with translation enabled before walker exists (addr=%08X)", addr);
    mmu->mmusr = 0;
}
