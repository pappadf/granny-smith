// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// predecode.h
// The predecoded-instruction page cache shared by the 68K and PowerPC
// cores (proposal-predecoded-interpreter-cores.md §3).  A block is 4 KB of
// guest code keyed by its HOST page: one 8-byte entry per 16-bit word (68K)
// or per instruction (PowerPC), decoded lazily at first execution.  Blocks
// are derived state — never checkpointed, rebuilt on demand — and are kept
// coherent with guest memory by the memory layer's code-page marks
// (memory.h "Code-page coherence"), which route every store into a cached
// page through the slow path and into predecode_invalidate_host().

#ifndef PREDECODE_H
#define PREDECODE_H

#include <stdbool.h>
#include <stdint.h>

// One predecoded entry: a handler id plus pre-extracted operands whose
// meaning is per id (the study's 8-byte format, unchanged).
typedef struct pd_entry {
    uint16_t id; // handler id; PD_UNDECODED (0) = not yet decoded
    uint8_t a; // per id: dst register byte offset / fall-through length / CR shift
    uint8_t b; // per id: src register byte offset / quick value / shift count
    uint32_t c; // per id: imm32 / sign-extended disp / target index / raw word
} pd_entry_t;

// Control ids shared by both architectures (below every T1/T0 id).
enum {
    PD_UNDECODED = 0, // entry never decoded (or invalidated)
    PD_CROSS = 1, // instruction straddles the page: generic path, fetched through memory
    PD_GENERIC = 2, // classifier declined: generic path (the one-instruction executor)
    PD_PAGE_END = 3, // the sentinel past the last entry: execution fell off the page, look the next one up
    PD_CONTROL_END = 16, // first architecture-specific id
};

// Which core owns a block: sets the entry granularity and how many
// preceding entries an invalidation has to reset (the longest 68020
// instruction is 11 words; a PowerPC instruction is one word).
typedef enum pd_arch { PD_ARCH_68K = 0, PD_ARCH_PPC = 1 } pd_arch_t;

#define PD_ENTRIES_68K         2048 // one per 16-bit word of the page
#define PD_ENTRIES_PPC         1024 // one per instruction word of the page
#define PD_INVALIDATE_BACK_68K 11
#define PD_INVALIDATE_BACK_PPC 1

// A decoded page.  The raw shadow keeps the guest words each entry was
// decoded from: the 68000 core latches `ir` from it, and debug builds
// audit every dispatched entry against live memory through it.
typedef struct pd_block {
    uint8_t *host; // key: host page pointer (page-aligned); NULL = free
    uint32_t guest_lo; // guest address the entries were decoded at: PC-relative and out-of-page targets are absolute,
                       // so another alias of the same host page re-decodes
    uint32_t region; // memory code region index the page lies in
    uint32_t page; // page index within that region
    uint32_t seq; // allocation sequence number (round-robin eviction)
    uint32_t writes; // guest stores into this page within the current window (thrash detector)
    uint32_t write_window; // pool lookup count when the current write window opened
    uint32_t execs; // instructions retired from this page within the window (accounted at block exits)
    uint32_t enter_budget; // *instructions when the executor entered this block (live execs = this - budget)
    uint32_t arch; // pd_arch_t
    bool thrashed; // demotion requested; released at the next lookup
    pd_entry_t e[PD_ENTRIES_68K + 1]; // +1: the PD_PAGE_END sentinel (index 2048 for 68K, 1024 for PPC)
    union {
        uint16_t raw16[PD_ENTRIES_68K]; // 68K: raw instruction words
        uint32_t raw32[PD_ENTRIES_PPC]; // PPC: raw instruction words
    };
} pd_block_t;

// === Runtime switches (machine.cpu.predecode / predecode.*) ===

bool predecode_enabled(void);
void predecode_set_enabled(bool on);

// The block the executor is running from (NULL between sprints and on the
// generic tier).  Executors call predecode_enter at every block transition
// and at sprint exit; the pair charges the instructions retired since the
// entry to the block being left, which is what the thrash detector weighs
// a page's stores against.
extern struct pd_block *g_pd_current;
static inline void predecode_enter(struct pd_block *blk, uint32_t budget) {
    struct pd_block *cur = g_pd_current;
    if (cur)
        cur->execs += cur->enter_budget - budget;
    g_pd_current = blk;
    if (blk)
        blk->enter_budget = budget;
}
// Demotion economics: a page is demoted once it took more than thrash_limit
// stores in a window AND those stores outnumber a ratio'th of the
// instructions it retired in the window (a suppressed store costs roughly
// `ratio` instructions' worth of predecode gain).
void predecode_set_thrash_ratio(uint32_t ratio);
int predecode_elide_level(void); // 0 = off, 1 = E1 (register-only definers), 2 = E2 (memory forms too)
void predecode_set_elide(int level);

// Tunables (also settable through the `predecode` node).  Setting the pool
// cap drops every block.
void predecode_set_pool_cap(uint32_t blocks);
void predecode_set_thrash(uint32_t limit, uint32_t window_lookups, uint32_t demote_hold_allocs);

// === Pool ===

// Find (or allocate) the block for a host page.  NULL when the page lies
// outside every code region, the cache is disabled, or the page is
// demoted (thrashing) — the caller then runs the generic tier until the
// next page transition.  `host_page` must be page-aligned.
// The block for host_page as executed at guest address guest_lo (the page's
// guest base).  A block found under another alias of the same host page is
// re-decoded: entries hold absolute PC-relative and out-of-page targets.
pd_block_t *predecode_lookup(uint8_t *host_page, uint32_t guest_lo, pd_arch_t arch);

// Release a block (demotion or eviction): clears the page's code mark so
// its write entries refill lazily.
void predecode_release(pd_block_t *blk);

// Drop every block (memory map replaced, hardware reset, model change).
void predecode_reset(void);

// The memory layer's g_mem_code_written_hook: a store or host-side writer
// changed `len` bytes at `host` — reset the entries covering them (plus the
// preceding ones an instruction could start in) and count toward demotion.
void predecode_invalidate_host(const uint8_t *host, uint32_t len);

// Decode histogram: decodes per id, per architecture (predecode.hist prints
// the top entries with names).  Reset with the pool.
void predecode_count_decode(pd_arch_t arch, uint16_t id);
// Name hooks, installed by the cores (cpu.c / ppc.c): id → handler name.
extern const char *(*g_pd_id_name[2])(uint16_t id);

// Statistics (predecode.stats); plain counters, reset with the pool.
typedef struct pd_stats {
    uint64_t lookups; // page transitions that consulted the pool
    uint64_t allocs; // blocks allocated
    uint64_t evictions; // blocks recycled under pool pressure
    uint64_t decodes; // entries decoded
    uint64_t invalidations; // invalidate calls that reset at least one entry
    uint64_t demotions; // blocks released for thrashing
    uint64_t elided; // entries retargeted to a no-flags twin
    uint64_t realiases; // blocks re-decoded because the same host page ran at another guest address
    uint64_t generic_steps; // instructions run through the generic tier (uncached, demoted, PD_GENERIC)
    uint64_t generic_cross; // ...of which: PD_CROSS entries (the instruction straddles the page)
    uint64_t generic_declined; // ...of which: PD_GENERIC entries (the classifier declined the shape)
    uint64_t generic_slowmode; // ...of which: the executor's own slow mode (post-fault user mode, trace)
    uint64_t relookup_nomap; // relookups that found no fast-path read entry for the PC's page
    uint64_t relookup_nopool; // relookups whose page the pool declined (no region, held, pool full)
    uint64_t lookup_noregion; // ...of which: the host page is in no registered code region
    uint64_t lookup_held; // ...of which: the page is demoted and held on the generic tier
} pd_stats_t;
extern pd_stats_t g_pd_stats;
extern uint32_t g_pd_blocks_live; // blocks currently holding a page

// Debug-build audit (GS_DEBUG): re-verify a dispatched entry's raw words
// against live memory.  Compiled to nothing in release builds.
#ifdef GS_DEBUG
void predecode_audit_fail(const pd_block_t *blk, uint32_t idx, uint32_t words);
#define PD_AUDIT_68K(blk, idx, words)                                                                                  \
    do {                                                                                                               \
        for (uint32_t _w = 0; _w < (words); _w++)                                                                      \
            if ((blk)->raw16[(idx) + _w] != __builtin_bswap16(*(const uint16_t *)((blk)->host + (((idx) + _w) << 1)))) \
                predecode_audit_fail((blk), (idx), (words));                                                           \
    } while (0)
#define PD_AUDIT_PPC(blk, idx)                                                                                         \
    do {                                                                                                               \
        if ((blk)->raw32[(idx)] != __builtin_bswap32(*(const uint32_t *)((blk)->host + ((idx) << 2))))                 \
            predecode_audit_fail((blk), (idx), 1);                                                                     \
    } while (0)
#else
#define PD_AUDIT_68K(blk, idx, words) ((void)0)
#define PD_AUDIT_PPC(blk, idx)        ((void)0)
#endif

// Attach the `predecode` object-model node (idempotent; process lifetime).
void predecode_object_install(void);

#endif // PREDECODE_H
