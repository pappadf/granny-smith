// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_jit.c
// The PowerPC translation tier (ppc_jit.h).  Three parts:
//   1. a WebAssembly byte writer;
//   2. the translator: a region's member pages -> one wasm module whose one
//      function runs the region's blocks;
//   3. the tier manager: regions, entry, invalidation, instantiation.
// Parts 2 and 3 exist only in the wasm build (they bake linear-memory
// addresses into the code); native builds keep the stubs at the end.
//
// The translated function:
//   (param p budget entry folds) (result budget)
//   - the registers the region names live in wasm locals, loaded at entry
//     and stored at every exit;
//   - the blocks of all member pages, in address order, nested inside one
//     loop and entered through a br_table: a branch to a block of the
//     region is `idx = b; br $loop`, an indirect one (blr, bctr) looks its
//     target up in the region's maps;
//   - each block first checks that it is still valid (a store into a word
//     it translated clears its flag) and that the budget and fold
//     allowance fit it with a slot to spare (the predecoded tier's fold
//     rule, below); otherwise it leaves at its first instruction;
//   - a crossing into another page re-checks, in instruction-translation
//     mode, that the fetch TLB still maps that page to the host page that
//     was translated;
//   - a load or store is inlined when it is naturally aligned, outside a
//     601 T=1 segment and has an active SoA entry; otherwise the function
//     leaves at it (DEOPT) and the predecoded loop runs it.
//
// Budget and folding (ppc_pd_run.h): every instruction takes a slot, except
// a branch, which folds (takes none, uses one of the sprint's fold
// allowance) unless it is the sprint's last slot or a branch to itself.  A
// block of n instructions ending in a branch therefore costs n - 1 slots
// and one fold; the guard `budget > n` leaves at least two slots when the
// branch retires, so it always folds, and a block never takes the sprint's
// last slot: *instructions reaches zero only in the predecoded loop.

#include "ppc_jit.h"

#include "ppc_internal.h"
#include "ppc_pd_ids.h"

#include "memory.h"
#include "object.h"
#include "predecode.h"
#include "value.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool g_ppc_jit_on;

// Tunables (the `jit` node; settable in every build so scripts run anywhere)
static uint32_t g_threshold = 100000; // instructions run in a page (predecoded) before it joins its region
static uint32_t g_cooldown = 64; // entries between two compiles of a region (0: none)
static uint32_t g_quiet = 64; // entries after which a region with invalidated blocks is retranslated
static uint32_t g_chunk_cap = 1024; // instructions per translated function (a region may have several)
static uint32_t g_budget_pct = 3; // jit.budget: the share of time translating may take
#define JIT_BUDGET_START_MS 50.0
static uint32_t g_trace; // jit.trace: print every entry and exit (debugging)
#define JIT_CHURN_MAX 3 // translations a member page may be invalidated after before it is left to the predecoded tier
#define JIT_COOL_MAX  65536u // the compile backoff's ceiling, in entries

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

// The predecoded tier's classifier (ppc_run.c): the translator treats as
// translatable exactly the shapes the predecoded loop specializes (T0).
uint16_t ppc_pd_classify_word(uint32_t iw, uint32_t ipc, uint32_t page_lo, pd_entry_t *e);

// Shapes the predecoded loop leaves generic (T1) but the translator handles
// from the raw word, with ids of its own above the T0 range:
//   JX_BC               every bc form the T0 ids are not (linking,
//                       decrementing with a condition): a = BO, b = BI,
//                       c = the raw word
//   JX_BCLR, JX_BCCTR   every bclr/bcctr form the T0 BLR/BCTR are not
//                       (conditional, decrementing, linking): a = BO, b = BI,
//                       c = LK
//   JX_LWZX ..          the X-form loads and stores: a = rT/rS offset,
//                       b = rA offset (JX_RA0: rA = 0, the non-update forms
//                       only), c = rB offset
//   JX_ADDX             the XO-form add/subtract family the T0 ids leave
//                       out (carrying, extended, OE): a = rT offset, b = rA
//                       offset, c = rB offset | kind << 8 | OE << 16 | Rc << 17
// and the D-form loads and stores with rA = 0 keep their T0 ids with
// b = JX_RA0.
#define JX_RA0 0xFFu
enum { // JX_ADDX kinds: rT = (subf ? ~rA : rA) + operand + carry-in
    AK_ADD, // rB, 0
    AK_ADDC, // rB, 0, sets CA
    AK_ADDE, // rB, CA, sets CA
    AK_ADDME, // -1, CA, sets CA
    AK_ADDZE, // 0, CA, sets CA
    AK_SUBF, // rB, 1
    AK_SUBFC, // rB, 1, sets CA
    AK_SUBFE, // rB, CA, sets CA
    AK_SUBFME, // -1, CA, sets CA
    AK_SUBFZE, // 0, CA, sets CA
    AK_NEG, // 0 - rA
};
enum {
    JX_BCLR = PPD_END,
    JX_BCCTR,
    JX_BC,
    JX_MCRXR, // a = crfD
    JX_MFSPR, // a = rT offset, c = the ppc_t field (a plain supervisor SPR)
    JX_MTSPR, // a = rS offset, c = the ppc_t field
    JX_LWZX,
    JX_LWZUX,
    JX_LHZX,
    JX_LHZUX,
    JX_LHAX,
    JX_LHAUX,
    JX_LBZX,
    JX_LBZUX,
    JX_STWX,
    JX_STWUX,
    JX_STHX,
    JX_STHUX,
    JX_STBX,
    JX_STBUX,
    JX_ADDX,
    JX_END
};
_Static_assert(JX_END < 65536, "translator id space overflow");

// The predecoded classifier, plus the translator's own shapes
static uint16_t jit_classify(uint32_t iw, uint32_t ipc, uint32_t page_lo, pd_entry_t *e) {
    uint16_t id = ppc_pd_classify_word(iw, ipc, page_lo, e);
    if (id > PPD_FIRST && id < PPD_END)
        return id;
    uint32_t op = iw >> 26, xo = (iw >> 1) & 0x3FFu;
    uint32_t rt = (iw >> 21) & 31u, ra = (iw >> 16) & 31u, rb = (iw >> 11) & 31u;
    if (op == 16) {
        *e = (pd_entry_t){.a = (uint8_t)rt, .b = (uint8_t)ra, .c = iw};
        return JX_BC;
    }
    if (op == 19 && (xo == 16 || xo == 528)) {
        *e = (pd_entry_t){.a = (uint8_t)rt, .b = (uint8_t)ra, .c = iw & 1u};
        return xo == 16 ? JX_BCLR : JX_BCCTR;
    }
    if (op >= 32 && op <= 45 && !(op & 1u) && ra == 0) { // lwz lbz stw stb lhz lha sth with rA = 0
        static const uint16_t dform[14] = {PPD_LWZ, 0,       PPD_LBZ, 0,       PPD_STW, 0,       PPD_STB,
                                           0,       PPD_LHZ, 0,       PPD_LHA, 0,       PPD_STH, 0};
        *e = (pd_entry_t){.a = PPD_GOFF(rt), .b = JX_RA0, .c = (uint32_t)(int32_t)(int16_t)iw};
        return dform[op - 32];
    }
    if (op == 31) { // XO-form add/subtract
        int kind = -1;
        switch (xo & 0x1FFu) {
        case 266:
            kind = AK_ADD;
            break;
        case 10:
            kind = AK_ADDC;
            break;
        case 138:
            kind = AK_ADDE;
            break;
        case 234:
            kind = AK_ADDME;
            break;
        case 202:
            kind = AK_ADDZE;
            break;
        case 40:
            kind = AK_SUBF;
            break;
        case 8:
            kind = AK_SUBFC;
            break;
        case 136:
            kind = AK_SUBFE;
            break;
        case 232:
            kind = AK_SUBFME;
            break;
        case 200:
            kind = AK_SUBFZE;
            break;
        case 104:
            kind = AK_NEG;
            break;
        }
        if (kind >= 0) {
            *e = (pd_entry_t){.a = PPD_GOFF(rt),
                              .b = PPD_GOFF(ra),
                              .c = PPD_GOFF(rb) | (uint32_t)kind << 8 | ((iw >> 10) & 1u) << 16 | (iw & 1u) << 17};
            return JX_ADDX;
        }
    }
    if (op == 31 && xo == 512 && !(iw & 0x007FF800u & ~0x03800000u)) { // mcrxr
        *e = (pd_entry_t){.a = (uint8_t)((iw >> 23) & 7u)};
        return JX_MCRXR;
    }
    if (op == 31 && (xo == 339 || xo == 467)) { // mfspr / mtspr of a plain supervisor SPR
        uint32_t n = ra | rb << 5, off;
        switch (n) {
        case 18:
            off = offsetof(ppc_t, dsisr);
            break;
        case 19:
            off = offsetof(ppc_t, dar);
            break;
        case 26:
            off = offsetof(ppc_t, srr0);
            break;
        case 27:
            off = offsetof(ppc_t, srr1);
            break;
        case 272:
        case 273:
        case 274:
        case 275:
            off = offsetof(ppc_t, sprg) + 4 * (n - 272);
            break;
        default:
            return id;
        }
        *e = (pd_entry_t){.a = PPD_GOFF(rt), .c = off};
        return xo == 339 ? JX_MFSPR : JX_MTSPR;
    }
    if (op != 31 || (iw & 1u))
        return id;
    uint16_t x;
    bool upd = false;
    switch (xo) {
    case 23:
        x = JX_LWZX;
        break;
    case 55:
        x = JX_LWZUX, upd = true;
        break;
    case 279:
        x = JX_LHZX;
        break;
    case 311:
        x = JX_LHZUX, upd = true;
        break;
    case 343:
        x = JX_LHAX;
        break;
    case 375:
        x = JX_LHAUX, upd = true;
        break;
    case 87:
        x = JX_LBZX;
        break;
    case 119:
        x = JX_LBZUX, upd = true;
        break;
    case 151:
        x = JX_STWX;
        break;
    case 183:
        x = JX_STWUX, upd = true;
        break;
    case 407:
        x = JX_STHX;
        break;
    case 439:
        x = JX_STHUX, upd = true;
        break;
    case 215:
        x = JX_STBX;
        break;
    case 247:
        x = JX_STBUX, upd = true;
        break;
    default:
        return id;
    }
    if (upd && ra == 0)
        return id; // the 601's r0-inhibited update: left to the predecoded loop
    *e = (pd_entry_t){.a = PPD_GOFF(rt), .b = ra ? PPD_GOFF(ra) : JX_RA0, .c = PPD_GOFF(rb)};
    return x;
}

// === 1. WebAssembly byte writer =============================================

static uint8_t *g_buf;
static uint32_t g_len, g_cap;

static void put(uint8_t b) {
    if (g_len == g_cap) {
        g_cap = g_cap ? g_cap * 2 : (256u << 10);
        uint8_t *nb = realloc(g_buf, g_cap);
        if (!nb) {
            fprintf(stderr, "ppcjit: out of memory\n");
            abort();
        }
        g_buf = nb;
    }
    g_buf[g_len++] = b;
}
static void uleb(uint32_t v) {
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        put(v ? (b | 0x80) : b);
    } while (v);
}
static void sleb(int32_t v) {
    for (;;) {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) {
            put(b);
            return;
        }
        put(b | 0x80);
    }
}

// clang-format off
enum {
    W_BLOCK = 0x02, W_LOOP = 0x03, W_IF = 0x04, W_ELSE = 0x05, W_END = 0x0B, W_BR = 0x0C, W_BR_IF = 0x0D,
    W_BR_TABLE = 0x0E, W_CALL = 0x10, W_RETURN_CALL_INDIRECT = 0x13, W_SELECT = 0x1B, W_LGET = 0x20, W_LSET = 0x21, W_LTEE = 0x22,
    W_LOAD = 0x28, W_LOAD8U = 0x2D, W_LOAD16S = 0x2E, W_LOAD16U = 0x2F, W_STORE = 0x36, W_STORE8 = 0x3A,
    W_STORE16 = 0x3B, W_CONST = 0x41, W_EQZ = 0x45, W_EQ = 0x46, W_NE = 0x47, W_LT_S = 0x48, W_LT_U = 0x49,
    W_GT_S = 0x4A, W_GT_U = 0x4B, W_LE_U = 0x4D, W_GE_U = 0x4F, W_CLZ = 0x67, W_ADD = 0x6A, W_SUB = 0x6B,
    W_MUL = 0x6C, W_AND = 0x71, W_OR = 0x72, W_XOR = 0x73, W_SHL = 0x74, W_SHR_S = 0x75, W_SHR_U = 0x76,
    W_ROTL = 0x77, W_ROTR = 0x78, W_I64_ADD = 0x7C, W_I64_SHR_U = 0x88, W_WRAP = 0xA7, W_EXTEND_U = 0xAD,
    W_EXT8_S = 0xC0, W_EXT16_S = 0xC1,
};
// clang-format on
#define BT_VOID 0x40
#define BT_I32  0x7F

static void op(uint8_t o) {
    put(o);
}
static void iconst(uint32_t v) {
    put(W_CONST);
    sleb((int32_t)v);
}
static void lget(uint32_t l) {
    put(W_LGET);
    uleb(l);
}
static void lset(uint32_t l) {
    put(W_LSET);
    uleb(l);
}
static void ltee(uint32_t l) {
    put(W_LTEE);
    uleb(l);
}
static void mem(uint8_t o, uint32_t align, uint32_t off) {
    put(o);
    uleb(align);
    uleb(off);
}
// A linear-memory address as a wasm i32 (the build is wasm32)
static uint32_t addr_of(const void *p) {
    return (uint32_t)(uintptr_t)p;
}

// Sections are written with a fixed 5-byte size, patched when closed
static uint32_t open_section(uint8_t id) {
    put(id);
    uint32_t at = g_len;
    for (int i = 0; i < 5; i++)
        put(0);
    return at;
}
static void close_sized(uint32_t at) {
    uint32_t size = g_len - at - 5;
    for (int i = 0; i < 5; i++) {
        uint8_t b = (size >> (7 * i)) & 0x7F;
        g_buf[at + i] = (i < 4) ? (b | 0x80) : b;
    }
}
static void put_name(const char *s) {
    uleb((uint32_t)strlen(s));
    while (*s)
        put((uint8_t)*s++);
}

// === 2. The translator ======================================================

#define JIT_REGION_SHIFT 20
#define JIT_REGION_PAGES (1u << (JIT_REGION_SHIFT - MEM_PAGE_SHIFT))
#define JIT_PAGE_INSNS   (MEM_PAGE_SIZE / 4)
#define NONE16           0xFFFFu

// Locals: the four parameters, then the guest state and temporaries
enum {
    L_P = 0,
    L_BUDGET,
    L_ENTRY,
    L_FOLDS,
    L_GPR, // 32 of them
    L_CR = L_GPR + 32,
    L_XER,
    L_LR,
    L_CTR,
    L_IDX,
    L_NPC,
    L_RTAB,
    L_WTAB,
    L_AMASK,
    L_TMASK,
    L_T0,
    L_T1,
    L_T2,
    L_T3,
    L_EA,
    L_COUNT
};
#define NPARAMS 4
#define GPR(r)  (L_GPR + (r))

// Where the translated code finds what it reads and writes
uint32_t g_ppc_jit_exit; // PPC_JIT_EXIT_*, written by a deopt exit
int32_t g_ppc_jit_folds; // the fold allowance left, written by every exit

// A member page as the translator sees it
typedef struct jt_page {
    uint32_t lpage; // logical page base
    const uint32_t *words; // the page's instruction words (a snapshot)
    const uint8_t *leader; // 1: the word starts a block; 2: a block stops there (another chunk's)
    uint16_t *map; // out: per word, the block that starts there (NONE16)
    uint16_t *cov; // out: per word, the block that translated it (NONE16)
    uint32_t host_adjust; // the fetch mapping the page was translated under
    const uint16_t *ids; // the words' classification (jit_classify)
    const pd_entry_t *ents;
} jt_page_t;

// Per region page, read by the translated code: the page's map and its
// expected fetch mapping (the indirect-branch lookup).  Zero map: not a member.
typedef struct jt_ref {
    uint32_t map;
    uint32_t host_adjust;
} jt_ref_t;

static struct {
    jt_page_t *pages;
    uint32_t n;
    int16_t slot[JIT_REGION_PAGES]; // region page -> index in pages, or -1
    uint32_t lbase; // the region's logical base
    bool it, pr; // the fetch mode it runs under (MSR[IT], MSR[PR])
    const jt_ref_t *refs; // the region's refs (indirect branches)
    uint8_t *bvalid; // the region's per-block valid flags
    bool is601; // mtxer keeps only the 601's implemented XER bits
    bool shared; // the module's memory is shared (pthreads build)
    // The region's tables, for chaining into its other chunks (§3.6): the
    // address of its slot pointers and of its chunk array, the chunk size,
    // and the field offsets the generated lookup reads
    uint32_t rslot, rchunk, chunk_size, chunk_fn, chunk_maps, slot_owner, slot_hadj;
} U;

// Translation state of the block being emitted
static uint32_t g_nblocks, g_cur_block, g_depth;
static uint32_t g_cur_page; // index in U.pages
static uint32_t g_cur_addr; // the instruction being emitted
static uint32_t g_left_slots; // budget slots of the block from this instruction on
static uint32_t g_block_addr; // the block's first instruction
static bool g_block_branch; // the block ends in a branch (it took a fold)

// Depth of $loop, $chain and $out from the current point (blocks
// b+1..n-1 are open)
static uint32_t depth_loop(void) {
    return g_nblocks - 1 - g_cur_block + g_depth;
}
static uint32_t depth_chain(void) {
    return depth_loop() + 1;
}
static uint32_t depth_out(void) {
    return depth_loop() + 2;
}
static uint32_t g_deopt_depth; // g_depth inside the block's $deopt

// The GPRs a block names live in their locals from the block's start (it
// loads them after its guard) to every way out of it, where they are
// stored back: short live ranges for the engine's register allocator, and
// an entry or exit costs only the registers of the block it passes through.
static uint32_t g_block_mask, g_block_defs; // the registers it loads; the ones it may write
// jit.regs = 1: the GPRs live in their locals for the whole function instead
// (loaded by the prologue, stored at $out and before a chain)
static uint32_t g_fn_regs;
static uint32_t g_fn_mask, g_fn_defs;
static void emit_store_regs(uint32_t defs) {
    for (uint32_t r = 0; r < 32; r++) {
        if (!(defs & (1u << r)))
            continue;
        lget(L_P);
        lget(GPR(r));
        mem(W_STORE, 2, offsetof(ppc_t, gpr) + 4u * r);
    }
}
static void emit_flush(void) {
    for (uint32_t r = 0; r < 32; r++) {
        if (!(g_block_defs & (1u << r)))
            continue;
        lget(L_P);
        lget(GPR(r));
        mem(W_STORE, 2, offsetof(ppc_t, gpr) + 4u * r);
    }
}

// Leave to `npc` (the block's registers stored)
static void exit_const(uint32_t npc) {
    emit_flush();
    iconst(npc);
    lset(L_NPC);
    op(W_BR);
    uleb(depth_out());
}

// The unit-wide block that starts at `target`, or NONE16
static uint16_t block_at(uint32_t target, int *slot_out) {
    if (target & 3)
        return NONE16;
    uint32_t off = target - U.lbase;
    if (off >= (JIT_REGION_PAGES << MEM_PAGE_SHIFT))
        return NONE16;
    int k = U.slot[off >> MEM_PAGE_SHIFT];
    if (k < 0)
        return NONE16;
    *slot_out = k;
    return U.pages[k].map ? U.pages[k].map[(off & MEM_PAGE_MASK) >> 2] : NONE16;
}

// The fetch-TLB check of a crossing into the page of `target` (in
// instruction-translation mode): leaves to `target` unless the TLB maps
// its page, for this privilege, to the host page that was translated.
static void emit_fetch_check(uint32_t target, uint32_t host_adjust) {
    if (!U.it)
        return; // real mode: identity, bound to the memory map's generation
    uint32_t page = target & ~(uint32_t)MEM_PAGE_MASK;
    const ppc_ftlb_entry_t *fe = &g_ppc_ftlb[(target >> MEM_PAGE_SHIFT) & (PPC_FTLB_SIZE - 1)];
    iconst(addr_of(fe));
    mem(W_LOAD, 2, offsetof(ppc_ftlb_entry_t, tag));
    iconst(page | (U.pr ? 2u : 0u) | 1u);
    op(W_NE);
    iconst(addr_of(fe));
    mem(W_LOAD, 2, offsetof(ppc_ftlb_entry_t, host_adjust));
    iconst(host_adjust);
    op(W_NE);
    op(W_OR);
    op(W_IF);
    put(BT_VOID);
    g_depth++;
    exit_const(target);
    op(W_END);
    g_depth--;
}

// The function's chain stub, after $chain (the registers stored, the
// guest address in L_NPC): go there through another chunk of the region --
// look its block up in the region's tables (the slot, the word's owner, the
// chunk's function and map; in instruction-translation mode the fetch TLB
// must map the page to the slot's host page), store the registers and
// tail-call that chunk's function; otherwise leave with L_NPC.  The
// tables outlive the code: a remapped member or a reset drops the region's
// functions first.
static void emit_chain_stub(void) {
    op(W_BLOCK); // $leave
    put(BT_VOID);
    lget(L_NPC);
    iconst(U.lbase);
    op(W_SUB);
    ltee(L_T0); // offset in the region
    iconst(JIT_REGION_PAGES << MEM_PAGE_SHIFT);
    op(W_GE_U);
    op(W_BR_IF);
    uleb(0);
    lget(L_T0);
    iconst(MEM_PAGE_SHIFT);
    op(W_SHR_U);
    iconst(2);
    op(W_SHL);
    iconst(U.rslot);
    op(W_ADD);
    mem(W_LOAD, 2, 0);
    ltee(L_T1); // the slot
    op(W_EQZ);
    op(W_BR_IF);
    uleb(0);
    lget(L_T1);
    lget(L_T0);
    iconst(MEM_PAGE_MASK & ~3u);
    op(W_AND);
    iconst(2);
    op(W_SHR_U);
    op(W_ADD);
    mem(W_LOAD8U, 0, U.slot_owner);
    ltee(L_T2); // the owner chunk
    iconst(0xFFu); // NONE8
    op(W_EQ);
    op(W_BR_IF);
    uleb(0);
    lget(L_T2);
    iconst(U.chunk_size);
    op(W_MUL);
    iconst(U.rchunk);
    op(W_ADD);
    ltee(L_T2); // the chunk
    mem(W_LOAD, 2, U.chunk_fn);
    ltee(L_T3); // its function
    op(W_EQZ);
    op(W_BR_IF);
    uleb(0);
    lget(L_T2);
    lget(L_T0);
    iconst(MEM_PAGE_SHIFT);
    op(W_SHR_U);
    iconst(2);
    op(W_SHL);
    op(W_ADD);
    mem(W_LOAD, 2, U.chunk_maps);
    ltee(L_EA); // its map of the page
    op(W_EQZ);
    op(W_BR_IF);
    uleb(0);
    lget(L_EA);
    lget(L_T0);
    iconst(MEM_PAGE_MASK & ~3u);
    op(W_AND);
    iconst(1);
    op(W_SHR_U);
    op(W_ADD);
    mem(W_LOAD16U, 1, 0);
    ltee(L_EA); // the block
    iconst(NONE16);
    op(W_EQ);
    op(W_BR_IF);
    uleb(0);
    if (U.it) {
        lget(L_NPC);
        iconst(MEM_PAGE_SHIFT);
        op(W_SHR_U);
        iconst(PPC_FTLB_SIZE - 1);
        op(W_AND);
        iconst(sizeof(ppc_ftlb_entry_t));
        op(W_MUL);
        iconst(addr_of(g_ppc_ftlb));
        op(W_ADD);
        ltee(L_T2);
        mem(W_LOAD, 2, offsetof(ppc_ftlb_entry_t, tag));
        lget(L_NPC);
        iconst(~(uint32_t)MEM_PAGE_MASK);
        op(W_AND);
        iconst((U.pr ? 2u : 0u) | 1u);
        op(W_OR);
        op(W_NE);
        lget(L_T2);
        mem(W_LOAD, 2, offsetof(ppc_ftlb_entry_t, host_adjust));
        lget(L_T1);
        mem(W_LOAD, 2, U.slot_hadj);
        op(W_NE);
        op(W_OR);
        op(W_BR_IF);
        uleb(0);
    }
    emit_store_regs(g_fn_defs); // (function-wide registers only: 0 otherwise)
    static const struct {
        uint32_t local, off;
    } spr[4] = {
        {L_CR,  offsetof(ppc_t, cr) },
        {L_XER, offsetof(ppc_t, xer)},
        {L_LR,  offsetof(ppc_t, lr) },
        {L_CTR, offsetof(ppc_t, ctr)}
    };
    for (int i = 0; i < 4; i++) {
        lget(L_P);
        lget(spr[i].local);
        mem(W_STORE, 2, spr[i].off);
    }
    lget(L_P);
    lget(L_BUDGET);
    lget(L_EA);
    lget(L_FOLDS);
    lget(L_T3);
    op(W_RETURN_CALL_INDIRECT);
    uleb(0); // the function type
    uleb(0); // the emulator's table
    op(W_END); // $leave: fall out of $out with L_NPC
}

// Go to the guest address in L_NPC: perhaps another chunk's (the chain stub)
static void emit_chain(void) {
    emit_flush();
    op(W_BR);
    uleb(depth_chain());
}

// Go to `target`: a block of the region (re-checking the fetch mapping
// when it is on another page), or leave
static void emit_goto(uint32_t target) {
    int k = -1;
    uint16_t b = block_at(target, &k);
    if (b == NONE16) {
        if (!(target & 3) && target - U.lbase < (JIT_REGION_PAGES << MEM_PAGE_SHIFT)) {
            iconst(target); // perhaps another chunk's
            lset(L_NPC);
            emit_chain();
        } else
            exit_const(target);
        return;
    }
    if ((uint32_t)k != g_cur_page)
        emit_fetch_check(target, U.pages[k].host_adjust);
    emit_flush();
    iconst(b);
    lset(L_IDX);
    op(W_BR);
    uleb(depth_loop());
}

// Go to the guest address in L_NPC (blr, bctr): look it up in the region's
// refs and maps, re-checking the fetch mapping; otherwise leave
static void emit_goto_npc(void) {
    op(W_BLOCK); // $miss
    put(BT_VOID);
    g_depth++;
    lget(L_NPC);
    iconst(U.lbase);
    op(W_SUB);
    ltee(L_T0); // offset in the region
    iconst(JIT_REGION_PAGES << MEM_PAGE_SHIFT);
    op(W_GE_U);
    op(W_BR_IF);
    uleb(0);
    lget(L_T0);
    iconst(MEM_PAGE_SHIFT);
    op(W_SHR_U);
    iconst(3);
    op(W_SHL);
    iconst(addr_of(U.refs));
    op(W_ADD);
    ltee(L_T1); // &refs[page]
    mem(W_LOAD, 2, offsetof(jt_ref_t, map));
    ltee(L_T2); // the page's map, or 0
    op(W_EQZ);
    op(W_BR_IF);
    uleb(0);
    if (U.it) {
        // the fetch TLB entry for the target page
        lget(L_NPC);
        iconst(MEM_PAGE_SHIFT);
        op(W_SHR_U);
        iconst(PPC_FTLB_SIZE - 1);
        op(W_AND);
        iconst(sizeof(ppc_ftlb_entry_t));
        op(W_MUL);
        iconst(addr_of(g_ppc_ftlb));
        op(W_ADD);
        ltee(L_T3);
        mem(W_LOAD, 2, offsetof(ppc_ftlb_entry_t, tag));
        lget(L_NPC);
        iconst(~(uint32_t)MEM_PAGE_MASK);
        op(W_AND);
        iconst((U.pr ? 2u : 0u) | 1u);
        op(W_OR);
        op(W_NE);
        lget(L_T3);
        mem(W_LOAD, 2, offsetof(ppc_ftlb_entry_t, host_adjust));
        lget(L_T1);
        mem(W_LOAD, 2, offsetof(jt_ref_t, host_adjust));
        op(W_NE);
        op(W_OR);
        op(W_BR_IF);
        uleb(0);
    }
    lget(L_T2);
    lget(L_T0);
    iconst(MEM_PAGE_MASK & ~3u);
    op(W_AND);
    iconst(1);
    op(W_SHR_U); // (offset in page / 4) * 2
    op(W_ADD);
    mem(W_LOAD16U, 1, 0);
    ltee(L_T2);
    iconst(NONE16);
    op(W_EQ);
    op(W_BR_IF);
    uleb(0);
    emit_flush();
    lget(L_T2);
    lset(L_IDX);
    op(W_BR);
    uleb(depth_loop());
    op(W_END); // $miss: another chunk's, or leave with L_NPC
    g_depth--;
    emit_chain();
}

// Leave at the current instruction for the predecoded loop: give back the
// block's slots from here on and its fold, say why.  The locals hold the
// state before this instruction.
static void emit_deopt(void) {
    // the block's deopt tail does it, from the instruction's index
    iconst((g_cur_addr - g_block_addr) >> 2);
    lset(L_IDX);
    op(W_BR);
    uleb(g_depth - g_deopt_depth);
}

// The block's deopt tail, after its $deopt: L_IDX is the index of the
// instruction it leaves at
static void emit_deopt_tail(uint32_t slots) {
    lget(L_BUDGET);
    iconst(slots);
    lget(L_IDX);
    op(W_SUB);
    op(W_ADD);
    lset(L_BUDGET);
    if (g_block_branch) {
        lget(L_FOLDS);
        iconst(1);
        op(W_ADD);
        lset(L_FOLDS);
    }
    iconst(addr_of(&g_ppc_jit_exit));
    iconst(PPC_JIT_EXIT_DEOPT);
    mem(W_STORE, 2, 0);
    lget(L_IDX);
    iconst(2);
    op(W_SHL);
    iconst(g_block_addr);
    op(W_ADD);
    lset(L_NPC);
    emit_flush();
    op(W_BR);
    uleb(depth_out());
}

// Big-endian <-> little-endian of the i32 on the stack (wasm has no bswap)
static void emit_bswap32(void) {
    ltee(L_T2);
    iconst(8);
    op(W_ROTL);
    iconst(0x00FF00FFu);
    op(W_AND);
    lget(L_T2);
    iconst(8);
    op(W_ROTR);
    iconst(0xFF00FF00u);
    op(W_AND);
    op(W_OR);
}
static void emit_bswap16(void) { // low 16 bits, upper zero
    ltee(L_T2);
    iconst(8);
    op(W_SHL);
    lget(L_T2);
    iconst(8);
    op(W_SHR_U);
    op(W_OR);
    iconst(0xFFFF);
    op(W_AND);
}

// The fast-path test of the access at L_EA: on the stack, nonzero when it
// must leave (crossing a page, a 601 T=1 segment, or no SoA entry); leaves the
// masked address in L_T0 and the entry in L_T1.
static void emit_access_test(uint32_t size, bool store) {
    lget(L_EA);
    lget(L_AMASK);
    op(W_AND);
    lset(L_T0);
    lget(store ? L_WTAB : L_RTAB);
    lget(L_T0);
    iconst(MEM_PAGE_SHIFT);
    op(W_SHR_U);
    iconst(2);
    op(W_SHL);
    op(W_ADD);
    mem(W_LOAD, 2, 0);
    ltee(L_T1);
    op(W_EQZ);
    lget(L_TMASK); // T=1 segments (601, MSR[DT] clear); 0 otherwise
    lget(L_EA);
    iconst(28);
    op(W_SHR_U);
    op(W_SHR_U);
    iconst(1);
    op(W_AND);
    op(W_OR);
    if (size > 1) { // misaligned is an ordinary access unless it crosses a page (big-endian;
                    // ppc_check_align_scalar, the 604's scalar gate)
        lget(L_EA);
        iconst(MEM_PAGE_MASK);
        op(W_AND);
        iconst(MEM_PAGE_SIZE - size);
        op(W_GT_U);
        op(W_OR);
    }
}

// Load: EA on the stack -> value on the stack (zero- or sign-extended)
static void emit_load(uint32_t size, bool sign) {
    lset(L_EA);
    emit_access_test(size, false);
    op(W_IF);
    put(BT_I32);
    g_depth++;
    emit_deopt(); // stack-polymorphic after the branch: the i32 is satisfied
    op(W_ELSE);
    lget(L_T1);
    lget(L_T0);
    op(W_ADD);
    if (size == 4) {
        mem(W_LOAD, 0, 0);
        emit_bswap32();
    } else if (size == 2) {
        mem(W_LOAD16U, 0, 0);
        emit_bswap16();
        if (sign)
            op(W_EXT16_S);
    } else {
        mem(W_LOAD8U, 0, 0);
    }
    op(W_END);
    g_depth--;
}

// Leave after the current instruction, which completed: give back the
// block's slots after it and its fold
static void emit_exit_after(void) {
    lget(L_BUDGET);
    iconst(g_left_slots - 1);
    op(W_ADD);
    lset(L_BUDGET);
    if (g_block_branch) {
        lget(L_FOLDS);
        iconst(1);
        op(W_ADD);
        lset(L_FOLDS);
    }
    exit_const(g_cur_addr + 4);
}

// Store: EA on the stack, value in L_T3.  `helper`: a store that misses the
// fast path may still complete through ppc_jit_code_store (a code page,
// user mode, the 604), and the function goes on -- or leaves right after it
// when the store invalidated translated code (an update form's rA, upd_reg,
// updated first; -1: none).
static void emit_store(uint32_t size, bool helper, int upd_reg) {
    lset(L_EA);
    emit_access_test(size, true);
    op(W_IF);
    put(BT_VOID);
    g_depth++;
    if (helper && U.pr && !U.is601) {
        lget(L_T0);
        lget(L_T3);
        iconst(size);
        op(W_CALL);
        uleb(0);
        ltee(L_T2);
        op(W_IF); // 1: not completed, leave at the store; 2: completed, leave after it
        put(BT_VOID);
        g_depth++;
        lget(L_T2);
        iconst(1);
        op(W_EQ);
        op(W_IF);
        put(BT_VOID);
        g_depth++;
        emit_deopt();
        op(W_END);
        g_depth--;
        if (upd_reg >= 0) { // the update form completes before it leaves
            lget(L_EA);
            lset(GPR(upd_reg));
        }
        emit_exit_after();
        op(W_END);
        g_depth--;
    } else {
        emit_deopt();
    }
    op(W_ELSE);
    lget(L_T1);
    lget(L_T0);
    op(W_ADD);
    lget(L_T3);
    if (size == 4) {
        emit_bswap32();
        mem(W_STORE, 0, 0);
    } else if (size == 2) {
        iconst(0xFFFF);
        op(W_AND);
        emit_bswap16();
        mem(W_STORE16, 0, 0);
    } else {
        mem(W_STORE8, 0, 0);
    }
    op(W_END);
    g_depth--;
}

// CR field `crf` = (L_T0 < L_T1 ? 8 : L_T0 > L_T1 ? 4 : 2) | XER[SO]
static void emit_cmp(uint32_t crf, bool is_unsigned) {
    uint32_t sh = 28 - 4 * crf;
    lget(L_CR);
    iconst(~(0xFu << sh));
    op(W_AND);
    iconst(8);
    iconst(4);
    iconst(2);
    lget(L_T0);
    lget(L_T1);
    op(is_unsigned ? W_GT_U : W_GT_S);
    op(W_SELECT);
    lget(L_T0);
    lget(L_T1);
    op(is_unsigned ? W_LT_U : W_LT_S);
    op(W_SELECT);
    lget(L_XER);
    iconst(31);
    op(W_SHR_U);
    op(W_OR);
    iconst(sh);
    op(W_SHL);
    op(W_OR);
    lset(L_CR);
}

// CR0 from the result in local `l` (the record forms)
static void emit_record(uint32_t l) {
    lget(l);
    lset(L_T0);
    iconst(0);
    lset(L_T1);
    emit_cmp(0, false);
}

// Set or clear XER[CA] from the 0/1 on the stack
static void emit_set_ca(void) {
    iconst(29);
    op(W_SHL);
    lget(L_XER);
    iconst(~PPC_XER_CA);
    op(W_AND);
    op(W_OR);
    lset(L_XER);
}

// The carry out of the 64-bit sum of the two i32 on the stack (as u32)
// plus `cin`, as 0/1; the 32-bit sum goes to local `sum`.
static void emit_add_carry(uint32_t a_local, uint32_t b_local, uint32_t cin, uint32_t sum) {
    lget(a_local);
    lget(b_local);
    op(W_ADD);
    if (cin) {
        iconst(cin);
        op(W_ADD);
    }
    lset(sum);
    lget(a_local);
    op(W_EXTEND_U);
    lget(b_local);
    op(W_EXTEND_U);
    op(W_I64_ADD);
    if (cin) {
        put(0x42); // i64.const
        sleb((int32_t)cin);
        op(W_I64_ADD);
    }
    put(0x42);
    sleb(32);
    op(W_I64_SHR_U);
    op(W_WRAP);
}

// CR bit n (big-endian numbering) of L_CR on the stack
static void emit_cr_bit(uint32_t n) {
    lget(L_CR);
    iconst(31 - n);
    op(W_SHR_U);
    iconst(1);
    op(W_AND);
}

#define RD(off) GPR((off) >> 2) // an entry's register byte offset -> its local

// Is this entry one the translator handles?  (The T0 set, minus isync,
// whose context synchronization stays in C, and minus branches to
// themselves, which take a slot instead of folding.)
static bool translatable(uint16_t id, uint32_t iw, uint32_t ipc) {
    if (id == JX_MFSPR || id == JX_MTSPR)
        return !U.pr; // (in user mode they raise the privilege exception)
    if (id == PPD_ISYNC)
        return true; // (a no-op unless BATs were written since: then it leaves)
    if (id == JX_BC) { // not to itself (a slot, not a fold)
        int32_t bd = (int32_t)(int16_t)(iw & 0xFFFCu);
        return ((iw & 2u) ? (uint32_t)bd : ipc + (uint32_t)bd) != ipc;
    }
    if (id >= JX_BCLR && id < JX_END)
        return true;
    if (id <= PPD_FIRST || id >= PPD_END)
        return false;
    switch (id) {
    case PPD_B_IN:
    case PPD_B_OUT:
    case PPD_BL_IN:
    case PPD_BL_OUT: {
        int32_t li = ((int32_t)(iw << 6) >> 6) & ~3;
        uint32_t t = (iw & 2u) ? (uint32_t)li : ipc + (uint32_t)li;
        return t != ipc;
    }
    case PPD_BDNZ_IN:
    case PPD_BDNZ_OUT:
    case PPD_BC_T_IN:
    case PPD_BC_T_OUT:
    case PPD_BC_F_IN:
    case PPD_BC_F_OUT: {
        int32_t bd = (int32_t)(int16_t)(iw & 0xFFFCu);
        uint32_t t = (iw & 2u) ? (uint32_t)bd : ipc + (uint32_t)bd;
        return t != ipc;
    }
    default:
        return true;
    }
}

static bool is_branch_id(uint16_t id) {
    return (id >= PPD_B_IN && id <= PPD_BCTR) || id == JX_BCLR || id == JX_BCCTR || id == JX_BC;
}

// A direct branch's target, from the raw word
static uint32_t branch_target(uint16_t id, uint32_t iw, uint32_t ipc) {
    if (id <= PPD_BL_OUT) {
        int32_t li = ((int32_t)(iw << 6) >> 6) & ~3;
        return (iw & 2u) ? (uint32_t)li : ipc + (uint32_t)li;
    }
    int32_t bd = (int32_t)(int16_t)(iw & 0xFFFCu);
    return (iw & 2u) ? (uint32_t)bd : ipc + (uint32_t)bd;
}

// GPRs an entry names (a superset of the ones it reads or writes)
static uint32_t entry_regs(uint16_t id, const pd_entry_t *e) {
    if (id == JX_MCRXR)
        return 0;
    if (id == JX_MFSPR || id == JX_MTSPR)
        return 1u << (e->a >> 2);
    if (id == JX_ADDX)
        return (1u << (e->a >> 2)) | (1u << (e->b >> 2)) | (1u << ((e->c & 0xFFu) >> 2));
    if (id >= JX_LWZX && id < JX_END)
        return (1u << (e->a >> 2)) | (1u << (e->c >> 2)) | (e->b == JX_RA0 ? 0u : 1u << (e->b >> 2));
    if (id >= PPD_LWZ && id <= PPD_STHU && e->b == JX_RA0)
        return 1u << (e->a >> 2);
    if (is_branch_id(id) || id == PPD_NOP || id == PPD_MCRF || (id >= PPD_CRAND && id <= PPD_CRNOR))
        return 0;
    if (id >= PPD_RLWINM && id <= PPD_RLWNM_RC) {
        uint32_t iw = e->c;
        return (1u << PPC_RT(iw)) | (1u << PPC_RA(iw)) | (1u << PPC_RB(iw));
    }
    uint32_t m = (1u << (e->a >> 2)) | (1u << (e->b >> 2));
    if (id <= PPD_CNTLZW_RC || id == PPD_CMPW || id == PPD_CMPLW)
        m |= 1u << ((e->c >> 2) & 31);
    return m;
}

// GPRs an entry may write (a subset of entry_regs)
static uint32_t entry_defs(uint16_t id, const pd_entry_t *e) {
    switch (id) {
    case JX_MCRXR:
    case JX_MTSPR:
        return 0;
    case JX_MFSPR:
        return 1u << (e->a >> 2);
    case JX_ADDX:
        return 1u << (e->a >> 2);
    case JX_LWZUX:
    case JX_LHZUX:
    case JX_LHAUX:
    case JX_LBZUX:
        return (1u << (e->a >> 2)) | (1u << (e->b >> 2));
    case JX_LWZX:
    case JX_LHZX:
    case JX_LHAX:
    case JX_LBZX:
        return 1u << (e->a >> 2);
    case JX_STWUX:
    case JX_STHUX:
    case JX_STBUX:
        return 1u << (e->b >> 2);
    case JX_STWX:
    case JX_STHX:
    case JX_STBX:
    case JX_BCLR:
    case JX_BCCTR:
        return 0;
    case PPD_LWZU:
    case PPD_LBZU:
    case PPD_LHZU:
    case PPD_LHAU:
        return (1u << (e->a >> 2)) | (1u << (e->b >> 2));
    case PPD_LWZ:
    case PPD_LBZ:
    case PPD_LHZ:
    case PPD_LHA:
        return 1u << (e->a >> 2);
    case PPD_STWU:
    case PPD_STBU:
    case PPD_STHU:
        return 1u << (e->b >> 2);
    case PPD_STW:
    case PPD_STB:
    case PPD_STH:
    case PPD_CMPWI:
    case PPD_CMPLWI:
    case PPD_CMPW:
    case PPD_CMPLW:
    case PPD_MTLR:
    case PPD_MTCTR:
    case PPD_MTXER:
    case PPD_MTCRF:
    case PPD_NOP:
    case PPD_ISYNC:
        return 0;
    case PPD_RLWINM:
    case PPD_RLWINM_RC:
    case PPD_RLWIMI:
    case PPD_RLWIMI_RC:
    case PPD_RLWNM:
    case PPD_RLWNM_RC:
        return 1u << PPC_RA(e->c);
    default:
        break;
    }
    if (is_branch_id(id) || id == PPD_MCRF || (id >= PPD_CRAND && id <= PPD_CRNOR))
        return 0;
    if ((id >= PPD_ADD && id <= PPD_SUBFIC) || (id >= PPD_MFLR && id <= PPD_MFCR))
        return 1u << (e->a >> 2); // the destination
    return entry_regs(id, e);
}

// Emit one translatable instruction
static void emit_insn(uint16_t id, const pd_entry_t *e, uint32_t iw, uint32_t ipc) {
    uint32_t a = e->a, b = e->b, c = e->c;
    switch (id) {
        // XO-form arithmetic and X-form logical (a = dest, b, c = sources)
#define BIN(ID, OPC, SWAP)                                                                                             \
    case ID:                                                                                                           \
    case ID##_RC:                                                                                                      \
        lget(RD(SWAP ? c : b));                                                                                        \
        lget(RD(SWAP ? b : c));                                                                                        \
        op(OPC);                                                                                                       \
        lset(RD(a));                                                                                                   \
        if (id == ID##_RC)                                                                                             \
            emit_record(RD(a));                                                                                        \
        return;
        BIN(PPD_ADD, W_ADD, 0)
        BIN(PPD_SUBF, W_SUB, 1)
        BIN(PPD_MULLW, W_MUL, 0)
        BIN(PPD_AND, W_AND, 0)
        BIN(PPD_OR, W_OR, 0)
        BIN(PPD_XOR, W_XOR, 0)
#undef BIN
    case PPD_ANDC:
    case PPD_ANDC_RC:
        lget(RD(b));
        lget(RD(c));
        iconst(~0u);
        op(W_XOR);
        op(W_AND);
        lset(RD(a));
        if (id == PPD_ANDC_RC)
            emit_record(RD(a));
        return;
    case PPD_NOR:
    case PPD_NOR_RC:
        lget(RD(b));
        lget(RD(c));
        op(W_OR);
        iconst(~0u);
        op(W_XOR);
        lset(RD(a));
        if (id == PPD_NOR_RC)
            emit_record(RD(a));
        return;
    case PPD_NEG:
    case PPD_NEG_RC:
        iconst(0);
        lget(RD(b));
        op(W_SUB);
        lset(RD(a));
        if (id == PPD_NEG_RC)
            emit_record(RD(a));
        return;
    case PPD_SLW:
    case PPD_SLW_RC:
    case PPD_SRW:
    case PPD_SRW_RC:
        iconst(0);
        lget(RD(b));
        lget(RD(c));
        op((id == PPD_SLW || id == PPD_SLW_RC) ? W_SHL : W_SHR_U); // wasm masks the count to 5 bits
        lget(RD(c));
        iconst(0x20);
        op(W_AND);
        op(W_SELECT); // count & 0x20 ? 0 : shifted
        lset(RD(a));
        if (id == PPD_SLW_RC || id == PPD_SRW_RC)
            emit_record(RD(a));
        return;
    case PPD_EXTSB:
    case PPD_EXTSB_RC:
    case PPD_EXTSH:
    case PPD_EXTSH_RC:
        lget(RD(b));
        op((id == PPD_EXTSB || id == PPD_EXTSB_RC) ? W_EXT8_S : W_EXT16_S);
        lset(RD(a));
        if (id == PPD_EXTSB_RC || id == PPD_EXTSH_RC)
            emit_record(RD(a));
        return;
    case PPD_CNTLZW:
    case PPD_CNTLZW_RC:
        lget(RD(b));
        op(W_CLZ); // clz(0) = 32, as the handler's
        lset(RD(a));
        if (id == PPD_CNTLZW_RC)
            emit_record(RD(a));
        return;
    case PPD_SRAWI:
    case PPD_SRAWI_RC:
        // CA = negative and any 1 bit shifted out
        if (c) {
            lget(RD(b));
            iconst(0);
            op(W_LT_S);
            lget(RD(b));
            iconst((1u << c) - 1u);
            op(W_AND);
            iconst(0);
            op(W_NE);
            op(W_AND);
        } else {
            iconst(0);
        }
        emit_set_ca();
        lget(RD(b));
        iconst(c);
        op(W_SHR_S);
        lset(RD(a));
        if (id == PPD_SRAWI_RC)
            emit_record(RD(a));
        return;
    // D-form immediates (a = dest, b = source, c = immediate)
    case PPD_ADDI:
        lget(RD(b));
        iconst(c);
        op(W_ADD);
        lset(RD(a));
        return;
    case PPD_LI:
        iconst(c);
        lset(RD(a));
        return;
    case PPD_MULLI:
        lget(RD(b));
        iconst(c);
        op(W_MUL);
        lset(RD(a));
        return;
    case PPD_ORI:
    case PPD_XORI:
    case PPD_ANDI_RC:
        lget(RD(b));
        iconst(c);
        op(id == PPD_ORI ? W_OR : id == PPD_XORI ? W_XOR : W_AND);
        lset(RD(a));
        if (id == PPD_ANDI_RC)
            emit_record(RD(a));
        return;
    case PPD_ADDIC:
    case PPD_ADDIC_RC:
        iconst(c);
        lset(L_T3);
        emit_add_carry(RD(b), L_T3, 0, L_EA); // L_EA: the sum (a scratch here)
        emit_set_ca();
        lget(L_EA);
        lset(RD(a));
        if (id == PPD_ADDIC_RC)
            emit_record(RD(a));
        return;
    case PPD_SUBFIC: // rT = ~rA + imm + 1, CA its carry
        lget(RD(b));
        iconst(~0u);
        op(W_XOR);
        lset(L_T3);
        iconst(c);
        lset(L_EA);
        emit_add_carry(L_T3, L_EA, 1, L_T1);
        emit_set_ca();
        lget(L_T1);
        lset(RD(a));
        return;
    // Rotates (c = the raw word)
    case PPD_RLWINM:
    case PPD_RLWINM_RC:
    case PPD_RLWNM:
    case PPD_RLWNM_RC:
    case PPD_RLWIMI:
    case PPD_RLWIMI_RC: {
        uint32_t w = c, rs = PPC_RT(w), ra = PPC_RA(w), m = ppc_mask(PPC_MB(w), PPC_ME(w));
        bool nm = (id == PPD_RLWNM || id == PPD_RLWNM_RC);
        bool mi = (id == PPD_RLWIMI || id == PPD_RLWIMI_RC);
        lget(GPR(rs));
        if (nm)
            lget(GPR(PPC_RB(w))); // wasm rotl masks the count to 5 bits
        else
            iconst(PPC_RB(w));
        op(W_ROTL);
        iconst(m);
        op(W_AND);
        if (mi) {
            lget(GPR(ra));
            iconst(~m);
            op(W_AND);
            op(W_OR);
        }
        lset(GPR(ra));
        if (id == PPD_RLWINM_RC || id == PPD_RLWNM_RC || id == PPD_RLWIMI_RC)
            emit_record(GPR(ra));
        return;
    }
    // Compares (a = crfD, b = rA, c = immediate or rB)
    case PPD_CMPWI:
    case PPD_CMPLWI:
        lget(RD(b));
        lset(L_T0);
        iconst(c);
        lset(L_T1);
        emit_cmp(a, id == PPD_CMPLWI);
        return;
    case PPD_CMPW:
    case PPD_CMPLW:
        lget(RD(b));
        lset(L_T0);
        lget(RD(c));
        lset(L_T1);
        emit_cmp(a, id == PPD_CMPLW);
        return;
    // SPR and CR moves (a = rT or rS)
    case PPD_MFLR:
        lget(L_LR);
        lset(RD(a));
        return;
    case PPD_MTLR:
        lget(RD(a));
        lset(L_LR);
        return;
    case PPD_MFCTR:
        lget(L_CTR);
        lset(RD(a));
        return;
    case PPD_MTCTR:
        lget(RD(a));
        lset(L_CTR);
        return;
    case PPD_MFXER:
        lget(L_XER);
        lset(RD(a));
        return;
    case PPD_MTXER:
        lget(RD(a));
        if (U.is601) {
            iconst(PPC_XER_SO | PPC_XER_OV | PPC_XER_CA | PPC_XER_CMPBYTE | PPC_XER_BYTES);
            op(W_AND);
        }
        lset(L_XER);
        return;
    case PPD_MFCR:
        lget(L_CR);
        lset(RD(a));
        return;
    case PPD_MTCRF:
        lget(RD(a));
        iconst(c);
        op(W_AND);
        lget(L_CR);
        iconst(~c);
        op(W_AND);
        op(W_OR);
        lset(L_CR);
        return;
    case PPD_CRAND:
    case PPD_CRANDC:
    case PPD_CROR:
    case PPD_CRORC:
    case PPD_CRXOR:
    case PPD_CREQV:
    case PPD_CRNAND:
    case PPD_CRNOR: {
        // a = bT, b = bA, c = bB (big-endian bit numbers)
        lget(L_CR);
        iconst(~(1u << (31 - a)));
        op(W_AND);
        emit_cr_bit(b);
        emit_cr_bit(c);
        if (id == PPD_CRANDC || id == PPD_CRORC) {
            iconst(1);
            op(W_XOR);
        }
        op((id == PPD_CRAND || id == PPD_CRANDC || id == PPD_CRNAND) ? W_AND
           : (id == PPD_CROR || id == PPD_CRORC || id == PPD_CRNOR)  ? W_OR
                                                                     : W_XOR);
        if (id == PPD_CREQV || id == PPD_CRNAND || id == PPD_CRNOR) {
            iconst(1);
            op(W_XOR);
        }
        iconst(31 - a);
        op(W_SHL);
        op(W_OR);
        lset(L_CR);
        return;
    }
    case PPD_MCRF: { // a = crfD, b = crfS
        uint32_t sd = 28 - 4 * a, ss = 28 - 4 * b;
        lget(L_CR);
        iconst(~(0xFu << sd));
        op(W_AND);
        lget(L_CR);
        iconst(ss);
        op(W_SHR_U);
        iconst(0xF);
        op(W_AND);
        iconst(sd);
        op(W_SHL);
        op(W_OR);
        lset(L_CR);
        return;
    }
    case PPD_NOP:
        return;
    // Loads and stores (a = rT/rS, b = rA, c = displacement)
    case PPD_LWZ:
    case PPD_LWZU:
    case PPD_LHZ:
    case PPD_LHZU:
    case PPD_LHA:
    case PPD_LHAU:
    case PPD_LBZ:
    case PPD_LBZU: {
        bool upd = (id == PPD_LWZU || id == PPD_LHZU || id == PPD_LHAU || id == PPD_LBZU);
        uint32_t size = (id == PPD_LWZ || id == PPD_LWZU) ? 4 : (id == PPD_LBZ || id == PPD_LBZU) ? 1 : 2;
        if (b == JX_RA0)
            iconst(c);
        else {
            lget(RD(b));
            iconst(c);
            op(W_ADD);
        }
        emit_load(size, id == PPD_LHA || id == PPD_LHAU);
        lset(L_T3);
        if (upd) { // as the handler: the update first, then the loaded value
            lget(L_EA);
            lset(RD(b));
        }
        lget(L_T3);
        lset(RD(a));
        return;
    }
    case PPD_STW:
    case PPD_STWU:
    case PPD_STH:
    case PPD_STHU:
    case PPD_STB:
    case PPD_STBU: {
        bool upd = (id == PPD_STWU || id == PPD_STHU || id == PPD_STBU);
        uint32_t size = (id == PPD_STW || id == PPD_STWU) ? 4 : (id == PPD_STB || id == PPD_STBU) ? 1 : 2;
        lget(RD(a));
        lset(L_T3);
        if (b == JX_RA0)
            iconst(c);
        else {
            lget(RD(b));
            iconst(c);
            op(W_ADD);
        }
        emit_store(size, true, upd ? (int)(b >> 2) : -1);
        if (upd) { // after the store: rS == rA stores the old value
            lget(L_EA);
            lset(RD(b));
        }
        return;
    }
    // Branches: the targets come from the raw word (the region decides
    // what is "in" it, not the page)
    case PPD_B_IN:
    case PPD_B_OUT:
    case PPD_BL_IN:
    case PPD_BL_OUT:
        if (id == PPD_BL_IN || id == PPD_BL_OUT) {
            iconst(ipc + 4);
            lset(L_LR);
        }
        emit_goto(branch_target(id, iw, ipc));
        return;
    case PPD_BDNZ_IN:
    case PPD_BDNZ_OUT:
        lget(L_CTR);
        iconst(1);
        op(W_SUB);
        ltee(L_CTR);
        op(W_IF);
        put(BT_VOID);
        g_depth++;
        emit_goto(branch_target(id, iw, ipc));
        op(W_END);
        g_depth--;
        return; // not taken: falls through (still folded)
    case PPD_BC_T_IN:
    case PPD_BC_T_OUT:
    case PPD_BC_F_IN:
    case PPD_BC_F_OUT:
        lget(L_CR);
        iconst(a); // 31 - BI
        op(W_SHR_U);
        iconst(1);
        op(W_AND);
        if (id == PPD_BC_F_IN || id == PPD_BC_F_OUT)
            op(W_EQZ);
        op(W_IF);
        put(BT_VOID);
        g_depth++;
        emit_goto(branch_target(id, iw, ipc));
        op(W_END);
        g_depth--;
        return;
    case PPD_BLR:
    case PPD_BCTR:
        lget(id == PPD_BLR ? L_LR : L_CTR);
        iconst(~3u);
        op(W_AND);
        ltee(L_NPC);
        // A branch to itself takes a slot instead of folding: the block
        // counted a fold; trade it for the slot it reserved
        iconst(ipc);
        op(W_EQ);
        op(W_IF);
        put(BT_VOID);
        g_depth++;
        lget(L_BUDGET);
        iconst(1);
        op(W_SUB);
        lset(L_BUDGET);
        lget(L_FOLDS);
        iconst(1);
        op(W_ADD);
        lset(L_FOLDS);
        op(W_END);
        g_depth--;
        emit_goto_npc();
        return;
    case JX_MCRXR: { // CR field a = XER[0:3]; those XER bits clear (OP_MCRXR)
        uint32_t sh = 28 - 4 * a;
        lget(L_CR);
        iconst(~(0xFu << sh));
        op(W_AND);
        lget(L_XER);
        iconst(28);
        op(W_SHR_U);
        iconst(sh);
        op(W_SHL);
        op(W_OR);
        lset(L_CR);
        lget(L_XER);
        iconst(0x0FFFFFFFu);
        op(W_AND);
        lset(L_XER);
        return;
    }
    case JX_MFSPR:
        lget(L_P);
        mem(W_LOAD, 2, c);
        lset(RD(a));
        return;
    case JX_MTSPR:
        lget(L_P);
        lget(RD(a));
        mem(W_STORE, 2, c);
        return;
    case PPD_ISYNC: {
        // ppc_context_sync publishes BAT writes to the fetch side: with none
        // pending (the copies equal the BATs) it changes nothing; otherwise
        // the predecoded loop runs it
        for (uint32_t i = 0; i < 4; i++) {
            lget(L_P);
            mem(W_LOAD, 2, offsetof(ppc_t, ibatu_cs) + 4 * i);
            lget(L_P);
            mem(W_LOAD, 2, offsetof(ppc_t, batu) + 4 * i);
            op(W_XOR);
            lget(L_P);
            mem(W_LOAD, 2, offsetof(ppc_t, ibatl_cs) + 4 * i);
            lget(L_P);
            mem(W_LOAD, 2, offsetof(ppc_t, batl) + 4 * i);
            op(W_XOR);
            op(W_OR);
            if (i)
                op(W_OR);
        }
        op(W_IF);
        put(BT_VOID);
        g_depth++;
        emit_deopt();
        op(W_END);
        g_depth--;
        return;
    }
    case JX_BC: { // a = BO, b = BI, c = the raw word (ppc_do_bc)
        bool test = false;
        if (!(a & 4u)) { // decrement CTR, test it
            lget(L_CTR);
            iconst(1);
            op(W_SUB);
            ltee(L_CTR);
            if (a & 2u)
                op(W_EQZ);
            else {
                iconst(0);
                op(W_NE);
            }
            test = true;
        }
        if (!(a & 16u)) { // the CR bit
            emit_cr_bit(b);
            if (!(a & 8u))
                op(W_EQZ);
            if (test)
                op(W_AND);
            test = true;
        }
        if (c & 1u) {
            iconst(ipc + 4);
            lset(L_LR);
        }
        if (test) {
            op(W_IF);
            put(BT_VOID);
            g_depth++;
        }
        emit_goto(branch_target(id, c, ipc));
        if (test) {
            op(W_END);
            g_depth--;
        }
        return; // not taken: falls through (still folded)
    }
    case JX_BCLR:
    case JX_BCCTR: { // a = BO, b = BI, c = LK (ppc_do_bclr / ppc_do_bcctr)
        // the target before anything changes (bcctr's invalid decrementing
        // form branches to the old CTR, as the handler)
        lget(id == JX_BCLR ? L_LR : L_CTR);
        iconst(~3u);
        op(W_AND);
        lset(L_NPC);
        bool test = false;
        if (!(a & 4u)) { // decrement CTR, test it
            lget(L_CTR);
            iconst(1);
            op(W_SUB);
            ltee(L_CTR);
            if (a & 2u)
                op(W_EQZ);
            else {
                iconst(0);
                op(W_NE);
            }
            test = true;
        }
        if (!(a & 16u)) { // the CR bit
            emit_cr_bit(b);
            if (!(a & 8u))
                op(W_EQZ);
            if (test)
                op(W_AND);
            test = true;
        }
        if (c) {
            iconst(ipc + 4);
            lset(L_LR);
        }
        if (test) {
            op(W_IF);
            put(BT_VOID);
            g_depth++;
        }
        // taken: a branch to itself takes a slot instead of folding
        lget(L_NPC);
        iconst(ipc);
        op(W_EQ);
        op(W_IF);
        put(BT_VOID);
        g_depth++;
        lget(L_BUDGET);
        iconst(1);
        op(W_SUB);
        lset(L_BUDGET);
        lget(L_FOLDS);
        iconst(1);
        op(W_ADD);
        lset(L_FOLDS);
        op(W_END);
        g_depth--;
        emit_goto_npc();
        if (test) {
            op(W_END);
            g_depth--;
        }
        return; // not taken: falls through (still folded)
    }
    case JX_ADDX: { // a = rT, b = rA, c = rB | kind << 8 | OE << 16 | Rc << 17 (ppc_add_body)
        uint32_t kind = (c >> 8) & 0xFFu, rb = c & 0xFFu;
        bool oe = (c >> 16) & 1u, rc = (c >> 17) & 1u;
        if (kind == AK_NEG) {
            lget(RD(b));
            lset(L_T3);
            iconst(0);
            lget(L_T3);
            op(W_SUB);
            lset(L_T1);
            if (oe) {
                lget(L_T3);
                iconst(0x80000000u);
                op(W_EQ);
                lset(L_T2); // the overflow, 0/1
            }
        } else {
            bool subf = kind >= AK_SUBF;
            uint32_t k = subf ? kind - AK_SUBF : kind; // the add form
            lget(RD(b));
            if (subf) {
                iconst(~0u);
                op(W_XOR);
            }
            lset(L_T3); // the first operand
            if (k == AK_ADD || k == AK_ADDC || k == AK_ADDE)
                lget(RD(rb));
            else
                iconst(k == AK_ADDME ? ~0u : 0u);
            lset(L_EA); // the second (a scratch here)
            if (k == AK_ADDE || k == AK_ADDME || k == AK_ADDZE) {
                lget(L_XER);
                iconst(29);
                op(W_SHR_U);
                iconst(1);
                op(W_AND);
            } else
                iconst(subf ? 1u : 0u);
            lset(L_T2); // the carry-in
            lget(L_T3);
            lget(L_EA);
            op(W_ADD);
            lget(L_T2);
            op(W_ADD);
            lset(L_T1); // the sum
            if (k != AK_ADD) { // CA: the carry out of the 64-bit sum
                lget(L_T3);
                op(W_EXTEND_U);
                lget(L_EA);
                op(W_EXTEND_U);
                op(W_I64_ADD);
                lget(L_T2);
                op(W_EXTEND_U);
                op(W_I64_ADD);
                put(0x42);
                sleb(32);
                op(W_I64_SHR_U);
                op(W_WRAP);
                emit_set_ca();
            }
            if (oe) { // operands of one sign, a result of the other
                lget(L_T3);
                lget(L_EA);
                op(W_XOR);
                iconst(~0u);
                op(W_XOR);
                lget(L_T3);
                lget(L_T1);
                op(W_XOR);
                op(W_AND);
                iconst(31);
                op(W_SHR_U);
                lset(L_T2);
            }
        }
        if (oe) { // OV = the overflow; SO |= it
            lget(L_XER);
            iconst(~PPC_XER_OV);
            op(W_AND);
            lget(L_T2);
            iconst(30);
            op(W_SHL);
            op(W_OR);
            lget(L_T2);
            iconst(31);
            op(W_SHL);
            op(W_OR);
            lset(L_XER);
        }
        lget(L_T1);
        lset(RD(a));
        if (rc)
            emit_record(RD(a));
        return;
    }
    case JX_LWZX:
    case JX_LWZUX:
    case JX_LHZX:
    case JX_LHZUX:
    case JX_LHAX:
    case JX_LHAUX:
    case JX_LBZX:
    case JX_LBZUX: {
        bool upd = (id == JX_LWZUX || id == JX_LHZUX || id == JX_LHAUX || id == JX_LBZUX);
        uint32_t size = (id == JX_LWZX || id == JX_LWZUX) ? 4 : (id == JX_LBZX || id == JX_LBZUX) ? 1 : 2;
        if (b == JX_RA0)
            iconst(0);
        else
            lget(RD(b));
        lget(RD(c));
        op(W_ADD);
        emit_load(size, id == JX_LHAX || id == JX_LHAUX);
        lset(L_T3);
        if (upd) { // the update first, then the loaded value (OP_LWZUX)
            lget(L_EA);
            lset(RD(b));
        }
        lget(L_T3);
        lset(RD(a));
        return;
    }
    case JX_STWX:
    case JX_STWUX:
    case JX_STHX:
    case JX_STHUX:
    case JX_STBX:
    case JX_STBUX: {
        bool upd = (id == JX_STWUX || id == JX_STHUX || id == JX_STBUX);
        uint32_t size = (id == JX_STWX || id == JX_STWUX) ? 4 : (id == JX_STBX || id == JX_STBUX) ? 1 : 2;
        lget(RD(a));
        lset(L_T3);
        if (b == JX_RA0)
            iconst(0);
        else
            lget(RD(b));
        lget(RD(c));
        op(W_ADD);
        emit_store(size, true, upd ? (int)(b >> 2) : -1);
        if (upd) { // after the store: rS == rA stores the old value
            lget(L_EA);
            lset(RD(b));
        }
        return;
    }
    default:
        break;
    }
    // translatable() admits nothing else
    fprintf(stderr, "ppcjit: no emitter for id %u at %08x\n", id, ipc);
    abort();
}

// The block table of the unit being translated
typedef struct jt_block {
    uint16_t page; // index in U.pages
    uint16_t start; // first instruction
    uint16_t n; // instructions
    uint8_t branch; // ends in a branch
} jt_block_t;
static jt_block_t *g_blocks;
static uint32_t g_blocks_cap;
static pd_entry_t *g_ents; // classified entries, per unit word
static uint16_t *g_ids;
static uint32_t g_ents_cap;

// Translate the unit in U into a module in g_buf.  Returns the number of
// blocks (0: nothing translatable; no module).
static uint32_t translate(void) {
    // Classify every leader-reachable word, find the blocks
    uint32_t words = U.n * JIT_PAGE_INSNS;
    if (words > g_ents_cap) {
        g_ents_cap = words;
        g_ents = realloc(g_ents, words * sizeof *g_ents);
        g_ids = realloc(g_ids, words * sizeof *g_ids);
    }
    if (words + 1 > g_blocks_cap) {
        g_blocks_cap = words + 1;
        g_blocks = realloc(g_blocks, g_blocks_cap * sizeof *g_blocks);
    }
    uint32_t nb = 0;
    for (uint32_t k = 0; k < U.n; k++) {
        jt_page_t *pg = &U.pages[k];
        if (!pg->map)
            continue; // not a page of this chunk
        bool own = false; // it leads blocks here
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) {
            pg->map[i] = NONE16;
            pg->cov[i] = NONE16;
            own |= pg->leader[i] == 1;
        }
        if (!own)
            continue;
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) {
            uint32_t x = k * JIT_PAGE_INSNS + i;
            g_ids[x] = pg->ids[i];
            g_ents[x] = pg->ents[i];
        }
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) {
            if (pg->leader[i] != 1)
                continue; // (2: a leader of another chunk: a block stops there, none starts)
            uint32_t x0 = k * JIT_PAGE_INSNS + i;
            if (!translatable(g_ids[x0], pg->words[i], pg->lpage + 4 * i))
                continue; // the predecoded loop runs it: no block
            if (nb >= NONE16)
                break;
            uint32_t n = 0;
            bool br = false;
            for (uint32_t j = i; j < JIT_PAGE_INSNS; j++) {
                uint32_t x = k * JIT_PAGE_INSNS + j;
                if (j > i && pg->leader[j])
                    break;
                if (!translatable(g_ids[x], pg->words[j], pg->lpage + 4 * j))
                    break;
                n++;
                if (is_branch_id(g_ids[x])) {
                    br = true;
                    break;
                }
            }
            pg->map[i] = (uint16_t)nb;
            for (uint32_t j = i; j < i + n; j++)
                pg->cov[j] = (uint16_t)nb;
            g_blocks[nb++] = (jt_block_t){(uint16_t)k, (uint16_t)i, (uint16_t)n, br};
        }
    }
    g_nblocks = nb;
    if (!nb)
        return 0;
    g_fn_mask = g_fn_defs = 0;
    if (g_fn_regs)
        for (uint32_t bi = 0; bi < nb; bi++)
            for (uint32_t i = 0; i < g_blocks[bi].n; i++) {
                uint32_t x = g_blocks[bi].page * JIT_PAGE_INSNS + g_blocks[bi].start + i;
                g_fn_mask |= entry_regs(g_ids[x], &g_ents[x]);
                g_fn_defs |= entry_defs(g_ids[x], &g_ents[x]);
            }

    g_len = 0;
    static const uint8_t hdr[8] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    for (int i = 0; i < 8; i++)
        put(hdr[i]);
    // Types: the function, (i32 i32 i32 i32) -> i32; the code-store
    // helper, (i32 i32 i32) -> i32
    uint32_t s = open_section(1);
    uleb(2);
    put(0x60);
    uleb(NPARAMS);
    for (int i = 0; i < NPARAMS; i++)
        put(BT_I32);
    uleb(1);
    put(BT_I32);
    put(0x60);
    uleb(3);
    for (int i = 0; i < 3; i++)
        put(BT_I32);
    uleb(1);
    put(BT_I32);
    close_sized(s);
    // Imports: the code-store helper (function 0); the memory (shared in
    // the pthreads build); the emulator's function table (chaining)
    s = open_section(2);
    uleb(3);
    put_name("e");
    put_name("s");
    put(0x00);
    uleb(1);
    put_name("e");
    put_name("m");
    put(0x02);
    if (U.shared) {
        put(0x03);
        uleb(1);
        uleb(65536);
    } else {
        put(0x00);
        uleb(1);
    }
    put_name("e");
    put_name("t");
    put(0x01); // a table
    put(0x70); // of funcref
    put(0x00);
    uleb(0);
    close_sized(s);
    s = open_section(3);
    uleb(1);
    uleb(0);
    close_sized(s);
    s = open_section(7);
    uleb(1);
    put_name("f");
    put(0x00);
    uleb(1);
    close_sized(s);
    s = open_section(10);
    uleb(1);
    uint32_t body = g_len;
    for (int i = 0; i < 5; i++)
        put(0);
    uleb(1);
    uleb(L_COUNT - NPARAMS);
    put(BT_I32);

    // Prologue: the registers the region names, CR/XER/LR/CTR, the memory
    // system's tables, the fold allowance
    for (uint32_t r = 0; r < 32; r++) {
        if (!(g_fn_mask & (1u << r)))
            continue;
        lget(L_P);
        mem(W_LOAD, 2, offsetof(ppc_t, gpr) + 4u * r);
        lset(GPR(r));
    }
    lget(L_P);
    mem(W_LOAD, 2, offsetof(ppc_t, cr));
    lset(L_CR);
    lget(L_P);
    mem(W_LOAD, 2, offsetof(ppc_t, xer));
    lset(L_XER);
    lget(L_P);
    mem(W_LOAD, 2, offsetof(ppc_t, lr));
    lset(L_LR);
    lget(L_P);
    mem(W_LOAD, 2, offsetof(ppc_t, ctr));
    lset(L_CTR);
    iconst(addr_of(&g_active_read));
    mem(W_LOAD, 2, 0);
    lset(L_RTAB);
    iconst(addr_of(&g_active_write));
    mem(W_LOAD, 2, 0);
    lset(L_WTAB);
    iconst(addr_of(&g_address_mask));
    mem(W_LOAD, 2, 0);
    lset(L_AMASK);
    // 601 T=1 segments matter with MSR[DT] clear only (ppc_dxlate)
    lget(L_P);
    mem(W_LOAD, 2, offsetof(ppc_t, msr));
    iconst(PPC_MSR_DT);
    op(W_AND);
    iconst(0);
    lget(L_P);
    mem(W_LOAD, 2, offsetof(ppc_t, sr_t_mask));
    op(W_SELECT); // DT ? 0 : sr_t_mask
    lset(L_TMASK);
    lget(L_ENTRY);
    lset(L_IDX);

    op(W_BLOCK);
    put(BT_VOID); // $out
    op(W_BLOCK);
    put(BT_VOID); // $chain
    op(W_LOOP);
    put(BT_VOID); // $loop
    for (uint32_t b = 0; b < nb; b++) {
        op(W_BLOCK);
        put(BT_VOID);
    }
    lget(L_IDX);
    op(W_BR_TABLE);
    uleb(nb);
    for (uint32_t b = 0; b < nb; b++)
        uleb(b);
    uleb(nb + 2); // default: $out (never taken)
    for (uint32_t b = 0; b < nb; b++) {
        op(W_END); // closes $b: block b's code follows
        g_cur_block = b;
        g_depth = 0;
        const jt_block_t *bl = &g_blocks[b];
        jt_page_t *pg = &U.pages[bl->page];
        g_cur_page = bl->page;
        uint32_t addr0 = pg->lpage + 4u * bl->start;
        uint32_t slots = bl->n - (bl->branch ? 1u : 0u);
        // Guard: still valid, the budget leaves a slot to spare, a fold
        // left for the branch; otherwise leave at the block's start
        g_block_mask = g_block_defs = 0; // (nothing loaded yet: nothing to store)
        iconst(addr_of(&U.bvalid[b]));
        mem(W_LOAD8U, 0, 0);
        op(W_EQZ);
        lget(L_BUDGET);
        iconst(bl->n);
        op(W_LE_U);
        op(W_OR);
        if (bl->branch) {
            lget(L_FOLDS);
            op(W_EQZ);
            op(W_OR);
        }
        op(W_IF);
        put(BT_VOID);
        g_depth++;
        exit_const(addr0);
        op(W_END);
        g_depth--;
        lget(L_BUDGET);
        iconst(slots);
        op(W_SUB);
        lset(L_BUDGET);
        if (bl->branch) {
            lget(L_FOLDS);
            iconst(1);
            op(W_SUB);
            lset(L_FOLDS);
        }
        g_block_branch = bl->branch;
        // The block's registers into their locals
        bool access = false; // a load or store: it may deopt
        for (uint32_t i = 0; i < bl->n; i++) {
            uint32_t x = bl->page * JIT_PAGE_INSNS + bl->start + i;
            if (!g_fn_regs) {
                g_block_mask |= entry_regs(g_ids[x], &g_ents[x]);
                g_block_defs |= entry_defs(g_ids[x], &g_ents[x]);
            }
            uint16_t id = g_ids[x];
            access |= (id >= PPD_LWZ && id <= PPD_STHU) || (id >= JX_LWZX && id <= JX_STBUX) || id == PPD_ISYNC;
        }
        for (uint32_t r = 0; r < 32; r++) {
            if (!(g_block_mask & (1u << r)))
                continue;
            lget(L_P);
            mem(W_LOAD, 2, offsetof(ppc_t, gpr) + 4u * r);
            lset(GPR(r));
        }
        // The body, inside $end (its way on) and $deopt (its accesses
        // that miss the fast path leave through the block's deopt tail)
        if (access) {
            op(W_BLOCK);
            put(BT_VOID); // $end
            op(W_BLOCK);
            put(BT_VOID); // $deopt
            g_depth = g_deopt_depth = 2;
        }
        g_block_addr = addr0;
        for (uint32_t i = 0; i < bl->n; i++) {
            uint32_t x = bl->page * JIT_PAGE_INSNS + bl->start + i;
            g_cur_addr = addr0 + 4 * i;
            g_left_slots = slots - i; // (only non-branches deopt: i < slots)
            emit_insn(g_ids[x], &g_ents[x], pg->words[bl->start + i], g_cur_addr);
        }
        // Fall into the next block when it starts right here; otherwise
        // go there (a block of the region, or leave)
        uint32_t next = addr0 + 4u * bl->n;
        bool here = b + 1 < nb && g_blocks[b + 1].page == bl->page && g_blocks[b + 1].start == bl->start + bl->n;
        if (!here)
            emit_goto(next);
        else if (access) {
            op(W_BR);
            uleb(1); // $end
        }
        if (access) {
            op(W_END); // $deopt
            g_depth = 1;
            emit_deopt_tail(slots);
            op(W_END); // $end
            g_depth = 0;
        }
        if (here)
            emit_flush(); // and fall into the next block
    }
    op(W_END); // $loop
    op(W_END); // $chain: perhaps another chunk's
    emit_chain_stub();
    op(W_END); // $out: L_NPC is the next pc
    emit_store_regs(g_fn_defs);
    lget(L_P);
    lget(L_CR);
    mem(W_STORE, 2, offsetof(ppc_t, cr));
    lget(L_P);
    lget(L_XER);
    mem(W_STORE, 2, offsetof(ppc_t, xer));
    lget(L_P);
    lget(L_LR);
    mem(W_STORE, 2, offsetof(ppc_t, lr));
    lget(L_P);
    lget(L_CTR);
    mem(W_STORE, 2, offsetof(ppc_t, ctr));
    lget(L_P);
    lget(L_NPC);
    mem(W_STORE, 2, offsetof(ppc_t, pc));
    iconst(addr_of(&g_ppc_jit_folds));
    lget(L_FOLDS);
    mem(W_STORE, 2, 0);
    lget(L_BUDGET);
    op(W_END);
    close_sized(body);
    close_sized(s);
    return nb;
}

// === 3. The tier manager ====================================================

// clang-format off
// Compile and instantiate synchronously against this module's memory;
// install the export in the function table (in a freed slot, or a new one)
// and return its index, which is the function pointer.
EM_JS(uint32_t, jit_instantiate, (const uint8_t *p, uint32_t n, uint32_t slot), {
    var table = wasmExports['__indirect_function_table'];
    var mod = new WebAssembly.Module(HEAPU8.slice(p, p + n));
    var inst = new WebAssembly.Instance(mod, { e: { m: wasmMemory, s: wasmExports['ppc_jit_code_store'], t: table } });
    var idx = slot;
    table.set(idx, inst.exports.f);
    return idx;
});
// The asynchronous path (jit.async): compile off the core thread, install
// from the promise's callback, which runs on the core thread between its
// ticks (the browser build's main loop returns to the event loop)
EM_JS(void, jit_compile_async, (const uint8_t *p, uint32_t n, uint32_t token), {
    var mods = globalThis.__gsJitMods || (globalThis.__gsJitMods = new Map());
    WebAssembly.compile(HEAPU8.slice(p, p + n)).then(function(mod) {
        mods.set(token, mod);
        wasmExports['ppc_jit_ready'](token);
    }, function(e) {
        wasmExports['ppc_jit_ready'](token);
    });
});
EM_JS(uint32_t, jit_install_async, (uint32_t token, uint32_t slot), {
    var mods = globalThis.__gsJitMods;
    var mod = mods && mods.get(token);
    if (!mod)
        return 0;
    mods.delete(token);
    var table = wasmExports['__indirect_function_table'];
    var inst = new WebAssembly.Instance(mod, { e: { m: wasmMemory, s: wasmExports['ppc_jit_code_store'], t: table } });
    table.set(slot, inst.exports.f);
    return slot;
});
EM_JS(void, jit_drop_async, (uint32_t token), {
    if (globalThis.__gsJitMods)
        globalThis.__gsJitMods.delete(token);
});
EM_JS(int, jit_host_is_node, (void), {
    return (typeof process !== 'undefined' && process.versions && process.versions.node) ? 1 : 0;
});
EM_JS(uint32_t, jit_table_grow, (uint32_t n), {
    return wasmExports['__indirect_function_table'].grow(n);
});
EM_JS(void, jit_release_slot, (uint32_t slot), {
    wasmExports['__indirect_function_table'].set(slot, null);
});
EM_JS(int, jit_memory_shared, (void), {
    return (typeof SharedArrayBuffer !== 'undefined' && wasmMemory.buffer instanceof SharedArrayBuffer) ? 1 : 0;
});
// clang-format on

typedef int32_t (*jit_fn_t)(ppc_t *p, int32_t budget, int32_t entry, int32_t folds);

// A region's reachable code is split into chunks of at most g_chunk_cap
// instructions, one function each, so the engine's optimizing compile of
// any one function stays short (it grows faster than linearly with the
// function's size: emu-core-bench measured 11 s for a 660 KB function).
// A chunk is a set of whole member pages, filled in address order; a page
// keeps its chunk from one translation to the next, so a translation
// regenerates only the chunks whose pages changed.  A branch into another
// chunk leaves, and the dispatcher enters that chunk.
#define JIT_MAX_CHUNKS       255 // (owner[] is a byte, NONE8 the last value)
#define JIT_ENTRY_HOT        8 // entries at a word before it leads a block
#define JIT_VOLATILE_LOOKUPS 2000000ull // a volatile page is tried again after this many lookups, doubling
#define JIT_SPLIT_COMPILES   16u // translations of its region before a chunk is retranslated for new leaders, doubling
#define JIT_UNSTABLE_WRITES  4 // a word stored into this often between translations is not translated
#define JIT_POOR_RUN         8 // a run that consumes fewer slots is poor ...
#define JIT_POOR_MAX         32 // ... and an entry with this many more poor than good is left alone
#define NONE8                0xFFu

// A member page of a region: what was translated and from where
typedef struct jit_slot {
    uint32_t lpage;
    uint8_t *host; // the host page (the coherence hook's key)
    uint32_t host_adjust; // the fetch mapping (host - lpage)
    uint32_t words[JIT_PAGE_INSNS];
    uint8_t extra[JIT_PAGE_INSNS]; // entries seen at run time at words that lead no block (saturating)
    uint8_t owner[JIT_PAGE_INSNS]; // per word: the chunk that translated it (NONE8)
    uint32_t hits; // stores that invalidated its blocks since the region's last translation
    uint32_t churn; // translations after which it had been invalidated again
    bool volatile_; // written too often: not translated for now (the predecoded tier runs it)
    uint32_t vol_count; // times it turned volatile
    uint64_t vol_until; // when it is tried again (g_st.lookups)
    uint8_t writes[JIT_PAGE_INSNS]; // stores into each word, halved at each translation of the region
    bool unstable[JIT_PAGE_INSNS]; // once written JIT_UNSTABLE_WRITES times between translations: not translated
    uint16_t ids[JIT_PAGE_INSNS]; // words[] classified (jit_classify)
    pd_entry_t ents[JIT_PAGE_INSNS];
    bool classified;
    uint8_t poor[JIT_PAGE_INSNS]; // per entry word: recent runs that ended early (JIT_POOR_MAX: not entered)
    uint16_t last_hit; // the last word whose write invalidated a block (jit.trace)
    uint16_t cov[JIT_PAGE_INSNS]; // per word: its block in that chunk (NONE16)
    struct jit_region *region;
    struct jit_slot *next_host; // the host-page chain
} jit_slot_t;

typedef struct jit_chunk {
    uint32_t fn; // the function (table index), 0 = none
    jt_ref_t refs[JIT_REGION_PAGES]; // read by the function: its maps of the member pages
    uint16_t *maps[JIT_REGION_PAGES]; // per member page: block per leader word (NONE16)
    uint8_t *bvalid; // per block, 1 = valid
    uint32_t bcap; // bvalid's size
    uint32_t nb; // blocks
    uint64_t sig; // what it was translated from (its pages' words, leaders, mappings)
    uint64_t sigw; // the same without the leaders
    uint32_t regens; // translations of it
    uint32_t nwords; // words it translated (jit.trace)
    bool empty; // translated to no block
    uint32_t pending; // the token of its translation being compiled asynchronously (0: none)
    uint32_t last_regen; // the region's translation count at the last one
} jit_chunk_t;

typedef struct jit_region {
    uint32_t key; // logical base | MSR[IT] << 1 | MSR[PR]
    jit_slot_t *slot[JIT_REGION_PAGES];
    jit_chunk_t chunk[JIT_MAX_CHUNKS];
    uint32_t nchunks;
    uint32_t ncompiles; // translations of the region
    uint32_t heat[JIT_REGION_PAGES]; // instructions run on pages that are not (validly) members
    uint32_t since; // entries since the last compile
    uint32_t cool; // entries required between compiles: doubles with every compile (backoff)
    uint32_t stale; // blocks invalidated since the last compile
    bool want; // entry points or members were added since the last compile
    struct jit_region *next;
} jit_region_t;

#define JIT_HASH 256
static jit_region_t *g_regions[JIT_HASH];
#define JIT_HOST_HASH 1024
static jit_slot_t *g_by_host[JIT_HOST_HASH];
static uint32_t g_mapgen; // the memory map the translations belong to
static uint32_t g_model = ~0u; // the CPU model they were made for
static bool g_inited;
// The region lookup's cache, and the page whose heat the next entry settles
#define JIT_RCACHE 64
static jit_region_t *g_rcache[JIT_RCACHE];
static jit_region_t *g_hot_R; // the declined page g_ppc_jit_cold describes
static uint32_t g_hot_k;
ppc_jit_cold_t g_ppc_jit_cold;
uint32_t g_ppc_jit_skip[PPC_JIT_SKIP];

// Compiles in flight (jit.async): the chunk each one is for
#define JIT_PENDING 512
static struct {
    uint32_t token, msr;
    jit_region_t *R;
    uint32_t ci;
} g_pending[JIT_PENDING];
static uint32_t g_token;
static uint32_t g_async; // jit.async: 1 = compile off the core thread
static bool g_async_set; // jit.async was set explicitly
static void skip_word(uint32_t pc, uint32_t msr) {
    g_ppc_jit_skip[(pc >> 2) & (PPC_JIT_SKIP - 1)] = PPC_JIT_SKIP_TAG(pc, msr);
}

// Counters (the `jit` node)
static struct {
    uint64_t compiles, functions, bytes, calls, deopts, block_invals, regions, insns, lookups, deferred;
    double compile_ms;
} g_st;

static uint32_t *g_free_slots;
static uint32_t g_nfree, g_free_cap;
static void release_fn(uint32_t fn) {
    if (!fn)
        return;
    jit_release_slot(fn);
    if (g_nfree == g_free_cap) {
        g_free_cap = g_free_cap ? 2 * g_free_cap : 64;
        g_free_slots = realloc(g_free_slots, g_free_cap * sizeof *g_free_slots);
    }
    g_free_slots[g_nfree++] = fn;
}

static uint32_t host_hash(const uint8_t *host) {
    return (uint32_t)(((uintptr_t)host >> MEM_PAGE_SHIFT) * 2654435761u) % JIT_HOST_HASH;
}

// Drop a region's functions (its members stay)
static void region_drop_code(jit_region_t *R) {
    for (uint32_t c = 0; c < R->nchunks; c++) {
        jit_chunk_t *ch = &R->chunk[c];
        release_fn(ch->fn);
        ch->fn = 0;
        ch->pending = 0;
        ch->nb = 0;
        ch->sig = 0;
        ch->empty = false;
        memset(ch->refs, 0, sizeof ch->refs);
    }
    R->nchunks = 0;
    for (uint32_t k = 0; k < JIT_REGION_PAGES; k++) {
        jit_slot_t *s = R->slot[k];
        if (!s)
            continue;
        memset(s->owner, NONE8, sizeof s->owner);
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++)
            s->cov[i] = NONE16;
    }
}

static void slot_unlink(jit_slot_t *s) {
    jit_slot_t **pp = &g_by_host[host_hash(s->host)];
    while (*pp && *pp != s)
        pp = &(*pp)->next_host;
    if (*pp)
        *pp = s->next_host;
    // The page stays a JIT code page while another slot translates it
    bool other = false;
    for (jit_slot_t *t = g_by_host[host_hash(s->host)]; t; t = t->next_host)
        other |= t->host == s->host;
    if (!other)
        memory_code_page_unmark_owner(s->host, MEM_CODE_JIT);
}

void ppc_jit_reset(void) {
    for (uint32_t h = 0; h < JIT_HASH; h++) {
        jit_region_t *r = g_regions[h];
        while (r) {
            jit_region_t *nx = r->next;
            region_drop_code(r);
            for (uint32_t c = 0; c < JIT_MAX_CHUNKS; c++) {
                for (uint32_t k = 0; k < JIT_REGION_PAGES; k++)
                    free(r->chunk[c].maps[k]);
                free(r->chunk[c].bvalid);
            }
            for (uint32_t k = 0; k < JIT_REGION_PAGES; k++)
                if (r->slot[k]) {
                    slot_unlink(r->slot[k]);
                    free(r->slot[k]);
                }
            free(r);
            r = nx;
        }
        g_regions[h] = NULL;
    }
    memset(g_rcache, 0, sizeof g_rcache);
    memset(g_pending, 0, sizeof g_pending); // (compiles in flight are dropped when they land)
    g_hot_R = NULL;
    g_ppc_jit_cold.key = 0;
    memset(g_ppc_jit_skip, 0, sizeof g_ppc_jit_skip);
    g_mapgen = g_mem_map_generation;
}

static jit_region_t *region_find(uint32_t key) {
    uint32_t h = (key >> JIT_REGION_SHIFT) * 31u % JIT_HASH;
    for (jit_region_t *r = g_regions[h]; r; r = r->next)
        if (r->key == key)
            return r;
    return NULL;
}
static jit_region_t *region_get(uint32_t key) {
    jit_region_t *r = region_find(key);
    if (r)
        return r;
    uint32_t h = (key >> JIT_REGION_SHIFT) * 31u % JIT_HASH;
    r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->key = key;
    r->next = g_regions[h];
    g_regions[h] = r;
    g_st.regions++;
    return r;
}

// Translate the region's member pages: the code reachable from the entry
// points seen so far, in chunks.  A branch to an unreached address leaves;
// its next entry makes it an entry point.
// A function slot of the table (grown 64 at a time), filled with the module
// just translated (token 0: g_buf, compiled now) or the asynchronous
// compile `token` has finished; then the lazy-compile mitigation (Phase 0):
// one call with a zero budget has the engine compile the function now, not
// at its first real entry (every block's guard leaves at once on a scratch
// state).  0 if the module is gone.
static uint32_t install_fn(uint32_t token, uint32_t msr) {
    if (!g_nfree) {
        uint32_t base = jit_table_grow(64);
        for (uint32_t t = 64; t-- > 0;)
            release_fn(base + t);
    }
    uint32_t slot = g_free_slots[--g_nfree];
    uint32_t fn = token ? jit_install_async(token, slot) : jit_instantiate(g_buf, g_len, slot);
    if (!fn) {
        g_free_slots[g_nfree++] = slot;
        return 0;
    }
    static ppc_t scratch;
    scratch.msr = msr;
    ((jit_fn_t)(uintptr_t)fn)(&scratch, 0, 0, 0);
    return fn;
}

static void pending_add(uint32_t token, jit_region_t *R, uint32_t ci, uint32_t msr) {
    uint32_t i = token % JIT_PENDING; // a slot reused before its compile lands drops that one
    g_pending[i].token = token;
    g_pending[i].R = R;
    g_pending[i].ci = ci;
    g_pending[i].msr = msr;
}

// An asynchronous compile landed (on the core thread, between ticks):
// install it if its chunk still waits for it
EMSCRIPTEN_KEEPALIVE void ppc_jit_ready(uint32_t token);
void ppc_jit_ready(uint32_t token) {
    uint32_t i = token % JIT_PENDING;
    if (g_pending[i].token != token) {
        jit_drop_async(token);
        return;
    }
    g_pending[i].token = 0;
    jit_chunk_t *ch = &g_pending[i].R->chunk[g_pending[i].ci];
    if (ch->pending != token) {
        jit_drop_async(token);
        return;
    }
    double t0 = emscripten_get_now();
    ch->pending = 0;
    ch->fn = install_fn(token, g_pending[i].msr);
    g_st.compile_ms += emscripten_get_now() - t0;
    memset(g_ppc_jit_skip, 0, sizeof g_ppc_jit_skip); // its entries are open now
}

static double g_t_start; // when translation began (the governor's clock)
static double g_prof[6]; // jit.trace = 7: where compile_region's time goes
static uint32_t g_regen, g_regen_new; // jit.trace = 6
static void compile_region(jit_region_t *R, ppc_t *p) {
    uint64_t bytes0 = g_st.bytes;
    memset(g_ppc_jit_skip, 0, sizeof g_ppc_jit_skip); // new blocks, new entries
    g_regen = g_regen_new = 0;
    static jt_page_t pages[JIT_REGION_PAGES];
    static uint32_t slot_page[JIT_REGION_PAGES]; // unit index -> region page
    static uint8_t leader[JIT_REGION_PAGES][JIT_PAGE_INSNS];
    static uint8_t reached[JIT_REGION_PAGES][JIT_PAGE_INSNS];
    static uint8_t cleader[JIT_REGION_PAGES][JIT_PAGE_INSNS]; // one chunk's leaders
    static uint16_t ccov[JIT_REGION_PAGES][JIT_PAGE_INSNS]; // one chunk's coverage
    static uint32_t stack[2 * JIT_REGION_PAGES * JIT_PAGE_INSNS];
    static const pd_entry_t *ran[JIT_REGION_PAGES]; // per unit page: the predecoded entries, or NULL
    double t0 = emscripten_get_now();
    uint32_t lbase = R->key & ~(uint32_t)((1u << JIT_REGION_SHIFT) - 1);
    U.pr = (R->key & 1u) != 0; // (translatable() asks, during reachability)
    // A page invalidated again after each of several translations is code
    // being written while it runs (the DR emulator's cache): leave it to
    // the predecoded tier rather than retranslate it for every write
    for (uint32_t k = 0; k < JIT_REGION_PAGES; k++) {
        jit_slot_t *s = R->slot[k];
        if (!s || s->volatile_)
            continue;
        if (s->hits && ++s->churn >= JIT_CHURN_MAX) {
            s->volatile_ = true;
            s->vol_until = g_st.lookups + (JIT_VOLATILE_LOOKUPS << (s->vol_count < 12 ? s->vol_count : 12));
            s->vol_count++;
            if (g_trace)
                printf("jit: volatile %08x (last write at word %u: %08x)\n", s->lpage, s->last_hit,
                       LOAD_BE32(s->host + 4 * s->last_hit));
            // its stores need not reach the tier, unless another slot translates the page
            bool other = false;
            for (jit_slot_t *t = g_by_host[host_hash(s->host)]; t; t = t->next_host)
                other |= t != s && t->host == s->host && !t->volatile_;
            if (!other)
                memory_code_page_unmark_owner(s->host, MEM_CODE_JIT);
        }
        s->hits = 0;
    }
    // Backoff: each translation doubles the entries the next one waits for
    R->cool = R->cool ? (R->cool * 2 > JIT_COOL_MAX ? JIT_COOL_MAX : R->cool * 2) : (g_cooldown ? g_cooldown : 1);
    R->since = 0;
    R->stale = 0;
    R->want = false;
    uint32_t n = 0;
    for (uint32_t k = 0; k < JIT_REGION_PAGES; k++) {
        U.slot[k] = -1;
        jit_slot_t *s = R->slot[k];
        if (!s)
            continue;
        if (s->volatile_) {
            memset(s->owner, NONE8, sizeof s->owner);
            continue;
        }
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) { // the page as it is now, classified
            uint32_t w = LOAD_BE32(s->host + 4 * i);
            if (w != s->words[i] || !s->classified) {
                s->words[i] = w;
                s->ids[i] = jit_classify(w, s->lpage + 4 * i, s->lpage, &s->ents[i]);
            }
        }
        s->classified = true;
        memset(s->poor, 0, sizeof s->poor); // the code may differ now
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) { // written often: for good (no flapping)
            s->unstable[i] |= s->writes[i] >= JIT_UNSTABLE_WRITES;
            s->writes[i] >>= 1;
        }
        memset(leader[n], 0, JIT_PAGE_INSNS);
        memset(reached[n], 0, JIT_PAGE_INSNS);
        // What the predecoded tier has run on the page: a word it never
        // decoded never executed (the pool decodes at dispatch), and code
        // that never ran is not worth a translation.  (A peek at the pool's
        // cache, not a lookup: no pool state changes.)
        pd_block_t *pb = g_pd_tcache[predecode_tcache_slot(s->host)];
        ran[n] = predecode_block_ok(pb, s->host, s->lpage) && pb->arch == PD_ARCH_PPC ? pb->e : NULL;
        U.slot[k] = (int16_t)n;
        slot_page[n] = k;
        n++;
    }
    double tp = emscripten_get_now();
    g_prof[0] += tp - t0;
    // Reachability from the entry points over the direct control flow:
    // every entry point, branch target and fall-through leads a block
    uint32_t sp = 0;
#define PUSH(addr_)                                                                                                    \
    do {                                                                                                               \
        uint32_t a_ = (addr_), o_ = a_ - lbase;                                                                        \
        if (!(a_ & 3) && o_ < (JIT_REGION_PAGES << MEM_PAGE_SHIFT) && U.slot[o_ >> MEM_PAGE_SHIFT] >= 0 &&             \
            sp < sizeof stack / sizeof stack[0]) {                                                                     \
            uint32_t s_ = (uint32_t)U.slot[o_ >> MEM_PAGE_SHIFT], i_ = (o_ & MEM_PAGE_MASK) >> 2;                      \
            leader[s_][i_] = 1;                                                                                        \
            if (!reached[s_][i_])                                                                                      \
                stack[sp++] = s_ * JIT_PAGE_INSNS + i_;                                                                \
        }                                                                                                              \
    } while (0)
    for (uint32_t u = 0; u < n; u++) {
        jit_slot_t *s = R->slot[slot_page[u]];
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++)
            if (s->extra[i] >= JIT_ENTRY_HOT)
                PUSH(s->lpage + 4 * i);
    }
    while (sp) {
        uint32_t x = stack[--sp], u = x / JIT_PAGE_INSNS, i = x % JIT_PAGE_INSNS;
        jit_slot_t *s = R->slot[slot_page[u]];
        for (; i < JIT_PAGE_INSNS; i++) {
            // (a word a chunk translated has run: once translated, the
            // predecoded entry may never be decoded again)
            // (nor is a word the guest keeps rewriting -- the 68K emulator
            // toggles some of its own branches -- or every rewrite would
            // retranslate its chunk: the predecoded loop runs it)
            if (reached[u][i] || (ran[u] && ran[u][i].id == PD_UNDECODED && s->owner[i] == NONE8))
                break;
            if (s->unstable[i]) {
                PUSH(s->lpage + 4 * i + 4); // (code: it runs; the predecoded loop comes back after it)
                break;
            }
            reached[u][i] = 1;
            uint32_t w = s->words[i], a = s->lpage + 4 * i;
            pd_entry_t e;
            uint16_t id = s->ids[i];
            e = s->ents[i];
            // (After an untranslated instruction, a call or the page's
            // last word the code may well be data -- the 68K emulator puts
            // operands after its calls: those continuations become leaders
            // only once execution enters them, as run-time entry points.)
            if (!translatable(id, w, a))
                break;
            if (is_branch_id(id)) {
                if (id != PPD_BLR && id != PPD_BCTR && id != JX_BCLR && id != JX_BCCTR)
                    PUSH(branch_target(id, w, a));
                if (id == PPD_BDNZ_IN || id == PPD_BDNZ_OUT || id == PPD_BC_T_IN || id == PPD_BC_T_OUT ||
                    id == PPD_BC_F_IN || id == PPD_BC_F_OUT || ((id == JX_BCLR || id == JX_BCCTR) && !e.c) ||
                    (id == JX_BC && (e.a & 0x14u) != 0x14u))
                    PUSH(a + 4); // a conditional branch falls through
                break;
            }
        }
    }
#undef PUSH
    g_prof[1] += emscripten_get_now() - tp, tp = emscripten_get_now();
    // Chunks: a reached word keeps its chunk from one translation to the
    // next, so code that did not change keeps its function; code reached
    // for the first time goes into new chunks, whole blocks in address
    // order, up to the cap each.  When the chunks run out, everything is
    // assigned afresh.
    uint32_t total[JIT_MAX_CHUNKS];
    bool used[JIT_MAX_CHUNKS];
    for (int fresh = 0; fresh < 2; fresh++) {
        memset(total, 0, sizeof total);
        memset(used, 0, sizeof used);
        for (uint32_t u = 0; u < n; u++) {
            jit_slot_t *s = R->slot[slot_page[u]];
            for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) {
                if (!reached[u][i] || fresh)
                    s->owner[i] = NONE8;
                else if (s->owner[i] != NONE8) {
                    used[s->owner[i]] = true;
                    total[s->owner[i]]++;
                }
            }
        }
        uint32_t c = NONE8;
        bool out = false;
        for (uint32_t u = 0; u < n && !out; u++) {
            jit_slot_t *s = R->slot[slot_page[u]];
            for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) {
                if (!reached[u][i] || s->owner[i] != NONE8)
                    continue;
                if (!leader[u][i] && i > 0 && reached[u][i - 1] && s->owner[i - 1] != NONE8) {
                    // a run that goes on past its chunk's words (code after a word
                    // it stopped at before): it stays in that chunk, or the new
                    // words would form a chunk with no block start
                    s->owner[i] = s->owner[i - 1];
                    total[s->owner[i]]++;
                    continue;
                }
                if (c == NONE8 || (leader[u][i] && total[c] >= g_chunk_cap)) {
                    for (c = 0; c < JIT_MAX_CHUNKS && (used[c] || (c < R->nchunks && R->chunk[c].fn && !fresh)); c++)
                        ;
                    if (c == JIT_MAX_CHUNKS) {
                        out = true;
                        break;
                    }
                    used[c] = true;
                }
                s->owner[i] = (uint8_t)c;
                total[c]++;
            }
        }
        if (!out)
            break;
    }
    U.lbase = lbase;
    U.it = (R->key & 2u) != 0;
    U.pr = (R->key & 1u) != 0;
    U.is601 = !ppc_is_604(p);
    U.shared = jit_memory_shared() != 0;
    U.rslot = addr_of(&R->slot[0]);
    U.rchunk = addr_of(&R->chunk[0]);
    U.chunk_size = sizeof(jit_chunk_t);
    U.chunk_fn = offsetof(jit_chunk_t, fn);
    U.chunk_maps = offsetof(jit_chunk_t, maps);
    U.slot_owner = offsetof(jit_slot_t, owner);
    U.slot_hadj = offsetof(jit_slot_t, host_adjust);
    U.pages = pages;
    U.n = n;
    uint32_t nchunks = 0;
    for (uint32_t ci = 0; ci < JIT_MAX_CHUNKS; ci++)
        if (used[ci] || (ci < R->nchunks && R->chunk[ci].fn))
            nchunks = ci + 1;
    R->nchunks = nchunks;
    g_prof[2] += emscripten_get_now() - tp, tp = emscripten_get_now();
    // What each chunk translates, in one pass: its words, leaders and
    // mappings (a word reached for the first time is never one its blocks
    // ran into: they reached every word they cover), and its pages
    static uint64_t sigs[JIT_MAX_CHUNKS], sigws[JIT_MAX_CHUNKS];
    static uint32_t nwords[JIT_MAX_CHUNKS], lastu[JIT_MAX_CHUNKS];
    static uint8_t cpages[JIT_MAX_CHUNKS][JIT_REGION_PAGES / 8];
#define MIX(c_, v_)  (sigs[c_] = (sigs[c_] ^ (uint64_t)(v_)) * 1099511628211ull)
#define MIXW(c_, v_) (sigws[c_] = (sigws[c_] ^ (uint64_t)(v_)) * 1099511628211ull)
    for (uint32_t ci = 0; ci < nchunks; ci++) {
        sigs[ci] = sigws[ci] = 1469598103934665603ull;
        nwords[ci] = 0;
        lastu[ci] = ~0u;
        memset(cpages[ci], 0, sizeof cpages[ci]);
    }
    for (uint32_t u = 0; u < n; u++) {
        jit_slot_t *s = R->slot[slot_page[u]];
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) {
            uint32_t o = s->owner[i];
            if (o == NONE8 || o >= nchunks)
                continue;
            if (lastu[o] != u) {
                lastu[o] = u;
                cpages[o][u >> 3] |= (uint8_t)(1u << (u & 7));
                MIX(o, slot_page[u]);
                MIX(o, s->host_adjust);
                MIXW(o, slot_page[u]);
                MIXW(o, s->host_adjust);
            }
            MIX(o, ((uint64_t)i << 40) | ((uint64_t)leader[u][i] << 32) | s->words[i]);
            MIXW(o, ((uint64_t)i << 40) | s->words[i]);
            nwords[o]++;
        }
    }
#undef MIX
#undef MIXW
    R->ncompiles++;
    for (uint32_t ci = 0; ci < nchunks; ci++) {
        jit_chunk_t *ch = &R->chunk[ci];
        uint64_t sig = sigs[ci];
        uint32_t words = nwords[ci];
        if (!used[ci]) { // no pages any more
            release_fn(ch->fn);
            ch->fn = 0;
            ch->pending = 0;
            ch->empty = false;
            ch->nb = 0;
            ch->sig = 0;
            memset(ch->refs, 0, sizeof ch->refs);
            continue;
        }
        // Only new leaders inside it (new code branching into the middle of
        // its blocks): the function is still right, the new entries just
        // are not translated yet.  It is retranslated for them only every
        // so often, the interval doubling with each of its translations.
        bool built = ch->fn || ch->pending || ch->empty;
        bool split = built && ch->sigw == sigws[ci] &&
                     R->ncompiles - ch->last_regen < (JIT_SPLIT_COMPILES << (ch->regens < 10 ? ch->regens : 10));
        if (built && (ch->sig == sig || split)) {
            // Unchanged words: keep the function; the words are the ones it
            // was translated from, so every block is valid again
            memset(ch->bvalid, 1, ch->nb + 1);
            continue;
        }
        if (g_trace == 6) {
            uint32_t first = 0;
            for (uint32_t u = 0; u < n && !first; u++)
                if ((cpages[ci][u >> 3] >> (u & 7)) & 1u)
                    for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++)
                        if (R->slot[slot_page[u]]->owner[i] == ci) {
                            first = R->slot[slot_page[u]]->lpage + 4 * i;
                            break;
                        }
            printf("jit:   chunk %u at %08x, %u words (was %u), %s\n", ci, first, words, ch->nwords,
                   ch->fn ? (ch->sigw == sigws[ci] ? "split" : "changed") : "new");
        }
        ch->nwords = words;
        ch->empty = false;
        ch->sigw = sigws[ci];
        ch->regens++;
        ch->last_regen = R->ncompiles;
        g_regen++;
        g_regen_new += !ch->fn && !ch->pending;
        release_fn(ch->fn);
        ch->fn = 0;
        ch->pending = 0; // (a compile in flight is dropped when it lands)
        ch->nb = 0;
        ch->sig = sig;
        memset(ch->refs, 0, sizeof ch->refs);
        for (uint32_t u = 0; u < n; u++) {
            uint32_t k = slot_page[u];
            jit_slot_t *s = R->slot[k];
            bool own = (cpages[ci][u >> 3] >> (u & 7)) & 1u;
            if (own)
                for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) {
                    // 1: leads a block of this chunk; 2: a block stops there (another
                    // chunk's word, or one not reached: a block must cover only words
                    // its chunk owns, or a store into them would not invalidate it)
                    cleader[u][i] = !reached[u][i] || s->owner[i] != ci ? 2 : leader[u][i] ? 1 : 0;
                    ccov[u][i] = NONE16;
                }
            if (own && !ch->maps[k])
                ch->maps[k] = malloc(JIT_PAGE_INSNS * sizeof(uint16_t));
            if (own && !ch->maps[k])
                return;
            pages[u] = (jt_page_t){s->lpage, s->words,       cleader[u], own ? ch->maps[k] : NULL,
                                   ccov[u],  s->host_adjust, s->ids,     s->ents};
        }
        if (words + 1 > ch->bcap) { // at most one block per word
            free(ch->bvalid);
            ch->bcap = words + 1;
            ch->bvalid = malloc(ch->bcap);
            if (!ch->bvalid) {
                ch->bcap = 0;
                return;
            }
        }
        memset(ch->bvalid, 1, words + 1);
        U.refs = ch->refs;
        U.bvalid = ch->bvalid;
        g_prof[3] += emscripten_get_now() - tp, tp = emscripten_get_now();
        uint32_t nb = translate();
        g_prof[4] += emscripten_get_now() - tp, tp = emscripten_get_now();
        for (uint32_t u = 0; u < n; u++) {
            if (!pages[u].map)
                continue;
            jit_slot_t *s = R->slot[slot_page[u]];
            for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++)
                if (s->owner[i] == ci)
                    s->cov[i] = ccov[u][i];
        }
        if (!nb) {
            ch->empty = true; // (nothing to translate: not again until it changes)
            continue;
        }
        ch->nb = nb;
        if (g_trace == 5) { // the size per translated instruction (debugging)
            uint32_t ni = 0;
            for (uint32_t bi = 0; bi < nb; bi++)
                ni += g_blocks[bi].n;
            printf("jit: chunk %08x/%u: %u blocks, %u instructions, %u bytes (%.1f per instruction)\n", lbase, ci, nb,
                   ni, g_len, (double)g_len / (ni ? ni : 1));
        }
        if (g_trace == 4) { // keep the module for inspection (debugging)
            char name[64];
            snprintf(name, sizeof name, "jit-%08x-%u-%llu.wasm", lbase, ci, (unsigned long long)g_st.functions);
            FILE *f = fopen(name, "wb");
            if (f) {
                fwrite(g_buf, 1, g_len, f);
                fclose(f);
            }
        }
        g_prof[3] += emscripten_get_now() - tp, tp = emscripten_get_now();
        if (g_async) {
            ch->pending = ++g_token ? g_token : ++g_token;
            pending_add(ch->pending, R, ci, p->msr);
            jit_compile_async(g_buf, g_len, ch->pending);
        } else {
            ch->fn = install_fn(0, p->msr);
        }
        g_prof[5] += emscripten_get_now() - tp, tp = emscripten_get_now();
        g_st.functions++;
        g_st.bytes += g_len;
        for (uint32_t u = 0; u < n; u++) {
            uint32_t k = slot_page[u];
            if (!pages[u].map)
                continue; // (no block there)
            ch->refs[k].map = addr_of(ch->maps[k]);
            ch->refs[k].host_adjust = R->slot[k]->host_adjust;
        }
    }
    for (uint32_t u = 0; u < n; u++)
        memory_code_page_mark_owner(R->slot[slot_page[u]]->host, MEM_CODE_JIT);
    g_prof[3] += emscripten_get_now() - tp;
    if (g_trace == 6)
        printf("jit:   -> %u chunks regenerated (%u new), %llu bytes\n", g_regen, g_regen_new,
               (unsigned long long)(g_st.bytes - bytes0));
    g_st.compiles++;
    if (g_trace == 7)
        printf("jit: compile split ms: prep %.0f reach %.0f assign %.0f chunks %.0f translate %.0f instantiate %.0f "
               "(n=%u)\n",
               g_prof[0], g_prof[1], g_prof[2], g_prof[3], g_prof[4], g_prof[5], n);
    g_st.compile_ms += emscripten_get_now() - t0;
}

// Make (or remake) the region page k a member under the current mapping
static jit_slot_t *slot_make(jit_region_t *R, uint32_t k, uint32_t lpage, uint32_t host_adjust) {
    jit_slot_t *s = R->slot[k];
    if (s && s->host_adjust != host_adjust) { // re-mapped: start over
        region_drop_code(R); // the functions must not read the old maps
        slot_unlink(s);
        free(s);
        s = R->slot[k] = NULL;
    }
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s)
            return NULL;
        s->lpage = lpage;
        s->host_adjust = host_adjust;
        s->host = (uint8_t *)(uintptr_t)(host_adjust + lpage);
        s->region = R;
        memset(s->owner, NONE8, sizeof s->owner);
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++)
            s->cov[i] = NONE16;
        uint32_t h = host_hash(s->host);
        s->next_host = g_by_host[h];
        g_by_host[h] = s;
        R->slot[k] = s;
    }
    return s;
}

// The chunk and block an entry at word idx of member s (region page k) runs, if any
static bool region_entry(jit_region_t *R, jit_slot_t *s, uint32_t k, uint32_t idx, jit_chunk_t **chp, uint16_t *bp) {
    uint8_t c = s->owner[idx];
    if (c == NONE8 || c >= R->nchunks)
        return false;
    jit_chunk_t *ch = &R->chunk[c];
    if (!ch->fn || !ch->maps[k])
        return false;
    uint16_t b = ch->maps[k][idx];
    if (b == NONE16 || !ch->bvalid[b])
        return false;
    *chp = ch;
    *bp = b;
    return true;
}

// jit.trace = 2: a histogram of the pcs translations leave at (jit.trace = 3
// prints it and clears it), to see why translated runs end
#define JIT_XH_SIZE 4096
static ppc_t *g_last_p;
static uint64_t g_xh_tail, g_xh_budget_sum, g_xh_calls;
static uint64_t g_xh_kind[5]; // exits: deopt, same chunk, other chunk, other region, other // exits with < 32 slots
                              // left; entry budgets
static struct {
    uint32_t pc, n, deopt, ea;
} g_xh[JIT_XH_SIZE];
static void exit_hist_add(uint32_t pc, bool deopt, uint32_t ea) {
    uint32_t h = (pc * 2654435761u) >> 20;
    for (uint32_t j = 0; j < JIT_XH_SIZE; j++, h = (h + 1) & (JIT_XH_SIZE - 1)) {
        if (g_xh[h].n && g_xh[h].pc != pc)
            continue;
        g_xh[h].pc = pc;
        g_xh[h].n++;
        g_xh[h].deopt += deopt;
        g_xh[h].ea = ea;
        return;
    }
}
// The EA of a D-form access at p->pc (the deopt histogram)
static uint32_t deopt_ea(ppc_t *p) {
    uint32_t key = (p->pc & ~(uint32_t)((1u << JIT_REGION_SHIFT) - 1)) | ((p->msr & PPC_MSR_IT) ? 2u : 0u) |
                   ((p->msr & PPC_MSR_PR) ? 1u : 0u);
    jit_region_t *R = region_get(key);
    jit_slot_t *s = R ? R->slot[(p->pc >> MEM_PAGE_SHIFT) & (JIT_REGION_PAGES - 1)] : NULL;
    if (!s)
        return 0;
    uint32_t w = s->words[(p->pc & MEM_PAGE_MASK) >> 2], op = w >> 26, ra = (w >> 16) & 31u;
    if (op >= 32 && op <= 45)
        return (ra ? p->gpr[ra] : 0) + (uint32_t)(int32_t)(int16_t)w;
    return 0;
}
static jit_region_t *region_find(uint32_t key);
static void exit_kind(ppc_t *p, jit_region_t *R, jit_chunk_t *ch) {
    if (g_ppc_jit_exit == PPC_JIT_EXIT_DEOPT) {
        g_xh_kind[0]++;
        return;
    }
    uint32_t key = (p->pc & ~(uint32_t)((1u << JIT_REGION_SHIFT) - 1)) | ((p->msr & PPC_MSR_IT) ? 2u : 0u) |
                   ((p->msr & PPC_MSR_PR) ? 1u : 0u);
    jit_region_t *T = region_find(key);
    jit_slot_t *s = T ? T->slot[(p->pc >> MEM_PAGE_SHIFT) & (JIT_REGION_PAGES - 1)] : NULL;
    uint8_t o = s ? s->owner[(p->pc & MEM_PAGE_MASK) >> 2] : NONE8;
    if (!s || o == NONE8)
        g_xh_kind[4]++;
    else if (T != R)
        g_xh_kind[3]++;
    else
        g_xh_kind[&T->chunk[o] == ch ? 1 : 2]++;
}
static int exit_hist_cmp(const void *a, const void *b) {
    uint32_t x = ((const uint32_t *)a)[1], y = ((const uint32_t *)b)[1];
    return x < y ? 1 : x > y ? -1 : 0;
}
static void exit_hist_dump(ppc_t *p) {
    qsort(g_xh, JIT_XH_SIZE, sizeof g_xh[0], exit_hist_cmp);
    uint64_t tot = 0;
    for (uint32_t j = 0; j < JIT_XH_SIZE; j++)
        tot += g_xh[j].n;
    printf("jit exits: %llu, %llu with < 32 slots left, mean entry budget %.0f\n", (unsigned long long)tot,
           (unsigned long long)g_xh_tail, g_xh_calls ? (double)g_xh_budget_sum / g_xh_calls : 0.0);
    printf("  kinds: deopt %llu, same chunk %llu, other chunk %llu, other region %llu, no block %llu\n",
           (unsigned long long)g_xh_kind[0], (unsigned long long)g_xh_kind[1], (unsigned long long)g_xh_kind[2],
           (unsigned long long)g_xh_kind[3], (unsigned long long)g_xh_kind[4]);
    memset(g_xh_kind, 0, sizeof g_xh_kind);
    g_xh_tail = g_xh_budget_sum = g_xh_calls = 0;
    uint32_t msr = p->msr;
    for (uint32_t j = 0; j < 40 && g_xh[j].n; j++) {
        uint32_t pc = g_xh[j].pc;
        uint32_t key = (pc & ~(uint32_t)((1u << JIT_REGION_SHIFT) - 1)) | ((msr & PPC_MSR_IT) ? 2u : 0u) |
                       ((msr & PPC_MSR_PR) ? 1u : 0u);
        jit_region_t *R = region_get(key);
        jit_slot_t *s = R ? R->slot[(pc >> MEM_PAGE_SHIFT) & (JIT_REGION_PAGES - 1)] : NULL;
        const char *why = "no member";
        uint32_t w = 0;
        if (s) {
            uint32_t i = (pc & MEM_PAGE_MASK) >> 2;
            jit_chunk_t *ch;
            uint16_t b;
            w = s->words[i];
            pd_entry_t e;
            uint16_t id = jit_classify(w, pc, s->lpage, &e);
            why = s->volatile_                                                                      ? "volatile"
                  : region_entry(R, s, (pc >> MEM_PAGE_SHIFT) & (JIT_REGION_PAGES - 1), i, &ch, &b) ? "block"
                  : !translatable(id, w, pc)                                                        ? "untranslated"
                                                                                                    : "no block";
        }
        printf("  %08x %9u (%5.1f%%) deopt=%u %-12s %08x", pc, g_xh[j].n, 100.0 * g_xh[j].n / (tot ? tot : 1),
               g_xh[j].deopt, why, w);
        if (g_xh[j].deopt) {
            uint32_t ea = g_xh[j].ea;
            printf(" ea=%08x r=%lx w=%lx", ea, (unsigned long)g_active_read[(ea & g_address_mask) >> MEM_PAGE_SHIFT],
                   (unsigned long)g_active_write[(ea & g_address_mask) >> MEM_PAGE_SHIFT]);
        }
        printf("\n");
    }
    // The untranslated instructions translations stopped at, by opcode
    // (primary << 10 | extended for 19/31/59/63): what to translate next
    static struct {
        uint32_t key, n, word;
    } agg[256];
    uint32_t na = 0;
    for (uint32_t j = 0; j < JIT_XH_SIZE; j++) {
        if (!g_xh[j].n || g_xh[j].deopt)
            continue;
        uint32_t pc = g_xh[j].pc;
        jit_slot_t *s = NULL; // (whichever fetch mode has the page)
        for (uint32_t m = 0; m < 4 && !s; m++) {
            jit_region_t *R = region_find((pc & ~(uint32_t)((1u << JIT_REGION_SHIFT) - 1)) | m);
            s = R ? R->slot[(pc >> MEM_PAGE_SHIFT) & (JIT_REGION_PAGES - 1)] : NULL;
        }
        if (!s)
            continue;
        uint32_t i = (pc & MEM_PAGE_MASK) >> 2, w = LOAD_BE32(s->host + 4 * i);
        if (translatable(s->ids[i], w, pc) && !s->unstable[i])
            continue;
        uint32_t op = w >> 26, k = op << 10;
        if (op == 19 || op == 31 || op == 59 || op == 63)
            k |= (w >> 1) & 0x3FFu;
        uint32_t a = 0;
        while (a < na && agg[a].key != k)
            a++;
        if (a == na) {
            if (na == 256)
                continue;
            agg[na++] = (typeof(agg[0])){k, 0, w};
        }
        agg[a].n += g_xh[j].n;
    }
    for (uint32_t a = 0; a < na; a++)
        printf("  untranslated op %2u xo %4u: %9u exits (e.g. %08x)\n", agg[a].key >> 10, agg[a].key & 0x3FFu, agg[a].n,
               agg[a].word);
    memset(g_xh, 0, sizeof g_xh);
}

bool ppc_jit_enter(ppc_t *p, uint32_t *instructions, uint64_t *folds_left, int *exit) {
    uint32_t msr = p->msr;
    if ((++g_st.lookups & 4095) == 0) // skipped words are asked about again now and then
        memset(g_ppc_jit_skip, 0, sizeof g_ppc_jit_skip);
    // Not translated: little-endian mode; supervisor data translation (every
    // access takes the slow path there); a deliverable interrupt (the
    // predecoded loop takes it first); the sprint's tail
    if ((msr & PPC_MSR_LE) || (msr & (PPC_MSR_DT | PPC_MSR_PR)) == PPC_MSR_DT)
        return false;
    if ((p->ext_irq | p->dec_pending) && (msr & PPC_MSR_EE))
        return false;
    uint32_t budget = *instructions;
    // The page the last entry declined: what the predecoded loop ran there
    // since (within the sprint) is its heat
    if (g_hot_R) {
        if (budget < g_ppc_jit_cold.budget)
            g_ppc_jit_cold.heat += g_ppc_jit_cold.budget - budget;
        g_hot_R->heat[g_hot_k] = g_ppc_jit_cold.heat;
        g_hot_R = NULL;
        g_ppc_jit_cold.key = 0;
    }
    if (budget < 3)
        return false;
    if (!g_inited) {
        g_inited = true;
        g_mapgen = g_mem_map_generation;
        if (!g_async_set) // a script under Node never returns to the event loop
            g_async = !jit_host_is_node();
    }
    if (g_mapgen != g_mem_map_generation || g_model != (uint32_t)p->cpu_model) {
        ppc_jit_reset(); // real-mode translations follow the memory map; ids follow the model
        g_model = (uint32_t)p->cpu_model;
    }
    uint32_t pc = p->pc;
    uint32_t key = (pc & ~(uint32_t)((1u << JIT_REGION_SHIFT) - 1)) | ((msr & PPC_MSR_IT) ? 2u : 0u) |
                   ((msr & PPC_MSR_PR) ? 1u : 0u);
    jit_region_t *R = g_rcache[(key >> JIT_REGION_SHIFT) & (JIT_RCACHE - 1)];
    if (!R || R->key != key) {
        R = region_get(key);
        if (!R)
            return false;
        g_rcache[(key >> JIT_REGION_SHIFT) & (JIT_RCACHE - 1)] = R;
    }
    uint32_t k = (pc >> MEM_PAGE_SHIFT) & (JIT_REGION_PAGES - 1);
    uint32_t idx = (pc & MEM_PAGE_MASK) >> 2;
    uint32_t host_adjust = (uint32_t)g_ppc_fetch.host_adjust;
    jit_slot_t *s = R->slot[k];
    R->since++;
    if (s && s->volatile_ && s->host_adjust == host_adjust) {
        if (g_st.lookups < s->vol_until) {
            skip_word(pc, msr);
            return false; // left to the predecoded tier
        }
        s->volatile_ = false; // try it again (its writes may have settled)
        s->churn = 0;
        s->classified = false;
        R->want = true;
    }
    bool member = s && s->host_adjust == host_adjust;
    jit_chunk_t *ch = NULL;
    uint16_t b = NONE16;
    if (member && s->owner[idx] < R->nchunks && R->chunk[s->owner[idx]].pending) {
        skip_word(pc, msr); // its translation is being compiled
        return false;
    }
    if (!member || !region_entry(R, s, k, idx, &ch, &b)) {
        // Not translated here (yet): count it, and translate when the page
        // is hot and the region has not just been compiled
        if (!member && R->heat[k] < g_threshold) {
            g_hot_R = R, g_hot_k = k;
            g_ppc_jit_cold =
                (ppc_jit_cold_t){(pc & ~(uint32_t)MEM_PAGE_MASK) | 4u | (key & 3u), budget, R->heat[k], g_threshold};
            return false;
        }
        if (member && s->extra[idx] < JIT_ENTRY_HOT && ++s->extra[idx] == JIT_ENTRY_HOT) {
            // a new entry point (one entered rarely is not worth a translation)
            R->want = true;
        }
        if (member && s->extra[idx] >= JIT_ENTRY_HOT && R->cool)
            skip_word(pc, msr); // counted: nothing more until the region is translated
        if (!member) {
            s = slot_make(R, k, pc & ~(uint32_t)MEM_PAGE_MASK, host_adjust);
            if (!s)
                return false;
            R->heat[k] = 0;
            s->extra[idx] = JIT_ENTRY_HOT;
            R->want = true;
        }
        uint32_t cool = R->cool > g_cooldown ? R->cool : g_cooldown;
        bool due = (R->want && R->since >= cool) || (R->stale && R->since >= (cool > g_quiet ? cool : g_quiet)) ||
                   !R->cool; // never translated
        if (!due)
            return false;
        // The governor: translating may take at most jit.budget percent of
        // the time since translation began (plus a start allowance), so
        // code that keeps changing (a boot) cannot cost more than that
        double now = emscripten_get_now();
        if (!g_t_start)
            g_t_start = now;
        if (g_budget_pct < 100 && g_st.compile_ms > JIT_BUDGET_START_MS + (now - g_t_start) * g_budget_pct / 100.0) {
            g_st.deferred++;
            R->since = 0; // ask again after another cooldown
            return false;
        }
        if (g_trace == 6)
            printf("jit: compile %08x want=%d stale=%u since=%u cool=%u at pc %08x (%s)\n", R->key, R->want, R->stale,
                   R->since, R->cool, pc, member ? "new entry" : "new member");
        compile_region(R, p);
        if (!region_entry(R, s, k, idx, &ch, &b))
            return false;
    }
    // An entry whose runs keep ending after a few instructions (a short
    // loop around an untranslated instruction, an access that always
    // misses) costs more to enter than it saves: the predecoded loop keeps it
    if (s->poor[idx] >= JIT_POOR_MAX) {
        skip_word(pc, msr);
        return false;
    }
    // Run it (the predecoded block being left is charged first, as at any
    // page transition)
    predecode_enter(NULL, budget);
    int32_t fin = *folds_left > 0x7FFFFFFFu ? 0x7FFFFFFF : (int32_t)*folds_left;
    g_ppc_jit_exit = PPC_JIT_EXIT_PC;
    g_ppc_jit_folds = fin;
    if (g_trace == 1) {
        printf("jit: enter pc=%08x budget=%u chunk=%u block=%u\n", pc, budget, (unsigned)(ch - R->chunk), b);
        for (uint32_t i = idx; i < idx + 16 && i < JIT_PAGE_INSNS; i++) {
            uint32_t live = LOAD_BE32(s->host + 4 * i);
            if (live != s->words[i])
                printf("jit:   stale word %08x: translated %08x, live %08x\n", s->lpage + 4 * i, s->words[i], live);
        }
    }
    int32_t left = ((jit_fn_t)(uintptr_t)ch->fn)(p, (int32_t)budget, (int32_t)b, fin);
    if (g_trace == 2)
        exit_hist_add(p->pc, g_ppc_jit_exit == PPC_JIT_EXIT_DEOPT,
                      g_ppc_jit_exit == PPC_JIT_EXIT_DEOPT ? deopt_ea(p) : 0),
            g_last_p = p, g_xh_tail += left < 32, g_xh_budget_sum += budget, g_xh_calls++, exit_kind(p, R, ch);
    else if (g_trace)
        printf("jit: exit  pc=%08x left=%d reason=%u\n", p->pc, left, g_ppc_jit_exit);
    *instructions = (uint32_t)left;
    *folds_left -= (uint64_t)(uint32_t)(fin - g_ppc_jit_folds);
    *exit = (int)g_ppc_jit_exit;
    g_st.calls++;
    g_st.insns += budget - (uint32_t)left;
    if (budget - (uint32_t)left < JIT_POOR_RUN)
        s->poor[idx]++;
    else if (s->poor[idx])
        s->poor[idx]--;
    if (*exit == PPC_JIT_EXIT_DEOPT)
        g_st.deopts++;
    return true;
}

// A store from translated code into a page whose write entry the code-page
// marks took away (a data word next to code: the 68K emulator's own
// variables): user mode, translation on, the 604.  The mapping is the
// page's read entry when the fill that planted it allowed stores
// (g_ppc_user_wok), so the store completes as it would through the write
// entry the switch core holds, after the coherence hooks.  `ea` is masked.
// Returns 1 when it did nothing (the translation leaves at the store), 2
// when it completed and invalidated translated code (it leaves after the
// store), 0 when it completed.
static bool g_jit_wrote; // ppc_jit_code_written invalidated a block
EMSCRIPTEN_KEEPALIVE int32_t ppc_jit_code_store(uint32_t ea, uint32_t value, uint32_t size);
int32_t ppc_jit_code_store(uint32_t ea, uint32_t value, uint32_t size) {
    uint32_t page = ea >> MEM_PAGE_SHIFT;
    static uint32_t why[6];
    if (g_trace == 10 && (++why[5] & 0xFFFF) == 0)
        printf("jit: code_store declines: tables %u, read %u, wok %u, cross %u, uncode %u\n", why[0], why[1], why[2],
               why[3], why[4]);
    if (g_active_write != g_user_write || !g_ppc_user_wok || page >= g_page_count)
        return why[0]++, 1;
    if ((ea & MEM_PAGE_MASK) > MEM_PAGE_SIZE - size)
        return why[3]++, 1;
    uintptr_t rd = g_user_read[page];
    uint8_t *host;
    if (!rd) { // the entries are gone; the translation TLB may still serve it
        host = ppc_mmu_user_store_host(ea);
        if (!host)
            return why[1]++, 1;
    } else if (g_ppc_user_wok[page] != rd)
        return why[2]++, 1;
    else
        host = (uint8_t *)(rd + ea);
    if (!memory_host_is_code(host))
        return why[4]++, 1; // unmarked since: the slow path brings the write entry back
    g_jit_wrote = false;
    memory_host_written(host, size);
    for (uint32_t i = 0; i < size; i++)
        host[i] = (uint8_t)(value >> (8 * (size - 1 - i)));
    return g_jit_wrote ? 2 : 0;
}

// The coherence hook: invalidate the blocks that translated the written words
void ppc_jit_code_written(const uint8_t *host, uint32_t len) {
    if (!len)
        return;
    if (g_trace == 9) {
        uint32_t pg_;
        int r_ = memory_code_region_of(host, &pg_);
        printf("jit: hook host+%u len %u region %d\n", r_ >= 0 ? (uint32_t)(host - g_mem_code_regions[r_].base) : 0,
               len, r_);
    }
    // The page holding `host`, as a member records it: pages are relative to
    // their code region (the RAM image is not 4 KB-aligned on the host)
    uint32_t pg;
    int r = memory_code_region_of(host, &pg);
    if (r < 0)
        return;
    const uint8_t *page = g_mem_code_regions[r].base + ((uintptr_t)pg << MEM_PAGE_SHIFT);
    for (jit_slot_t *s = g_by_host[host_hash(page)]; s; s = s->next_host) {
        if (s->host != page)
            continue;
        jit_region_t *R = s->region;
        uint32_t off = (uint32_t)(host - page);
        uint32_t end = off + len > MEM_PAGE_SIZE ? MEM_PAGE_SIZE : off + len;
        bool hit = false;
        for (uint32_t w = off >> 2; w <= (end - 1) >> 2; w++) {
            uint8_t c = s->owner[w];
            uint16_t b = s->cov[w];
            if (s->writes[w] < 255)
                s->writes[w]++;
            if (g_trace == 9)
                printf("jit: write %08x owner=%u cov=%u nchunks=%u valid=%d vol=%d\n", s->lpage + 4 * w, c, b,
                       R->nchunks,
                       (c != NONE8 && b != NONE16 && c < R->nchunks && R->chunk[c].bvalid) ? R->chunk[c].bvalid[b] : -1,
                       s->volatile_);
            if (c == NONE8 || b == NONE16 || c >= R->nchunks || !R->chunk[c].bvalid)
                continue;
            if (R->chunk[c].bvalid[b]) {
                if (g_trace == 8)
                    printf("jit: inval %08x old %08x new %08x\n", s->lpage + 4 * w, s->words[w],
                           LOAD_BE32(s->host + 4 * w));
                s->last_hit = (uint16_t)w;
                R->chunk[c].bvalid[b] = 0;
                R->stale++;
                g_st.block_invals++;
                hit = true;
            }
        }
        if (hit) {
            g_jit_wrote = true;
            s->hits++;
            R->since = 0; // retranslate only once the writes have quieted down
        }
    }
}

#else // !__EMSCRIPTEN__: no code generation

bool ppc_jit_enter(struct ppc *p, uint32_t *instructions, uint64_t *folds_left, int *exit) {
    (void)p;
    (void)instructions;
    (void)folds_left;
    (void)exit;
    return false;
}
void ppc_jit_code_written(const uint8_t *host, uint32_t len) {
    (void)host;
    (void)len;
}
void ppc_jit_reset(void) {}

#endif // __EMSCRIPTEN__

// === 4. The `jit` node ======================================================

enum {
    JA_ENABLED,
    JA_AVAILABLE,
    JA_THRESHOLD,
    JA_COOLDOWN,
    JA_QUIET,
    JA_REGIONS,
    JA_COMPILES,
    JA_FUNCTIONS,
    JA_CHUNK_CAP,
    JA_TRACE,
    JA_BYTES,
    JA_CALLS,
    JA_DEOPTS,
    JA_BLOCK_INVALS,
    JA_COMPILE_US,
    JA_INSNS,
    JA_LOOKUPS,
    JA_DEFERRED,
    JA_ASYNC,
    JA_BUDGET,
    JA_REGS
};

static value_t jit_attr_get(struct object *self, const member_t *m) {
    (void)self;
    switch ((int)(uintptr_t)m->attr.user_data) {
    case JA_ENABLED:
        return val_uint(1, g_ppc_jit_on);
    case JA_THRESHOLD:
        return val_uint(4, g_threshold);
    case JA_COOLDOWN:
        return val_uint(4, g_cooldown);
    case JA_QUIET:
        return val_uint(4, g_quiet);
    case JA_CHUNK_CAP:
        return val_uint(4, g_chunk_cap);
    case JA_TRACE:
        return val_uint(4, g_trace);
    case JA_BUDGET:
        return val_uint(4, g_budget_pct);
#ifdef __EMSCRIPTEN__
    case JA_AVAILABLE:
        return val_uint(1, 1);
    case JA_REGIONS:
        return val_uint(8, g_st.regions);
    case JA_COMPILES:
        return val_uint(8, g_st.compiles);
    case JA_FUNCTIONS:
        return val_uint(8, g_st.functions);
    case JA_BYTES:
        return val_uint(8, g_st.bytes);
    case JA_CALLS:
        return val_uint(8, g_st.calls);
    case JA_DEOPTS:
        return val_uint(8, g_st.deopts);
    case JA_BLOCK_INVALS:
        return val_uint(8, g_st.block_invals);
    case JA_COMPILE_US:
        return val_uint(8, (uint64_t)(g_st.compile_ms * 1000.0));
    case JA_INSNS:
        return val_uint(8, g_st.insns);
    case JA_REGS:
        return val_uint(4, g_fn_regs);
    case JA_LOOKUPS:
        return val_uint(8, g_st.lookups);
    case JA_DEFERRED:
        return val_uint(8, g_st.deferred);
    case JA_ASYNC:
        return val_uint(4, g_async);
#endif
    default:
        return val_uint(1, 0);
    }
}

static value_t jit_attr_set(struct object *self, const member_t *m, value_t in) {
    (void)self;
    switch ((int)(uintptr_t)m->attr.user_data) {
    case JA_ENABLED:
#ifdef __EMSCRIPTEN__
        g_ppc_jit_on = in.u != 0;
        if (!g_ppc_jit_on)
            ppc_jit_reset();
        return val_none();
#else
        if (in.u)
            return val_err("jit: no code generation in this build (wasm only)");
        return val_none();
#endif
    case JA_THRESHOLD:
        g_threshold = (uint32_t)in.u;
        return val_none();
    case JA_COOLDOWN:
        g_cooldown = (uint32_t)in.u;
        return val_none();
    case JA_QUIET:
        g_quiet = (uint32_t)in.u;
        return val_none();
    case JA_CHUNK_CAP:
        if (in.u < 64 || in.u > 65536)
            return val_err("chunk_cap must be 64..65536 instructions");
        g_chunk_cap = (uint32_t)in.u;
        return val_none();
    case JA_ASYNC:
#ifdef __EMSCRIPTEN__
        g_async = in.u ? 1 : 0;
        g_async_set = true;
#endif
        return val_none();
    case JA_BUDGET:
        if (in.u > 100)
            return val_err("budget is a percentage (100: no limit)");
        g_budget_pct = (uint32_t)in.u;
        return val_none();
    case JA_REGS:
#ifdef __EMSCRIPTEN__
        g_fn_regs = in.u ? 1 : 0;
        ppc_jit_reset();
#endif
        return val_none();
    case JA_TRACE:
#ifdef __EMSCRIPTEN__
        if (in.u == 3 && g_last_p) {
            exit_hist_dump(g_last_p);
            return val_none();
        }
#endif
        g_trace = (uint32_t)in.u;
        return val_none();
    default:
        return val_err("read-only jit attribute");
    }
}

static value_t jit_method_reset(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    ppc_jit_reset();
    return val_none();
}

#define JIT_ATTR(name_, id_, doc_, set_)                                                                               \
    {                                                                                                                  \
        .kind = MK_ATTR, .name = name_, .doc = doc_, .attr = {                                                         \
            .type = VK_UINT,                                                                                           \
            .get = jit_attr_get,                                                                                       \
            .set = set_,                                                                                               \
            .user_data = (const void *)(uintptr_t)(id_)                                                                \
        }                                                                                                              \
    }

static const member_t jit_members[] = {
    JIT_ATTR("enabled", JA_ENABLED, "1 = hot PowerPC code runs as generated WebAssembly (wasm builds only)",
             jit_attr_set),
    JIT_ATTR("available", JA_AVAILABLE, "1 when this build can generate code", NULL),
    JIT_ATTR("threshold", JA_THRESHOLD,
             "instructions the predecoded tier runs in a page before it joins its region's translation", jit_attr_set),
    JIT_ATTR("cooldown", JA_COOLDOWN, "entries between two translations of one region (0: none)", jit_attr_set),
    JIT_ATTR("quiet", JA_QUIET, "entries after a code write before its region is retranslated", jit_attr_set),
    JIT_ATTR("regions", JA_REGIONS, "regions created", NULL),
    JIT_ATTR("compiles", JA_COMPILES, "region translations", NULL),
    JIT_ATTR("functions", JA_FUNCTIONS, "functions generated (a region translates into chunks of chunk_cap)", NULL),
    JIT_ATTR("chunk_cap", JA_CHUNK_CAP, "the most instructions one generated function translates", jit_attr_set),
    JIT_ATTR("trace", JA_TRACE, "1 = print every entry into and exit from translated code (debugging)", jit_attr_set),
    JIT_ATTR("bytes", JA_BYTES, "bytes of WebAssembly generated", NULL),
    JIT_ATTR("calls", JA_CALLS, "entries into translated code", NULL),
    JIT_ATTR("deopts", JA_DEOPTS, "exits at an access that missed the memory fast path", NULL),
    JIT_ATTR("block_invals", JA_BLOCK_INVALS, "translated blocks invalidated by code writes", NULL),
    JIT_ATTR("compile_us", JA_COMPILE_US, "time spent translating and compiling, microseconds", NULL),
    JIT_ATTR("instructions", JA_INSNS, "budget slots consumed inside translated code", NULL),
    JIT_ATTR("lookups", JA_LOOKUPS, "times the predecoded loop asked for a translation", NULL),
    JIT_ATTR("deferred", JA_DEFERRED, "translations the governor put off (jit.budget)", NULL),
    JIT_ATTR("async", JA_ASYNC, "1 = compile off the core thread, install between ticks (default except under Node)",
             jit_attr_set),
    JIT_ATTR("budget", JA_BUDGET, "the share of time, in percent, translating may take (100: no limit)", jit_attr_set),
    JIT_ATTR("regs", JA_REGS, "0 = guest registers in locals per block, 1 = per function", jit_attr_set),
    {.kind = MK_METHOD,
                                                                                                  .name = "reset",
                                                                                                  .doc = "Drop every translation",
                                                                                                  .method = {.args = NULL, .nargs = 0, .result = VK_NONE, .fn = jit_method_reset, .ui_flags = MM_MUTATE}},
};

static const class_desc_t jit_class = {
    .name = "jit",
    .doc = "The PowerPC translation tier: hot code as generated WebAssembly (wasm builds)",
    .members = jit_members,
    .n_members = sizeof(jit_members) / sizeof(jit_members[0]),
};

static struct object *s_jit_object;

void ppc_jit_object_install(void) {
    if (s_jit_object)
        return;
#ifdef __EMSCRIPTEN__ // (no tier to tell about code writes otherwise)
    g_pd_tier_written_hook = ppc_jit_code_written;
    g_pd_tier_reset_hook = ppc_jit_reset;
#endif
    s_jit_object = object_new(&jit_class, NULL, "jit");
    if (s_jit_object) {
        object_set_label(s_jit_object, "JIT");
        object_set_order(s_jit_object, 23);
        object_set_category(s_jit_object, M_CAT_ADVANCED);
        object_attach(object_root(), s_jit_object);
    }
}
