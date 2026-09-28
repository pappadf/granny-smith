// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mmu_checkpoint.c
// Save/restore of the 68030 PMMU guest registers — see mmu_checkpoint.h.

#include "mmu_checkpoint.h"

// Serialise the PMMU guest registers in canonical order.  Order matches the
// block previously inlined verbatim in se30/iicx/iix/iici/iisi/iifx.
void mmu_checkpoint_save(mmu_state_t *mmu, checkpoint_t *cp) {
    system_write_checkpoint_data(cp, &mmu->tc, sizeof(mmu->tc));
    system_write_checkpoint_data(cp, &mmu->crp, sizeof(mmu->crp));
    system_write_checkpoint_data(cp, &mmu->srp, sizeof(mmu->srp));
    system_write_checkpoint_data(cp, &mmu->tt0, sizeof(mmu->tt0));
    system_write_checkpoint_data(cp, &mmu->tt1, sizeof(mmu->tt1));
    system_write_checkpoint_data(cp, &mmu->mmusr, sizeof(mmu->mmusr));
    system_write_checkpoint_data(cp, &mmu->enabled, sizeof(mmu->enabled));
    // tlb_was_enabled drives mmu_invalidate_tlb's disabled -> disabled
    // early-out.  Nothing restored it, so a restored machine started from
    // whatever mmu_init left rather than from the saved transition state.
    // Today the two happen to agree in every reachable case, so this is
    // exactness rather than a live bug -- but the early-out is one edit away
    // from depending on it, and the stream is build-ID gated so widening it
    // costs nothing as long as both halves land together (they do, here).
    system_write_checkpoint_data(cp, &mmu->tlb_was_enabled, sizeof(mmu->tlb_was_enabled));
}

// Restore the PMMU guest registers, reading the exact order save wrote.
void mmu_checkpoint_restore(mmu_state_t *mmu, checkpoint_t *cp) {
    system_read_checkpoint_data(cp, &mmu->tc, sizeof(mmu->tc));
    system_read_checkpoint_data(cp, &mmu->crp, sizeof(mmu->crp));
    system_read_checkpoint_data(cp, &mmu->srp, sizeof(mmu->srp));
    system_read_checkpoint_data(cp, &mmu->tt0, sizeof(mmu->tt0));
    system_read_checkpoint_data(cp, &mmu->tt1, sizeof(mmu->tt1));
    system_read_checkpoint_data(cp, &mmu->mmusr, sizeof(mmu->mmusr));
    system_read_checkpoint_data(cp, &mmu->enabled, sizeof(mmu->enabled));
    system_read_checkpoint_data(cp, &mmu->tlb_was_enabled, sizeof(mmu->tlb_was_enabled));
}
