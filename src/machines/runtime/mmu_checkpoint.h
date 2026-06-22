// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mmu_checkpoint.h
// Family-agnostic save/restore of the seven guest-visible 68030 PMMU
// registers (tc / crp / srp / tt0 / tt1 / mmusr / enabled).  This block was
// byte-identical in every II-family machine's checkpoint path (proposal
// §1.1); extracting it keeps save and restore in lockstep in one place.

#ifndef GS_MACHINES_RUNTIME_MMU_CHECKPOINT_H
#define GS_MACHINES_RUNTIME_MMU_CHECKPOINT_H

#include "checkpoint.h"
#include "mmu.h"

// Serialise the PMMU guest registers in canonical order.
void mmu_checkpoint_save(mmu_state_t *mmu, checkpoint_t *cp);

// Restore the PMMU guest registers; MUST read the same order save wrote.
void mmu_checkpoint_restore(mmu_state_t *mmu, checkpoint_t *cp);

#endif // GS_MACHINES_RUNTIME_MMU_CHECKPOINT_H
