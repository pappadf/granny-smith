// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mmu040.h
// MC68040 MMU interface.  A sibling of, not a patch to, the 68030 PMMU in
// mmu.c (proposal-machine-quadra-700-900-950.md §6.5): URP/SRP roots, a
// fixed three-level table walk, four transparent-translation registers,
// and the 040 forms of PTEST/PFLUSH.  Registers are reached via MOVEC
// (the 040 has no PMOVE); the CPU decoder in cpu_68040.c calls in here.

#ifndef MMU040_H
#define MMU040_H

#include <stdbool.h>
#include <stdint.h>

// === TC register (MOVEC $003) — 16 bits used ===
#define TC040_E (1u << 15) // enable translation
#define TC040_P (1u << 14) // page size: 0 = 4K, 1 = 8K

// === Transparent translation registers (ITT0/ITT1/DTT0/DTT1) ===
#define TT040_BASE(tt)   (((tt) >> 24) & 0xFF) // logical address base
#define TT040_MASK(tt)   (((tt) >> 16) & 0xFF) // address mask (1 = don't care)
#define TT040_E(tt)      (((tt) >> 15) & 1) // enable
#define TT040_SFIELD(tt) (((tt) >> 13) & 3) // 0=user only, 1=super only, 2/3=both
#define TT040_W(tt)      (((tt) >> 2) & 1) // write protect
// Writable bit mask: BASE|MASK|E|S-field|U1U0|CM|W
#define TT040_WRITE_MASK 0xFFFFE364u

// === MMUSR (MOVEC $805) — PTEST result ===
#define MMUSR040_PA_MASK  0xFFFFF000u // physical address of page
#define MMUSR040_B        (1u << 11) // bus error during walk
#define MMUSR040_G        (1u << 10) // global
#define MMUSR040_S        (1u << 7) // supervisor protected
#define MMUSR040_CM_SHIFT 5 // cache mode (bits 6:5)
#define MMUSR040_M        (1u << 4) // modified
#define MMUSR040_W        (1u << 2) // write protected
#define MMUSR040_T        (1u << 1) // transparent (TTR hit)
#define MMUSR040_R        (1u << 0) // resident

// MC68040 MMU state.  Registers are written via MOVEC in cpu_68040.c;
// the table walker (Phase B) consumes them.  Owned by the CPU instance
// (the 040 MMU is on-chip; cpu_init creates it, cpu_delete frees it).
typedef struct mmu040_state {
    uint32_t tc; // translation control (E, P; 16-bit register)
    uint32_t itt0; // instruction transparent translation 0
    uint32_t itt1; // instruction transparent translation 1
    uint32_t dtt0; // data transparent translation 0
    uint32_t dtt1; // data transparent translation 1
    uint32_t urp; // user root pointer (512-byte aligned)
    uint32_t srp; // supervisor root pointer
    uint32_t mmusr; // MMU status register (PTEST result)
    bool enabled; // TC.E — translation active
} mmu040_state_t;

// === Lifecycle ===

// Allocate a zeroed 040 MMU state (translation disabled).
mmu040_state_t *mmu040_init(void);

// Free the state.
void mmu040_delete(mmu040_state_t *mmu);

// === Register write helpers (called from MOVEC in cpu_68040.c) ===
// Each masks the value to the architecturally writable bits and flushes
// the software TLB when the write can change translations.

void mmu040_set_tc(mmu040_state_t *mmu, uint32_t value);
void mmu040_set_ttr(mmu040_state_t *mmu, uint32_t *reg, uint32_t value);
void mmu040_set_root(mmu040_state_t *mmu, uint32_t *reg, uint32_t value);

// === TLB management ===

// Invalidate the software TLB (the SoA arrays in memory.c).  Called on
// root/TC/TTR writes and by PFLUSH.
void mmu040_invalidate_tlb(mmu040_state_t *mmu);

// PFLUSH (An) / PFLUSHN (An): flush the ATC entry for one page.
// v1 flushes the whole TLB (correct, coarse); `nonglobal_only` mirrors
// the PFLUSHN/PFLUSHAN forms (global entries are not modeled separately).
void mmu040_pflush_page(mmu040_state_t *mmu, uint32_t addr, bool nonglobal_only);

// PFLUSHA / PFLUSHAN: flush all ATC entries.
void mmu040_pflush_all(mmu040_state_t *mmu, bool nonglobal_only);

// === PTEST ===

// PTESTR/PTESTW (An): walk the translation for `addr` under function code
// `fc` (from DFC per MC68040UM) and load MMUSR with the result.  Phase B
// implements the real three-level walk; until a 040 machine exists this
// reports a resident identity translation when the MMU is disabled.
void mmu040_ptest(mmu040_state_t *mmu, uint32_t addr, bool write, uint32_t fc);

#endif // MMU040_H
