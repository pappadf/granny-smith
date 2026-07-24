// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mmu040.c
// MC68040 MMU: register file, transparent-translation registers, and the
// fixed three-level table walk (MC68040UM §3).  A sibling of, not a patch
// to, the 68030 PMMU in mmu.c: translation dispatches here through the
// mmu_state_t.m040 link, while the physical resolver, SoA fill, and TLB
// tracking are shared (mmu_fill_soa_page / mmu_read_physical_* in mmu.c).

#include "mmu040.h"

#include "log.h"
#include "memory.h"
#include "mmu.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mmu");

// === Descriptor field constants (MC68040UM Figures 3-11/3-12) ===
#define DESC040_UDT_MASK 3u // upper-level descriptor type (2/3 = resident)
#define DESC040_W        (1u << 2) // write protected (accumulates through levels)
#define DESC040_U        (1u << 3) // used (set by the walk)
#define DESC040_M        (1u << 4) // modified (set before a write completes)
#define DESC040_S        (1u << 7) // supervisor protected (page level)
#define DESC040_G        (1u << 10) // global (page level)

// Allocate a zeroed 040 MMU state (translation disabled).
mmu040_state_t *mmu040_init(void) {
    mmu040_state_t *mmu = (mmu040_state_t *)calloc(1, sizeof(mmu040_state_t));
    return mmu;
}

// Free the state.
void mmu040_delete(mmu040_state_t *mmu) {
    free(mmu);
}

// Invalidate the software TLB.  When attached to a bus-side MMU state, reuse
// the PMMU's tracked invalidation (fast path + dis→dis shortcut keyed on the
// mirrored enable bit); otherwise zero the arrays directly (bare unit tests).
void mmu040_invalidate_tlb(mmu040_state_t *mmu) {
    if (mmu && mmu->bus) {
        mmu_invalidate_tlb(mmu->bus);
        return;
    }
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

// MOVEC to TC: only E (bit 15) and P (bit 14) are implemented bits.  The
// enable state mirrors into the bus-side mmu_state_t so the memory.c
// fast-path checks (`g_mmu->enabled`) stay unchanged.
void mmu040_set_tc(mmu040_state_t *mmu, uint32_t value) {
    if (!mmu)
        return;
    mmu->tc = value & (TC040_E | TC040_P);
    mmu->enabled = (mmu->tc & TC040_E) != 0;
    LOG(2, "mmu040 TC <- $%04X (enabled=%d) urp=$%08X srp=$%08X dtt0=$%08X dtt1=$%08X itt0=$%08X itt1=$%08X", mmu->tc,
        mmu->enabled ? 1 : 0, mmu->urp, mmu->srp, mmu->dtt0, mmu->dtt1, mmu->itt0, mmu->itt1);
    if (mmu->bus)
        mmu->bus->enabled = mmu->enabled;
    mmu040_invalidate_tlb(mmu);
}

// MOVEC to ITT0/ITT1/DTT0/DTT1: mask to the writable bits, then flush.
void mmu040_set_ttr(mmu040_state_t *mmu, uint32_t *reg, uint32_t value) {
    if (!mmu || !reg)
        return;
    *reg = value & TT040_WRITE_MASK;
    LOG(2, "mmu040 TTR <- $%08X (raw $%08X)", *reg, value);
    mmu040_invalidate_tlb(mmu);
}

// MOVEC to URP/SRP: roots are 512-byte aligned (bits 8:0 read as zero).
void mmu040_set_root(mmu040_state_t *mmu, uint32_t *reg, uint32_t value) {
    if (!mmu || !reg)
        return;
    *reg = value & 0xFFFFFE00u;
    mmu040_invalidate_tlb(mmu);
}

// PFLUSH (An) / PFLUSHN (An): v1 flushes everything (correct, coarse — the
// SoA repopulates lazily; global entries are not modeled separately).
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

// ============================================================================
// Transparent translation registers
// ============================================================================

// One TTR against one access.  FC selection per the S-field: 0 = user only,
// 1 = supervisor only, 2/3 = both.
static bool ttr_matches(uint32_t tt, uint32_t addr, bool supervisor) {
    if (!TT040_E(tt))
        return false;
    uint32_t s = TT040_SFIELD(tt);
    if (s == 0 && supervisor)
        return false;
    if (s == 1 && !supervisor)
        return false;
    uint32_t hi = (addr >> 24) & 0xFFu;
    uint32_t mask = TT040_MASK(tt);
    return ((hi ^ TT040_BASE(tt)) & ~mask & 0xFFu) == 0;
}

// Find the matching TTR value for this access, or 0 (a TTR with E=0 can
// never be returned, so 0 doubles as "no match").  Our SoA merges the
// instruction and data streams, so both TTR pairs are consulted: the data
// registers first (data accesses dominate), then the instruction pair.
static uint32_t ttr_lookup(mmu040_state_t *mmu, uint32_t addr, bool supervisor) {
    if (ttr_matches(mmu->dtt0, addr, supervisor))
        return mmu->dtt0;
    if (ttr_matches(mmu->dtt1, addr, supervisor))
        return mmu->dtt1;
    if (ttr_matches(mmu->itt0, addr, supervisor))
        return mmu->itt0;
    if (ttr_matches(mmu->itt1, addr, supervisor))
        return mmu->itt1;
    return 0;
}

// True if any enabled TTR matches this access.  Write protection is not a
// match failure — the fault for a write to a W=1 region is raised in
// mmu040_handle_fault, mirroring the hardware access-error rule.
bool mmu040_check_tt(mmu040_state_t *mmu, uint32_t addr, bool write, bool supervisor) {
    (void)write;
    if (!mmu)
        return false;
    return ttr_lookup(mmu, addr, supervisor) != 0;
}

// ============================================================================
// Three-level table walk (MC68040UM §3.2)
// ============================================================================

// Result of a walk, in emulator terms plus the MMUSR image PTEST needs.
typedef struct m040_walk_result {
    uint32_t physical_addr; // translated physical address
    uint32_t mmusr; // 040 MMUSR image for this translation
    bool valid; // page descriptor reached and resident
    bool write_protected; // W accumulated across all levels
    bool supervisor_only; // S bit from the page descriptor
    bool modified; // M bit state after any update
} m040_walk_result_t;

// Fetch a descriptor longword from physical memory via the shared resolver.
static inline uint32_t desc_read(struct mmu_state *bus, uint32_t phys_addr) {
    return mmu_read_physical_uint32(bus, phys_addr);
}

// Write a descriptor back (U/M bit updates).  Descriptor tables live in RAM;
// the shared writer refuses ROM/unmapped targets, which is the right failure
// mode for a garbage root.
static inline void desc_write(struct mmu_state *bus, uint32_t phys_addr, uint32_t value) {
    (void)mmu_write_physical_uint32(bus, phys_addr, value);
}

// Walk the translation tables for `la`.  With `update_um` set, the walk
// performs the architectural U-bit updates (and the M-bit update for an
// allowed write), exactly once per descriptor touched (MC68040UM §3.2.2.3:
// the processor never clears U or M).
static m040_walk_result_t m040_walk(mmu040_state_t *mmu, struct mmu_state *bus, uint32_t la, bool write,
                                    bool supervisor, bool update_um) {
    m040_walk_result_t r = {0};
    bool page8k = (mmu->tc & TC040_P) != 0;
    uint32_t root = supervisor ? mmu->srp : mmu->urp;

    // Root level: RI = LA[31:25], 128 four-byte descriptors per table.
    uint32_t rdesc_addr = (root & 0xFFFFFE00u) | (((la >> 25) & 0x7Fu) << 2);
    uint32_t rdesc = desc_read(bus, rdesc_addr);
    if ((rdesc & DESC040_UDT_MASK) < 2)
        return r; // invalid — R stays clear in MMUSR
    bool wp = (rdesc & DESC040_W) != 0;
    if (update_um && !(rdesc & DESC040_U))
        desc_write(bus, rdesc_addr, rdesc | DESC040_U);

    // Pointer level: PI = LA[24:18], 128 four-byte descriptors per table.
    uint32_t pdesc_addr = (rdesc & 0xFFFFFE00u) | (((la >> 18) & 0x7Fu) << 2);
    uint32_t pdesc = desc_read(bus, pdesc_addr);
    if ((pdesc & DESC040_UDT_MASK) < 2)
        return r;
    wp |= (pdesc & DESC040_W) != 0;
    if (update_um && !(pdesc & DESC040_U))
        desc_write(bus, pdesc_addr, pdesc | DESC040_U);

    // Page level: PGI = LA[17:12] (4K, 64 entries) or LA[17:13] (8K, 32).
    uint32_t table_mask = page8k ? 0xFFFFFF80u : 0xFFFFFF00u;
    uint32_t pgi = page8k ? ((la >> 13) & 0x1Fu) : ((la >> 12) & 0x3Fu);
    uint32_t pgdesc_addr = (pdesc & table_mask) | (pgi << 2);
    uint32_t pgdesc = desc_read(bus, pgdesc_addr);

    uint32_t pdt = pgdesc & 3u;
    if (pdt == 2) {
        // Indirect descriptor: bits 31:2 point at the real page descriptor,
        // which must itself be resident (indirect-to-indirect is invalid).
        pgdesc_addr = pgdesc & 0xFFFFFFFCu;
        pgdesc = desc_read(bus, pgdesc_addr);
        pdt = pgdesc & 3u;
        if (pdt == 0 || pdt == 2)
            return r;
    } else if (pdt == 0) {
        return r;
    }

    wp |= (pgdesc & DESC040_W) != 0;
    bool s_only = (pgdesc & DESC040_S) != 0;
    bool modified = (pgdesc & DESC040_M) != 0;

    if (update_um) {
        uint32_t new_desc = pgdesc | DESC040_U;
        // M is set before a write completes, except on write-protect or
        // supervisor violations (MC68040UM §3.2.2.3).
        if (write && !wp && !(s_only && !supervisor))
            new_desc |= DESC040_M;
        if (new_desc != pgdesc)
            desc_write(bus, pgdesc_addr, new_desc);
        modified = (new_desc & DESC040_M) != 0;
    }

    r.physical_addr = page8k ? ((pgdesc & 0xFFFFE000u) | (la & 0x1FFFu)) : ((pgdesc & 0xFFFFF000u) | (la & 0xFFFu));
    r.valid = true;
    r.write_protected = wp;
    r.supervisor_only = s_only;
    r.modified = modified;

    // MMUSR image: PA plus the page attributes (G, U1/U0, S, CM carried
    // through at their descriptor positions), M, W, R.
    r.mmusr = (r.physical_addr & MMUSR040_PA_MASK) | (pgdesc & (DESC040_G | 0x300u | DESC040_S | 0x60u)) |
              (modified ? MMUSR040_M : 0) | (wp ? MMUSR040_W : 0) | MMUSR040_R;
    return r;
}

// ============================================================================
// TLB-miss handling (dispatched from mmu_handle_fault)
// ============================================================================

// Mirror of mmu.c's fault epilogue: after a fill attempt, if the SoA entry
// stayed zero the physical page is a device, unmapped, or logpointed.  Only
// clearly-garbage physical addresses (past the RAM controller's reach and
// below the ROM window) bus-error; device windows dispatch in memory.c.
static inline bool m040_fault_epilogue(struct mmu_state *bus, uint32_t emu_page, uint32_t phys_page, bool write) {
    uint32_t page_index = emu_page >> PAGE_SHIFT;
    if ((int)page_index < g_page_count) {
        uintptr_t *active = write ? g_active_write : g_active_read;
        if (active && active[page_index] == 0) {
            if (phys_page >= bus->ram_size_max && phys_page < bus->rom_phys_base)
                return false;
        }
    }
    return true;
}

bool mmu040_handle_fault(struct mmu_state *bus, uint32_t logical_addr, bool write, bool supervisor) {
    mmu040_state_t *mmu = bus ? bus->m040 : NULL;
    if (!mmu || !mmu->enabled)
        return false;

    uint32_t emu_page = logical_addr & ~(uint32_t)PAGE_MASK;

    // Transparent translation: identity mapping, no walk.
    uint32_t tt = ttr_lookup(mmu, logical_addr, supervisor);
    if (tt) {
        if (write && TT040_W(tt)) {
            g_bus_error_is_pmmu = true; // protection fault: retry semantics
            return false;
        }
        mmu_fill_soa_page(bus, emu_page, emu_page, supervisor, !supervisor, !TT040_W(tt));
        // Unmapped physical under a TTR: bus-error only inside the NuBus
        // window on reads; silent otherwise (same policy as the PMMU path).
        if (!write) {
            uint32_t page_index = emu_page >> PAGE_SHIFT;
            if ((int)page_index < g_page_count && g_supervisor_read && g_supervisor_read[page_index] == 0 &&
                logical_addr >= bus->nubus_berr_start && logical_addr <= bus->nubus_berr_end) {
                g_bus_error_is_pmmu = false; // bus timeout: skip semantics
                return false;
            }
        }
        return true;
    }

    // Three-level walk with architectural U/M updates.
    m040_walk_result_t r = m040_walk(mmu, bus, logical_addr, write, supervisor, true);

    // Publish the walk's MMUSR, mirroring the PMMU path: kernels read the
    // status from their bus-error handlers without issuing PTEST.
    mmu->mmusr = r.valid ? r.mmusr : 0;

    if (!r.valid) {
        g_bus_error_is_pmmu = true; // nonresident: handler pages in, retries
        return false;
    }
    if (r.supervisor_only && !supervisor) {
        g_bus_error_is_pmmu = true;
        return false;
    }
    if (r.write_protected && write) {
        g_bus_error_is_pmmu = true;
        return false;
    }

    uint32_t phys_page = r.physical_addr & ~(uint32_t)PAGE_MASK;

    // Write-array fill policy: only once the page is marked modified (which
    // this access establishes when it is a write).  A read fault on a clean
    // page leaves the write SoA empty so the first write re-faults and the
    // walk sets M — the architectural modified-bit protocol, which VM's
    // dirty-page accounting relies on.
    bool writable = !r.write_protected && (write || r.modified);

    // URP == SRP means both modes share one table (the common Mac OS setup):
    // one walk serves both SoAs.  Split roots fill only the walked FC.
    bool shared_roots = (mmu->urp == mmu->srp);
    bool fill_super = supervisor || shared_roots;
    bool fill_user = (!supervisor || shared_roots) && !r.supervisor_only;

    mmu_fill_soa_page(bus, emu_page, phys_page, fill_super, fill_user, writable);
    return m040_fault_epilogue(bus, emu_page, phys_page, write);
}

// Side-effect-free translation for debugger reads and memory.c dispatch
// decisions (no SoA fill, no U/M updates).
bool mmu040_translate_checked(struct mmu_state *bus, uint32_t logical_addr, bool supervisor, uint32_t *pa_out) {
    mmu040_state_t *mmu = bus ? bus->m040 : NULL;
    if (!mmu || !mmu->enabled) {
        if (pa_out)
            *pa_out = logical_addr;
        return true; // MMU off: identity
    }
    if (ttr_lookup(mmu, logical_addr, supervisor)) {
        if (pa_out)
            *pa_out = logical_addr;
        return true;
    }
    m040_walk_result_t r = m040_walk(mmu, bus, logical_addr, false, supervisor, false);
    if (pa_out)
        *pa_out = r.valid ? r.physical_addr : logical_addr;
    return r.valid;
}

// PTESTR/PTESTW (An): load MMUSR with the translation result.  The address
// space is selected by DFC (bit 2 = supervisor); the walk performs the same
// U/M updates as a normal table search (MC68040UM §3.4.2).
void mmu040_ptest(mmu040_state_t *mmu, uint32_t addr, bool write, uint32_t fc) {
    if (!mmu)
        return;
    bool supervisor = (fc & 4u) != 0;
    if (!mmu->enabled) {
        mmu->mmusr = (addr & MMUSR040_PA_MASK) | MMUSR040_R;
        return;
    }
    if (ttr_lookup(mmu, addr, supervisor)) {
        // TTR hit: transparent + resident (MC68040UM Table 3-?; PA = LA).
        mmu->mmusr = (addr & MMUSR040_PA_MASK) | MMUSR040_T | MMUSR040_R;
        return;
    }
    if (!mmu->bus) {
        LOG(1, "mmu040: PTEST with translation enabled but no bus attachment (addr=%08X)", addr);
        mmu->mmusr = 0;
        return;
    }
    m040_walk_result_t r = m040_walk(mmu, mmu->bus, addr, write, supervisor, true);
    mmu->mmusr = r.valid ? r.mmusr : 0; // R clear on nonresident
}
