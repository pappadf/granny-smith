// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_mmu.c
// The 601/604 MMU front end (proposal-powerpc-601-pdm.md §3.5; TNT
// proposal §4.3): T=1 I/O-controller segments, BAT match, and the hashed
// page table search, with three software caches in front of the full walk:
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
//   - ISI SRR1: HTAB miss sets bit 1 ONLY ($40000000); the nanokernel's
//     InstStorageInt masks $40200000 but with `andis.`/`beq`, so one bit
//     is enough, and Copland reads bit 10 as a hard access error.
//     Protection sets bit 4 ($08000000), a non-memory-forced T=1 fetch
//     sets NO bits (Table 6-3 footnote);
//   - tlbie invalidates the congruence class selected by EA[13-19] —
//     128 classes (Figure 6-15) — and the kernel's FlushTLB loop relies
//     on exactly that granularity;
//   - the $00A00 I/O controller error resumes at the FOLLOWING
//     instruction with DSISR unchanged (Table 5-21).
//
// 604-specific rules (604UM Ch. 5, PEM Ch. 7), keyed on cpu_model:
//   - the ARCHITECTED BAT format — BEPI/BL/Vs/Vp upper, BRPN/WIMG/PP
//     lower — with four SPLIT pairs each way: instruction translation
//     consults the IBATs (batu/batl), data the DBATs;
//   - BATs take precedence over the segment: T=1 is consulted only after
//     a BAT miss, and with MSR[DR]=0 there is no segment consult at all
//     (real addressing mode — sr_t_mask stays zero on this model);
//   - a direct-store access that no BAT claimed takes the DSI/ISI path
//     (no XIO device exists): DSISR[0] direct-store error (atomics and
//     external control DSISR[5]), ISI SRR1[3] for fetches — there is no
//     $00A00 vector on the 604;
//   - ISI SRR1 for an HTAB miss sets bit 1 alone ($40000000 — no 601
//     bit-10 companion);
//   - tlbie invalidates the class selected by EA[14-19] — 64 classes,
//     both TLBs (604UM §5.4.3.2; the 64-iteration flush idiom).

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

// tlbie congruence class: 601 EA[13-19] = 128 classes (601UM Figure 6-15);
// 604 EA[14-19] = 64 classes (604UM §5.4.3.2).
static inline uint32_t tlbie_class_mask(const ppc_t *p) {
    return ppc_is_604(p) ? 63u : 127u;
}

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

// Memory-logpoint install/uninstall reshaped the watch arrays: drop the
// translation TLB too — a stale entry would keep rewriting a now-watched
// EA to physical before ppc_dxlate_slow's keep-logical check can run.
// (The user SoA arrays are zeroed by the installer itself.)
void ppc_mmu_logpoints_changed(void) {
    memset(g_xtlb, 0, sizeof(g_xtlb));
    ppc_mmu_flush_fetch();
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
// whose page index matches modulo the model's class count (128 on the
// 601, 64 on the 604).  Over-invalidation relative to the hardware's
// two-way class is fine; under-invalidation would break the kernel's
// flush loops (one tlbie per class).
void ppc_mmu_tlbie(ppc_t *p, uint32_t ea) {
    uint32_t mask = tlbie_class_mask(p);
    uint32_t cls = (ea >> PAGE_SHIFT) & mask;
    if (g_fill_track_overflow) {
        user_soa_invalidate_all();
    } else {
        for (int i = 0; i < g_fill_track_count; i++) {
            uint32_t pg = g_fill_track[i];
            if ((pg & mask) != cls || pg >= g_page_count)
                continue;
            g_user_read[pg] = 0;
            g_user_write[pg] = 0;
        }
    }
    for (int i = 0; i < XTLB_SIZE; i++)
        if (((g_xtlb[i].tag >> PAGE_SHIFT) & mask) == cls)
            g_xtlb[i].tag = 0;
    for (int i = 0; i < FTLB_SIZE; i++)
        if (((g_ftlb[i].tag >> PAGE_SHIFT) & mask) == cls)
            g_ftlb[i].tag = 0;
    g_ppc_fetch.span = 0;
}

// Recompute the which-segments-have-T=1 mask consulted by the inline
// translation-off fast path (601 data accesses reach T=1 segments even
// with MSR[DT]=0 — 601UM §6.5.2).  The 604 in real addressing mode never
// consults a segment (PEM §7.2), so its mask stays zero and the fast path
// short-circuits every translation-off access.
void ppc_recompute_sr_t_mask(ppc_t *p) {
    uint32_t m = 0;
    if (!ppc_is_604(p))
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
static bool bat_xlate(const uint32_t *batu, const uint32_t *batl, uint32_t ea, bool user, bool store, xl_out_t *out,
                      xl_result_t *res) {
    for (int i = 0; i < 4; i++) {
        uint32_t bl = batl[i];
        if (!(bl & 0x40u))
            continue; // V
        uint32_t cmp_mask = ~(((bl & 0x3Fu) << 17) | 0x1FFFFu);
        if ((ea & cmp_mask) != (batu[i] & cmp_mask))
            continue;
        uint32_t bu = batu[i];
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

// Architected BAT protection (PEM Table 7-16): PP 00 = no access,
// x1 = read-only, 10 = read/write.  No key participates.
static inline bool bat604_pp_allows(uint32_t pp, bool store) {
    if (pp == 0u)
        return false;
    if (pp & 1u)
        return !store;
    return true;
}

// 604 BAT match + protection over one of the split files (PEM §7.4.2,
// Figure 7-13): BATU = BEPI[0:14] | BL[19:29] | Vs[30] | Vp[31]; BATL =
// BRPN[0:14] | WIMG[25:28] | PP[30:31].  Validity is per-mode (Vs
// supervisor / Vp user), block sizes 128 KB (BL=0) through 256 MB.
static bool bat604_xlate(const uint32_t *batu, const uint32_t *batl, uint32_t ea, bool user, bool store, xl_out_t *out,
                         xl_result_t *res) {
    for (int i = 0; i < 4; i++) {
        uint32_t bu = batu[i];
        if (!(bu & (user ? 1u : 2u)))
            continue; // Vp / Vs
        uint32_t cmp_mask = ~((((bu >> 2) & 0x7FFu) << 17) | 0x1FFFFu);
        if ((ea & cmp_mask) != (bu & 0xFFFE0000u & cmp_mask))
            continue;
        uint32_t bl = batl[i];
        uint32_t pp = bl & 3u;
        if (!bat604_pp_allows(pp, store)) {
            *res = XL_PROT;
            return true;
        }
        out->pa = ((bl & 0xFFFE0000u) & cmp_mask) | (ea & ~cmp_mask);
        out->wimg = (bl >> 4) & 7u; // W/I/M in the shared 3-bit convention (G unmodeled)
        out->w_ok = bat604_pp_allows(pp, true); // no C bit on BAT mappings
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
                if (nlo != lo) {
                    memory_host_written(pte + 4, 4); // a PTEG that is also cached code (absurd, but honest)
                    STORE_BE32(pte + 4, nlo);
                }
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

// The memory-forced direct-store case shared by both models: a T=1
// segment with BUID $07F maps straight to memory (601UM §6.10.4; PEM
// §7.8 defines the same encoding architecturally).
static inline bool tseg_memory_forced(uint32_t sr, uint32_t ea, xl_out_t *out) {
    if (((sr >> 20) & 0x1FFu) != 0x07Fu)
        return false;
    out->pa = ((sr & 0xFu) << 28) | (ea & 0x0FFFFFFFu);
    out->wimg = 0x3u; // WIM assumed 011 (§6.10.4)
    out->w_ok = true; // bypasses all protection
    return true;
}

// Full translation for one access.  `translation_on` reflects MSR[DT] (or
// MSR[IT] for fetches); `ifetch` selects the 604's IBAT file over the
// DBATs (the 601's unified BATs serve both sides).
//
// 601 order (Figure 6-4, Table 6-10): the segment's T bit decides first —
// T=1 prevails over any BAT match — then BAT, then the hashed table; with
// translation off only T=1 segments translate.
//
// 604 order (PEM §7.4/7.5): with translation off, real addressing mode —
// EA = PA, nothing consulted; with it on, BATs take precedence and the
// segment (T=1 direct-store, else the hashed table) is reached only on a
// BAT miss.
static xl_result_t xlate(ppc_t *p, uint32_t ea, bool user, bool store, bool ifetch, bool translation_on,
                         bool nosideffect, xl_out_t *out) {
    if (ppc_is_604(p)) {
        if (!translation_on) { // real addressing mode
            out->pa = ea;
            out->wimg = 1u;
            out->w_ok = true;
            return XL_OK;
        }
        xl_result_t res;
        if (bat604_xlate(ifetch ? p->ibatu_cs : p->dbatu, ifetch ? p->ibatl_cs : p->dbatl, ea, user, store, out, &res))
            return res;
        uint32_t sr = p->sr[ea >> 28];
        if (sr & 0x80000000u)
            return tseg_memory_forced(sr, ea, out) ? XL_OK : XL_IOSEG;
        return htab_search(p, ea, sr, user, store, nosideffect, out);
    }

    uint32_t sr = p->sr[ea >> 28];
    if (sr & 0x80000000u) // T=1: I/O controller interface segment
        return tseg_memory_forced(sr, ea, out) ? XL_OK : XL_IOSEG;
    if (!translation_on) { // direct translation: EA = PA, no protection
        out->pa = ea;
        out->wimg = 1u; // WIM = 001 (§6.6)
        out->w_ok = true;
        return XL_OK;
    }
    xl_result_t res;
    if (bat_xlate(ifetch ? p->ibatu_cs : p->batu, ifetch ? p->ibatl_cs : p->batl, ea, user, store, out, &res))
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
    // Index-collision guard: dxlate_slow hands back PHYSICAL addresses that
    // then index these (logically-filled) arrays.  A fill at an index that
    // doubles as a watched physical page would swallow those accesses on
    // the fast path — through the WRONG host mapping — so keep it empty.
    if (g_mem_logpoint_phys_page_count && g_mem_logpoint_phys_page_count[lpage])
        return;
    if (g_fill_track_count >= PPC_FILL_TRACK_MAX)
        g_fill_track_overflow = true;
    else
        g_fill_track[g_fill_track_count++] = lpage;
    uintptr_t adjusted = (uintptr_t)pe->host_base - (lpage << PAGE_SHIFT);
    g_user_read[lpage] = adjusted;
    if (write_ok && pe->writable) // refused on a predecoded code page (memory.h)
        g_user_write[lpage] = memory_write_fill(lpage, pe->host_base, adjusted);
}

// ============================================================
// Data-access entry point (ppc_dxlate slow half)
// ============================================================

// DSI delivery (601UM Table 5-10: bit 1 not-found, bit 4 protection,
// bit 5 atomics-to-T=1, bit 6 store).
static void raise_dsi(ppc_t *p, uint32_t ea, uint32_t dsisr) {
    p->dar = ea;
    p->dsisr = dsisr;
    LOG(4, "DSI: ea $%08X dsisr $%08X pc $%08X r14 $%08X r25 $%08X r28 $%08X r31 $%08X sr0 $%08X", ea, dsisr,
        p->instruction_pc, p->gpr[14], p->gpr[25], p->gpr[28], p->gpr[31], p->sr[0]);
    ppc_exception(p, PPC_VEC_DSI, 0, p->instruction_pc);
}

// True when iw takes a DSI with DSISR bit 5 instead of completing in a
// T=1 segment: lwarx/stwcx./lscbx on the 601; lwarx/stwcx./eciwx/ecowx on
// the 604 (Table 4-9 — its lscbx is illegal long before memory access).
static bool iw_is_atomic_class(ppc_t *p, uint32_t iw) {
    if (PPC_OPCD(iw) != 31)
        return false;
    uint32_t xo = PPC_XO10(iw);
    if (xo == 20 || xo == 150)
        return true;
    return ppc_is_604(p) ? (xo == 310 || xo == 438) : xo == 277;
}

// See ppc_internal.h for the contract: on return false *addr is the
// address to access — the EA itself when the user SoA now covers it, the
// physical address otherwise.  True = exception raised, abandon.
bool ppc_dxlate_slow(ppc_t *p, uint32_t iw, uint32_t *addr, bool store) {
    uint32_t ea = *addr;
    bool user = (p->msr & PPC_MSR_PR) != 0;
    bool dt = (p->msr & PPC_MSR_DT) != 0;

    // Logically-watched page (memory logpoint): skip the xtlb so the
    // access keeps its LOGICAL address through the memory slow path — the
    // logpoint must fire with the address the guest used, and the slow
    // path resolves the physical backing via g_mem_logical_xlate.
    bool lp_watched = g_mem_logpoint_page_count && g_mem_logpoint_page_count[ea >> PAGE_SHIFT];

    // Translation TLB (serves supervisor accesses and user pages that
    // could not be SoA-filled).
    uint32_t tag = (ea & 0xFFFFF000u) | (user ? 2u : 0u) | 1u;
    xtlb_entry_t *te = &g_xtlb[(ea >> PAGE_SHIFT) & (XTLB_SIZE - 1)];
    if (!lp_watched && te->tag == tag && (!store || te->w_ok)) {
        *addr = te->pa_page | (ea & 0xFFFu);
        return false;
    }

    xl_out_t out;
    xl_result_t res = xlate(p, ea, user, store, false, dt, false, &out);
    if (res == XL_IOSEG) {
        // Non-memory-forced T=1: the atomics/external-control class takes
        // a DSI with DSISR bit 5 on both models.  Past that they diverge:
        // the 601 gives FP load/stores the alignment exception and models
        // everything else as the 601-only $00A00 I/O controller error —
        // SRR0 = FOLLOWING instruction, DSISR unchanged (601UM Table
        // 5-21); the 604 has no $00A00 and delivers the direct-store
        // error DSI (DSISR bit 0 — no XIO device ever answers, 604UM
        // Table 4-9 / Table 4-2 DSI row).
        if (iw_is_atomic_class(p, iw)) {
            raise_dsi(p, ea, PPC_DSISR_ATOMIC | (store ? PPC_DSISR_STORE : 0));
            return true;
        }
        if (ppc_is_604(p)) {
            raise_dsi(p, ea, PPC_DSISR_DIRECT | (store ? PPC_DSISR_STORE : 0));
            return true;
        }
        if (ppc_iw_is_fp_ls(iw)) {
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

    // Logically-watched plain-RAM page: hand the memory slow path the
    // LOGICAL address (uncached — every access must re-enter here) so the
    // logpoint hook fires with it.  Device targets keep the physical
    // rewrite: the slow path would misdispatch a logical address on the
    // identity page table.
    if (lp_watched) {
        uint32_t ppg = out.pa >> PAGE_SHIFT;
        if (ppg < g_page_count && g_page_table[ppg].host_base && !g_page_table[ppg].dev)
            return false; // *addr stays the EA
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

// dcbz's translated form (601UM Table 6-3; 604UM §4.5.6): a W=1 or I=1
// mapping takes the alignment exception (R already updated by the walk);
// a non-memory-forced T=1 segment makes it a no-op (§6.10.6 / PEM "cache
// operations to direct-store are no-ops"); the 604 additionally takes the
// alignment exception with its data cache disabled or locked (HID0[17]
// clear / HID0[19] set — the reset state until the ROM enables caches).
// The framebuffer is mapped write-through, so the W case is
// guest-visible, not a corner.
// Returns: 0 = proceed (zero at *addr), 1 = exception raised, 2 = no-op.
int ppc_dxlate_dcbz(ppc_t *p, uint32_t iw, uint32_t *addr) {
    uint32_t ea = *addr;
    bool dt = (p->msr & PPC_MSR_DT) != 0;
    bool is604 = ppc_is_604(p);
    if (is604 && (!(p->hid0 & 0x00004000u) || (p->hid0 & 0x00001000u))) { // DCE clear or DCL set
        ppc_align_exception(p, iw, ea);
        return 1;
    }
    if (!dt && !(p->sr_t_mask & (1u << (ea >> 28))))
        return 0; // direct translation: plain zeroing (mask covers the 604 unconditionally)
    bool user = (p->msr & PPC_MSR_PR) != 0;
    uint32_t sr = p->sr[ea >> 28];
    // 601 order: T=1 prevails, so the no-op is decided before any walk.
    // The 604 checks T only after a BAT miss — the xlate result decides.
    if (!is604 && (sr & 0x80000000u) && ((sr >> 20) & 0x1FFu) != 0x07Fu)
        return 2; // T=1 I/O controller: no-op (§6.10.6)

    xl_out_t out;
    xl_result_t res = xlate(p, ea, user, true, false, dt, false, &out);
    if (res == XL_IOSEG)
        return 2; // 604 direct-store after BAT miss: cache-op no-op
    if (res == XL_PROT) {
        raise_dsi(p, ea, PPC_DSISR_PROT | PPC_DSISR_STORE);
        return 1;
    }
    if (res != XL_OK) {
        raise_dsi(p, ea, PPC_DSISR_NOTFOUND | PPC_DSISR_STORE);
        return 1;
    }
    // 601: memory-forced T=1 zeroes real memory (WIM 011 describes the
    // bus attributes; HWInit's RAM work runs through these segments) —
    // its W/I alignment rule applies to BAT/page mappings only.  The 604
    // faults any noncacheable/write-through target (604UM §4.5.6).
    if ((is604 || !(sr & 0x80000000u)) && (out.wimg & 0x6u)) { // W or I
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
        xl_result_t res = xlate(p, pc, user, false, true, true, false, &out);
        if (res == XL_IOSEG) {
            // T=1 fetch: the 601 raises ISI with NO SRR1 status bits
            // (quirk, Table 6-3 footnote); the 604 sets the architected
            // direct-store-fetch bit SRR1[3] (PEM Table 7-14).
            ppc_exception(p, PPC_VEC_ISI, ppc_is_604(p) ? 0x10000000u : 0, pc);
            return false;
        }
        if (res == XL_PROT) {
            ppc_exception(p, PPC_VEC_ISI, 0x08000000u, pc);
            return false;
        }
        if (res != XL_OK) {
            // HTAB miss: SRR1 bit 1 ONLY ($40000000), on BOTH models.
            // We used to set bit 10 as well on the 601 because the
            // nanokernel's InstStorageInt masks $40200000 — but it does
            // that with `andis. r8,r11,$4020` followed by `beq` (ROM
            // file offset $311878, the only such instruction in the
            // image), so either bit alone satisfies it.  Bit 10 is NOT
            // a page-fault bit, and Copland's GetFaultInformation reads
            // it as one of the two hard-error bits ($10200000) and
            // turns an ordinary instruction page fault into
            // kAccessException — which panicked its kernel.  Two
            // independent guests only agree if a 601 leaves bit 10
            // clear for an HTAB miss; the 604's architected value is
            // bit 1 alone anyway.
            ppc_exception(p, PPC_VEC_ISI, 0x40000000u, pc);
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
    xl_result_t res = xlate(p, ea, user, false, !data, on, true, &out);
    if (res == XL_OK)
        return out.pa;
    // Protection failures still resolve for debug reads (retry with the
    // supervisor key); only a true miss reports failure.
    if (res == XL_PROT && xlate(p, ea, false, false, !data, on, true, &out) == XL_OK)
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
    xl_result_t res = xlate(p, ea, true, false, false, true, true, &out);
    if (res == XL_OK)
        return out.pa;
    if (res == XL_PROT && xlate(p, ea, false, false, false, true, true, &out) == XL_OK)
        return out.pa;
    if (ok)
        *ok = false;
    return ea;
}
