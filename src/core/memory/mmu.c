// SPDX-License-Identifier: MIT
// mmu.c
// 68030 PMMU (Paged Memory Management Unit) implementation.
// Lazy-fill TLB using SoA pointer arrays: on a TLB miss the slow path
// calls mmu_handle_fault() which walks the guest translation tables and
// populates the SoA entry so subsequent accesses hit the fast path.

#include "mmu.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mmu");

// Global MMU state pointer (NULL for 68000 machines)
mmu_state_t *g_mmu = NULL;

// Last CRP observed while the CPU was in user mode.  Updated by the
// supervisor→user transition in cpu_internal.h.  0 until first user-mode
// entry.  Cleared when the MMU is destroyed.
uint64_t g_last_user_crp = 0;

// ============================================================================
// Per-configuration SoA sets + population tracking
// ============================================================================

// Instead of zeroing all 1M+ page entries on every TLB invalidation (32 MB of
// memset on a 32-bit address space), track which pages have been populated and
// only zero those.  Typical working sets are ~2000-3000 pages (8-12 MB of RAM
// + ROM + VRAM), reducing invalidation cost from O(address_space) to O(working_set).
#define TLB_TRACK_MAX 8192 // max tracked pages before fallback to full memset

static uint32_t g_tlb_track[TLB_TRACK_MAX]; // set 0's population list (sets 1+ allocate their own)

// The guest's tables use early-termination block descriptors covering large
// ranges (e.g. one level-A descriptor covering 32 MB).  The real 68030 ATC
// caches one descriptor for the whole covered range and never re-walks; the
// old emulator approximation eagerly materialised every covered 4 KB page
// into the SoA arrays (~9,200 entries per walk), which dominated steady-state
// host time under System 6's per-VBL _SwapMMUMode invalidation storm (see
// docs/proposals/proposal-performance-optimizations.md §5.1).  Instead, cache
// the walked descriptor itself; on a later fault inside the covered range,
// fill only the touched 4 KB page from the cached descriptor — no re-walk.
typedef struct atc_block {
    uint32_t log_base; // logical range base (aligned to coverage)
    uint32_t log_mask; // ~(coverage-1)
    uint32_t phys_base; // physical range base (same alignment)
    bool supervisor_only; // S bit from the walked descriptor
    bool write_protected; // W bit from the walked descriptor
    bool fc_super; // FC class of the walk (matters when TC.SRE=1)
    bool valid; // entry live?
} atc_block_t;
#define ATC_BLOCKS 16 // small, round-robin; the real 68030 ATC holds 22 entries

// An MMU configuration signature: the full register tuple, compared exactly
// (no hashing — a collision would silently serve wrong translations).  The
// MMU-disabled identity view is the tuple {enabled=false, 0...}: the identity
// mapping depends on no MMU register.
typedef struct soa_key {
    uint64_t crp, srp; // root pointers
    uint32_t tc, tt0, tt1; // translation control + transparent windows
    bool enabled; // TC.E
} soa_key_t;

// One cached translation view: four SoA arrays + population tracker + block
// cache, keyed by the MMU configuration that filled it.  System 6's per-VBL
// _SwapMMUMode ping-pongs between two enabled configs (24/32-bit) plus the
// disabled window — with one set per config, the switch is a pointer swap
// instead of a zero-and-refill of the whole working set (§6 P2 of the
// perf proposal).
typedef struct soa_set {
    uintptr_t *sr, *sw, *ur, *uw; // the four SoA arrays
    uint32_t *track; // populated page indices (for fast zeroing)
    int track_count; // entries in track list
    bool track_overflow; // true → zero via full memset instead
    atc_block_t blocks[ATC_BLOCKS]; // per-config block-descriptor cache
    int atc_next; // round-robin replacement cursor
    soa_key_t key; // config this set's contents belong to
    bool key_valid; // false → contents unreusable (evict/zero only)
    bool owns_storage; // true → arrays/tracker are ours to free
    uint64_t lru; // activation stamp for eviction
} soa_set_t;
#define SOA_SETS 3 // disabled view + the two _SwapMMUMode configs

static soa_set_t g_soa_sets[SOA_SETS]; // set 0 wraps the memory.c base arrays
static int g_soa_active = 0; // index of the set the globals point at
static uint64_t g_soa_lru_stamp = 0; // monotonic activation counter

// Record that a page index has been populated in the active set's SoA arrays
void tlb_track_page(uint32_t page_index) {
    soa_set_t *s = &g_soa_sets[g_soa_active];
    if (s->track_overflow || !s->track)
        return; // already in fallback mode (or pre-attach: nothing to track)
    if (s->track_count >= TLB_TRACK_MAX) {
        s->track_overflow = true;
        return;
    }
    s->track[s->track_count++] = page_index;
}

// Zero one set's SoA contents (via its tracker, or full memset on overflow)
// and drop its block-descriptor cache.  The key is left untouched — zeroed
// content is a valid empty view of the same configuration.
static void soa_zero_set(soa_set_t *s) {
    if (s->sr) {
        if (s->track_overflow) {
            size_t sz = (size_t)g_page_count * sizeof(uintptr_t);
            memset(s->sr, 0, sz);
            memset(s->sw, 0, sz);
            memset(s->ur, 0, sz);
            memset(s->uw, 0, sz);
        } else {
            // Bounds-check each index in case the tracker carries entries from
            // a previous machine with a larger page table.
            for (int i = 0; i < s->track_count; i++) {
                uint32_t p = s->track[i];
                if (p >= g_page_count)
                    continue;
                s->sr[p] = 0;
                s->sw[p] = 0;
                s->ur[p] = 0;
                s->uw[p] = 0;
            }
        }
    }
    s->track_count = 0;
    s->track_overflow = false;
    memset(s->blocks, 0, sizeof(s->blocks));
    s->atc_next = 0;
}

// Repoint the fast-path globals at a set.  Preserves which FC class (user or
// supervisor) the active pointers select — activation sites run in supervisor
// mode, but the nuke path (checkpoint restore) may execute with a user-mode
// CPU image whose g_active_* must keep selecting the user tables.
static void soa_map_set(int idx) {
    bool user_active = g_user_read && g_active_read == g_user_read;
    soa_set_t *s = &g_soa_sets[idx];
    g_supervisor_read = s->sr;
    g_supervisor_write = s->sw;
    g_user_read = s->ur;
    g_user_write = s->uw;
    g_active_read = user_active ? g_user_read : g_supervisor_read;
    g_active_write = user_active ? g_user_write : g_supervisor_write;
    g_soa_active = idx;
    s->lru = ++g_soa_lru_stamp;
}

// Build the key describing the MMU's current configuration.
static soa_key_t soa_current_key(mmu_state_t *mmu) {
    soa_key_t k = {0};
    if (mmu && mmu->enabled) {
        k.enabled = true;
        k.tc = mmu->tc;
        k.crp = mmu->crp;
        k.srp = mmu->srp;
        k.tt0 = mmu->tt0;
        k.tt1 = mmu->tt1;
    }
    return k;
}

static bool soa_key_eq(const soa_key_t *a, const soa_key_t *b) {
    return a->enabled == b->enabled && a->tc == b->tc && a->crp == b->crp && a->srp == b->srp && a->tt0 == b->tt0 &&
           a->tt1 == b->tt1;
}

// Lazily allocate a set's storage (sets 1+; set 0 wraps the memory.c arrays).
static void soa_alloc_set(soa_set_t *s) {
    size_t sz = (size_t)g_page_count * sizeof(uintptr_t);
    s->sr = (uintptr_t *)calloc(1, sz);
    s->sw = (uintptr_t *)calloc(1, sz);
    s->ur = (uintptr_t *)calloc(1, sz);
    s->uw = (uintptr_t *)calloc(1, sz);
    s->track = (uint32_t *)calloc(TLB_TRACK_MAX, sizeof(uint32_t));
    s->owns_storage = true;
}

// Switch the fast path to the set matching the MMU's current configuration:
// reuse a matching cached set as-is (the steady-state _SwapMMUMode path — a
// pointer swap, no zeroing, no refill), or evict the least-recently-used set,
// zero it, and re-key it.
static void soa_activate_current(mmu_state_t *mmu) {
    soa_key_t k = soa_current_key(mmu);
    // Hit: an existing set already holds this configuration's view.
    for (int i = 0; i < SOA_SETS; i++) {
        soa_set_t *s = &g_soa_sets[i];
        if (s->key_valid && soa_key_eq(&s->key, &k)) {
            soa_map_set(i);
            return;
        }
    }
    // Miss: evict — prefer an invalid-keyed set, else the LRU one.
    int victim = 0;
    for (int i = 1; i < SOA_SETS; i++) {
        soa_set_t *v = &g_soa_sets[victim], *s = &g_soa_sets[i];
        if (v->key_valid != s->key_valid ? !s->key_valid : s->lru < v->lru)
            victim = i;
    }
    soa_set_t *v = &g_soa_sets[victim];
    if (!v->sr && !v->owns_storage)
        soa_alloc_set(v); // fresh storage arrives zeroed
    else
        soa_zero_set(v);
    v->key = k;
    v->key_valid = true;
    soa_map_set(victim);
}

// Mark the active set's contents as unreusable while keeping them mapped.
// Used for the FD (flush-disable) PMOVE forms: hardware keeps serving the
// resident ATC entries across the register write (the IIfx ROM's PMOVEFD CRP
// mid-swap depends on it), but the mixed old/new contents must never be
// key-hit by a later activation.
static void soa_taint_active(void) {
    g_soa_sets[g_soa_active].key_valid = false;
}

// === Lifecycle (called from memory.c map init/teardown) =====================

// Capture the freshly allocated memory.c SoA arrays as set 0 and reset all
// cached sets.  Called at the end of memory_map_init; the machine starts with
// the MMU off, so set 0 begins keyed as the disabled identity view.
void mmu_soa_attach(void) {
    for (int i = 0; i < SOA_SETS; i++) {
        soa_set_t *s = &g_soa_sets[i];
        if (s->owns_storage) {
            free(s->sr);
            free(s->sw);
            free(s->ur);
            free(s->uw);
            free(s->track);
        }
        memset(s, 0, sizeof(*s));
    }
    soa_set_t *s0 = &g_soa_sets[0];
    s0->sr = g_supervisor_read;
    s0->sw = g_supervisor_write;
    s0->ur = g_user_read;
    s0->uw = g_user_write;
    s0->track = g_tlb_track;
    // memory_map_init may pre-populate entries without tracking; make the
    // first zeroing of set 0 a full memset (matches the old tracker's
    // start-in-overflow default).
    s0->track_overflow = true;
    s0->key_valid = true; // key = {0} = disabled identity view
    g_soa_active = 0;
    g_soa_lru_stamp = 0;
    s0->lru = ++g_soa_lru_stamp;
}

// Release set storage and point the globals back at the memory.c base arrays
// (set 0) so memory_map_delete frees the right allocation.  Idempotent;
// called from both mmu_delete and memory_map_delete.
void mmu_soa_detach(void) {
    if (g_soa_sets[0].sr) {
        g_supervisor_read = g_soa_sets[0].sr;
        g_supervisor_write = g_soa_sets[0].sw;
        g_user_read = g_soa_sets[0].ur;
        g_user_write = g_soa_sets[0].uw;
        g_active_read = NULL;
        g_active_write = NULL;
    }
    for (int i = 0; i < SOA_SETS; i++) {
        soa_set_t *s = &g_soa_sets[i];
        if (s->owns_storage) {
            free(s->sr);
            free(s->sw);
            free(s->ur);
            free(s->uw);
            free(s->track);
        }
        memset(s, 0, sizeof(*s));
    }
    g_soa_active = 0;
}

// Zero a single page's entries in every cached set (not just the active one).
// Memory logpoints force covered pages onto the slow path; a page left
// populated in an inactive set would bypass the logpoint after a config
// switch.
void mmu_soa_logpoint_zero_page(uint32_t page_index) {
    for (int i = 0; i < SOA_SETS; i++) {
        soa_set_t *s = &g_soa_sets[i];
        if (!s->sr || page_index >= g_page_count)
            continue;
        s->sr[page_index] = 0;
        s->sw[page_index] = 0;
        s->ur[page_index] = 0;
        s->uw[page_index] = 0;
    }
}

// Zero every cached set's contents (keys stay — an empty view is still a
// valid view of its configuration).  Physical-space logpoint installs use
// this: aliases of the watched physical page can live in any set.
void mmu_soa_zero_all_sets(void) {
    for (int i = 0; i < SOA_SETS; i++)
        soa_zero_set(&g_soa_sets[i]);
}

// === ATC block-descriptor cache (per active set) ============================

// Find the cached block covering logical_addr for this FC class, if any.
// When TC.SRE=0 a single walk serves both FC classes (super and user share
// the CRP), so the FC recorded at walk time doesn't restrict the hit.
static inline atc_block_t *atc_probe(mmu_state_t *mmu, uint32_t logical_addr, bool supervisor) {
    bool sre_split = TC_SRE(mmu->tc) != 0;
    atc_block_t *blocks = g_soa_sets[g_soa_active].blocks;
    for (int i = 0; i < ATC_BLOCKS; i++) {
        atc_block_t *b = &blocks[i];
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
static void atc_record(uint32_t log_base, uint32_t log_mask, uint32_t phys_base, bool supervisor_only,
                       bool write_protected, bool fc_super) {
    soa_set_t *s = &g_soa_sets[g_soa_active];
    atc_block_t *b = &s->blocks[s->atc_next];
    s->atc_next = (s->atc_next + 1) % ATC_BLOCKS;
    b->log_base = log_base;
    b->log_mask = log_mask;
    b->phys_base = phys_base;
    b->supervisor_only = supervisor_only;
    b->write_protected = write_protected;
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
    return 0; // unmapped physical address
}

// Resolve a physical address to a host pointer (RAM, ROM, or VRAM).
// Returns NULL if the physical address is not backed by host memory.
// ROM addresses are wrapped modulo rom_size to handle mirroring.
//
// Forced inline: hot-path called from mmu_fill_soa_entry on every TLB miss.
// Out-of-line, it was the top gprof entry at ~13% of SE/30 boot time.
static inline __attribute__((always_inline)) uint8_t *phys_to_host(mmu_state_t *mmu, uint32_t phys_addr) {
    if (mmu->ram_b_size) {
        // Two physical RAM banks (Macintosh IIsi).  Bank A mirrors within
        // [0, ram_b_phys_base); Bank B mirrors within its 64 MB window.
        if (phys_addr < mmu->ram_b_phys_base)
            return mmu->physical_ram + (phys_addr % mmu->ram_a_size);
        if (phys_addr - mmu->ram_b_phys_base < mmu->ram_b_window)
            return mmu->physical_ram_b + ((phys_addr - mmu->ram_b_phys_base) % mmu->ram_b_size);
        // not RAM — fall through to ROM/VRAM/VROM
    } else if (phys_addr < mmu->physical_ram_size) {
        return mmu->physical_ram + phys_addr;
    }
    if (mmu->physical_rom && phys_addr >= mmu->rom_phys_base && phys_addr < mmu->rom_region_end) {
        uint32_t offset = (phys_addr - mmu->rom_phys_base) % mmu->physical_rom_size;
        return mmu->physical_rom + offset;
    }
    // Range checks use (addr - base < size) so they don't wrap when base+size
    // would exceed UINT32_MAX (defensive; SE/30 placements are well below the
    // wrap, but VROM at $FExxxxxx is close enough to flag).
    if (mmu->physical_vram && phys_addr >= mmu->vram_phys_base &&
        (phys_addr - mmu->vram_phys_base) < mmu->physical_vram_size)
        return mmu->physical_vram + (phys_addr - mmu->vram_phys_base);
    // VROM region (read-only video declaration ROM)
    if (mmu->physical_vrom && phys_addr >= mmu->vrom_phys_base &&
        (phys_addr - mmu->vrom_phys_base) < mmu->physical_vrom_size)
        return mmu->physical_vrom + (phys_addr - mmu->vrom_phys_base);
    // Alternate VRAM address (page-table-mapped I/O space)
    if (mmu->vram_phys_alt && mmu->physical_vram && phys_addr >= mmu->vram_phys_alt &&
        (phys_addr - mmu->vram_phys_alt) < mmu->physical_vram_size)
        return mmu->physical_vram + (phys_addr - mmu->vram_phys_alt);
    // Alternate VROM address (page-table-mapped I/O space)
    if (mmu->vrom_phys_alt && mmu->physical_vrom && phys_addr >= mmu->vrom_phys_alt &&
        (phys_addr - mmu->vrom_phys_alt) < mmu->physical_vrom_size)
        return mmu->physical_vrom + (phys_addr - mmu->vrom_phys_alt);
    return NULL;
}

// Check if physical address is in writable RAM or VRAM (not ROM)
static inline __attribute__((always_inline)) bool phys_is_writable(mmu_state_t *mmu, uint32_t phys_addr) {
    if (mmu->ram_b_size) {
        // Two physical RAM banks (IIsi): both are writable DRAM.
        if (phys_addr < mmu->ram_b_phys_base)
            return true;
        if (phys_addr - mmu->ram_b_phys_base < mmu->ram_b_window)
            return true;
        // not RAM — fall through to ROM/VRAM checks
    } else if (phys_addr < mmu->physical_ram_size) {
        return true;
    }
    // ROM mirror region is read-only
    if (mmu->physical_rom && phys_addr >= mmu->rom_phys_base && phys_addr < mmu->rom_region_end)
        return false;
    if (mmu->physical_vram && phys_addr >= mmu->vram_phys_base &&
        (phys_addr - mmu->vram_phys_base) < mmu->physical_vram_size)
        return true;
    // VROM is read-only
    if (mmu->physical_vrom && phys_addr >= mmu->vrom_phys_base &&
        (phys_addr - mmu->vrom_phys_base) < mmu->physical_vrom_size)
        return false;
    // Alternate VRAM address is writable
    if (mmu->vram_phys_alt && mmu->physical_vram && phys_addr >= mmu->vram_phys_alt &&
        (phys_addr - mmu->vram_phys_alt) < mmu->physical_vram_size)
        return true;
    // Alternate VROM address is read-only
    if (mmu->vrom_phys_alt && mmu->physical_vrom && phys_addr >= mmu->vrom_phys_alt &&
        (phys_addr - mmu->vrom_phys_alt) < mmu->physical_vrom_size)
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

    // Table base address from root pointer (lower 32 bits, bits 31:4).
    // 68030 PMMU requires descriptor tables to be 16-byte aligned; bits 3:0
    // are reserved in the root and carry the WP (bit 2) and U (bit 3) flags
    // in nested short-format table descriptors.  Masking only bits 1:0 would
    // pick up WP as an address bit and shift the table base by 4 bytes — one
    // entry's worth — breaking every subsequent lookup by one index.
    uint32_t table_addr = root_lower & 0xFFFFFFF0;

    // Current bit position in logical address (start after IS bits)
    uint32_t bit_pos = 32 - is;
    int levels_walked = 0;

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

        if (dt == DESC_DT_INVALID) {
            // Invalid descriptor → bus error
            result.mmusr |= MMUSR_I;
            result.mmusr |= (levels_walked & 7);
            return result;
        }

        if (dt == DESC_DT_PAGE) {
            // Page descriptor (early termination) — translation complete.
            // The remaining address bits below bit_pos form the page offset.
            // Guard against `1u << 32` UB if a degenerate TC (IS=0, no TI
            // levels) left bit_pos at 32 — treat as invalid translation.
            if (bit_pos >= 32) {
                result.mmusr |= MMUSR_I;
                result.mmusr |= (levels_walked & 7);
                return result;
            }
            uint32_t page_mask = (1u << bit_pos) - 1;
            // Short: address in same word as flags; Long: address in lower word.
            uint32_t phys_base = desc_lo & ~page_mask & 0xFFFFFFFC;

            result.physical_addr = phys_base | (logical_addr & page_mask);
            result.page_size_bits = bit_pos;
            result.valid = true;
            result.write_protected = (desc_hi >> 2) & 1; // W bit
            result.modified = (desc_hi >> 4) & 1; // M bit
            // S bit: only in long-format descriptors (bit 8 of upper word)
            if (long_desc)
                result.supervisor_only = (desc_hi >> 8) & 1;

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
// The `supervisor` flag is the FC class used for the walk (true=super, false=user).
// When TC.SRE=1 (separate supervisor/user roots), the super and user MMU tables
// may map the same logical page to different physical pages, so we only
// populate the SoA matching the walk's FC.  When SRE=0, a single walk
// produces the mapping for both FCs so we populate both tables.
static void mmu_fill_soa_entry(mmu_state_t *mmu, uint32_t logical_page, uint32_t physical_page, bool supervisor_only,
                               bool write_protected, bool supervisor, bool tt_match) {
    // Get host pointer for the physical page
    uint8_t *host_ptr = phys_to_host(mmu, physical_page);
    if (!host_ptr)
        return; // unmapped physical address — leave SoA entry as zero

    bool host_writable = phys_is_writable(mmu, physical_page);
    uint32_t page_index = logical_page >> PAGE_SHIFT;
    if ((int)page_index >= g_page_count)
        return;

    // Memory logpoint: if this logical page has a logpoint installed, the SoA
    // must stay zero so every access routes through the slow path where the
    // logpoint check runs.  The next access will re-enter this code via
    // mmu_handle_fault, but the entry will again be suppressed — at the
    // steady-state cost of one extra call per access, which is the whole
    // point of a watchpoint.
    if (g_mem_logpoint_page_count && g_mem_logpoint_page_count[page_index])
        return;
    // Same rule for physical-space logpoints: if the physical page being
    // mapped is watched, suppress the fill.  This catches aliased mappings
    // (same physical page reached via multiple logical addresses), which a
    // purely logical-space logpoint misses.
    if (g_mem_logpoint_phys_page_count && g_mem_logpoint_phys_page_count[physical_page >> PAGE_SHIFT])
        return;

    // Compute adjusted base: host_ptr points to start of physical page,
    // but we want (uintptr_t)(base + logical_addr) to yield the host address.
    uintptr_t adjusted = (uintptr_t)host_ptr - logical_page;

    // Track this page for fast invalidation
    tlb_track_page(page_index);

    // Fill rules:
    //   TT match: TT registers are FC-specific (supervisor-only or user-only),
    //     so fill ONLY the SoA matching the walk's FC.  Filling both would
    //     leak the supervisor TT identity into the user SoA, where the same
    //     VA actually maps to a different PA via the (C)RP.
    //   Table walk under SRE=0: super and user share CRP, so fill both SoAs.
    //   Table walk under SRE=1: super uses SRP, user uses CRP — they may map
    //     the same VA to different PAs, so only fill the matching SoA.
    bool sre_split = TC_SRE(mmu->tc) != 0;
    bool fill_super = tt_match ? supervisor : (!sre_split || supervisor);
    bool fill_user = tt_match ? !supervisor : ((!sre_split || !supervisor) && !supervisor_only);
    if (!tt_match && supervisor_only)
        fill_user = false;

    if (fill_super) {
        if (g_supervisor_read)
            g_supervisor_read[page_index] = adjusted;
        if (g_supervisor_write && !write_protected && host_writable)
            g_supervisor_write[page_index] = adjusted;
    }

    if (fill_user) {
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
    if (g_mmu == mmu) {
        g_mmu = NULL;
        // Clear the user-CRP snapshot so a fresh machine doesn't inherit
        // a stale CRP from the previous instance.
        g_last_user_crp = 0;
        // Release cached sets (their contents belong to this machine's
        // tables) and re-point the globals at the memory.c base arrays so
        // memory_map_delete — which follows in every machine's teardown —
        // frees the right storage.
        mmu_soa_detach();
    }
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

// Register a VROM region so table walks and TT matches can resolve it
void mmu_register_vrom(mmu_state_t *mmu, uint8_t *vrom, uint32_t phys_base, uint32_t size) {
    if (!mmu)
        return;
    mmu->physical_vrom = vrom;
    mmu->vrom_phys_base = phys_base;
    mmu->physical_vrom_size = size;
}

// === memory_map_host_region — public bus-map API (proposal §3.2.3) =========
//
// The names below live on the memory map (declared in memory.h) but the
// storage they manipulate is still the 4-slot mmu_state_t today; this
// is a deliberate v1 rename-only move.  The storage refactor (move
// host-region list into memory_map_t, drop the fixed slots) is a known
// follow-up that becomes forced when a second card per machine lands.
// Until then the forwarders use g_mmu, which every glue030-family
// machine sets up before calling these.  No fast-path change — these
// run only at machine init.

void memory_map_host_region(memory_map_t *m, const char *name, uint8_t *host_ptr, uint32_t phys_base, uint32_t size,
                            bool writable) {
    (void)m; // forwarder uses g_mmu in v1
    if (!g_mmu)
        return;
    // V1 limitation: there's exactly one VRAM slot and one VROM slot in
    // mmu_state_t. Warn loudly if a caller's host region would overwrite
    // a populated slot — without the warning, a second call silently
    // detaches the previous machine layout.
    if (writable) {
        if (g_mmu->physical_vram)
            LOG(1, "memory_map_host_region: '%s' overwrites existing VRAM slot (phys $%08X size $%X)",
                name ? name : "?", g_mmu->vram_phys_base, g_mmu->physical_vram_size);
        mmu_register_vram(g_mmu, host_ptr, phys_base, size);
    } else {
        if (g_mmu->physical_vrom)
            LOG(1, "memory_map_host_region: '%s' overwrites existing VROM slot (phys $%08X size $%X)",
                name ? name : "?", g_mmu->vrom_phys_base, g_mmu->physical_vrom_size);
        mmu_register_vrom(g_mmu, host_ptr, phys_base, size);
    }
}

void memory_map_host_region_alias(memory_map_t *m, uint32_t alias_phys_base, uint32_t original_phys_base) {
    (void)m;
    if (!g_mmu)
        return;
    if (g_mmu->physical_vram && original_phys_base == g_mmu->vram_phys_base) {
        g_mmu->vram_phys_alt = alias_phys_base;
        return;
    }
    if (g_mmu->physical_vrom && original_phys_base == g_mmu->vrom_phys_base) {
        g_mmu->vrom_phys_alt = alias_phys_base;
        return;
    }
    // Unrecognised original — silently ignore in v1; once the storage
    // refactor lands and host regions become a list, we'll match by phys
    // base instead of by named slot. Log so callers know their alias was
    // dropped on the floor.
    LOG(1, "memory_map_host_region_alias: no host region at phys $%08X; alias $%08X dropped", original_phys_base,
        alias_phys_base);
}

void memory_set_bus_error_range(memory_map_t *m, uint32_t start, uint32_t end) {
    (void)m;
    if (!g_mmu)
        return;
    g_mmu->nubus_berr_start = start;
    g_mmu->nubus_berr_end = end;
}

// Invalidate the entire software TLB.  This is the "nuke" path used by
// machine reset, checkpoint restore, and ROM reload: every cached set is
// zeroed and un-keyed, then the active set is re-keyed to the MMU's current
// configuration.  Guest PMOVE/PFLUSH traffic no longer comes here — it goes
// through mmu_tc_written / mmu_root_tt_written / mmu_pflush below, which
// preserve cached sets per configuration.
//
// When the MMU is disabled, host-backed pages (RAM/ROM/VRAM) are installed
// lazily on first access by the memory.c slow path via rebuild_soa_page (the
// eager repopulate that used to live here dominated SE/30 boot; see
// docs/notes/mmu-tlb-invalidate-perf.md).
void mmu_invalidate_tlb(mmu_state_t *mmu) {
    for (int i = 0; i < SOA_SETS; i++) {
        soa_zero_set(&g_soa_sets[i]);
        g_soa_sets[i].key_valid = false;
    }
    soa_set_t *a = &g_soa_sets[g_soa_active];
    a->key = soa_current_key(mmu);
    a->key_valid = true;
}

// ============================================================================
// PMOVE / PFLUSH notifications (called from cpu_pmmu_general)
// ============================================================================

// PMOVE to TC completed (mmu->tc / mmu->enabled already updated).  A flushing
// write (fd=false), or any write that flips the enable state, activates the
// set matching the new configuration — the steady-state _SwapMMUMode path,
// where both configs stay cached and the switch is a pointer swap.  An FD
// (flush-disable) rewrite under a live config keeps the current view mapped
// (hardware keeps serving resident ATC entries) but taints it so the now-
// mixed contents can never be key-hit by a later activation.
void mmu_tc_written(mmu_state_t *mmu, bool fd, bool was_enabled) {
    if (!mmu)
        return;
    if (!fd || was_enabled != mmu->enabled) {
        soa_activate_current(mmu);
        return;
    }
    if (mmu->enabled)
        soa_taint_active();
}

// PMOVE to CRP/SRP/TT0/TT1 completed.  While translation is off these writes
// don't perturb the identity view — the register simply becomes part of the
// key at the next enable.  While enabled, a flushing write conservatively
// kills every enabled set's contents (the rewritten register may redefine any
// cached translation) and re-keys the active set to the new tuple; an FD
// write taints the active set instead (hardware ATC residency, as for TC).
void mmu_root_tt_written(mmu_state_t *mmu, bool fd) {
    if (!mmu || !mmu->enabled)
        return;
    if (fd) {
        soa_taint_active();
        return;
    }
    for (int i = 0; i < SOA_SETS; i++) {
        soa_set_t *s = &g_soa_sets[i];
        if (s->key_valid && !s->key.enabled)
            continue; // the disabled identity view doesn't depend on MMU registers
        soa_zero_set(s);
        s->key_valid = false;
    }
    soa_set_t *a = &g_soa_sets[g_soa_active];
    a->key = soa_current_key(mmu);
    a->key_valid = true;
}

// PFLUSH executed: on hardware the ATC dies no matter what E currently is, so
// every enabled configuration's cached contents die with it — a set rebuilt
// later re-walks the guest tables, which is exactly what PFLUSH demands.
// This is what keeps guests that edit PTEs and then PFLUSH (A/UX) correct
// under set reuse.  The disabled identity view is not an ATC artifact and
// survives.
void mmu_pflush(mmu_state_t *mmu) {
    (void)mmu;
    for (int i = 0; i < SOA_SETS; i++) {
        soa_set_t *s = &g_soa_sets[i];
        if (s->key_valid && !s->key.enabled)
            continue;
        soa_zero_set(s);
    }
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
static inline bool mmu_fault_epilogue(mmu_state_t *mmu, uint32_t emu_page, uint32_t phys_page, bool write) {
    uint32_t page_index = emu_page >> PAGE_SHIFT;
    if ((int)page_index < g_page_count) {
        uintptr_t *active = write ? g_active_write : g_active_read;
        if (active && active[page_index] == 0) {
            // For closer ranges (e.g., $006DB000 from corrupted page tables),
            // the f_trap handler detects unmapped instruction fetches separately.
            if (phys_page >= mmu->ram_size_max && phys_page < mmu->rom_phys_base)
                return false;
        }
    }
    return true;
}

// Handle a TLB miss: perform table walk or TT check, fill SoA entry.
// `probe_atc` selects whether the block-descriptor cache may satisfy the miss
// without a walk; PLOAD passes false to force a fresh table walk (its whole
// purpose is to reload the ATC from the current guest tables).
static bool mmu_handle_fault_internal(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor,
                                      bool probe_atc) {
    if (!mmu || !mmu->enabled)
        return false;

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
            if ((int)page_index < g_page_count && g_supervisor_read && g_supervisor_read[page_index] == 0 &&
                logical_addr >= mmu->nubus_berr_start && logical_addr <= mmu->nubus_berr_end) {
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
        if (b && !(b->supervisor_only && !supervisor) && !(b->write_protected && write)) {
            uint32_t phys_page = b->phys_base + (emu_page - b->log_base);
            mmu_fill_soa_entry(mmu, emu_page, phys_page, b->supervisor_only, b->write_protected, supervisor, false);
            return mmu_fault_epilogue(mmu, emu_page, phys_page, write);
        }
    }

    // Perform table walk
    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, write, supervisor);

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

    // TEMP DIAG: trace user-mode faults on page 0 (icode copyout to user VA 0).
    if (getenv("GS_VA0_TRACE") && write && !supervisor && logical_addr < 0x1000) {
        static int n = 0;
        if (n++ < 30)
            fprintf(stderr, "[VA0] la=%08x W usr valid=%d wp=%d suponly=%d phys=%08x mmusr=%08x crp=%08x:%08x\n",
                    logical_addr, result.valid, result.write_protected, result.supervisor_only, result.physical_addr,
                    result.mmusr, (unsigned)(mmu->crp >> 32), (unsigned)mmu->crp);
    }

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
    mmu_fill_soa_entry(mmu, emu_page, phys_page, result.supervisor_only, result.write_protected, supervisor, false);

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
        atc_record(logical_addr & log_mask, log_mask, result.physical_addr & log_mask, result.supervisor_only,
                   result.write_protected, supervisor);
    }

    // If phys_to_host returned NULL (unmapped physical), the SoA entry
    // stays zero.  On real hardware, the physical bus access would fail
    // (no device responds) and generate a bus error — see mmu_fault_epilogue.
    return mmu_fault_epilogue(mmu, emu_page, phys_page, write);
}

// Handle a TLB miss: perform table walk or TT check, fill SoA entry.
bool mmu_handle_fault(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor) {
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

    // Perform table walk (without modifying SoA entries)
    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, write, supervisor);

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

    if (mmu_check_tt(mmu, logical_addr, false, supervisor))
        return logical_addr;

    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, false, supervisor);

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
    if (mmu_check_tt(mmu, logical_addr, false, supervisor)) {
        if (pa_out)
            *pa_out = logical_addr;
        return true; // transparent translation: identity, valid
    }
    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, false, supervisor);
    if (pa_out)
        *pa_out = result.valid ? result.physical_addr : logical_addr;
    return result.valid;
}

// Translate against an explicit CRP root (e.g. a snapshot of MAE's CRP).
// Side-effect-free: temporarily swaps `mmu->crp`, performs a user-mode walk,
// restores the original CRP.  TT checks are skipped for the page offset only
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
    // Swap CRP, walk in user mode, restore.  The walk reads guest tables
    // via phys_to_host but does not touch the SoA arrays.
    uint64_t saved_crp = mmu->crp;
    mmu->crp = crp_root;
    mmu_walk_result_t result = mmu_table_walk(mmu, logical_addr, false, /*supervisor=*/false);
    mmu->crp = saved_crp;
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

bool mmu_write_physical_uint16(mmu_state_t *mmu, uint32_t phys_addr, uint16_t value) {
    if (!mmu_write_physical_uint8(mmu, phys_addr, (uint8_t)(value >> 8)))
        return false;
    return mmu_write_physical_uint8(mmu, phys_addr + 1, (uint8_t)value);
}

bool mmu_write_physical_uint32(mmu_state_t *mmu, uint32_t phys_addr, uint32_t value) {
    if (!mmu_write_physical_uint16(mmu, phys_addr, (uint16_t)(value >> 16)))
        return false;
    return mmu_write_physical_uint16(mmu, phys_addr + 2, (uint16_t)value);
}
