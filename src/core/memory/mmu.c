// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// mmu.c
// 68030 PMMU (Paged Memory Management Unit) implementation.
// Lazy-fill TLB using SoA pointer arrays: on a TLB miss the slow path
// calls mmu_handle_fault() which walks the guest translation tables and
// populates the SoA entry so subsequent accesses hit the fast path.

#include "mmu.h"
#include "log.h"
#include "memory.h"
#include "mmu040.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mmu");

// Global MMU state pointer (NULL for 68000 machines)
mmu_state_t *g_mmu = NULL;

// ============================================================================
// ATC-style block-descriptor cache
// ============================================================================

// The guest's tables use early-termination block descriptors covering large
// ranges (e.g. one level-A descriptor covering 32 MB).  The real 68030 ATC
// caches one descriptor for the whole covered range and never re-walks; the
// old emulator approximation eagerly materialised every covered 4 KB page
// into the SoA arrays (~9,200 entries per walk), which dominated steady-state
// host time under System 6's per-VBL _SwapMMUMode invalidation storm.
// Instead, cache the walked descriptor itself; on a later fault inside the
// covered range, fill only the touched 4 KB page from the cached descriptor —
// no re-walk.
// The cache itself is the MMU's (mmu_state_t.atc): one per machine, like the
// hardware's.

// Drop every cached block descriptor.  Runs wherever a real ATC dies: any
// invalidation that actually zeroes the SoA (PMOVE to TC/CRP/SRP/TT without
// FD, PFLUSH variants, machine reset).  The FD (flush-disable) PMOVE forms
// skip mmu_invalidate_tlb entirely, so cached blocks survive them — exactly
// the hardware contract A/UX's "clear root after PMOVE, rely on ATC
// residency" sequence needs.
static void atc_flush(mmu_state_t *mmu) {
    if (!mmu)
        return;
    memset(mmu->atc, 0, sizeof(mmu->atc));
    mmu->atc_next = 0;
}

// Find the cached block covering logical_addr for this FC class, if any.
// When TC.SRE=0 a single walk serves both FC classes (super and user share
// the CRP), so the FC recorded at walk time doesn't restrict the hit.
static inline atc_block_t *atc_probe(mmu_state_t *mmu, uint32_t logical_addr, bool supervisor) {
    bool sre_split = TC_SRE(mmu->tc) != 0;
    for (int i = 0; i < ATC_BLOCKS; i++) {
        atc_block_t *b = &mmu->atc[i];
        if (!b->valid)
            continue;
        if ((logical_addr & b->log_mask) != b->log_base)
            continue;
        if (sre_split && b->fc_super != supervisor)
            continue;
        return b;
    }
    return NULL;
}

// Invalidate any cached block covering logical_addr for this FC class.
// Called before recording a fresh walk so a re-walked range (PLOAD, or a
// mapping the guest reshaped from block to page tables) can never leave a
// stale duplicate behind.
static void atc_invalidate_covering(mmu_state_t *mmu, uint32_t logical_addr, bool supervisor) {
    atc_block_t *b;
    while ((b = atc_probe(mmu, logical_addr, supervisor)) != NULL)
        b->valid = false;
}

// Record a successful walk's early-termination descriptor in the block cache.
static void atc_record(mmu_state_t *mmu, uint32_t log_base, uint32_t log_mask, uint32_t phys_base, bool supervisor_only,
                       bool write_protected, bool modified, bool fc_super) {
    atc_block_t *b = &mmu->atc[mmu->atc_next];
    mmu->atc_next = (mmu->atc_next + 1) % ATC_BLOCKS;
    b->log_base = log_base;
    b->log_mask = log_mask;
    b->phys_base = phys_base;
    b->supervisor_only = supervisor_only;
    b->write_protected = write_protected;
    b->modified = modified;
    b->fc_super = fc_super;
    b->valid = true;
}

// ============================================================================
// Helpers: physical memory access during table walks
// ============================================================================

// Read a 32-bit big-endian value from physical RAM at the given address.
// Used during table walks to fetch descriptors from the guest's page tables.
// ROM region addresses are wrapped modulo rom_size to handle mirroring.
//
// Forced inline: this is on the per-fault hot path (mmu_table_walk fetches
// 2–3 descriptors per fault).  Out-of-line, it costs ~9% of SE/30 boot time
// in function-call overhead alone (gprof).
static inline __attribute__((always_inline)) uint32_t phys_read32(mmu_state_t *mmu, uint32_t phys_addr) {
    // Two-bank RAM (e.g. IIsi): resolve through the bank windows.  Page tables
    // the walker reads from live in Bank B (system RAM), so this must cover it.
    if (mmu->ram_b_size) {
        if (phys_addr < mmu->ram_b_phys_base) {
            uint32_t off = phys_addr % mmu->ram_a_size;
            if (off <= mmu->ram_a_size - 4)
                return LOAD_BE32(mmu->physical_ram + off);
        } else if (phys_addr - mmu->ram_b_phys_base < mmu->ram_b_window) {
            uint32_t off = (phys_addr - mmu->ram_b_phys_base) % mmu->ram_b_size;
            if (off <= mmu->ram_b_size - 4)
                return LOAD_BE32(mmu->physical_ram_b + off);
        }
        // fall through to ROM mirror handling below
    }
    // Subtract before adding so a phys_addr near UINT32_MAX can't wrap past
    // the bound check.
    else if (mmu->physical_ram_size >= 4 && phys_addr <= mmu->physical_ram_size - 4) {
        return LOAD_BE32(mmu->physical_ram + phys_addr);
    }
    // Check if address falls in ROM mirror region and wrap to actual ROM data
    if (mmu->physical_rom && phys_addr >= mmu->rom_phys_base && phys_addr < mmu->rom_region_end) {
        uint32_t offset = (phys_addr - mmu->rom_phys_base) % mmu->physical_rom_size;
        if (mmu->physical_rom_size >= 4 && offset <= mmu->physical_rom_size - 4)
            return LOAD_BE32(mmu->physical_rom + offset);
    }
    // Host-backed regions (card VRAM, declaration ROMs, aliases).  This was the
    // ONE resolver of the three that skipped them: phys_to_host and
    // phys_is_writable both scan the list, so a descriptor placed in NuBus VRAM
    // could be written by mmu_write_physical_uint8 and then read back here as
    // 0 -- DT = 0 -- which the walker reports as an invalid descriptor and the
    // CPU takes as a spurious bus error.  Bounded for the 4-byte read, which
    // the page-granular phys_to_host does not need to do.
    for (int i = 0; i < mmu->host_region_count; i++) {
        const mmu_host_region_t *r = &mmu->host_regions[i];
        uint32_t off = phys_addr - r->phys_base;
        if (phys_addr >= r->phys_base && r->size >= 4 && off <= r->size - 4)
            return LOAD_BE32(r->host + off);
    }
    return 0; // unmapped physical address
}

// Resolve a physical address to a host pointer (RAM, ROM, or VRAM), and
// report through *writable (when non-NULL) whether the backing is writable
// (RAM and writable host regions; never ROM).  Returns NULL -- and
// *writable false -- if the physical address is not backed by host memory.
// ROM addresses are wrapped modulo rom_size to handle mirroring.  One range
// table for both questions, so the pointer and the writability can never
// disagree about which region an address is in.
//
// Forced inline: hot-path called from mmu_fill_soa_page on every TLB miss.
// Out-of-line, it was the top gprof entry at ~13% of SE/30 boot time.
static inline __attribute__((always_inline)) uint8_t *phys_resolve(mmu_state_t *mmu, uint32_t phys_addr,
                                                                   bool *writable) {
    bool w = false;
    uint8_t *host = NULL;
    if (mmu->ram_b_size && phys_addr < mmu->ram_b_phys_base) {
        // Two physical RAM banks (Macintosh IIsi).  Bank A mirrors within
        // [0, ram_b_phys_base); Bank B mirrors within its 64 MB window.
        host = mmu->physical_ram + (phys_addr % mmu->ram_a_size);
        w = true;
    } else if (mmu->ram_b_size && phys_addr - mmu->ram_b_phys_base < mmu->ram_b_window) {
        host = mmu->physical_ram_b + ((phys_addr - mmu->ram_b_phys_base) % mmu->ram_b_size);
        w = true;
    } else if (!mmu->ram_b_size && phys_addr < mmu->physical_ram_size) {
        host = mmu->physical_ram + phys_addr;
        w = true;
    } else if (mmu->physical_rom && phys_addr >= mmu->rom_phys_base && phys_addr < mmu->rom_region_end) {
        // ROM mirror region: read-only
        host = mmu->physical_rom + (phys_addr - mmu->rom_phys_base) % mmu->physical_rom_size;
    } else {
        // Host-backed regions (card VRAM, declaration ROMs, aliases) in
        // registration order, each carrying its own writability.  Range
        // checks use (addr - base < size) so they don't wrap when base+size
        // would exceed UINT32_MAX (VROM at $FExxxxxx is close enough to flag).
        for (int i = 0; i < mmu->host_region_count; i++) {
            const mmu_host_region_t *r = &mmu->host_regions[i];
            if (phys_addr >= r->phys_base && (phys_addr - r->phys_base) < r->size) {
                host = r->host + (phys_addr - r->phys_base);
                w = r->writable;
                break;
            }
        }
    }
    if (writable)
        *writable = w;
    return host;
}

// Resolve a physical address to a host pointer; NULL if not host-backed.
static inline __attribute__((always_inline)) uint8_t *phys_to_host(mmu_state_t *mmu, uint32_t phys_addr) {
    return phys_resolve(mmu, phys_addr, NULL);
}

// Check if physical address is in writable RAM or VRAM (not ROM)
static inline __attribute__((always_inline)) bool phys_is_writable(mmu_state_t *mmu, uint32_t phys_addr) {
    bool w;
    phys_resolve(mmu, phys_addr, &w);
    return w;
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
    if (mmu->m040)
        return mmu040_check_tt(mmu->m040, addr, write, supervisor);
    return tt_matches(mmu->tt0, addr, write, supervisor) || tt_matches(mmu->tt1, addr, write, supervisor);
}

// ============================================================================
// Table Walk
// ============================================================================

// Name of a 68030 descriptor type code, for the walk trace.
static const char *mmu_dt_name(uint32_t dt) {
    static const char *const names[4] = {"invalid", "page", "table", "table"};
    return names[dt & 3];
}

// Walk the guest's PMMU translation descriptor table tree.
// Resolves a logical address to a physical address + permission bits.
// `update_um` selects whether the search maintains the architectural history
// bits in the guest's tables.  True for real translations and PLOAD; FALSE for
// PTEST, which M68000PRM states "alters neither the used or modified bits of
// the translation tables nor the address translation cache", and for the
// side-effect-free debug translators.  The 68040 walker takes the same flag
// but defaults the other way, so the two cannot share a default.
// `trace`, when non-NULL, records one step per root pointer and table level
// for the debugger's `walk` (mmu_trace.h); the real translations pass NULL.
// `root_ptr` is the root pointer the search starts from (`root_name` names it
// for the trace): the caller picks it, so a walk against a root other than the
// live CRP/SRP never has to modify the MMU state.
static mmu_walk_result_t mmu_table_walk_from(mmu_state_t *mmu, uint64_t root_ptr, const char *root_name,
                                             uint32_t logical_addr, bool write, bool supervisor, bool update_um,
                                             mmu_trace_t *trace) {
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

    // LIMIT, carried from the root pointer or the long-format descriptor just
    // followed.  It bounds the index into the table at the NEXT level, so it
    // cannot be checked where it is read.
    bool limit_active = false;
    bool limit_is_lower = false;
    uint32_t limit_value = 0;

    // Root pointer: upper 32 bits contain flags, lower 32 bits contain address
    uint32_t root_upper = (uint32_t)(root_ptr >> 32);
    uint32_t root_lower = (uint32_t)(root_ptr & 0xFFFFFFFF);

    // DT from root pointer (bits 1:0 of upper word)
    uint32_t root_dt = root_upper & 3;
    mmu_trace_step_t *ts = mmu_trace_step(trace, "root");
    if (ts) {
        mmu_trace_str(ts, "name", root_name);
        mmu_trace_hex(ts, "desc", root_upper);
        mmu_trace_hex(ts, "desc_lo", root_lower);
        mmu_trace_uint(ts, "dt", root_dt);
        mmu_trace_str(ts, "type", mmu_dt_name(root_dt));
    }
    if (root_dt == DESC_DT_INVALID) {
        result.mmusr |= MMUSR_I;
        result.page_size_bits = 32; // the whole space is invalid
        mmu_trace_str(ts, "reason", "invalid");
        mmu_trace_outcome(ts, "fault");
        return result;
    }

    // Descriptor size from root DT: 2=short (4 bytes), 3=long (8 bytes)
    bool long_desc = (root_dt == DESC_DT_TABLE8);

    // Table base address from root pointer (lower 32 bits, bits 31:4).
    // 68030 PMMU requires descriptor tables to be 16-byte aligned; bits 3:0
    // are reserved in the root and carry the WP (bit 2) and U (bit 3) flags
    // in nested short-format table descriptors.  Masking only bits 1:0 would
    // pick up WP as an address bit and shift the table base by 4 bytes — one
    // entry's worth — breaking every subsequent lookup by one index.
    uint32_t table_addr = root_lower & 0xFFFFFFF0;

    // The root pointer carries its own L/U + LIMIT, bounding the index into
    // the FIRST table (MC68030UM Figure 9-35: "LIMIT -- LIMIT ON TABLE INDEX
    // FOR THIS TABLE ADDRESS").  Seed the carried limit from it so level A is
    // checked like every level below.
    limit_active = true;
    limit_is_lower = (root_upper >> 31) & 1;
    limit_value = (root_upper >> 16) & 0x7FFF;
    if (ts) {
        mmu_trace_hex(ts, "next", table_addr);
        mmu_trace_uint(ts, "limit", limit_value);
        mmu_trace_bool(ts, "lower_limit", limit_is_lower);
        mmu_trace_outcome(ts, "next");
    }

    // Current bit position in logical address (start after IS bits)
    uint32_t bit_pos = 32 - is;
    int levels_walked = 0;

    // Protection accumulates across the WHOLE search, not just the leaf.
    // MC68030UM 9.5.5.4 (WRITE PROTECT): "When a table search encounters a WP
    // bit set in ANY table or page descriptor, the table search is completed
    // and an ATC descriptor ... is created with the WP bit set ... The WP bit
    // can be used to protect the entire area of memory defined in a branch of
    // the translation tree."  9.5.5.3 (SUPERVISOR ONLY) is the same shape for
    // S, with the added detail that only the LONG formats carry an S bit.
    // Taking either from the page descriptor alone let protection placed on a
    // POINTER table be bypassed by a permissive leaf, which defeats the point
    // of putting it there.  mmu040.c already accumulates this way.
    bool acc_wp = false;
    bool acc_s = false;

    // Walk through up to 4 table levels (A, B, C, D)
    for (int level = 0; level < 4; level++) {
        uint32_t index_bits = ti[level];
        if (index_bits == 0)
            continue; // skip empty levels

        // index_bits is from a 4-bit TC field so it's in [0, 15] — well clear
        // of the `1u << 32` UB threshold. Defensive guard so a future widening
        // of the field doesn't silently invoke UB.
        if (index_bits >= 32) {
            result.mmusr |= MMUSR_I;
            return result;
        }

        // Extract index from logical address
        bit_pos -= index_bits;
        uint32_t index = (logical_addr >> bit_pos) & ((1u << index_bits) - 1);
        ts = mmu_trace_step(trace, "level");
        if (ts) {
            static const char *const level_names[4] = {"A", "B", "C", "D"};
            mmu_trace_str(ts, "name", level_names[level]);
            mmu_trace_uint(ts, "index", index);
        }
        result.page_size_bits = bit_pos; // what this level's descriptor covers, if the walk stops here

        // Limit check on the index into THIS table, from the long-format
        // descriptor that pointed here.  MC68030UM: "When the L/U bit is set,
        // the limit is a lower limit, and an index less than the limit is out
        // of bounds.  When the L/U bit is zero, the limit is an upper limit,
        // and an index greater than the limit is out of bounds."  The field is
        // disabled by L/U = 1 with limit 0, or L/U = 0 with limit $7FFF, both
        // of which the comparisons below satisfy without a special case.
        //
        // On violation: "During a table search for a normal translation or a
        // PLOAD instruction, if a limit violation is detected, the ATC is
        // loaded with an entry having the bus error (B) bit set.  If a limit
        // violation is detected during a table search for a PTEST instruction,
        // the invalid (I) and limit (L) bits are set in the MMUSR."  MMUSR_L
        // was defined and never set by anything until now.
        if (limit_active && (limit_is_lower ? (index < limit_value) : (index > limit_value))) {
            result.mmusr |= MMUSR_I | MMUSR_L | MMUSR_B;
            result.mmusr |= (levels_walked & 7);
            if (ts) {
                mmu_trace_uint(ts, "limit", limit_value);
                mmu_trace_bool(ts, "lower_limit", limit_is_lower);
                mmu_trace_str(ts, "reason", "limit");
                mmu_trace_outcome(ts, "fault");
            }
            return result;
        }

        // Fetch descriptor from physical memory.
        // Short format: all fields (DT, flags, address) live in one 32-bit word.
        // Long format: upper 32 bits hold LIMIT/flags/DT; lower 32 bits hold
        // the table or page physical address.
        uint32_t desc_addr = table_addr + index * (long_desc ? 8 : 4);
        uint32_t desc_hi = phys_read32(mmu, desc_addr);
        uint32_t desc_lo = long_desc ? phys_read32(mmu, desc_addr + 4) : desc_hi;
        result.descriptor_addr = desc_addr; // track for PTEST's A-reg output
        levels_walked++;

        // DT is always in bits 1:0 of the first (upper, for long) word
        uint32_t dt = desc_hi & 3;
        if (ts) {
            mmu_trace_hex(ts, "addr", desc_addr);
            mmu_trace_hex(ts, "desc", desc_hi);
            if (long_desc)
                mmu_trace_hex(ts, "desc_lo", desc_lo);
            mmu_trace_uint(ts, "dt", dt);
            mmu_trace_str(ts, "type", mmu_dt_name(dt));
        }

        if (dt == DESC_DT_INVALID) {
            // Invalid descriptor → bus error
            result.mmusr |= MMUSR_I;
            result.mmusr |= (levels_walked & 7);
            mmu_trace_str(ts, "reason", "invalid");
            mmu_trace_outcome(ts, "fault");
            return result;
        }
        if (ts) {
            mmu_trace_bool(ts, "wp", ((desc_hi >> 2) & 1) != 0);
            mmu_trace_bool(ts, "u", ((desc_hi >> 3) & 1) != 0);
            if (long_desc)
                mmu_trace_bool(ts, "s", ((desc_hi >> 8) & 1) != 0);
        }

        // OR in this descriptor's protection bits.  Must happen before long_desc
        // is reassigned at the bottom of the loop: that reassignment describes
        // the NEXT level's format, not this one's.
        acc_wp |= ((desc_hi >> 2) & 1) != 0; // WP, both formats
        if (long_desc)
            acc_s |= ((desc_hi >> 8) & 1) != 0; // S, long format only

        // U (bit 3) is set on EVERY descriptor the search touches, pointer
        // tables included.  MC68030UM, descriptor field definitions: "This bit
        // is automatically set by the processor when a descriptor is accessed
        // in which the U bit is clear except after a supervisor violation is
        // detected ... Updates of the U bit are performed before the MC68030
        // allows a page to be accessed.  The processor never clears this bit."
        // The same text notes a pointer may have its U set for an address that
        // is denied at a lower level, which is why this is not deferred until
        // the walk is known to succeed.
        if (update_um && !(acc_s && !supervisor) && !((desc_hi >> 3) & 1))
            (void)mmu_write_physical_uint32(mmu, desc_addr, desc_hi | (1u << 3));

        if (dt == DESC_DT_PAGE) {
            // Page descriptor (early termination) — translation complete.
            // The remaining address bits below bit_pos form the page offset.
            // Guard against `1u << 32` UB if a degenerate TC (IS=0, no TI
            // levels) left bit_pos at 32 — treat as invalid translation.
            if (bit_pos >= 32) {
                result.mmusr |= MMUSR_I;
                result.mmusr |= (levels_walked & 7);
                mmu_trace_str(ts, "reason", "invalid");
                mmu_trace_outcome(ts, "fault");
                return result;
            }
            uint32_t page_mask = (1u << bit_pos) - 1;
            // Short: address in same word as flags; Long: address in lower word.
            // The page address field is bits 31..PS: only the bits below the
            // PAGE SIZE are unused, however many index bits the early
            // termination left unwalked.  MC68030UM 9.5.3.1: "the physical
            // address in the ATC entry is the sum of the page address field in
            // the descriptor plus an offset.  The offset is the logical address
            // with the bits used in the search set to zero."  Masking the frame
            // to the whole covered range instead dropped the frame's low bits:
            // the IIci ROM maps logical 0 to physical $50000 (past the RBV's
            // in-RAM screen buffer) with a level-A descriptor $00050019, and that
            // came out as logical == physical, so the A/UX launcher's root
            // pointer block, written through logical $2000, landed at physical
            // $2000 while the ROM trampoline read it (MMU off) at $52000.
            uint32_t ps_mask = (1u << TC_PS(tc)) - 1;
            // The extra low-bit clear is not redundant for a malformed TC:
            // a PS below 2 leaves DT bits inside the mask.
            uint32_t phys_base = desc_lo & ~ps_mask & 0xFFFFFFFC;

            result.physical_addr = phys_base + (logical_addr & page_mask);
            result.page_size_bits = bit_pos;
            result.valid = true;
            // Accumulated above, this descriptor included.  M is deliberately
            // NOT accumulated: it is a per-page modified flag, not a protection
            // attribute, and nothing in 9.5.5 ORs it.
            result.write_protected = acc_wp;
            result.supervisor_only = acc_s;
            result.modified = (desc_hi >> 4) & 1;

            // M (bit 4) is set in the PAGE descriptor ahead of the write.
            // MC68030UM, descriptor field definitions: "The MC68030 sets the M
            // bit in the corresponding page descriptor before a write operation
            // to a page for which the M bit is zero, except after a descriptor
            // with the WP bit set is encountered, or after a supervisor
            // violation is encountered.  An access is considered to be a write
            // for updating purposes if either the R/W or RMC signal is low.
            // The MC68030 never clears this bit."
            if (update_um && write && !result.modified && !acc_wp && !(acc_s && !supervisor)) {
                (void)mmu_write_physical_uint32(mmu, desc_addr, desc_hi | (1u << 4));
                result.modified = true;
            }

            if (ts) {
                mmu_trace_bool(ts, "m", result.modified);
                mmu_trace_bool(ts, "ci", ((desc_hi >> 6) & 1) != 0);
                mmu_trace_hex(ts, "phys", result.physical_addr);
                mmu_trace_hex(ts, "size", 1u << bit_pos);
                mmu_trace_outcome(ts, "hit");
            }

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

        // Table descriptor (short=DT2, long=DT3) — follow to next level.
        // Short: bits 31:4 hold TA; bit 3=U, bit 2=WP, bits 1:0=DT.
        // Long:  bits 31:4 of the lower word hold TA; bits 3:0 must be zero.
        // Either way mask with 0xFFFFFFF0 to strip the flag nibble.
        table_addr = desc_lo & 0xFFFFFFF0;
        if (ts) {
            mmu_trace_hex(ts, "next", table_addr);
            if (long_desc) {
                mmu_trace_uint(ts, "limit", (desc_hi >> 16) & 0x7FFF);
                mmu_trace_bool(ts, "lower_limit", ((desc_hi >> 31) & 1) != 0);
            }
            mmu_trace_outcome(ts, "next");
        }
        // A long-format table descriptor carries L/U in bit 31 of its upper
        // word and the 15-bit LIMIT in bits 30:16; both bound the next level's
        // index.  Short-format descriptors have no limit field, so following
        // one clears any limit inherited from above.
        limit_active = long_desc;
        if (long_desc) {
            limit_is_lower = (desc_hi >> 31) & 1;
            limit_value = (desc_hi >> 16) & 0x7FFF;
        }
        long_desc = (dt == DESC_DT_TABLE8);
    }

    // Ran out of table levels without finding a page descriptor.
    // This shouldn't happen with a correctly configured TC, but treat as invalid.
    result.mmusr |= MMUSR_I;
    result.mmusr |= (levels_walked & 7);
    ts = mmu_trace_step(trace, "level");
    mmu_trace_str(ts, "reason", "no page descriptor");
    mmu_trace_outcome(ts, "fault");
    return result;
}

// Walk from the live root pointer: the SRP for a supervisor access when
// TC.SRE splits the trees, else the CRP.
static inline mmu_walk_result_t mmu_table_walk(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor,
                                               bool update_um, mmu_trace_t *trace) {
    bool use_srp = TC_SRE(mmu->tc) && supervisor;
    return mmu_table_walk_from(mmu, use_srp ? mmu->srp : mmu->crp, use_srp ? "srp" : "crp", logical_addr, write,
                               supervisor, update_um, trace);
}

// ============================================================================
// TLB Fill
// ============================================================================

// Fill the SoA arrays for a single page based on walk result and permissions.
// The `supervisor` flag is the FC class used for the walk (true=super, false=user).
// When TC.SRE=1 (separate supervisor/user roots), the super and user MMU tables
// may map the same logical page to different physical pages, so we only
// populate the SoA matching the walk's FC.  When SRE=0, a single walk
// produces the mapping for both FCs so we populate both tables.
// Fill the SoA entries for one translated page from a 68030 table walk.
//
// The mechanical half of this -- resolve the host pointer, bound the page
// index, suppress the fill for a logical- or physical-space logpoint, compute
// the adjusted base, track the page, write the four arrays -- is
// mmu_fill_soa_page, which the 68040 walker already calls directly.  What is
// specific to the 030 is only the POLICY for which of the two SoAs to fill, so
// that is all that lives here.
//
// Fill rules:
//   TT match: the transparent-translation registers are FC-specific
//     (supervisor-only or user-only), so fill ONLY the SoA matching the walk's
//     FC.  Filling both would leak the supervisor TT identity into the user
//     SoA, where the same VA actually maps elsewhere via the (C)RP.
//   Table walk, SRE=0: supervisor and user share the CRP, so fill both.
//   Table walk, SRE=1: supervisor uses the SRP and user the CRP, which may map
//     the same VA to different PAs -- fill only the matching SoA.
static void mmu_fill_soa_entry(mmu_state_t *mmu, uint32_t logical_page, uint32_t physical_page, bool supervisor_only,
                               bool write_protected, bool supervisor, bool tt_match) {
    bool fill_super, fill_user;
    if (tt_match) {
        // TT registers are FC-specific: only the walk's own SoA
        fill_super = supervisor;
        fill_user = !supervisor;
    } else if (!TC_SRE(mmu->tc)) {
        // One root for both FCs: one walk fills both, but never a
        // supervisor-only page into the user side
        fill_super = true;
        fill_user = !supervisor_only;
    } else {
        // Separate roots (SRE=1): only the walk's own FC
        fill_super = supervisor;
        fill_user = !supervisor && !supervisor_only;
    }
    mmu_fill_soa_page(mmu, logical_page, physical_page, fill_super, fill_user, !write_protected);
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
    mmu->tlb_was_enabled = false;

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
void mmu_register_host_region(mmu_state_t *mmu, uint8_t *host, uint32_t phys_base, uint32_t size, bool writable) {
    if (!mmu || !host || size == 0)
        return;
    // Re-registration of the same physical window replaces the entry (a
    // machine re-running its layout must not accumulate duplicates).
    for (int i = 0; i < mmu->host_region_count; i++) {
        mmu_host_region_t *r = &mmu->host_regions[i];
        if (r->phys_base == phys_base && r->size == size) {
            r->host = host;
            r->writable = writable;
            return;
        }
    }
    if (mmu->host_region_count >= MMU_HOST_REGION_MAX) {
        LOG(0, "mmu_register_host_region: list full (%d); region $%08X+$%X dropped", MMU_HOST_REGION_MAX, phys_base,
            size);
        return;
    }
    mmu_host_region_t *r = &mmu->host_regions[mmu->host_region_count++];
    r->host = host;
    r->phys_base = phys_base;
    r->size = size;
    r->writable = writable;
    r->alias = false;
}

// Walk the host-region list, projecting each region (and optionally its
// Mode-24 slot alias) into the CPU page table via the machine's fill hook.
void mmu_host_regions_fill_pages(mmu_state_t *mmu, mmu_fill_page_fn fill, bool mode24_alias) {
    if (!mmu || !fill)
        return;
    for (int i = 0; i < mmu->host_region_count; i++) {
        const mmu_host_region_t *r = &mmu->host_regions[i];
        if (r->alias)
            continue; // resolver-only: device windows may overlap the alias range
        uint32_t pages = r->size >> PAGE_SHIFT;
        uint32_t start = r->phys_base >> PAGE_SHIFT;
        for (uint32_t p = 0; p < pages && start + p < g_page_count; p++)
            fill(start + p, r->host + (p << PAGE_SHIFT), r->writable);
        // Mode-24 (24-bit Memory Manager) slot window: slot s ($9..$E) has a
        // 1 MB region at $00s00000 mirroring the start of its 32-bit slot
        // space at $Fs000000 (GLUE/BBU decode both to the same slot).
        if (mode24_alias && r->writable) {
            uint32_t high = r->phys_base & 0xFF000000u;
            if (high >= 0xF9000000u && high <= 0xFE000000u) {
                int slot = (int)((r->phys_base >> 24) & 0xFu);
                uint32_t alias_bytes = 0x100000u; // 1 MB Mode-24 slot window
                if (alias_bytes > r->size)
                    alias_bytes = r->size;
                uint32_t alias_pages = alias_bytes >> PAGE_SHIFT;
                uint32_t start24 = ((uint32_t)slot << 20) >> PAGE_SHIFT; // $00s00000
                for (uint32_t p = 0; p < alias_pages && start24 + p < g_page_count; p++)
                    fill(start24 + p, r->host + (p << PAGE_SHIFT), true);
            }
        }
    }
}

void mmu_set_ram_bank_b(mmu_state_t *mmu, uint32_t ram_a_size, uint8_t *bank_b_host, uint32_t bank_b_phys_base,
                        uint32_t bank_b_size, uint32_t bank_b_window) {
    if (!mmu)
        return;
    mmu->ram_a_size = ram_a_size;
    mmu->physical_ram_b = bank_b_host;
    mmu->ram_b_phys_base = bank_b_phys_base;
    mmu->ram_b_size = bank_b_size;
    mmu->ram_b_window = bank_b_window;
    // From here on the single-bank resolvers must not shadow Bank B: cap the
    // contiguous RAM size at Bank A so any stray `phys < physical_ram_size`
    // path can only ever reach Bank A.
    mmu->physical_ram_size = ram_a_size;
}

// Attach a 68040 register file (owned by the CPU) to this bus-side MMU state.
// From here on translation dispatches to the mmu040.c walker; `enabled`
// mirrors the 040's TC.E (kept in sync by mmu040_set_tc via the backlink).
void mmu_attach_mmu040(mmu_state_t *mmu, struct mmu040_state *m040) {
    if (!mmu)
        return;
    mmu->m040 = m040;
    if (m040) {
        m040->bus = mmu;
        mmu->enabled = m040->enabled;
    }
}

// Fill one 4 KiB SoA page from an already-resolved translation (68040 walker
// entry point).  Shares host resolution, logpoint suppression, and TLB
// tracking with the PMMU fill above.
void mmu_fill_soa_page(mmu_state_t *mmu, uint32_t logical_page, uint32_t physical_page, bool fill_super, bool fill_user,
                       bool writable) {
    bool host_writable;
    uint8_t *host_ptr = phys_resolve(mmu, physical_page, &host_writable);
    if (!host_ptr)
        return; // unmapped physical address — leave SoA entry as zero

    host_writable = host_writable && writable;
    uint32_t page_index = logical_page >> PAGE_SHIFT;
    if (page_index >= g_page_count)
        return;

    // Memory logpoints force the slow path — see mmu_fill_soa_entry.  The
    // physical array is sized by the page table like the logical one, so a
    // physical page past it cannot be watched (and must not be indexed).
    uint32_t phys_index = physical_page >> PAGE_SHIFT;
    if (g_mem_logpoint_page_count && g_mem_logpoint_page_count[page_index])
        return;
    if (g_mem_logpoint_phys_page_count && phys_index < g_page_count && g_mem_logpoint_phys_page_count[phys_index])
        return;

    uintptr_t adjusted = (uintptr_t)host_ptr - logical_page;

    if (fill_super) {
        if (g_supervisor_read)
            g_supervisor_read[page_index] = adjusted;
        if (g_supervisor_write && host_writable)
            g_supervisor_write[page_index] = adjusted;
    }
    if (fill_user) {
        if (g_user_read)
            g_user_read[page_index] = adjusted;
        if (g_user_write && host_writable)
            g_user_write[page_index] = adjusted;
    }
    // Track after the stores: the entry is on the list exactly when it is
    // populated, so the next invalidation zeroes it (or, past the tracker's
    // capacity, falls back to zeroing everything).
    tlb_track_page(page_index);
}

// Clone the registered host region at `original_phys_base` at a second
// physical address.  The clone is flagged `alias`: it resolves through
// phys_to_host but is never page-filled (mmu_host_regions_fill_pages skips
// it).  False when no region starts at `original_phys_base`.
bool mmu_register_host_region_alias(mmu_state_t *mmu, uint32_t alias_phys_base, uint32_t original_phys_base) {
    if (!mmu)
        return false;
    for (int i = 0; i < mmu->host_region_count; i++) {
        const mmu_host_region_t *r = &mmu->host_regions[i];
        if (r->phys_base == original_phys_base) {
            mmu_register_host_region(mmu, r->host, alias_phys_base, r->size, r->writable);
            // Flag the entry the call created or replaced -- found by its
            // window, not assumed to be the last one (a full list drops it)
            for (int j = 0; j < mmu->host_region_count; j++)
                if (mmu->host_regions[j].phys_base == alias_phys_base && mmu->host_regions[j].size == r->size)
                    mmu->host_regions[j].alias = true;
            return true;
        }
    }
    return false;
}

// Invalidate the software TLB.  The memory map's population tracker zeroes
// only the populated entries (memory_soa_invalidate).
void mmu_invalidate_tlb(mmu_state_t *mmu) {
    // Fast path: when the MMU is disabled and was disabled the previous
    // time we ran (no enabled→disabled transition), the SoA fast-path
    // already holds the direct host-backed mappings the boot ROM relies
    // on; PMOVE updates to TC/SRP/TT0/TT1 don't perturb them, so the
    // zero-and-repopulate walk below is wasted work — and a hot one,
    // because the IIcx PrimaryInit's JMFB driver fires _SwapMMUMode
    // many thousands of times during slot-scan / sBlock dispatch and
    // each call PMOVE-writes TC.  The full path still runs on
    // disabled→enabled and enabled→disabled transitions, where the SoA
    // really does need to switch shape.
    bool now_enabled = mmu && mmu->enabled;
    if (!now_enabled && mmu && !mmu->tlb_was_enabled) {
        mmu->tlb_was_enabled = now_enabled;
        return;
    }
    if (mmu)
        mmu->tlb_was_enabled = now_enabled;
    // A real invalidation is where the hardware ATC dies too: drop the cached
    // block descriptors along with the SoA fill.  (The dis→dis early-out above
    // and the FD PMOVE forms — which never call here — both preserve them.)
    atc_flush(mmu);
    // Zero every SoA entry populated since the last invalidation -- the
    // memory map tracks them, so typically ~2000-3000 pages rather than the
    // whole 1M-page arrays.
    memory_soa_invalidate();

    // When the MMU is disabled, host-backed pages (RAM/ROM/VRAM) are
    // installed lazily on first access by the memory.c slow path via
    // rebuild_soa_page. The eager repopulate that used to live here did a
    // linear scan of all g_page_count AoS slots (32 MB for SE/30) to find
    // ~1500 host-backed pages — that scan dominated SE/30 boot at 37% of
    // CPU time. See docs/notes/mmu-tlb-invalidate-perf.md.
}

// Shared tail of the fault path: after a fill attempt, if the SoA entry
// stayed zero (physical page is device, unmapped, or logpoint-suppressed),
// decide bus error vs silent success.
//
// On real hardware, unmapped physical addresses cause bus errors.  However,
// Mac OS legitimately probes RAM expansion addresses (e.g. $00F80000,
// $80000000) via page table entries and expects to read $FF without
// faulting, and our deferred bus error mechanism is incompatible with the
// ROM's bail-out handler for data probes.  So only bus error for physical
// addresses that are clearly garbage — outside all known hardware regions.

// Handle a TLB miss: perform table walk or TT check, fill SoA entry.
// `probe_atc` selects whether the block-descriptor cache may satisfy the miss
// without a walk; PLOAD passes false to force a fresh table walk (its whole
// purpose is to reload the ATC from the current guest tables).
static bool mmu_handle_fault_internal(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor,
                                      bool probe_atc) {
    if (!mmu || !mmu->enabled) {
        g_bus_error_is_pmmu = false; // no PMMU to blame: never a retry frame from stale state
        return false;
    }

    // Align to emulator page boundary for SoA entry
    uint32_t emu_page = logical_addr & ~(uint32_t)PAGE_MASK;

    // Check transparent translation first
    if (mmu_check_tt(mmu, logical_addr, write, supervisor)) {
        // TT match: identity mapping (logical = physical)
        mmu_fill_soa_entry(mmu, emu_page, emu_page, false, false, supervisor, true);
        // If phys_to_host returned NULL (unmapped physical), the SoA entry
        // stays zero.  For reads, only bus error within the configured NuBus
        // expansion slot range (e.g. $F9-$FD on SE/30).  Outside that range,
        // return 0 silently — the hardware doesn't bus error for internal
        // pseudo-slots like slot $F.  Writes are always silently dropped.
        if (!write) {
            uint32_t page_index = emu_page >> PAGE_SHIFT;
            if (page_index < g_page_count && g_supervisor_read && g_supervisor_read[page_index] == 0 &&
                memory_addr_faults_when_unmapped(logical_addr)) {
                // TT + unmapped physical = plain bus timeout; ROM handlers
                // expect skip semantics (Format $A).
                g_bus_error_is_pmmu = false;
                return false;
            }
        }
        return true;
    }

    // Block-descriptor cache: a prior walk of this range cached its
    // early-termination descriptor (the ATC residency the real 68030 gives).
    // On hit, fill just the touched page — no guest-table re-walk, and
    // mmu->mmusr stays untouched (matching ATC-hit behaviour on hardware).
    // Accesses the cached permissions would REJECT fall through to the real
    // walk so the failure publishes MMUSR exactly as before (A/UX's bus-error
    // path reads it).
    if (probe_atc) {
        atc_block_t *b = atc_probe(mmu, logical_addr, supervisor);
        // A write to a block whose descriptor still has M clear must fall
        // through to the real walk, so the walk can set M -- otherwise the
        // block cache would silently bypass the modified-bit protocol for every
        // page the block covers.
        if (b && !(b->supervisor_only && !supervisor) && !(b->write_protected && write) && !(write && !b->modified)) {
            uint32_t phys_page = b->phys_base + (emu_page - b->log_base);
            bool b_writable = !b->write_protected && b->modified;
            mmu_fill_soa_entry(mmu, emu_page, phys_page, b->supervisor_only, !b_writable, supervisor, false);
            return mmu_fault_epilogue(mmu, emu_page, phys_page, write, supervisor);
        }
    }

    // Perform table walk.  This is the real translation path (and PLOAD), so
    // the architectural history bits are maintained.
    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, write, supervisor, /*update_um=*/true, NULL);

    // Publish the walk's MMUSR to mmu->mmusr so that any PMOVE MMUSR,EA the
    // kernel issues from its bus-error handler reflects the actual fault
    // condition (Invalid descriptor / Write-Protected / Supervisor-Only /
    // Bus error during walk).  Real M68030 sets MMUSR as a side effect of
    // the failing access; A/UX 3.0.1's bus-error path relies on this — it
    // reads MMUSR ~8000 times during boot without ever issuing PTEST.
    // Leaving mmu->mmusr stale (only mmu_test_address updated it) caused
    // the kernel to misclassify every fault as the previous PTEST's
    // condition and never reach the right page-allocator branch.
    if (mmu)
        mmu->mmusr = result.mmusr;

    if (!result.valid) {
        // Invalid descriptor: PMMU walk fault, retry semantics (Format $B)
        g_bus_error_is_pmmu = true;
        return false;
    }

    // Check supervisor-only restriction
    if (result.supervisor_only && !supervisor) {
        g_bus_error_is_pmmu = true;
        return false; // user accessing supervisor page → bus error
    }

    // Check write protection
    if (result.write_protected && write) {
        g_bus_error_is_pmmu = true;
        return false; // write to write-protected page → bus error
    }

    // Fill the SoA entry for this emulator page.
    // The physical address from the walk gives us the physical page base.
    // We need to map the emulator's 4KB page granularity.
    uint32_t phys_page = result.physical_addr & ~(uint32_t)PAGE_MASK;
    // Write-array fill policy: a page becomes writable through the SoA only
    // once it is marked modified -- which this access establishes when it is a
    // write.  A read fault on a clean page deliberately leaves the write entry
    // empty so the first write re-faults and the walk sets M.  That is the
    // architectural modified-bit protocol (MC68030UM's ATC M-bit description:
    // on a write to a page whose entry has M clear the processor "aborts the
    // access and initiates a table search, setting the M bit in the page
    // descriptor ... and the access is retried"), and it is what a guest's VM
    // dirty-page accounting depends on.  Without this gate the write-back
    // above would be cosmetic: nothing would ever re-fault to trigger it.
    // mmu040.c has had the identical policy all along.
    bool soa_writable = !result.write_protected && (write || result.modified);
    mmu_fill_soa_entry(mmu, emu_page, phys_page, result.supervisor_only, !soa_writable, supervisor, false);

    // When the descriptor covers more than one emulator 4KB page (e.g. an
    // early-termination page descriptor at level A with 32 MB coverage), the
    // real 68030 ATC caches a single entry for the whole range and never
    // re-walks — the guest can clear the page-table root and still execute
    // correctly on real HW (observed in A/UX 3.0.1 boot, which clears
    // $104000 immediately after PMOVE TC while relying on ATC residency).
    // Mirror that by caching the walked descriptor; later faults inside the
    // range fill their page from the cache without re-walking.  Any stale
    // cached block for this range is dropped first so a fresh walk (PLOAD,
    // or a mapping reshaped from block to page tables) always supersedes it.
    atc_invalidate_covering(mmu, logical_addr, supervisor);
    uint32_t ps_bits = result.page_size_bits;
    if (ps_bits > PAGE_SHIFT && ps_bits < 32) {
        uint32_t log_mask = ~((1u << ps_bits) - 1);
        // The physical range starts at the descriptor's page frame, which need
        // not be aligned to the coverage (see the walk): recover it from the
        // translated address rather than masking it to the range.
        uint32_t phys_range_base = result.physical_addr - (logical_addr & ~log_mask);
        atc_record(mmu, logical_addr & log_mask, log_mask, phys_range_base, result.supervisor_only,
                   result.write_protected, result.modified, supervisor);
    }

    // If phys_to_host returned NULL (unmapped physical), the SoA entry
    // stays zero.  On real hardware, the physical bus access would fail
    // (no device responds) and generate a bus error — see mmu_fault_epilogue.
    return mmu_fault_epilogue(mmu, emu_page, phys_page, write, supervisor);
}

// Handle a TLB miss: perform table walk or TT check, fill SoA entry.
bool mmu_handle_fault(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor) {
    if (mmu && mmu->m040)
        return mmu040_handle_fault(mmu, logical_addr, write, supervisor);
    return mmu_handle_fault_internal(mmu, logical_addr, write, supervisor, true);
}

// PLOAD: force a fresh table walk and (re)load the cached translation,
// bypassing the block-descriptor cache — the freshly walked descriptor
// replaces any stale cached block for the range.
bool mmu_pload(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor) {
    return mmu_handle_fault_internal(mmu, logical_addr, write, supervisor, false);
}

// PTEST: test address translation without faulting
uint16_t mmu_test_address(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor,
                          uint32_t *desc_addr_out) {
    if (desc_addr_out)
        *desc_addr_out = 0;
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

    // Perform table walk (without modifying SoA entries).  M68000PRM PTEST:
    // the instruction "alters neither the used or modified bits of the
    // translation tables nor the address translation cache", so update_um is
    // false -- the opposite default from the 68040, whose PTEST does update
    // them (MC68040UM 3.7.3).
    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, write, supervisor, /*update_um=*/false, NULL);

    mmu->mmusr = result.mmusr;
    if (desc_addr_out)
        *desc_addr_out = result.descriptor_addr;
    return result.mmusr;
}

// Public wrappers around the file-local phys_to_host / phys_is_writable.
uint8_t *mmu_phys_to_host(mmu_state_t *mmu, uint32_t phys_addr) {
    if (!mmu)
        return NULL;
    return phys_to_host(mmu, phys_addr);
}
bool mmu_phys_is_writable(mmu_state_t *mmu, uint32_t phys_addr) {
    if (!mmu)
        return false;
    return phys_is_writable(mmu, phys_addr);
}

// Debug-only translation: resolve logical address to physical without side effects.
// Returns the physical address, or logical_addr if translation fails.
uint32_t mmu_translate_debug(mmu_state_t *mmu, uint32_t logical_addr, bool supervisor) {
    if (!mmu || !mmu->enabled)
        return logical_addr;

    if (mmu->m040) {
        uint32_t pa;
        return mmu040_translate_checked(mmu, logical_addr, supervisor, &pa) ? pa : logical_addr;
    }

    if (mmu_check_tt(mmu, logical_addr, false, supervisor))
        return logical_addr;

    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, false, supervisor, /*update_um=*/false, NULL);

    if (result.valid)
        return result.physical_addr;

    return logical_addr;
}

// Validity-reporting sibling of mmu_translate_debug.  Same side-effect-free
// walk, but distinguishes a genuine identity mapping from a failed walk: both
// leave *pa_out == logical_addr, yet only the former returns true.  A failed
// walk MUST fault rather than be mistaken for identity-mapped device I/O.
bool mmu_translate_checked(mmu_state_t *mmu, uint32_t logical_addr, bool supervisor, uint32_t *pa_out) {
    if (!mmu || !mmu->enabled) {
        if (pa_out)
            *pa_out = logical_addr;
        return true; // MMU off: identity, always valid
    }
    if (mmu->m040)
        return mmu040_translate_checked(mmu, logical_addr, supervisor, pa_out);
    if (mmu_check_tt(mmu, logical_addr, false, supervisor)) {
        if (pa_out)
            *pa_out = logical_addr;
        return true; // transparent translation: identity, valid
    }
    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, false, supervisor, /*update_um=*/false, NULL);
    if (pa_out)
        *pa_out = result.valid ? result.physical_addr : logical_addr;
    return result.valid;
}

// The debugger's translation (translate / walk / map): side-effect-free, with
// the protection the queried privilege sees and, when `trace` is given, the
// TT check and every table level the walk read.
void mmu_debug_translate(mmu_state_t *mmu, uint32_t logical_addr, bool supervisor, bool fetch, mmu_xlate_t *out,
                         mmu_trace_t *trace) {
    if (!mmu || !mmu->enabled) {
        mmu_xlate_identity(out, logical_addr); // MMU off: identity everywhere
        return;
    }
    if (mmu->m040) {
        mmu040_debug_translate(mmu->m040, mmu, logical_addr, supervisor, fetch, out, trace);
        return;
    }
    out->space = NULL;

    // Transparent translation first: a match bypasses the tables.  The 030's
    // TT match does not distinguish instruction fetches from data.
    mmu_trace_step_t *ts = mmu_trace_step(trace, "tt");
    const char *tt_name = tt_matches(mmu->tt0, logical_addr, false, supervisor)   ? "tt0"
                          : tt_matches(mmu->tt1, logical_addr, false, supervisor) ? "tt1"
                                                                                  : NULL;
    if (tt_name) {
        uint32_t tt = tt_name[2] == '0' ? mmu->tt0 : mmu->tt1;
        mmu_trace_str(ts, "name", tt_name);
        mmu_trace_hex(ts, "value", tt);
        mmu_trace_outcome(ts, "hit");
        out->phys = logical_addr;
        out->valid = true;
        out->via = "tt";
        // A TT register with RW=1 matches one direction only; a write it does
        // not match goes through the tables instead.
        out->access = mmu_access(true, tt_matches(tt, logical_addr, true, supervisor));
        out->span_bits = 24; // TT registers match on A31-A24
        return;
    }

    mmu_walk_result_t r = mmu_table_walk(mmu, logical_addr, false, supervisor, /*update_um=*/false, trace);
    out->via = "page";
    out->valid = r.valid;
    out->phys = r.valid ? r.physical_addr : logical_addr;
    out->access = mmu_access(!(r.supervisor_only && !supervisor), !r.write_protected);
    out->span_bits = r.page_size_bits;
}

// Translate against an explicit CRP root (e.g. a snapshot of MAE's CRP).
// Side-effect-free: a user-mode walk from the supplied root, which leaves the
// MMU's own CRP untouched (it used to be swapped in and back out).  TT checks are skipped for the page offset only
// after the walk to keep the result page-faithful to the supplied CRP.
bool mmu_translate_with_crp(mmu_state_t *mmu, uint32_t logical_addr, uint64_t crp_root, uint32_t *pa_out) {
    if (!mmu || !pa_out)
        return false;
    // Without a known CRP we cannot reach the target address space.
    if (crp_root == 0)
        return false;
    // If the MMU is disabled, every address is identity-mapped.
    if (!mmu->enabled) {
        *pa_out = logical_addr;
        return true;
    }
    // TT registers do not depend on the CRP, so an early TT match is
    // valid for the supplied address space too.
    if (mmu_check_tt(mmu, logical_addr, false, /*supervisor=*/false)) {
        *pa_out = logical_addr;
        return true;
    }
    // User-mode walk from the supplied root.  The walk reads guest tables
    // via phys_to_host but does not touch the SoA arrays.
    mmu_walk_result_t result = mmu_table_walk_from(mmu, crp_root, "crp", logical_addr, false, /*supervisor=*/false,
                                                   /*update_um=*/false, NULL);
    if (!result.valid)
        return false;
    *pa_out = result.physical_addr;
    return true;
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
    // Byte by byte: the two bytes may sit in different regions (or the
    // second past the end of the first), and each is resolved on its own.
    return (uint16_t)(((uint16_t)mmu_read_physical_uint8(mmu, phys_addr) << 8) |
                      mmu_read_physical_uint8(mmu, phys_addr + 1));
}

// Read a 32-bit big-endian value from physical memory for debug commands.
uint32_t mmu_read_physical_uint32(mmu_state_t *mmu, uint32_t phys_addr) {
    if (!mmu)
        return 0;
    return phys_read32(mmu, phys_addr);
}

// Write helpers: literal physical writes, bypassing the CPU SoA / TLB.  Used
// by debug commands (`set-phys`) and by harness code that needs to poke a
// kernel-allocated structure regardless of which CPU mode happens to be
// active.  Only RAM and VRAM are writable; ROM/VROM regions silently no-op
// (matching the read helpers' behaviour for unmapped pages).
//
// Returns true on success (physical address mapped to writable host memory),
// false otherwise.
bool mmu_write_physical_uint8(mmu_state_t *mmu, uint32_t phys_addr, uint8_t value) {
    if (!mmu)
        return false;
    if (!phys_is_writable(mmu, phys_addr))
        return false; // unmapped or ROM/VROM
    uint8_t *host = phys_to_host(mmu, phys_addr);
    if (!host)
        return false;
    *host = value;
    return true;
}

// All-or-nothing big-endian write of `size` bytes: every byte is resolved to
// writable host memory first, so a write that would fault part-way writes
// nothing rather than leaving the leading bytes changed.
static bool write_physical_n(mmu_state_t *mmu, uint32_t phys_addr, uint32_t value, unsigned size) {
    if (!mmu)
        return false;
    uint8_t *host[4];
    for (unsigned i = 0; i < size; i++) {
        bool writable;
        host[i] = phys_resolve(mmu, phys_addr + i, &writable);
        if (!host[i] || !writable)
            return false; // unmapped or ROM/VROM: nothing written
    }
    for (unsigned i = 0; i < size; i++)
        *host[i] = (uint8_t)(value >> (8 * (size - 1 - i))); // most significant byte first
    return true;
}

bool mmu_write_physical_uint16(mmu_state_t *mmu, uint32_t phys_addr, uint16_t value) {
    return write_physical_n(mmu, phys_addr, value, 2);
}

bool mmu_write_physical_uint32(mmu_state_t *mmu, uint32_t phys_addr, uint32_t value) {
    return write_physical_n(mmu, phys_addr, value, 4);
}
