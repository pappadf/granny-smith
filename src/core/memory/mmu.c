// SPDX-License-Identifier: MIT
// mmu.c
// 68030 PMMU (Paged Memory Management Unit) implementation.
// Lazy-fill TLB using SoA pointer arrays: on a TLB miss the slow path
// calls mmu_handle_fault() which walks the guest translation tables and
// populates the SoA entry so subsequent accesses hit the fast path.

#include "mmu.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global MMU state pointer (NULL for 68000 machines)
mmu_state_t *g_mmu = NULL;

// ============================================================================
// TLB population tracking (for fast invalidation)
// ============================================================================

// Instead of zeroing all 1M+ page entries on every TLB invalidation (32 MB of
// memset on a 32-bit address space), track which pages have been populated and
// only zero those.  Typical working sets are ~2000-3000 pages (8-12 MB of RAM
// + ROM + VRAM), reducing invalidation cost from O(address_space) to O(working_set).
#define TLB_TRACK_MAX 8192 // max tracked pages before fallback to full memset

static uint32_t g_tlb_track[TLB_TRACK_MAX]; // populated page indices
static int g_tlb_track_count = 0; // entries in tracking list
// Start in overflow mode so the first invalidation (after memory_init
// populates entries without tracking) does a full memset.  Subsequent
// invalidations use the fast tracked path.
static bool g_tlb_track_overflow = true;

// Record that a page index has been populated in the SoA TLB arrays
static inline void tlb_track_page(uint32_t page_index) {
    if (g_tlb_track_overflow)
        return; // already in fallback mode
    if (g_tlb_track_count >= TLB_TRACK_MAX) {
        g_tlb_track_overflow = true;
        return;
    }
    g_tlb_track[g_tlb_track_count++] = page_index;
}

// ============================================================================
// Helpers: physical memory access during table walks
// ============================================================================

// Read a 32-bit big-endian value from physical RAM at the given address.
// Used during table walks to fetch descriptors from the guest's page tables.
// ROM region addresses are wrapped modulo rom_size to handle mirroring.
static uint32_t phys_read32(mmu_state_t *mmu, uint32_t phys_addr) {
    if (phys_addr + 4 <= mmu->physical_ram_size) {
        return LOAD_BE32(mmu->physical_ram + phys_addr);
    }
    // Check if address falls in ROM mirror region and wrap to actual ROM data
    if (mmu->physical_rom && phys_addr >= mmu->rom_phys_base && phys_addr < mmu->rom_region_end) {
        uint32_t offset = (phys_addr - mmu->rom_phys_base) % mmu->physical_rom_size;
        if (offset + 4 <= mmu->physical_rom_size)
            return LOAD_BE32(mmu->physical_rom + offset);
    }
    return 0; // unmapped physical address
}

// Resolve a physical address to a host pointer (RAM, ROM, or VRAM).
// Returns NULL if the physical address is not backed by host memory.
// ROM addresses are wrapped modulo rom_size to handle mirroring.
static uint8_t *phys_to_host(mmu_state_t *mmu, uint32_t phys_addr) {
    if (phys_addr < mmu->physical_ram_size)
        return mmu->physical_ram + phys_addr;
    if (mmu->physical_rom && phys_addr >= mmu->rom_phys_base && phys_addr < mmu->rom_region_end) {
        uint32_t offset = (phys_addr - mmu->rom_phys_base) % mmu->physical_rom_size;
        return mmu->physical_rom + offset;
    }
    if (mmu->physical_vram && phys_addr >= mmu->vram_phys_base &&
        phys_addr < mmu->vram_phys_base + mmu->physical_vram_size)
        return mmu->physical_vram + (phys_addr - mmu->vram_phys_base);
    // VROM region (read-only video declaration ROM)
    if (mmu->physical_vrom && phys_addr >= mmu->vrom_phys_base &&
        phys_addr < mmu->vrom_phys_base + mmu->physical_vrom_size)
        return mmu->physical_vrom + (phys_addr - mmu->vrom_phys_base);
    // Alternate VRAM address (page-table-mapped I/O space)
    if (mmu->vram_phys_alt && mmu->physical_vram && phys_addr >= mmu->vram_phys_alt &&
        phys_addr < mmu->vram_phys_alt + mmu->physical_vram_size)
        return mmu->physical_vram + (phys_addr - mmu->vram_phys_alt);
    // Alternate VROM address (page-table-mapped I/O space)
    if (mmu->vrom_phys_alt && mmu->physical_vrom && phys_addr >= mmu->vrom_phys_alt &&
        phys_addr < mmu->vrom_phys_alt + mmu->physical_vrom_size)
        return mmu->physical_vrom + (phys_addr - mmu->vrom_phys_alt);
    return NULL;
}

// Check if physical address is in writable RAM or VRAM (not ROM)
static bool phys_is_writable(mmu_state_t *mmu, uint32_t phys_addr) {
    if (phys_addr < mmu->physical_ram_size)
        return true;
    // ROM mirror region is read-only
    if (mmu->physical_rom && phys_addr >= mmu->rom_phys_base && phys_addr < mmu->rom_region_end)
        return false;
    if (mmu->physical_vram && phys_addr >= mmu->vram_phys_base &&
        phys_addr < mmu->vram_phys_base + mmu->physical_vram_size)
        return true;
    // VROM is read-only
    if (mmu->physical_vrom && phys_addr >= mmu->vrom_phys_base &&
        phys_addr < mmu->vrom_phys_base + mmu->physical_vrom_size)
        return false;
    // Alternate VRAM address is writable
    if (mmu->vram_phys_alt && mmu->physical_vram && phys_addr >= mmu->vram_phys_alt &&
        phys_addr < mmu->vram_phys_alt + mmu->physical_vram_size)
        return true;
    // Alternate VROM address is read-only
    if (mmu->vrom_phys_alt && mmu->physical_vrom && phys_addr >= mmu->vrom_phys_alt &&
        phys_addr < mmu->vrom_phys_alt + mmu->physical_vrom_size)
        return false;
    return false;
}

// ============================================================================
// Transparent Translation
// ============================================================================

// Check if a single TT register matches the given access
static bool tt_matches(uint32_t tt, uint32_t addr, bool write, bool supervisor) {
    if (!TT_ENABLE(tt))
        return false;

    // Function code matching: build the FC value for this access
    // FC: 001=user data, 010=user program, 101=super data, 110=super program
    // Simplified: supervisor=1 → FC bit 2 set
    uint32_t fc = supervisor ? 5 : 1; // data space
    uint32_t fc_base = TT_FC_BASE(tt);
    uint32_t fc_mask = TT_FC_MASK(tt);
    // FC matches if (fc & ~fc_mask) == (fc_base & ~fc_mask)
    if ((fc & ~fc_mask) != (fc_base & ~fc_mask))
        return false;

    // Address matching: compare upper 8 bits with base, masked by mask field
    uint32_t addr_upper = (addr >> 24) & 0xFF;
    uint32_t tt_base = TT_BASE(tt);
    uint32_t tt_mask = TT_MASK(tt);
    if ((addr_upper & ~tt_mask) != (tt_base & ~tt_mask))
        return false;

    // R/W field matching (if enabled)
    if (TT_RW(tt)) {
        // RWM: 0=match writes only, 1=match reads only
        bool match_reads = TT_RWM(tt);
        if (match_reads && write)
            return false;
        if (!match_reads && !write)
            return false;
    }

    return true;
}

// Check TT0 and TT1 transparent translation registers
bool mmu_check_tt(mmu_state_t *mmu, uint32_t addr, bool write, bool supervisor) {
    if (!mmu)
        return false;
    return tt_matches(mmu->tt0, addr, write, supervisor) || tt_matches(mmu->tt1, addr, write, supervisor);
}

// ============================================================================
// Table Walk
// ============================================================================

// Walk the guest's PMMU translation descriptor table tree.
// Resolves a logical address to a physical address + permission bits.
static mmu_walk_result_t mmu_table_walk(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor) {
    (void)write; // write check handled by caller after walk
    mmu_walk_result_t result = {0};
    result.valid = false;
    result.mmusr = 0;

    uint32_t tc = mmu->tc;

    // Extract TC configuration fields
    uint32_t is = TC_IS(tc); // initial shift
    uint32_t ti[4]; // table index sizes for levels A, B, C, D
    ti[0] = TC_TIA(tc);
    ti[1] = TC_TIB(tc);
    ti[2] = TC_TIC(tc);
    ti[3] = TC_TID(tc);

    // Select root pointer based on SRE bit and supervisor mode
    uint64_t root_ptr;
    if (TC_SRE(tc) && supervisor)
        root_ptr = mmu->srp;
    else
        root_ptr = mmu->crp;

    // Root pointer: upper 32 bits contain flags, lower 32 bits contain address
    uint32_t root_upper = (uint32_t)(root_ptr >> 32);
    uint32_t root_lower = (uint32_t)(root_ptr & 0xFFFFFFFF);

    // DT from root pointer (bits 1:0 of upper word)
    uint32_t root_dt = root_upper & 3;
    if (root_dt == DESC_DT_INVALID) {
        result.mmusr |= MMUSR_I;
        return result;
    }

    // Descriptor size from root DT: 2=short (4 bytes), 3=long (8 bytes)
    bool long_desc = (root_dt == DESC_DT_TABLE8);

    // Table base address from root pointer (lower 32 bits, bits 31:2)
    uint32_t table_addr = root_lower & 0xFFFFFFFC;

    // Current bit position in logical address (start after IS bits)
    uint32_t bit_pos = 32 - is;
    int levels_walked = 0;

    // Walk through up to 4 table levels (A, B, C, D)
    for (int level = 0; level < 4; level++) {
        uint32_t index_bits = ti[level];
        if (index_bits == 0)
            continue; // skip empty levels

        // Extract index from logical address
        bit_pos -= index_bits;
        uint32_t index = (logical_addr >> bit_pos) & ((1u << index_bits) - 1);

        // Fetch descriptor from physical memory
        uint32_t desc_addr = table_addr + index * (long_desc ? 8 : 4);
        uint32_t desc = phys_read32(mmu, desc_addr);
        levels_walked++;

        // Check descriptor type
        uint32_t dt = desc & 3;

        if (dt == DESC_DT_INVALID) {
            // Invalid descriptor → bus error
            result.mmusr |= MMUSR_I;
            result.mmusr |= (levels_walked & 7);
            return result;
        }

        if (dt == DESC_DT_PAGE) {
            // Page descriptor (early termination) — translation complete
            // The remaining address bits below bit_pos form the page offset
            uint32_t page_mask = (1u << bit_pos) - 1;
            uint32_t phys_base = desc & ~page_mask & 0xFFFFFFFC;

            result.physical_addr = phys_base | (logical_addr & page_mask);
            result.valid = true;
            result.write_protected = (desc >> 2) & 1; // W bit
            result.modified = (desc >> 4) & 1; // M bit
            // S bit: only in long-format descriptors (bit 8)
            if (long_desc)
                result.supervisor_only = (desc >> 8) & 1;

            // Build MMUSR
            if (result.write_protected)
                result.mmusr |= MMUSR_W;
            if (result.modified)
                result.mmusr |= MMUSR_M;
            if (result.supervisor_only)
                result.mmusr |= MMUSR_S;
            result.mmusr |= (levels_walked & 7);
            return result;
        }

        // Table descriptor (short=DT2, long=DT3) — follow to next level
        table_addr = desc & 0xFFFFFFFC;
        long_desc = (dt == DESC_DT_TABLE8);
    }

    // Ran out of table levels without finding a page descriptor.
    // This shouldn't happen with a correctly configured TC, but treat as invalid.
    result.mmusr |= MMUSR_I;
    result.mmusr |= (levels_walked & 7);
    return result;
}

// ============================================================================
// TLB Fill
// ============================================================================

// Fill the SoA arrays for a single page based on walk result and permissions.
static void mmu_fill_soa_entry(mmu_state_t *mmu, uint32_t logical_page, uint32_t physical_page, bool supervisor_only,
                               bool write_protected) {
    // Get host pointer for the physical page
    uint8_t *host_ptr = phys_to_host(mmu, physical_page);
    if (!host_ptr)
        return; // unmapped physical address — leave SoA entry as zero

    bool host_writable = phys_is_writable(mmu, physical_page);
    uint32_t page_index = logical_page >> PAGE_SHIFT;
    if ((int)page_index >= g_page_count)
        return;

    // Compute adjusted base: host_ptr points to start of physical page,
    // but we want (uintptr_t)(base + logical_addr) to yield the host address.
    uintptr_t adjusted = (uintptr_t)host_ptr - logical_page;

    // Track this page for fast invalidation
    tlb_track_page(page_index);

    // Supervisor read: always populated for valid pages
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;

    // Supervisor write: only if not write-protected and physical RAM
    if (g_supervisor_write && !write_protected && host_writable)
        g_supervisor_write[page_index] = adjusted;

    // User access: only if not supervisor-only
    if (!supervisor_only) {
        if (g_user_read)
            g_user_read[page_index] = adjusted;
        if (g_user_write && !write_protected && host_writable)
            g_user_write[page_index] = adjusted;
    }
}

// ============================================================================
// Public API
// ============================================================================

// Create MMU state
mmu_state_t *mmu_init(uint8_t *physical_ram, uint32_t ram_size, uint32_t ram_size_max, uint8_t *physical_rom,
                      uint32_t rom_size, uint32_t rom_phys_base, uint32_t rom_region_end) {
    mmu_state_t *mmu = (mmu_state_t *)calloc(1, sizeof(mmu_state_t));
    if (!mmu)
        return NULL;

    mmu->physical_ram = physical_ram;
    mmu->physical_ram_size = ram_size;
    mmu->ram_size_max = ram_size_max;
    mmu->physical_rom = physical_rom;
    mmu->physical_rom_size = rom_size;
    mmu->rom_phys_base = rom_phys_base;
    mmu->rom_region_end = rom_region_end;
    mmu->enabled = false;

    return mmu;
}

// Destroy MMU state
void mmu_delete(mmu_state_t *mmu) {
    if (!mmu)
        return;
    if (g_mmu == mmu)
        g_mmu = NULL;
    free(mmu);
}

// Register a VRAM region so table walks and TT matches can resolve it
void mmu_register_vram(mmu_state_t *mmu, uint8_t *vram, uint32_t phys_base, uint32_t size) {
    if (!mmu)
        return;
    mmu->physical_vram = vram;
    mmu->vram_phys_base = phys_base;
    mmu->physical_vram_size = size;
}

// Register a VROM region so table walks and TT matches can resolve it
void mmu_register_vrom(mmu_state_t *mmu, uint8_t *vrom, uint32_t phys_base, uint32_t size) {
    if (!mmu)
        return;
    mmu->physical_vrom = vrom;
    mmu->vrom_phys_base = phys_base;
    mmu->physical_vrom_size = size;
}

// Invalidate the software TLB.  Uses the tracking list to zero only
// populated entries — typically ~2000-3000 pages vs 1M+ for a full memset.
// Falls back to full memset if the tracking list overflowed.
void mmu_invalidate_tlb(mmu_state_t *mmu) {
    if (g_tlb_track_overflow) {
        // Tracking overflowed — fall back to zeroing everything
        size_t sz = (size_t)g_page_count * sizeof(uintptr_t);
        if (g_supervisor_read)
            memset(g_supervisor_read, 0, sz);
        if (g_supervisor_write)
            memset(g_supervisor_write, 0, sz);
        if (g_user_read)
            memset(g_user_read, 0, sz);
        if (g_user_write)
            memset(g_user_write, 0, sz);
    } else {
        // Fast path: zero only pages that were actually populated
        for (int i = 0; i < g_tlb_track_count; i++) {
            uint32_t p = g_tlb_track[i];
            if (g_supervisor_read)
                g_supervisor_read[p] = 0;
            if (g_supervisor_write)
                g_supervisor_write[p] = 0;
            if (g_user_read)
                g_user_read[p] = 0;
            if (g_user_write)
                g_user_write[p] = 0;
        }
    }

    // Reset tracking state
    g_tlb_track_count = 0;
    g_tlb_track_overflow = false;

    // When the MMU is disabled, repopulate SoA from the AoS cold-path table
    // so that direct physical memory mappings (RAM, ROM, VRAM) remain usable.
    if (!mmu || !mmu->enabled) {
        for (int p = 0; p < g_page_count; p++) {
            page_entry_t *pe = &g_page_table[p];
            if (pe->host_base && !pe->dev) {
                uint32_t guest_base = (uint32_t)p << PAGE_SHIFT;
                uintptr_t adjusted = (uintptr_t)pe->host_base - guest_base;
                // Track each page we populate
                tlb_track_page(p);
                if (g_supervisor_read)
                    g_supervisor_read[p] = adjusted;
                if (g_user_read)
                    g_user_read[p] = adjusted;
                if (pe->writable) {
                    if (g_supervisor_write)
                        g_supervisor_write[p] = adjusted;
                    if (g_user_write)
                        g_user_write[p] = adjusted;
                }
            }
        }
    }
}

// Handle a TLB miss: perform table walk or TT check, fill SoA entry.
bool mmu_handle_fault(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor) {
    if (!mmu || !mmu->enabled)
        return false;

    // Align to emulator page boundary for SoA entry
    uint32_t emu_page = logical_addr & ~(uint32_t)PAGE_MASK;

    // Check transparent translation first
    if (mmu_check_tt(mmu, logical_addr, write, supervisor)) {
        // TT match: identity mapping (logical = physical)
        mmu_fill_soa_entry(mmu, emu_page, emu_page, false, false);
        // If phys_to_host returned NULL (unmapped physical), the SoA entry
        // stays zero.  For reads, only bus error within the configured NuBus
        // expansion slot range (e.g. $F9-$FD on SE/30).  Outside that range,
        // return 0 silently — the hardware doesn't bus error for internal
        // pseudo-slots like slot $F.  Writes are always silently dropped.
        if (!write) {
            uint32_t page_index = emu_page >> PAGE_SHIFT;
            if ((int)page_index < g_page_count && g_supervisor_read && g_supervisor_read[page_index] == 0 &&
                logical_addr >= mmu->nubus_berr_start && logical_addr <= mmu->nubus_berr_end)
                return false; // unmapped physical read in NuBus range → bus error
        }
        return true;
    }

    // Perform table walk
    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, write, supervisor);

    if (!result.valid)
        return false; // invalid descriptor → caller should raise bus error

    // Check supervisor-only restriction
    if (result.supervisor_only && !supervisor)
        return false; // user accessing supervisor page → bus error

    // Check write protection
    if (result.write_protected && write)
        return false; // write to write-protected page → bus error

    // Fill the SoA entry for this emulator page.
    // The physical address from the walk gives us the physical page base.
    // We need to map the emulator's 4KB page granularity.
    uint32_t phys_page = result.physical_addr & ~(uint32_t)PAGE_MASK;
    mmu_fill_soa_entry(mmu, emu_page, phys_page, result.supervisor_only, result.write_protected);

    // If phys_to_host returned NULL (unmapped physical), the SoA entry
    // stays zero.  On real hardware, the physical bus access would fail
    // (no device responds) and generate a bus error.
    //
    // However, Mac OS legitimately probes RAM expansion addresses (e.g.
    // $00F80000, $80000000) via page table entries and expects to read
    // $FF without faulting.  Only bus error for physical addresses that
    // are clearly garbage — outside all known hardware regions.  The gap
    // $60000000-$EFFFFFFF has no hardware on any 68k Mac.
    uint32_t page_index = emu_page >> PAGE_SHIFT;
    if ((int)page_index < g_page_count) {
        uintptr_t *active = write ? g_active_write : g_active_read;
        if (active && active[page_index] == 0) {
            // On real hardware, unmapped physical addresses cause bus errors.
            // However, our deferred bus error mechanism is incompatible with
            // the ROM's bail-out handler for data probes (the probe instruction
            // completes before the bus error fires, breaking the expected flow).
            // So we only bus error for addresses far beyond any hardware range.
            // For closer ranges (e.g., $006DB000 from corrupted page tables),
            // the f_trap handler detects unmapped instruction fetches separately.
            if (phys_page >= mmu->ram_size_max && phys_page < mmu->rom_phys_base)
                return false;
        }
    }

    return true;
}

// PTEST: test address translation without faulting
uint16_t mmu_test_address(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor) {
    if (!mmu)
        return MMUSR_I;

    // Check transparent translation first
    if (mmu->enabled && mmu_check_tt(mmu, logical_addr, write, supervisor)) {
        mmu->mmusr = MMUSR_T;
        return MMUSR_T;
    }

    if (!mmu->enabled) {
        // MMU disabled: identity mapping, no faults
        mmu->mmusr = 0;
        return 0;
    }

    // Perform table walk (without modifying SoA entries)
    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, write, supervisor);

    mmu->mmusr = result.mmusr;
    return result.mmusr;
}

// Debug-only translation: resolve logical address to physical without side effects.
// Returns the physical address, or logical_addr if translation fails.
uint32_t mmu_translate_debug(mmu_state_t *mmu, uint32_t logical_addr) {
    if (!mmu || !mmu->enabled)
        return logical_addr;

    // Check transparent translation first (identity mapping)
    if (mmu_check_tt(mmu, logical_addr, false, true))
        return logical_addr;

    // Perform table walk (read-only, supervisor mode)
    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, false, true);

    if (result.valid)
        return result.physical_addr;

    // Translation failed — return logical address as fallback
    return logical_addr;
}

// Read a byte from physical memory for debug commands (P: prefix).
// Returns 0 for unmapped physical addresses.
uint8_t mmu_read_physical_uint8(mmu_state_t *mmu, uint32_t phys_addr) {
    if (!mmu)
        return 0;
    uint8_t *host = phys_to_host(mmu, phys_addr);
    if (!host)
        return 0;
    return *host;
}

// Read a 16-bit big-endian value from physical memory for debug commands.
uint16_t mmu_read_physical_uint16(mmu_state_t *mmu, uint32_t phys_addr) {
    if (!mmu)
        return 0;
    uint8_t *host = phys_to_host(mmu, phys_addr);
    if (!host)
        return 0;
    return (uint16_t)(host[0] << 8 | host[1]);
}

// Read a 32-bit big-endian value from physical memory for debug commands.
uint32_t mmu_read_physical_uint32(mmu_state_t *mmu, uint32_t phys_addr) {
    if (!mmu)
        return 0;
    return phys_read32(mmu, phys_addr);
}
