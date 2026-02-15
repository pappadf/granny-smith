// SPDX-License-Identifier: MIT
// mmu.h
// 68030 PMMU (Paged Memory Management Unit) stub interface.
// Full implementation will walk guest translation tables and rebuild
// the page table for logical-to-physical address mapping.

#ifndef MMU_H
#define MMU_H

#include "memory.h"
#include <stdbool.h>
#include <stdint.h>

// MMU state for 68030 — registers set via PMOVE instructions
typedef struct mmu_state {
    // 68030 MMU registers (set via PMOVE)
    uint64_t crp; // CPU root pointer (64-bit descriptor)
    uint64_t srp; // Supervisor root pointer
    uint32_t tc; // Translation control
    uint32_t tt0, tt1; // Transparent translation registers
    uint16_t mmusr; // MMU status register

    bool enabled; // TC.E bit — is translation active?

    // Host-side state
    page_entry_t *page_table; // pointer to the active page table
    uint8_t *physical_ram; // base of physical RAM buffer
    uint32_t physical_ram_size;
} mmu_state_t;

// TODO: Implement for IIcx support
// Create and destroy MMU state
// mmu_state_t *mmu_init(page_entry_t *page_table, uint8_t *ram, uint32_t ram_size);
// void mmu_delete(mmu_state_t *mmu);

// TODO: Rebuild page table from guest translation tables
// Called when guest modifies MMU registers via PMOVE or executes PFLUSH.
// Walks root pointer → level-1 → level-2 → page descriptors and populates
// page_table[] entries so that subsequent memory accesses use the fast
// inline path with no per-access MMU overhead.
// void mmu_rebuild_page_table(mmu_state_t *mmu);

// TODO: Identity mapping setup (used when TC.E=0)
// void mmu_setup_identity(mmu_state_t *mmu);

// TODO: PTEST instruction support — test address translation without faulting
// uint16_t mmu_test_address(mmu_state_t *mmu, uint32_t logical_addr);

#endif // MMU_H
