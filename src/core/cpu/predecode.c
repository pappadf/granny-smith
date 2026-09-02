// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// predecode.c
// The predecoded-instruction page pool (predecode.h): allocation keyed by
// host page, round-robin eviction, sub-page invalidation driven by the
// memory layer's code-page marks, thrash demotion, the debug audit, and
// the `predecode` object-model node.  The decoders and executors that fill
// and run the entries live in the cores (cpu_pd_run.h, ppc_pd_run.c).

#include "predecode.h"

#include "log.h"
#include "memory.h"
#include "object.h"
#include "value.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("predecode");

// === Switches and tunables ===

static bool g_pd_enabled = true; // the predecoded executors (predecode.enabled=0: the switch cores)
static int g_pd_elide = 2; // flag-liveness elision level (68K): E2
static uint32_t g_pd_pool_cap = 2048; // blocks (16 KB of 68K entries + 4 KB shadow each)
static uint32_t g_pd_thrash_limit = 32; // stores into one page within the window before the ratio rule applies
static uint32_t g_pd_thrash_window = 4096; // the window, in pool lookups (page transitions)
static uint32_t g_pd_demote_hold = 65536; // pool lookups (page transitions) a demoted page stays on the generic tier
static uint32_t g_pd_thrash_ratio = 64; // stores × ratio > instructions retired from the page → demote
pd_block_t *g_pd_current = NULL;

// === Pool state ===

pd_stats_t g_pd_stats;
uint32_t g_pd_blocks_live = 0;
const char *(*g_pd_id_name[2])(uint16_t id) = {NULL, NULL};

// Decodes per id and architecture (256 KB each; the histogram behind the
// specialization order of proposal §4.2/§6.2).
static uint32_t *g_pd_hist[2];

void predecode_count_decode(pd_arch_t arch, uint16_t id) {
    if (!g_pd_hist[arch])
        g_pd_hist[arch] = (uint32_t *)calloc(65536, sizeof(uint32_t));
    if (g_pd_hist[arch])
        g_pd_hist[arch][id]++;
    g_pd_stats.decodes++;
}

static pd_block_t **g_pd_pool = NULL; // every block ever allocated, up to the cap
static uint32_t g_pd_pool_count = 0;
static uint32_t g_pd_pool_next = 0; // round-robin recycle cursor
static uint32_t g_pd_seq = 0; // allocation sequence

// Per code region: page → block slot, and the demotion stamp per page.
static pd_block_t **g_pd_slots[MEM_CODE_REGIONS_MAX];
static uint32_t *g_pd_demoted[MEM_CODE_REGIONS_MAX];
static uint8_t *g_pd_demote_count[MEM_CODE_REGIONS_MAX]; // demotions so far per page: the hold backs off
static uint32_t g_pd_slot_pages[MEM_CODE_REGIONS_MAX];
static uint32_t g_pd_generation = 0; // g_mem_map_generation the slots belong to

bool predecode_enabled(void) {
    return g_pd_enabled;
}

void predecode_set_enabled(bool on) {
    g_pd_enabled = on;
}

int predecode_elide_level(void) {
    return g_pd_elide;
}

void predecode_set_elide(int level) {
    g_pd_elide = level < 0 ? 0 : level > 2 ? 2 : level;
}

void predecode_set_thrash(uint32_t limit, uint32_t window_lookups, uint32_t demote_hold_lookups) {
    g_pd_thrash_limit = limit;
    g_pd_thrash_window = window_lookups;
    g_pd_demote_hold = demote_hold_lookups;
}

void predecode_set_thrash_ratio(uint32_t ratio) {
    g_pd_thrash_ratio = ratio;
}

// Forget the per-region slot tables (the regions they index are gone).
static void slots_reset(void) {
    for (int r = 0; r < MEM_CODE_REGIONS_MAX; r++) {
        free(g_pd_slots[r]);
        free(g_pd_demoted[r]);
        free(g_pd_demote_count[r]);
        g_pd_slots[r] = NULL;
        g_pd_demoted[r] = NULL;
        g_pd_demote_count[r] = NULL;
        g_pd_slot_pages[r] = 0;
    }
}

// Make sure region r has a slot table sized to the region.
static bool slots_ensure(int r) {
    if (g_pd_slots[r])
        return true;
    uint32_t pages = (uint32_t)((g_mem_code_regions[r].size + MEM_PAGE_SIZE - 1) >> PAGE_SHIFT);
    g_pd_slots[r] = (pd_block_t **)calloc(pages, sizeof(pd_block_t *));
    g_pd_demoted[r] = (uint32_t *)calloc(pages, sizeof(uint32_t));
    g_pd_demote_count[r] = (uint8_t *)calloc(pages, sizeof(uint8_t));
    if (!g_pd_slots[r] || !g_pd_demoted[r] || !g_pd_demote_count[r]) {
        free(g_pd_slots[r]);
        free(g_pd_demoted[r]);
        free(g_pd_demote_count[r]);
        g_pd_slots[r] = NULL;
        g_pd_demoted[r] = NULL;
        g_pd_demote_count[r] = NULL;
        return false;
    }
    g_pd_slot_pages[r] = pages;
    return true;
}

// Detach a block from its page: clear the slot and the code mark.
static void block_detach(pd_block_t *blk) {
    if (!blk->host)
        return;
    if (blk->region < MEM_CODE_REGIONS_MAX && g_pd_slots[blk->region] && blk->page < g_pd_slot_pages[blk->region] &&
        g_pd_slots[blk->region][blk->page] == blk)
        g_pd_slots[blk->region][blk->page] = NULL;
    memory_code_page_unmark(blk->host);
    blk->host = NULL;
    blk->thrashed = false;
    if (g_pd_current == blk)
        g_pd_current = NULL;
    if (g_pd_blocks_live)
        g_pd_blocks_live--;
}

void predecode_release(pd_block_t *blk) {
    if (blk)
        block_detach(blk);
}

void predecode_reset(void) {
    for (uint32_t i = 0; i < g_pd_pool_count; i++)
        block_detach(g_pd_pool[i]);
    slots_reset();
    g_pd_pool_next = 0;
    g_pd_blocks_live = 0;
    g_pd_current = NULL;
    memset(&g_pd_stats, 0, sizeof(g_pd_stats));
    for (int a = 0; a < 2; a++)
        if (g_pd_hist[a])
            memset(g_pd_hist[a], 0, 65536 * sizeof(uint32_t));
    g_pd_generation = g_mem_map_generation;
}

void predecode_set_pool_cap(uint32_t blocks) {
    if (blocks < 4)
        blocks = 4;
    predecode_reset(); // detach every block, then free the pool itself
    for (uint32_t i = 0; i < g_pd_pool_count; i++)
        free(g_pd_pool[i]);
    free(g_pd_pool);
    g_pd_pool = NULL;
    g_pd_pool_count = 0;
    g_pd_pool_cap = blocks;
}

// Take a block for a new page: a fresh one below the cap, else the next
// round-robin victim (evicted from its page first).
static pd_block_t *block_take(void) {
    pd_block_t *blk;
    if (g_pd_pool_count < g_pd_pool_cap) {
        if (!g_pd_pool) {
            g_pd_pool = (pd_block_t **)calloc(g_pd_pool_cap, sizeof(pd_block_t *));
            if (!g_pd_pool)
                return NULL;
        }
        blk = (pd_block_t *)calloc(1, sizeof(pd_block_t));
        if (!blk)
            return NULL;
        g_pd_pool[g_pd_pool_count++] = blk;
        return blk;
    }
    // Round-robin over the whole pool: the least recently ALLOCATED page
    // goes (hot code pages allocate once and stay).
    blk = g_pd_pool[g_pd_pool_next];
    g_pd_pool_next = (g_pd_pool_next + 1) % g_pd_pool_count;
    if (blk->host) {
        block_detach(blk);
        g_pd_stats.evictions++;
    }
    return blk;
}

pd_block_t *predecode_lookup(uint8_t *host_page, pd_arch_t arch) {
    if (!g_pd_enabled)
        return NULL;
    if (__builtin_expect(g_mem_code_written_hook == NULL, 0))
        g_mem_code_written_hook = predecode_invalidate_host; // first use without the object node (unit tests)
    if (__builtin_expect(g_pd_generation != g_mem_map_generation, 0))
        predecode_reset(); // the regions changed under the pool: start over
    g_pd_stats.lookups++;
    uint32_t page;
    int r = memory_code_region_of(host_page, &page);
    if (r < 0) {
        g_pd_stats.lookup_noregion++;
        return NULL; // device window, card VRAM, declaration ROM: generic tier
    }
    if (!g_pd_slots[r] && !slots_ensure(r))
        return NULL;
    if (page >= g_pd_slot_pages[r])
        return NULL;
    pd_block_t *blk = g_pd_slots[r][page];
    if (blk) {
        if (__builtin_expect(blk->thrashed, 0)) {
            // Demote: the page mixes hot stores with code — back to the
            // generic tier (today's speed) until the hold expires.
            // The hold doubles with every demotion of the same page (a page
            // that keeps coming back and thrashing is a data page).
            uint8_t n = g_pd_demote_count[r] ? g_pd_demote_count[r][page] : 0;
            g_pd_demoted[r][page] = (uint32_t)g_pd_stats.lookups + (g_pd_demote_hold << (n < 8 ? n : 8));
            if (g_pd_demote_count[r] && n < 255)
                g_pd_demote_count[r][page] = n + 1;
            block_detach(blk);
            g_pd_stats.demotions++;
            return NULL;
        }
        return blk;
    }
    if ((uint32_t)g_pd_stats.lookups - g_pd_demoted[r][page] > 0x80000000u) {
        g_pd_stats.lookup_held++;
        return NULL; // still held on the generic tier (hold expiry ahead of the lookup count)
    }
    blk = block_take();
    if (!blk)
        return NULL;
    blk->host = host_page;
    blk->region = (uint32_t)r;
    blk->page = page;
    blk->seq = ++g_pd_seq;
    blk->writes = 0;
    blk->write_window = (uint32_t)g_pd_stats.lookups;
    blk->execs = 0;
    blk->enter_budget = 0;
    blk->arch = (uint32_t)arch;
    blk->thrashed = false;
    // All entries undecoded; the raw shadow is filled as entries decode.
    // The sentinel past the last entry catches a straight-line fall-through
    // off the page (an entry there would index the shadow out of bounds).
    memset(blk->e, 0, sizeof(blk->e));
    blk->e[arch == PD_ARCH_PPC ? PD_ENTRIES_PPC : PD_ENTRIES_68K].id = PD_PAGE_END;
    g_pd_slots[r][page] = blk;
    memory_code_page_mark(host_page); // suppress the page's write entries
    g_pd_stats.allocs++;
    g_pd_blocks_live++;
    return blk;
}

void predecode_invalidate_host(const uint8_t *host, uint32_t len) {
    if (len == 0)
        return;
    uint32_t page;
    int r = memory_code_region_of(host, &page);
    if (r < 0 || !g_pd_slots[r] || page >= g_pd_slot_pages[r])
        return;
    pd_block_t *blk = g_pd_slots[r][page];
    if (!blk)
        return;
    uint32_t off = (uint32_t)((uintptr_t)host - (uintptr_t)blk->host);
    uint32_t end = off + len; // exclusive; the memory layer splits ranges per page
    if (end > MEM_PAGE_SIZE)
        end = MEM_PAGE_SIZE;
    uint32_t first, last;
    if (blk->arch == PD_ARCH_PPC) {
        first = off >> 2;
        last = (end - 1) >> 2;
        first = first >= PD_INVALIDATE_BACK_PPC ? first - PD_INVALIDATE_BACK_PPC : 0;
    } else {
        // An entry's decision depends on its own words and, through the
        // elision pass, on its successor's: reset the longest instruction's
        // worth of predecessors too.
        first = off >> 1;
        last = (end - 1) >> 1;
        first = first >= PD_INVALIDATE_BACK_68K ? first - PD_INVALIDATE_BACK_68K : 0;
    }
    bool any = false;
    for (uint32_t i = first; i <= last; i++) {
        if (blk->e[i].id != PD_UNDECODED) {
            blk->e[i].id = PD_UNDECODED;
            any = true;
        }
    }
    if (any)
        g_pd_stats.invalidations++;
    // Thrash detector: stores into this page within a window of page
    // transitions.  The page keeps its block until the CPU's next lookup
    // (its executor may be running from it right now).
    uint32_t now = (uint32_t)g_pd_stats.lookups;
    uint32_t budget = g_bus_error_instr_ptr ? *g_bus_error_instr_ptr : 0;
    if (now - blk->write_window > g_pd_thrash_window) {
        blk->write_window = now; // a fresh window starts at this store
        blk->writes = 0;
        blk->execs = 0;
        if (blk == g_pd_current)
            blk->enter_budget = budget;
    }
    // Instructions retired from the page this window, including the ones
    // of the current visit (the executor may be storing into its own page).
    uint32_t execs = blk->execs;
    if (blk == g_pd_current)
        execs += blk->enter_budget - budget;
    if (++blk->writes > g_pd_thrash_limit && (uint64_t)blk->writes * g_pd_thrash_ratio > execs)
        blk->thrashed = true;
}

#ifdef GS_DEBUG
// The single most important check of the design: a dispatched entry whose
// raw words no longer match memory means a writer bypassed the marks.
void predecode_audit_fail(const pd_block_t *blk, uint32_t idx, uint32_t words) {
    uint32_t gran = blk->arch == PD_ARCH_PPC ? 4u : 2u;
    fprintf(stderr, "predecode: coherence audit failed: host page %p entry %u (%u word%s) region %u page %u\n",
            (const void *)blk->host, idx, words, words == 1 ? "" : "s", blk->region, blk->page);
    for (uint32_t w = 0; w < words; w++) {
        uint32_t off = (idx + w) * gran;
        uint32_t cached = blk->arch == PD_ARCH_PPC ? blk->raw32[idx + w] : blk->raw16[idx + w];
        uint32_t live = blk->arch == PD_ARCH_PPC ? LOAD_BE32(blk->host + off) : LOAD_BE16(blk->host + off);
        fprintf(stderr, "  +%03X cached %0*X live %0*X\n", off, (int)gran * 2, cached, (int)gran * 2, live);
    }
    LOG(0, "predecode coherence audit failed (see stderr)");
    assert(!"predecode coherence audit failed");
    abort();
}
#endif

// === Object model: `predecode` (root sibling of `machine`) ===

static value_t pd_attr_enabled(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(1, g_pd_enabled ? 1 : 0);
}

static value_t pd_set_enabled(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    predecode_set_enabled((in.u & 1u) != 0);
    return val_none();
}

static value_t pd_attr_elide(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(1, (uint64_t)g_pd_elide);
}

static value_t pd_set_elide(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    predecode_set_elide((int)in.u);
    predecode_reset(); // decisions already baked into entries follow the old level
    return val_none();
}

// Tunables and counters share one getter/setter pair keyed on user_data.
enum {
    PDA_POOL_CAP = 1,
    PDA_THRASH_LIMIT,
    PDA_THRASH_WINDOW,
    PDA_DEMOTE_HOLD,
    PDA_BLOCKS,
    PDA_LOOKUPS,
    PDA_ALLOCS,
    PDA_EVICTIONS,
    PDA_DECODES,
    PDA_INVALIDATIONS,
    PDA_DEMOTIONS,
    PDA_ELIDED,
    PDA_SUPPRESSED_WRITES,
    PDA_THRASH_RATIO,
    PDA_GENERIC_STEPS,
    PDA_GENERIC_CROSS,
    PDA_GENERIC_DECLINED,
    PDA_GENERIC_SLOWMODE,
    PDA_RELOOKUP_NOMAP,
    PDA_RELOOKUP_NOPOOL,
    PDA_LOOKUP_NOREGION,
    PDA_LOOKUP_HELD,
};

static value_t pd_attr_get(struct object *self, const member_t *m) {
    (void)self;
    switch ((int)(uintptr_t)m->attr.user_data) {
    case PDA_POOL_CAP:
        return val_uint(4, g_pd_pool_cap);
    case PDA_THRASH_LIMIT:
        return val_uint(4, g_pd_thrash_limit);
    case PDA_THRASH_WINDOW:
        return val_uint(4, g_pd_thrash_window);
    case PDA_DEMOTE_HOLD:
        return val_uint(4, g_pd_demote_hold);
    case PDA_BLOCKS:
        return val_uint(4, g_pd_blocks_live);
    case PDA_LOOKUPS:
        return val_uint(8, g_pd_stats.lookups);
    case PDA_ALLOCS:
        return val_uint(8, g_pd_stats.allocs);
    case PDA_EVICTIONS:
        return val_uint(8, g_pd_stats.evictions);
    case PDA_DECODES:
        return val_uint(8, g_pd_stats.decodes);
    case PDA_INVALIDATIONS:
        return val_uint(8, g_pd_stats.invalidations);
    case PDA_DEMOTIONS:
        return val_uint(8, g_pd_stats.demotions);
    case PDA_ELIDED:
        return val_uint(8, g_pd_stats.elided);
    case PDA_SUPPRESSED_WRITES:
        return val_uint(8, g_mem_code_write_count);
    case PDA_THRASH_RATIO:
        return val_uint(4, g_pd_thrash_ratio);
    case PDA_GENERIC_STEPS:
        return val_uint(8, g_pd_stats.generic_steps);
    case PDA_GENERIC_CROSS:
        return val_uint(8, g_pd_stats.generic_cross);
    case PDA_GENERIC_DECLINED:
        return val_uint(8, g_pd_stats.generic_declined);
    case PDA_GENERIC_SLOWMODE:
        return val_uint(8, g_pd_stats.generic_slowmode);
    case PDA_RELOOKUP_NOMAP:
        return val_uint(8, g_pd_stats.relookup_nomap);
    case PDA_RELOOKUP_NOPOOL:
        return val_uint(8, g_pd_stats.relookup_nopool);
    case PDA_LOOKUP_NOREGION:
        return val_uint(8, g_pd_stats.lookup_noregion);
    case PDA_LOOKUP_HELD:
        return val_uint(8, g_pd_stats.lookup_held);
    default:
        return val_err("unknown predecode attribute");
    }
}

static value_t pd_attr_set(struct object *self, const member_t *m, value_t in) {
    (void)self;
    switch ((int)(uintptr_t)m->attr.user_data) {
    case PDA_POOL_CAP:
        if (in.u < 16 || in.u > 65536)
            return val_err("pool_cap must be 16..65536 blocks");
        predecode_set_pool_cap((uint32_t)in.u);
        return val_none();
    case PDA_THRASH_LIMIT:
        g_pd_thrash_limit = (uint32_t)in.u;
        return val_none();
    case PDA_THRASH_WINDOW:
        g_pd_thrash_window = (uint32_t)in.u;
        return val_none();
    case PDA_DEMOTE_HOLD:
        g_pd_demote_hold = (uint32_t)in.u;
        return val_none();
    case PDA_THRASH_RATIO:
        g_pd_thrash_ratio = (uint32_t)in.u;
        return val_none();
    default:
        return val_err("read-only predecode attribute");
    }
}

// `predecode.hist([top])` — print the most-decoded ids (static shape
// frequency of the code executed so far) with their names and tier.
static value_t pd_method_hist(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    uint32_t top = (argc >= 1 && argv[0].u > 0) ? (uint32_t)argv[0].u : 40u;
    for (int a = 0; a < 2; a++) {
        if (!g_pd_hist[a])
            continue;
        uint64_t total = 0;
        for (uint32_t i = 0; i < 65536; i++)
            total += g_pd_hist[a][i];
        if (!total)
            continue;
        printf("%s: %llu decoded entries\n", a == PD_ARCH_PPC ? "ppc" : "68k", (unsigned long long)total);
        for (uint32_t n = 0; n < top; n++) {
            uint32_t best = 0, best_i = 0;
            for (uint32_t i = 0; i < 65536; i++)
                if (g_pd_hist[a][i] > best) {
                    best = g_pd_hist[a][i];
                    best_i = i;
                }
            if (!best)
                break;
            const char *name = g_pd_id_name[a] ? g_pd_id_name[a]((uint16_t)best_i) : NULL;
            printf("  %5.1f%% %9u  %-5u %s\n", 100.0 * best / (double)total, best, best_i, name ? name : "?");
            g_pd_hist[a][best_i] = 0; // consumed; the histogram is a diagnostic snapshot
        }
    }
    return val_none();
}

static const arg_decl_t pd_hist_args[] = {
    {.name = "top", .kind = V_UINT, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "entries to print (default 40)"},
};

static value_t pd_method_reset(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    predecode_reset();
    return val_none();
}

#define PD_ATTR_RW(name_, id_, doc_)                                                                                   \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .doc = doc_, .attr = {                                                          \
            .type = V_UINT,                                                                                            \
            .get = pd_attr_get,                                                                                        \
            .set = pd_attr_set,                                                                                        \
            .user_data = (const void *)(uintptr_t)(id_)                                                                \
        }                                                                                                              \
    }
#define PD_ATTR_RO(name_, id_, doc_)                                                                                   \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .doc = doc_, .flags = VAL_RO, .attr = {                                         \
            .type = V_UINT,                                                                                            \
            .get = pd_attr_get,                                                                                        \
            .set = NULL,                                                                                               \
            .user_data = (const void *)(uintptr_t)(id_)                                                                \
        }                                                                                                              \
    }

static const member_t predecode_members[] = {
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "1 = the predecoded executors run the main CPU; 0 = the switch cores (A/B from the shell)",
     .attr = {.type = V_UINT, .get = pd_attr_enabled, .set = pd_set_enabled}                             },
    {.kind = M_ATTR,
     .name = "elide",
     .doc = "68K flag-liveness elision level: 0 off, 1 register-only definers (E1), 2 memory forms too (E2)",
     .attr = {.type = V_UINT, .get = pd_attr_elide, .set = pd_set_elide}                                 },
    PD_ATTR_RW("pool_cap", PDA_POOL_CAP, "maximum blocks in the pool (setting it drops every block)"),
    PD_ATTR_RW("thrash_limit", PDA_THRASH_LIMIT, "stores into one code page within thrash_window before it is demoted"),
    PD_ATTR_RW("thrash_window", PDA_THRASH_WINDOW, "the demotion window, in page transitions"),
    PD_ATTR_RW("demote_hold", PDA_DEMOTE_HOLD, "page transitions a demoted page stays on the generic tier"),
    PD_ATTR_RW("thrash_ratio", PDA_THRASH_RATIO,
               "demote when stores x ratio exceed the instructions the page retired in the window"),
    PD_ATTR_RO("blocks", PDA_BLOCKS, "blocks currently holding a code page"),
    PD_ATTR_RO("lookups", PDA_LOOKUPS, "page transitions that consulted the pool"),
    PD_ATTR_RO("allocs", PDA_ALLOCS, "blocks allocated"),
    PD_ATTR_RO("evictions", PDA_EVICTIONS, "blocks recycled under pool pressure"),
    PD_ATTR_RO("decodes", PDA_DECODES, "entries decoded"),
    PD_ATTR_RO("invalidations", PDA_INVALIDATIONS, "stores that reset at least one cached entry"),
    PD_ATTR_RO("demotions", PDA_DEMOTIONS, "blocks released for thrashing"),
    PD_ATTR_RO("elided", PDA_ELIDED, "entries retargeted to a no-flags twin"),
    PD_ATTR_RO("generic_steps", PDA_GENERIC_STEPS, "instructions run through the generic tier"),
    PD_ATTR_RO("generic_cross", PDA_GENERIC_CROSS, "...of which page-straddling instructions"),
    PD_ATTR_RO("generic_declined", PDA_GENERIC_DECLINED, "...of which shapes the classifier declined"),
    PD_ATTR_RO("generic_slowmode", PDA_GENERIC_SLOWMODE, "...of which the executor's slow mode (post-fault, trace)"),
    PD_ATTR_RO("relookup_nomap", PDA_RELOOKUP_NOMAP, "page transitions with no fast-path read entry for the PC"),
    PD_ATTR_RO("relookup_nopool", PDA_RELOOKUP_NOPOOL, "page transitions the pool declined (no region, held, full)"),
    PD_ATTR_RO("lookup_noregion", PDA_LOOKUP_NOREGION, "...of which the host page lies in no code region"),
    PD_ATTR_RO("lookup_held", PDA_LOOKUP_HELD, "...of which the page is demoted and held"),
    PD_ATTR_RO("suppressed_writes", PDA_SUPPRESSED_WRITES,
               "guest stores that hit a code page (slow path + invalidate)"),
    {.kind = M_METHOD,
     .name = "hist",
     .doc = "Print the most-decoded ids with their names (static shape histogram; consumes the counts)",
     .method = {.args = pd_hist_args, .nargs = 1, .result = V_NONE, .fn = pd_method_hist}                },
    {.kind = M_METHOD,
     .name = "reset",
     .doc = "Drop every block and zero the counters",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = pd_method_reset, .ui_flags = MM_MUTATE}},
};

static const class_desc_t predecode_class = {
    .name = "predecode",
    .members = predecode_members,
    .n_members = sizeof(predecode_members) / sizeof(predecode_members[0]),
};

static struct object *s_predecode_object = NULL;

void predecode_object_install(void) {
    if (s_predecode_object)
        return;
    s_predecode_object = object_new(&predecode_class, NULL, "predecode");
    if (s_predecode_object) {
        object_set_label(s_predecode_object, "Predecode");
        object_attach(object_root(), s_predecode_object);
    }
    // The memory layer reports code-page stores here from now on.
    g_mem_code_written_hook = predecode_invalidate_host;
}
