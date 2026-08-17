// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_mmu.c
// The 601 MMU front end (Phase D, proposal-powerpc-601-pdm.md §3.5):
// T=1 I/O-controller segments, BAT match, and the hashed page table
// search, with three software caches in front of the full walk:
//
//   1. the user SoA fast path — with MSR[PR]=1 and MSR[DT]=1 the active
//      maps are g_user_read/write, which hold LOGICAL page fills made
//      here (the supervisor arrays keep the machine's eager physical
//      identity view, so exception entry/rfi cost nothing);
//   2. a small direct-mapped translation TLB serving supervisor
//      translated accesses (the nanokernel's MemRetry paths) and user
//      pages that cannot be SoA-filled (device I/O);
//   3. a fetch window + fetch TLB caching instruction-page host pointers.
//
// All cache state lives in file statics, NOT in ppc_t: entries embed host
// pointers and are derived state — putting them in the checkpointed blob
// would break checkpoint byte-determinism.  Everything here rebuilds
// lazily after restore/invalidation.
//
// 601-specific rules implemented here (Motorola/IBM, "PowerPC 601 RISC
// Microprocessor User's Manual", 1995 — "601UM"):
//   - a segment with T=1 PREVAILS over any BAT match (Table 6-10), and
//     data accesses consult SR[T] independent of MSR[DT] (§6.5.2) — this
//     is how HWInit reaches RAM and I/O "untranslated";
//   - the 601 BAT layout (Tables 6-11/6-12): V+BSM in the LOWER register,
//     WIM/Ks/Ku/PP in the upper; Ks/Ku are protection keys (key = Ks
//     supervisor / Ku user, §6.4), never match qualifiers;
//   - R is set even when protection denies the access (§6.8.4); C only
//     on permitted stores — and the nanokernel harvests C from evicted
//     PTEs, so the in-RAM write-back is load-bearing;
//   - ISI SRR1: HTAB miss sets bits 1 AND 10 ($40200000 — the mask the
//     nanokernel's InstStorageInt tests), protection sets bit 4
//     ($08000000), a non-memory-forced T=1 fetch sets NO bits (Table 6-3
//     footnote);
//   - tlbie invalidates the congruence class selected by EA[13-19] —
//     128 classes (Figure 6-15) — and the kernel's FlushTLB loop relies
//     on exactly that granularity;
//   - the $00A00 I/O controller error resumes at the FOLLOWING
//     instruction with DSISR unchanged (Table 5-21).

#include "ppc_internal.h"

#include "log.h"

LOG_USE_CATEGORY_NAME("ppcmmu");

// ============================================================
// Cache state (file statics; see header comment for why)
// ============================================================

// Direct-mapped translation TLB for the slow (non-SoA) paths.  Tag packs
// the EA page with the privilege the entry was created under; `w_ok`
// requires that a store through the entry needs no PTE side effects
// (protection allows it AND the C bit is already set).
#define XTLB_SIZE 256
typedef struct {
    uint32_t tag; // (ea & 0xFFFFF000) | user<<1 | valid
    uint32_t pa_page; // physical page base
    uint32_t w_ok; // store permitted with no walk side effects
} xtlb_entry_t;
static xtlb_entry_t g_xtlb[XTLB_SIZE];

// Fetch cache: a one-page window (checked inline in ppc_run.c via the
// extern below) backed by a small direct-mapped TLB of page→host maps.
ppc_fetch_window_t g_ppc_fetch; // the inline-checked window
#define FTLB_SIZE 64
typedef struct {
    uint32_t tag; // (pc & 0xFFFFF000) | user<<1 | valid
    uintptr_t host_adjust; // host_base - page_base
} ftlb_entry_t;
static ftlb_entry_t g_ftlb[FTLB_SIZE];

// Pages filled into the user SoA arrays since the last full invalidation.
// Same shape as mmu.c's tracker but user-arrays-only: on PDM the
// supervisor arrays hold the machine's eager identity map and must
// survive MMU invalidations.
#define PPC_FILL_TRACK_MAX 16384
static uint32_t g_fill_track[PPC_FILL_TRACK_MAX];
static int g_fill_track_count;
static bool g_fill_track_overflow = true; // conservative until first inval

// tlbie congruence class: EA[13-19] = 128 classes (601UM Figure 6-15).
#define TLBIE_CLASS_MASK 127u

// ============================================================
// Invalidation
// ============================================================

void ppc_mmu_flush_fetch(void) {
    g_ppc_fetch.span = 0;
    memset(g_ftlb, 0, sizeof(g_ftlb));
}

// Zero every tracked user-SoA fill (or everything on tracker overflow).
static void user_soa_invalidate_all(void) {
    if (g_fill_track_overflow) {
        size_t sz = (size_t)g_page_count * sizeof(uintptr_t);
        if (g_user_read)
            memset(g_user_read, 0, sz);
        if (g_user_write)
            memset(g_user_write, 0, sz);
    } else {
        for (int i = 0; i < g_fill_track_count; i++) {
            uint32_t pg = g_fill_track[i];
            if (pg >= g_page_count)
                continue;
            g_user_read[pg] = 0;
            g_user_write[pg] = 0;
        }
    }
    g_fill_track_count = 0;
    g_fill_track_overflow = false;
}

// Full invalidation: context change (mtsr/BAT/SDR1 value change, HMC
// bank remap, checkpoint restore).
void ppc_mmu_invalidate_all(ppc_t *p) {
    (void)p;
    user_soa_invalidate_all();
    memset(g_xtlb, 0, sizeof(g_xtlb));
    ppc_mmu_flush_fetch();
}

// tlbie: invalidate the EA's congruence class — every cached translation
// whose page index matches modulo 128.  Over-invalidation relative to
// the hardware's two-way class is fine; under-invalidation would break
// the kernel's FlushTLB loop (one tlbie per class over 1 MB).
void ppc_mmu_tlbie(ppc_t *p, uint32_t ea) {
    (void)p;
    uint32_t cls = (ea >> PAGE_SHIFT) & TLBIE_CLASS_MASK;
    if (g_fill_track_overflow) {
        user_soa_invalidate_all();
    } else {
        for (int i = 0; i < g_fill_track_count; i++) {
            uint32_t pg = g_fill_track[i];
            if ((pg & TLBIE_CLASS_MASK) != cls || pg >= g_page_count)
                continue;
            g_user_read[pg] = 0;
            g_user_write[pg] = 0;
        }
    }
    for (int i = 0; i < XTLB_SIZE; i++)
        if (((g_xtlb[i].tag >> PAGE_SHIFT) & TLBIE_CLASS_MASK) == cls)
            g_xtlb[i].tag = 0;
    for (int i = 0; i < FTLB_SIZE; i++)
        if (((g_ftlb[i].tag >> PAGE_SHIFT) & TLBIE_CLASS_MASK) == cls)
            g_ftlb[i].tag = 0;
    g_ppc_fetch.span = 0;
}

// Recompute the which-segments-have-T=1 mask consulted by the inline
// translation-off fast path (data accesses reach T=1 segments even with
// MSR[DT]=0 — 601UM §6.5.2).
void ppc_recompute_sr_t_mask(ppc_t *p) {
    uint32_t m = 0;
    for (int i = 0; i < 16; i++)
        if (p->sr[i] & 0x80000000u)
            m |= 1u << i;
    p->sr_t_mask = m;
}

// Segment-register write with the invalidation the 601 requires; a
// rewrite of the same value is free — the nanokernel reloads all 16 SRs
// with identical values on every address-space touch (SetSpace).
void ppc_set_sr(ppc_t *p, uint32_t n, uint32_t v) {
    if (p->sr[n] == v)
        return;
    p->sr[n] = v;
    ppc_recompute_sr_t_mask(p);
    ppc_mmu_invalidate_all(p);
}

// ============================================================
// The translation core
// ============================================================

// Result of one translation attempt.
typedef enum {
    XL_OK = 0, // *pa valid
    XL_NOTFOUND, // no BAT match and no PTE
    XL_PROT, // key/PP violation
    XL_IOSEG, // T=1 segment, BUID != $07F
} xl_result_t;

// Attributes of a successful translation.
typedef struct {
    uint32_t pa;
    uint32_t wimg; // W/I/M (+G always 0 on 601) of the mapping
    bool w_ok; // store permitted with no further walk side effects
} xl_out_t;

// PP/key evaluation (601UM Table 6-7).
// key=0: PP 00/01/10 rw, 11 ro.  key=1: 00 none, 01 ro, 10 rw, 11 ro.
static inline bool pp_allows(uint32_t key, uint32_t pp, bool store) {
    if (!key)
        return !(store && pp == 3u);
    if (pp == 2u)
        return true;
    return !store && pp != 0u;
}

// 601 BAT match + protection (601UM §6.7, Tables 6-11/6-12): BATU =
// BLPI[0:14] | WIM[25:27] | Ks[28] | Ku[29] | PP[30:31]; BATL =
// PBN[0:14] | V[25] | BSM[26:31].  Returns true when a BAT matched
// (result in *res); protection key = Ks supervisor / Ku user.
static bool bat_xlate(ppc_t *p, uint32_t ea, bool user, bool store, xl_out_t *out, xl_result_t *res) {
    for (int i = 0; i < 4; i++) {
        uint32_t bl = p->batl[i];
        if (!(bl & 0x40u))
            continue; // V
        uint32_t cmp_mask = ~(((bl & 0x3Fu) << 17) | 0x1FFFFu);
        if ((ea & cmp_mask) != (p->batu[i] & cmp_mask))
            continue;
        uint32_t bu = p->batu[i];
        uint32_t key = user ? ((bu >> 2) & 1u) : ((bu >> 3) & 1u);
        uint32_t pp = bu & 3u;
        if (!pp_allows(key, pp, store)) {
            *res = XL_PROT;
            return true;
        }
        out->pa = ((bl & 0xFFFE0000u) & cmp_mask) | (ea & ~cmp_mask);
        out->wimg = (bu >> 4) & 7u;
        out->w_ok = pp_allows(key, pp, true); // no C bit on BAT mappings
        *res = XL_OK;
        return true;
    }
    return false;
}

// Resolve physical RAM/ROM to a host pointer during the table search.
// The PDM identity page table is the physical resolver of record (it
// tracks HMC bank moves); HTAB reads must not touch the mode-dependent
// SoA maps.
static inline uint8_t *phys_host(uint32_t pa) {
    uint32_t pg = pa >> PAGE_SHIFT;
    if (pg >= g_page_count)
        return NULL;
    uint8_t *host = g_page_table[pg].host_base;
    return host ? host + (pa & PAGE_MASK) : NULL;
}

// Hashed page table search (601UM §6.9): SDR1 = HTABORG[0:15] |
// HTABMASK[23:31]; hash1 = low 19 VSID bits XOR page index; PTEG PA =
// ORG[0:6] || (ORG[7:15] | (hash[0:8] & MASK)) || hash[9:18] || 000000.
// On a match R is set unconditionally (even on protection violation,
// §6.8.4) and C on permitted stores, written back to the in-RAM PTE.
static xl_result_t htab_search(ppc_t *p, uint32_t ea, uint32_t sr, bool user, bool store, bool nosideffect,
                               xl_out_t *out) {
    uint32_t vsid = sr & 0x00FFFFFFu;
    uint32_t page_idx = (ea >> 12) & 0xFFFFu;
    uint32_t api = (ea >> 22) & 0x3Fu;
    uint32_t htaborg = p->sdr1 & 0xFFFF0000u;
    uint32_t htabmask = p->sdr1 & 0x1FFu;
    uint32_t hash = (vsid & 0x7FFFFu) ^ page_idx;
    // PTE word 0 to match: V | VSID[1:24] | H | API[26:31]
    uint32_t match = 0x80000000u | (vsid << 7) | api;

    for (int h = 0; h < 2; h++) {
        uint32_t hs = (h ? ~hash : hash) & 0x7FFFFu;
        uint32_t pteg = htaborg | ((((hs >> 10) & 0x1FFu) & htabmask) << 16) | ((hs & 0x3FFu) << 6);
        uint8_t *pte = phys_host(pteg);
        if (!pte)
            continue; // PTEG outside RAM: nothing to find there
        uint32_t want = match | (h ? 0x40u : 0u);
        for (int s = 0; s < 8; s++, pte += 8) {
            if (LOAD_BE32(pte) != want)
                continue;
            uint32_t lo = LOAD_BE32(pte + 4);
            uint32_t key = user ? ((sr >> 29) & 1u) : ((sr >> 30) & 1u);
            bool allowed = pp_allows(key, lo & 3u, store);
            // R set even when protection denies (601UM §6.8.4); C only
            // when the store is permitted.  Suppressed for the
            // side-effect-free debug translate.
            if (!nosideffect) {
                uint32_t nlo = lo | 0x100u | ((store && allowed) ? 0x80u : 0u);
                if (nlo != lo)
                    STORE_BE32(pte + 4, nlo);
                lo = nlo;
            }
            if (!allowed)
                return XL_PROT;
            out->pa = (lo & 0xFFFFF000u) | (ea & 0xFFFu);
            out->wimg = (lo >> 4) & 7u;
            // Stores through a cached entry must not skip the C update:
            // w_ok only once C is set and protection allows writes.
            out->w_ok = (lo & 0x80u) != 0 && pp_allows(key, lo & 3u, true);
            return XL_OK;
        }
    }
    return XL_NOTFOUND;
}

// Full translation for one access.  601 order (Figure 6-4, Table 6-10):
// the segment's T bit decides first — T=1 prevails over any BAT match —
// then BAT, then the hashed table.  `translation_on` reflects MSR[DT]
// (or MSR[IT] for fetches): with it off only T=1 segments translate.
static xl_result_t xlate(ppc_t *p, uint32_t ea, bool user, bool store, bool translation_on, bool nosideffect,
                         xl_out_t *out) {
    uint32_t sr = p->sr[ea >> 28];
    if (sr & 0x80000000u) { // T=1: I/O controller interface segment
        if (((sr >> 20) & 0x1FFu) == 0x07Fu) { // memory-forced BUID
            out->pa = ((sr & 0xFu) << 28) | (ea & 0x0FFFFFFFu);
            out->wimg = 0x3u; // WIM assumed 011 (§6.10.4)
            out->w_ok = true; // bypasses all protection
            return XL_OK;
        }
        return XL_IOSEG;
    }
    if (!translation_on) { // direct translation: EA = PA, no protection
        out->pa = ea;
        out->wimg = 1u; // WIM = 001 (§6.6)
        out->w_ok = true;
        return XL_OK;
    }
    xl_result_t res;
    if (bat_xlate(p, ea, user, store, out, &res))
        return res;
    return htab_search(p, ea, sr, user, store, nosideffect, out);
}

// ============================================================
// SoA fill (user translated fast path)
// ============================================================

// Try to give the user SoA arrays a logical fill for ea→pa.  Mirrors
// mmu_fill_soa_page's rules (host-backed only, logpoint suppression,
// tracking) but touches ONLY the user arrays.  `write_ok` is the walk's
// w_ok — protection allows stores and no C update remains owed.
static void user_soa_fill(uint32_t ea, uint32_t pa, bool write_ok) {
    uint32_t lpage = ea >> PAGE_SHIFT;
    uint32_t ppage = pa >> PAGE_SHIFT;
    if (lpage >= g_page_count || ppage >= g_page_count)
        return;
    page_entry_t *pe = &g_page_table[ppage];
    if (!pe->host_base || pe->dev)
        return; // not plain host memory
    if (g_mem_logpoint_page_count && g_mem_logpoint_page_count[lpage])
        return; // logical logpoint: stay slow
    if (g_mem_logpoint_phys_page_count && g_mem_logpoint_phys_page_count[ppage])
        return; // physical logpoint: stay slow
    if (g_fill_track_count >= PPC_FILL_TRACK_MAX)
        g_fill_track_overflow = true;
    else
        g_fill_track[g_fill_track_count++] = lpage;
    uintptr_t adjusted = (uintptr_t)pe->host_base - (lpage << PAGE_SHIFT);
    g_user_read[lpage] = adjusted;
    if (write_ok && pe->writable)
        g_user_write[lpage] = adjusted;
}

// ============================================================
// Data-access entry point (ppc_dxlate slow half)
// ============================================================

// DSI delivery (601UM Table 5-10: bit 1 not-found, bit 4 protection,
// bit 5 atomics-to-T=1, bit 6 store).
static void raise_dsi(ppc_t *p, uint32_t ea, uint32_t dsisr) {
    p->dar = ea;
    p->dsisr = dsisr;
    LOG(4, "DSI: ea $%08X dsisr $%08X pc $%08X r24 $%08X", ea, dsisr, p->instruction_pc, p->gpr[24]);
    ppc_exception(p, PPC_VEC_DSI, 0, p->instruction_pc);
}

// True when iw is lwarx/stwcx./lscbx — the three instructions that take
// a DSI (DSISR bit 5) instead of completing in a T=1 segment.
static bool iw_is_atomic_class(uint32_t iw) {
    if (PPC_OPCD(iw) != 31)
        return false;
    uint32_t xo = PPC_XO10(iw);
    return xo == 20 || xo == 150 || xo == 277;
}

// True when iw is a floating-point load/store (alignment exception in a
// non-memory-forced T=1 segment, 601UM §5.4.10).
static bool iw_is_fp_ls(uint32_t iw) {
    uint32_t op = PPC_OPCD(iw);
    if (op >= 48 && op <= 55)
        return true;
    if (op != 31)
        return false;
    uint32_t xo = PPC_XO10(iw);
    return xo == 535 || xo == 567 || xo == 599 || xo == 631 || xo == 663 || xo == 695 || xo == 727 || xo == 759;
}

// See ppc_internal.h for the contract: on return false *addr is the
// address to access — the EA itself when the user SoA now covers it, the
// physical address otherwise.  True = exception raised, abandon.
bool ppc_dxlate_slow(ppc_t *p, uint32_t iw, uint32_t *addr, bool store) {
    uint32_t ea = *addr;
    bool user = (p->msr & PPC_MSR_PR) != 0;
    bool dt = (p->msr & PPC_MSR_DT) != 0;

    // Translation TLB (serves supervisor accesses and user pages that
    // could not be SoA-filled).
    uint32_t tag = (ea & 0xFFFFF000u) | (user ? 2u : 0u) | 1u;
    xtlb_entry_t *te = &g_xtlb[(ea >> PAGE_SHIFT) & (XTLB_SIZE - 1)];
    if (te->tag == tag && (!store || te->w_ok)) {
        *addr = te->pa_page | (ea & 0xFFFu);
        return false;
    }

    xl_out_t out;
    xl_result_t res = xlate(p, ea, user, store, dt, false, &out);
    if (res == XL_IOSEG) {
        // Non-memory-forced T=1: atomics take a DSI (DSISR bit 5), FP
        // load/stores the alignment exception; everything else models
        // the failed bus reply as the 601-only $00A00 I/O controller
        // error — SRR0 = FOLLOWING instruction, DSISR unchanged
        // (601UM Table 5-21).  No I/O controller exists on PDM.
        if (iw_is_atomic_class(iw)) {
            raise_dsi(p, ea, 0x04000000u | (store ? PPC_DSISR_STORE : 0));
            return true;
        }
        if (iw_is_fp_ls(iw)) {
            ppc_align_exception(p, iw, ea);
            return true;
        }
        p->dar = ea;
        ppc_exception(p, PPC_VEC_IOERROR, 0, p->pc);
        return true;
    }
    if (res == XL_PROT) {
        raise_dsi(p, ea, PPC_DSISR_PROT | (store ? PPC_DSISR_STORE : 0));
        return true;
    }
    if (res != XL_OK) {
        raise_dsi(p, ea, PPC_DSISR_NOTFOUND | (store ? PPC_DSISR_STORE : 0));
        return true;
    }

    if (user && dt) {
        // Prefer a logical SoA fill so the page goes fast-path.
        user_soa_fill(ea, out.pa, out.w_ok);
        uintptr_t filled = (store ? g_user_write : g_user_read)[ea >> PAGE_SHIFT];
        if (filled) {
            *addr = ea;
            return false;
        }
    }
    // Not SoA-fillable (device page, logpointed, supervisor mode):
    // cache in the translation TLB and access physically.
    te->tag = tag;
    te->pa_page = out.pa & 0xFFFFF000u;
    te->w_ok = out.w_ok ? 1u : 0u;
    *addr = out.pa;
    return false;
}

// dcbz's translated form (601UM Table 6-3): a W=1 or I=1 mapping takes
// the alignment exception (R already updated by the walk); a
// non-memory-forced T=1 segment makes it a no-op.  The framebuffer is
// mapped write-through, so the W case is guest-visible, not a corner.
// Returns: 0 = proceed (zero at *addr), 1 = exception raised, 2 = no-op.
int ppc_dxlate_dcbz(ppc_t *p, uint32_t iw, uint32_t *addr) {
    uint32_t ea = *addr;
    bool dt = (p->msr & PPC_MSR_DT) != 0;
    if (!dt && !(p->sr_t_mask & (1u << (ea >> 28))))
        return 0; // direct translation: plain zeroing
    bool user = (p->msr & PPC_MSR_PR) != 0;
    uint32_t sr = p->sr[ea >> 28];
    if ((sr & 0x80000000u) && ((sr >> 20) & 0x1FFu) != 0x07Fu)
        return 2; // T=1 I/O controller: no-op (§6.10.6)

    xl_out_t out;
    xl_result_t res = xlate(p, ea, user, true, dt, false, &out);
    if (res == XL_PROT) {
        raise_dsi(p, ea, PPC_DSISR_PROT | PPC_DSISR_STORE);
        return 1;
    }
    if (res != XL_OK) {
        raise_dsi(p, ea, PPC_DSISR_NOTFOUND | PPC_DSISR_STORE);
        return 1;
    }
    // Memory-forced T=1 zeroes real memory (WIM 011 describes the bus
    // attributes; HWInit's RAM work runs through these segments) — the
    // W/I alignment rule applies to BAT/page mappings only.
    if (!(sr & 0x80000000u) && (out.wimg & 0x6u)) { // W or I
        ppc_align_exception(p, iw, ea);
        return 1;
    }
    if (user && dt) {
        user_soa_fill(ea, out.pa, out.w_ok);
        if (g_user_write[ea >> PAGE_SHIFT]) {
            *addr = ea;
            return 0;
        }
    }
    *addr = out.pa;
    return 0;
}

// ============================================================
// Instruction fetch
// ============================================================

// Refill the fetch window for pc and deliver the word there via *iw.
// MSR[IT] is checked BEFORE the segment's T bit (§6.8.2.2): with IT off
// this is the identity map read through the physical page table (never
// the mode-dependent SoA arrays).  Returns false when ISI was raised.
bool ppc_fetch_fill(ppc_t *p, uint32_t pc, uint32_t *iw) {
    uint32_t page = pc & ~(uint32_t)PAGE_MASK;
    bool user = (p->msr & PPC_MSR_PR) != 0;
    uint32_t pa = pc;

    if (p->msr & PPC_MSR_IT) {
        uint32_t tag = page | (user ? 2u : 0u) | 1u;
        ftlb_entry_t *fe = &g_ftlb[(pc >> PAGE_SHIFT) & (FTLB_SIZE - 1)];
        if (fe->tag == tag) {
            g_ppc_fetch.lo = page;
            g_ppc_fetch.span = MEM_PAGE_SIZE;
            g_ppc_fetch.host_adjust = fe->host_adjust;
            *iw = LOAD_BE32((uint8_t *)(fe->host_adjust + pc));
            return true;
        }
        xl_out_t out;
        xl_result_t res = xlate(p, pc, user, false, true, false, &out);
        if (res == XL_IOSEG) {
            // T=1 fetch: ISI with NO SRR1 status bits (601 quirk,
            // Table 6-3 footnote).
            ppc_exception(p, PPC_VEC_ISI, 0, pc);
            return false;
        }
        if (res == XL_PROT) {
            ppc_exception(p, PPC_VEC_ISI, 0x08000000u, pc);
            return false;
        }
        if (res != XL_OK) {
            // HTAB miss: SRR1 bits 1 and 10 (Table 5-11) — the
            // nanokernel's InstStorageInt tests exactly this mask.
            ppc_exception(p, PPC_VEC_ISI, 0x40200000u, pc);
            return false;
        }
        pa = out.pa;
        uint8_t *host = phys_host(pa & ~(uint32_t)PAGE_MASK);
        bool watched = (g_mem_logpoint_page_count && g_mem_logpoint_page_count[page >> PAGE_SHIFT]) ||
                       (g_mem_logpoint_phys_page_count && g_mem_logpoint_phys_page_count[pa >> PAGE_SHIFT]);
        if (host && !watched) {
            uintptr_t adj = (uintptr_t)host - page;
            fe->tag = tag;
            fe->host_adjust = adj;
            g_ppc_fetch.lo = page;
            g_ppc_fetch.span = MEM_PAGE_SIZE;
            g_ppc_fetch.host_adjust = adj;
            *iw = LOAD_BE32((uint8_t *)(adj + pc));
            return true;
        }
    } else {
        uint32_t ppg = pa >> PAGE_SHIFT;
        uint8_t *host = phys_host(pa & ~(uint32_t)PAGE_MASK);
        bool watched = (g_mem_logpoint_page_count && g_mem_logpoint_page_count[ppg]) ||
                       (g_mem_logpoint_phys_page_count && g_mem_logpoint_phys_page_count[ppg]);
        if (host && !watched) {
            g_ppc_fetch.lo = page;
            g_ppc_fetch.span = MEM_PAGE_SIZE;
            g_ppc_fetch.host_adjust = (uintptr_t)host - page;
            *iw = LOAD_BE32((uint8_t *)(g_ppc_fetch.host_adjust + pc));
            return true;
        }
    }

    // Not host-backed (device window) or memory-logpointed: single-word
    // fetch through the physical slow path so logpoints and device
    // semantics stay honest.  No window: every fetch here re-resolves.
    g_ppc_fetch.span = 0;
    *iw = memory_read_uint32_slow(pa & g_address_mask);
    return true;
}

// ============================================================
// Debug surface
// ============================================================

// Side-effect-free translation for the debug interfaces (debug.mac, the
// disassembler view): no R/C updates, no fills, no exceptions.
// data=true follows the data-access rules (MSR[DT]), else fetch rules.
uint32_t ppc_mmu_translate_debug(ppc_t *p, uint32_t ea, bool data, bool *ok) {
    if (ok)
        *ok = true;
    bool on = data ? (p->msr & PPC_MSR_DT) != 0 : (p->msr & PPC_MSR_IT) != 0;
    if (!data && !on)
        return ea; // fetch with IT off is always direct
    bool user = (p->msr & PPC_MSR_PR) != 0;
    xl_out_t out;
    xl_result_t res = xlate(p, ea, user, false, on, true, &out);
    if (res == XL_OK)
        return out.pa;
    // Protection failures still resolve for debug reads (retry with the
    // supervisor key); only a true miss reports failure.
    if (res == XL_PROT && xlate(p, ea, false, false, on, true, &out) == XL_OK)
        return out.pa;
    if (ok)
        *ok = false;
    return ea;
}

// The 68k world's view: user data context with translation forced on,
// independent of where the sprint stopped (the nanokernel relocates
// logical low memory once the framebuffer claims physical 0, so a
// supervisor-context read of a 68k global lands in the frame buffer —
// debug.mac must always resolve against the user mapping; §3.9e).
uint32_t ppc_mmu_translate_mac(ppc_t *p, uint32_t ea, bool *ok) {
    xl_out_t out;
    if (ok)
        *ok = true;
    xl_result_t res = xlate(p, ea, true, false, true, true, &out);
    if (res == XL_OK)
        return out.pa;
    if (res == XL_PROT && xlate(p, ea, false, false, true, true, &out) == XL_OK)
        return out.pa;
    if (ok)
        *ok = false;
    return ea;
}
