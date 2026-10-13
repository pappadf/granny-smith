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
static uint32_t g_threshold = 16; // entries into a page before it joins its region
static uint32_t g_cooldown = 64; // entries between two compiles of a region (0: none)
static uint32_t g_quiet = 64; // entries after which a region with invalidated blocks is retranslated
static uint32_t g_chunk_cap = 4096; // instructions per translated function (a region may have several)

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

// The predecoded tier's classifier (ppc_run.c): the translator treats as
// translatable exactly the shapes the predecoded loop specializes (T0).
uint16_t ppc_pd_classify_word(uint32_t iw, uint32_t ipc, uint32_t page_lo, pd_entry_t *e);

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
    W_BR_TABLE = 0x0E, W_SELECT = 0x1B, W_LGET = 0x20, W_LSET = 0x21, W_LTEE = 0x22,
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
} U;

// Translation state of the block being emitted
static uint32_t g_nblocks, g_cur_block, g_depth;
static uint32_t g_cur_page; // index in U.pages
static uint32_t g_cur_addr; // the instruction being emitted
static uint32_t g_left_slots; // budget slots of the block from this instruction on
static bool g_block_branch; // the block ends in a branch (it took a fold)
static uint32_t g_reg_mask; // GPRs the region names

// Depth of $loop and $out from the current point (blocks b+1..n-1 are open)
static uint32_t depth_loop(void) {
    return g_nblocks - 1 - g_cur_block + g_depth;
}
static uint32_t depth_out(void) {
    return depth_loop() + 1;
}

static void exit_const(uint32_t npc) {
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
    return U.pages[k].map[(off & MEM_PAGE_MASK) >> 2];
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

// Go to `target`: a block of the region (re-checking the fetch mapping
// when it is on another page), or leave
static void emit_goto(uint32_t target) {
    int k = -1;
    uint16_t b = block_at(target, &k);
    if (b == NONE16) {
        exit_const(target);
        return;
    }
    if ((uint32_t)k != g_cur_page)
        emit_fetch_check(target, U.pages[k].host_adjust);
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
    lget(L_T2);
    lset(L_IDX);
    op(W_BR);
    uleb(depth_loop());
    op(W_END); // $miss: leave with L_NPC
    g_depth--;
    op(W_BR);
    uleb(depth_out());
}

// Leave at the current instruction for the predecoded loop: give back the
// block's slots from here on and its fold, say why.  The locals hold the
// state before this instruction.
static void emit_deopt(void) {
    lget(L_BUDGET);
    iconst(g_left_slots);
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
    exit_const(g_cur_addr);
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
// must leave (misaligned, a 601 T=1 segment, or no SoA entry); leaves the
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
    if (size > 1) {
        lget(L_EA);
        iconst(size - 1);
        op(W_AND);
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

// Store: EA on the stack, value in L_T3
static void emit_store(uint32_t size) {
    lset(L_EA);
    emit_access_test(size, true);
    op(W_IF);
    put(BT_VOID);
    g_depth++;
    emit_deopt();
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
    if (id <= PPD_FIRST || id >= PPD_END || id == PPD_ISYNC)
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
    return id >= PPD_B_IN && id <= PPD_BCTR;
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
        lget(RD(b));
        iconst(c);
        op(W_ADD);
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
        lget(RD(b));
        iconst(c);
        op(W_ADD);
        emit_store(size);
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
    g_reg_mask = 0;
    for (uint32_t k = 0; k < U.n; k++) {
        jt_page_t *pg = &U.pages[k];
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) {
            pg->map[i] = NONE16;
            pg->cov[i] = NONE16;
            uint32_t x = k * JIT_PAGE_INSNS + i;
            g_ids[x] = ppc_pd_classify_word(pg->words[i], pg->lpage + 4 * i, pg->lpage, &g_ents[x]);
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
                g_reg_mask |= entry_regs(g_ids[x], &g_ents[x]);
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

    g_len = 0;
    static const uint8_t hdr[8] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    for (int i = 0; i < 8; i++)
        put(hdr[i]);
    // Type: (i32 i32 i32 i32) -> i32
    uint32_t s = open_section(1);
    uleb(1);
    put(0x60);
    uleb(NPARAMS);
    for (int i = 0; i < NPARAMS; i++)
        put(BT_I32);
    uleb(1);
    put(BT_I32);
    close_sized(s);
    // Import: the memory (shared in the pthreads build)
    s = open_section(2);
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
    close_sized(s);
    s = open_section(3);
    uleb(1);
    uleb(0);
    close_sized(s);
    s = open_section(7);
    uleb(1);
    put_name("f");
    put(0x00);
    uleb(0);
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
    for (int r = 0; r < 32; r++) {
        if (!(g_reg_mask & (1u << r)))
            continue;
        lget(L_P);
        mem(W_LOAD, 2, offsetof(ppc_t, gpr) + 4u * (uint32_t)r);
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
    uleb(nb + 1); // default: $out (never taken)
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
    }
    op(W_END); // $loop
    op(W_END); // $out: L_NPC is the next pc
    for (int r = 0; r < 32; r++) {
        if (!(g_reg_mask & (1u << r)))
            continue;
        lget(L_P);
        lget(GPR(r));
        mem(W_STORE, 2, offsetof(ppc_t, gpr) + 4u * (uint32_t)r);
    }
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
    var inst = new WebAssembly.Instance(mod, { e: { m: wasmMemory } });
    var idx = slot ? slot : table.grow(1);
    table.set(idx, inst.exports.f);
    return idx;
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
// Chunks are whole blocks in address order; a branch into another chunk
// leaves, and the dispatcher enters that chunk.
#define JIT_MAX_CHUNKS 32
#define NONE8          0xFFu

// A member page of a region: what was translated and from where
typedef struct jit_slot {
    uint32_t lpage;
    uint8_t *host; // the host page (the coherence hook's key)
    uint32_t host_adjust; // the fetch mapping (host - lpage)
    uint32_t words[JIT_PAGE_INSNS];
    uint8_t extra[JIT_PAGE_INSNS]; // entry points seen at run time
    uint8_t owner[JIT_PAGE_INSNS]; // per word: the chunk that translated it (NONE8)
    uint16_t cov[JIT_PAGE_INSNS]; // per word: its block in that chunk (NONE16)
    struct jit_region *region;
    struct jit_slot *next_host; // the host-page chain
} jit_slot_t;

typedef struct jit_chunk {
    uint32_t fn; // the function (table index), 0 = none
    jt_ref_t refs[JIT_REGION_PAGES]; // read by the function: its maps of the member pages
    uint16_t *maps[JIT_REGION_PAGES]; // per member page: block per leader word (NONE16)
    uint8_t *bvalid; // per block, 1 = valid
} jit_chunk_t;

typedef struct jit_region {
    uint32_t key; // logical base | MSR[IT] << 1 | MSR[PR]
    jit_slot_t *slot[JIT_REGION_PAGES];
    jit_chunk_t chunk[JIT_MAX_CHUNKS];
    uint32_t nchunks;
    uint32_t heat[JIT_REGION_PAGES]; // entries seen on pages that are not (validly) members
    uint32_t since; // entries since the last compile
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

// Counters (the `jit` node)
static struct {
    uint64_t compiles, functions, bytes, calls, deopts, block_invals, regions;
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
        memset(ch->refs, 0, sizeof ch->refs);
    }
    R->nchunks = 0;
    for (uint32_t k = 0; k < JIT_REGION_PAGES; k++)
        if (R->slot[k])
            memset(R->slot[k]->owner, NONE8, sizeof R->slot[k]->owner);
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
    g_mapgen = g_mem_map_generation;
}

static jit_region_t *region_get(uint32_t key) {
    uint32_t h = (key >> JIT_REGION_SHIFT) * 31u % JIT_HASH;
    for (jit_region_t *r = g_regions[h]; r; r = r->next)
        if (r->key == key)
            return r;
    jit_region_t *r = calloc(1, sizeof *r);
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
static void compile_region(jit_region_t *R, ppc_t *p) {
    static jt_page_t pages[JIT_REGION_PAGES];
    static uint32_t slot_page[JIT_REGION_PAGES]; // unit index -> region page
    static uint8_t leader[JIT_REGION_PAGES][JIT_PAGE_INSNS];
    static uint8_t reached[JIT_REGION_PAGES][JIT_PAGE_INSNS];
    static uint8_t cleader[JIT_REGION_PAGES][JIT_PAGE_INSNS]; // one chunk's leaders
    static uint16_t ccov[JIT_REGION_PAGES][JIT_PAGE_INSNS]; // one chunk's coverage
    static uint32_t stack[2 * JIT_REGION_PAGES * JIT_PAGE_INSNS];
    double t0 = emscripten_get_now();
    uint32_t lbase = R->key & ~(uint32_t)((1u << JIT_REGION_SHIFT) - 1);
    region_drop_code(R);
    R->since = 0;
    R->stale = 0;
    R->want = false;
    uint32_t n = 0;
    for (uint32_t k = 0; k < JIT_REGION_PAGES; k++) {
        U.slot[k] = -1;
        jit_slot_t *s = R->slot[k];
        if (!s)
            continue;
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) { // the page as it is now
            s->words[i] = LOAD_BE32(s->host + 4 * i);
            s->cov[i] = NONE16;
        }
        memset(leader[n], 0, JIT_PAGE_INSNS);
        memset(reached[n], 0, JIT_PAGE_INSNS);
        U.slot[k] = (int16_t)n;
        slot_page[n] = k;
        n++;
    }
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
            if (s->extra[i])
                PUSH(s->lpage + 4 * i);
    }
    while (sp) {
        uint32_t x = stack[--sp], u = x / JIT_PAGE_INSNS, i = x % JIT_PAGE_INSNS;
        jit_slot_t *s = R->slot[slot_page[u]];
        for (; i < JIT_PAGE_INSNS; i++) {
            if (reached[u][i])
                break;
            reached[u][i] = 1;
            uint32_t w = s->words[i], a = s->lpage + 4 * i;
            pd_entry_t e;
            uint16_t id = ppc_pd_classify_word(w, a, s->lpage, &e);
            if (!translatable(id, w, a)) {
                PUSH(a + 4); // the predecoded loop runs it, then re-enters after it
                break;
            }
            if (is_branch_id(id)) {
                if (id != PPD_BLR && id != PPD_BCTR)
                    PUSH(branch_target(id, w, a));
                if ((id != PPD_B_IN && id != PPD_B_OUT && id != PPD_BLR && id != PPD_BCTR) || id == PPD_BL_IN ||
                    id == PPD_BL_OUT)
                    PUSH(a + 4); // a conditional branch falls through; a call returns here
                break;
            }
        }
        if (i == JIT_PAGE_INSNS)
            PUSH(s->lpage + MEM_PAGE_SIZE); // ran off the page
    }
#undef PUSH
    // Chunks: whole blocks in address order, up to the cap each
    uint32_t c = 0, count = 0;
    bool any = false;
    for (uint32_t u = 0; u < n; u++) {
        jit_slot_t *s = R->slot[slot_page[u]];
        for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) {
            if (!reached[u][i])
                continue;
            if (leader[u][i] && count >= g_chunk_cap && c + 1 < JIT_MAX_CHUNKS) {
                c++;
                count = 0;
            }
            s->owner[i] = (uint8_t)c;
            count++;
            any = true;
        }
    }
    U.lbase = lbase;
    U.it = (R->key & 2u) != 0;
    U.pr = (R->key & 1u) != 0;
    U.is601 = !ppc_is_604(p);
    U.shared = jit_memory_shared() != 0;
    U.pages = pages;
    U.n = n;
    R->nchunks = any ? c + 1 : 0;
    for (uint32_t ci = 0; ci < R->nchunks; ci++) {
        jit_chunk_t *ch = &R->chunk[ci];
        for (uint32_t u = 0; u < n; u++) {
            uint32_t k = slot_page[u];
            jit_slot_t *s = R->slot[k];
            for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++) {
                // 1: leads a block of this chunk; 2: leads another chunk's (a block stops there)
                cleader[u][i] = (leader[u][i] && reached[u][i]) ? (s->owner[i] == ci ? 1 : 2) : 0;
                ccov[u][i] = NONE16;
            }
            if (!ch->maps[k])
                ch->maps[k] = malloc(JIT_PAGE_INSNS * sizeof(uint16_t));
            if (!ch->maps[k])
                return;
            pages[u] = (jt_page_t){s->lpage, s->words, cleader[u], ch->maps[k], ccov[u], s->host_adjust};
        }
        if (!ch->bvalid)
            ch->bvalid = malloc(JIT_REGION_PAGES * JIT_PAGE_INSNS + 1);
        if (!ch->bvalid)
            return;
        memset(ch->bvalid, 1, (size_t)n * JIT_PAGE_INSNS + 1);
        U.refs = ch->refs;
        U.bvalid = ch->bvalid;
        uint32_t nb = translate();
        if (!nb)
            continue;
        ch->fn = jit_instantiate(g_buf, g_len, g_nfree ? g_free_slots[--g_nfree] : 0);
        // The lazy-compile mitigation (Phase 0): one call with a zero budget
        // has the engine compile the function now, not at its first real
        // entry.  Every block's guard leaves at once on a scratch state.
        static ppc_t scratch;
        scratch.msr = p->msr;
        ((jit_fn_t)(uintptr_t)ch->fn)(&scratch, 0, 0, 0);
        g_st.functions++;
        g_st.bytes += g_len;
        for (uint32_t u = 0; u < n; u++) {
            uint32_t k = slot_page[u];
            ch->refs[k].map = addr_of(ch->maps[k]);
            ch->refs[k].host_adjust = R->slot[k]->host_adjust;
            for (uint32_t i = 0; i < JIT_PAGE_INSNS; i++)
                if (ccov[u][i] != NONE16)
                    R->slot[k]->cov[i] = ccov[u][i];
        }
    }
    for (uint32_t u = 0; u < n; u++)
        memory_code_page_mark_owner(R->slot[slot_page[u]]->host, MEM_CODE_JIT);
    g_st.compiles++;
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

bool ppc_jit_enter(ppc_t *p, uint32_t *instructions, uint64_t *folds_left, int *exit) {
    uint32_t msr = p->msr;
    // Not translated: little-endian mode; supervisor data translation (every
    // access takes the slow path there); a deliverable interrupt (the
    // predecoded loop takes it first); the sprint's tail
    if ((msr & PPC_MSR_LE) || (msr & (PPC_MSR_DT | PPC_MSR_PR)) == PPC_MSR_DT)
        return false;
    if ((p->ext_irq | p->dec_pending) && (msr & PPC_MSR_EE))
        return false;
    uint32_t budget = *instructions;
    if (budget < 3)
        return false;
    if (!g_inited) {
        g_inited = true;
        g_mapgen = g_mem_map_generation;
    }
    if (g_mapgen != g_mem_map_generation || g_model != (uint32_t)p->cpu_model) {
        ppc_jit_reset(); // real-mode translations follow the memory map; ids follow the model
        g_model = (uint32_t)p->cpu_model;
    }
    uint32_t pc = p->pc;
    uint32_t key = (pc & ~(uint32_t)((1u << JIT_REGION_SHIFT) - 1)) | ((msr & PPC_MSR_IT) ? 2u : 0u) |
                   ((msr & PPC_MSR_PR) ? 1u : 0u);
    jit_region_t *R = region_get(key);
    if (!R)
        return false;
    uint32_t k = (pc >> MEM_PAGE_SHIFT) & (JIT_REGION_PAGES - 1);
    uint32_t idx = (pc & MEM_PAGE_MASK) >> 2;
    uint32_t host_adjust = (uint32_t)g_ppc_fetch.host_adjust;
    jit_slot_t *s = R->slot[k];
    R->since++;
    bool member = s && s->host_adjust == host_adjust;
    jit_chunk_t *ch = NULL;
    uint16_t b = NONE16;
    if (!member || !region_entry(R, s, k, idx, &ch, &b)) {
        // Not translated here (yet): count it, and translate when the page
        // is hot and the region has not just been compiled
        if (!member && ++R->heat[k] < g_threshold)
            return false;
        if (member && !s->extra[idx]) {
            s->extra[idx] = 1; // a new entry point
            R->want = true;
        }
        if (!member) {
            s = slot_make(R, k, pc & ~(uint32_t)MEM_PAGE_MASK, host_adjust);
            if (!s)
                return false;
            R->heat[k] = 0;
            s->extra[idx] = 1;
            R->want = true;
        }
        bool due = (R->want && R->since >= g_cooldown) || (R->stale && R->since >= g_quiet) || !R->nchunks;
        if (!due)
            return false;
        compile_region(R, p);
        if (!region_entry(R, s, k, idx, &ch, &b))
            return false;
    }
    // Run it (the predecoded block being left is charged first, as at any
    // page transition)
    predecode_enter(NULL, budget);
    int32_t fin = *folds_left > 0x7FFFFFFFu ? 0x7FFFFFFF : (int32_t)*folds_left;
    g_ppc_jit_exit = PPC_JIT_EXIT_PC;
    g_ppc_jit_folds = fin;
    int32_t left = ((jit_fn_t)(uintptr_t)ch->fn)(p, (int32_t)budget, (int32_t)b, fin);
    *instructions = (uint32_t)left;
    *folds_left -= (uint64_t)(uint32_t)(fin - g_ppc_jit_folds);
    *exit = (int)g_ppc_jit_exit;
    g_st.calls++;
    if (*exit == PPC_JIT_EXIT_DEOPT)
        g_st.deopts++;
    return true;
}

// The coherence hook: invalidate the blocks that translated the written words
void ppc_jit_code_written(const uint8_t *host, uint32_t len) {
    if (!len)
        return;
    const uint8_t *page = (const uint8_t *)((uintptr_t)host & ~(uintptr_t)MEM_PAGE_MASK);
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
            if (c == NONE8 || b == NONE16 || c >= R->nchunks || !R->chunk[c].bvalid)
                continue;
            if (R->chunk[c].bvalid[b]) {
                R->chunk[c].bvalid[b] = 0;
                R->stale++;
                g_st.block_invals++;
                hit = true;
            }
        }
        if (hit)
            R->since = 0; // retranslate only once the writes have quieted down
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
    JA_BYTES,
    JA_CALLS,
    JA_DEOPTS,
    JA_BLOCK_INVALS,
    JA_COMPILE_US
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
    JIT_ATTR("threshold", JA_THRESHOLD, "entries into a page before it joins its region's translation", jit_attr_set),
    JIT_ATTR("cooldown", JA_COOLDOWN, "entries between two translations of one region (0: none)", jit_attr_set),
    JIT_ATTR("quiet", JA_QUIET, "entries after a code write before its region is retranslated", jit_attr_set),
    JIT_ATTR("regions", JA_REGIONS, "regions created", NULL),
    JIT_ATTR("compiles", JA_COMPILES, "region translations", NULL),
    JIT_ATTR("functions", JA_FUNCTIONS, "functions generated (a region translates into chunks of chunk_cap)", NULL),
    JIT_ATTR("chunk_cap", JA_CHUNK_CAP, "the most instructions one generated function translates", jit_attr_set),
    JIT_ATTR("bytes", JA_BYTES, "bytes of WebAssembly generated", NULL),
    JIT_ATTR("calls", JA_CALLS, "entries into translated code", NULL),
    JIT_ATTR("deopts", JA_DEOPTS, "exits at an access that missed the memory fast path", NULL),
    JIT_ATTR("block_invals", JA_BLOCK_INVALS, "translated blocks invalidated by code writes", NULL),
    JIT_ATTR("compile_us", JA_COMPILE_US, "time spent translating and compiling, microseconds", NULL),
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
    g_pd_tier_written_hook = ppc_jit_code_written;
    g_pd_tier_reset_hook = ppc_jit_reset;
    s_jit_object = object_new(&jit_class, NULL, "jit");
    if (s_jit_object) {
        object_set_label(s_jit_object, "JIT");
        object_set_order(s_jit_object, 23);
        object_set_category(s_jit_object, M_CAT_ADVANCED);
        object_attach(object_root(), s_jit_object);
    }
}
