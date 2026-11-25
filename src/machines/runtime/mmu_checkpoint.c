// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mmu_checkpoint.c
// Save/restore of the 68030 PMMU guest registers — see mmu_checkpoint.h.

#include "mmu_checkpoint.h"
#include "checkpoint.h"

// The PMMU guest registers in canonical order, for both directions.  Order
// matches the block previously inlined verbatim in se30/iicx/iix/iici/iisi/iifx.
//
// tlb_was_enabled drives mmu_invalidate_tlb's disabled -> disabled
// early-out.  Nothing restored it, so a restored machine started from
// whatever mmu_init left rather than from the saved transition state.
// Today the two happen to agree in every reachable case, so this is
// exactness rather than a live bug -- but the early-out is one edit away
// from depending on it, and the stream is build-ID gated so widening it
// costs nothing as long as both halves land together (they do, here).
#define MMU030_CP_FIELDS(OP, cp, mmu)                                                                                  \
    CP_FIELD(OP, cp, (mmu)->tc);                                                                                       \
    CP_FIELD(OP, cp, (mmu)->crp);                                                                                      \
    CP_FIELD(OP, cp, (mmu)->srp);                                                                                      \
    CP_FIELD(OP, cp, (mmu)->tt0);                                                                                      \
    CP_FIELD(OP, cp, (mmu)->tt1);                                                                                      \
    CP_FIELD(OP, cp, (mmu)->mmusr);                                                                                    \
    CP_FIELD(OP, cp, (mmu)->enabled);                                                                                  \
    CP_FIELD(OP, cp, (mmu)->tlb_was_enabled)

// Serialise the PMMU guest registers in canonical order.
void mmu_checkpoint_save(mmu_state_t *mmu, checkpoint_t *cp) {
    MMU030_CP_FIELDS(system_write_checkpoint_data, cp, mmu);
}

// Restore the PMMU guest registers, reading the exact order save wrote.
void mmu_checkpoint_restore(mmu_state_t *mmu, checkpoint_t *cp) {
    MMU030_CP_FIELDS(system_read_checkpoint_data, cp, mmu);
}
