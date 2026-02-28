// SPDX-License-Identifier: MIT
// mmu.h
// 68030 PMMU (Paged Memory Management Unit) interface.
// Implements lazy-fill TLB using SoA pointer arrays in memory.h.
// Table walks resolve guest translation descriptors to physical addresses.

#ifndef MMU_H
#define MMU_H

#include <stdbool.h>
#include <stdint.h>

// Forward declaration (memory.h has the full definition)
typedef struct page_entry page_entry_t;

// === TC Register field extraction ===
#define TC_ENABLE(tc) ((tc) >> 31)
#define TC_SRE(tc)    (((tc) >> 25) & 1)
#define TC_FCL(tc)    (((tc) >> 24) & 1)
#define TC_PS(tc)     (((tc) >> 20) & 0xF)
#define TC_IS(tc)     (((tc) >> 16) & 0xF)
#define TC_TIA(tc)    (((tc) >> 12) & 0xF)
#define TC_TIB(tc)    (((tc) >> 8) & 0xF)
#define TC_TIC(tc)    (((tc) >> 4) & 0xF)
#define TC_TID(tc)    ((tc) & 0xF)

// === Descriptor type codes ===
#define DESC_DT_INVALID 0 // invalid descriptor
#define DESC_DT_PAGE    1 // page descriptor (early termination OK)
#define DESC_DT_TABLE4  2 // valid 4-byte (short) table descriptor
#define DESC_DT_TABLE8  3 // valid 8-byte (long) table descriptor

// === TT Register field extraction ===
#define TT_ENABLE(tt)  (((tt) >> 15) & 1)
#define TT_BASE(tt)    (((tt) >> 24) & 0xFF)
#define TT_MASK(tt)    (((tt) >> 16) & 0xFF)
#define TT_RW(tt)      (((tt) >> 13) & 1) // 0=match both, 1=match R/W field
#define TT_RWM(tt)     (((tt) >> 12) & 1) // 0=match writes, 1=match reads
#define TT_CI(tt)      (((tt) >> 10) & 1) // cache inhibit
#define TT_FC_BASE(tt) (((tt) >> 4) & 7) // function code base
#define TT_FC_MASK(tt) ((tt) & 7) // function code mask

// === MMUSR bit positions ===
#define MMUSR_B       (1u << 15) // bus error during walk
#define MMUSR_L       (1u << 14) // limit violation
#define MMUSR_S       (1u << 13) // supervisor-only page
#define MMUSR_W       (1u << 10) // write-protected
#define MMUSR_I       (1u << 9) // invalid descriptor
#define MMUSR_M       (1u << 8) // modified
#define MMUSR_T       (1u << 6) // transparent translation
#define MMUSR_N_SHIFT 0 // number of levels traversed (bits 2:0)

// Result of a table walk
typedef struct mmu_walk_result {
    uint32_t physical_addr; // resolved physical page address
    bool valid; // true if walk succeeded
    bool supervisor_only; // S bit from descriptor
    bool write_protected; // W bit from descriptor
    bool modified; // M bit from descriptor
    uint16_t mmusr; // MMUSR bits for PTEST
} mmu_walk_result_t;

// MMU state for 68030 — registers set via PMOVE instructions
typedef struct mmu_state {
    // 68030 MMU registers (set via PMOVE)
    uint64_t crp; // CPU root pointer (64-bit descriptor)
    uint64_t srp; // Supervisor root pointer
    uint32_t tc; // Translation control
    uint32_t tt0; // Transparent translation register 0
    uint32_t tt1; // Transparent translation register 1
    uint16_t mmusr; // MMU status register

    bool enabled; // TC.E bit — is translation active?

    // Host-side state for table walks
    uint8_t *physical_ram; // base of physical RAM buffer
    uint32_t physical_ram_size; // size of RAM in bytes
    uint8_t *physical_rom; // base of physical ROM buffer
    uint32_t physical_rom_size; // size of ROM in bytes
    uint32_t rom_phys_base; // physical address where ROM is mapped
} mmu_state_t;

// === Lifecycle ===

// Create MMU state with pointers to physical RAM/ROM
mmu_state_t *mmu_init(uint8_t *physical_ram, uint32_t ram_size, uint8_t *physical_rom, uint32_t rom_size,
                      uint32_t rom_phys_base);

// Destroy MMU state
void mmu_delete(mmu_state_t *mmu);

// === TLB Management ===

// Invalidate the entire software TLB (zero all four SoA arrays).
// Called on PMOVE to TC/CRP/SRP and PFLUSHA.
void mmu_invalidate_tlb(mmu_state_t *mmu);

// === Address Translation ===

// Handle a TLB miss: perform table walk (or TT check), fill SoA entry.
// Returns true if translation succeeded (SoA entry populated, retry access).
// Returns false if translation failed (invalid descriptor → caller raises bus error).
bool mmu_handle_fault(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor);

// Test address translation without faulting (PTEST instruction).
// Performs table walk and returns MMUSR value.
uint16_t mmu_test_address(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor);

// Check transparent translation registers (TT0/TT1).
// Returns true if address matches a TT register (bypass table walk).
bool mmu_check_tt(mmu_state_t *mmu, uint32_t addr, bool write, bool supervisor);

// Global MMU state pointer (set by machine init, NULL for 68000 machines)
extern struct mmu_state *g_mmu;

#endif // MMU_H
