// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_run.c
// The PPC (MPC601/MPC604) interpreter: instantiates the shared decode tree
// (ppc_decode.h) with the execution OP_ table (ppc_ops.h) to generate
// ppc_execute(), and wraps it in the main-CPU sprint loop.  Fixed-width
// 32-bit fetch through the global fast-path memory accessors — this core
// is a MAIN CPU (cores.md main-vs-aux rule).
//
// The multi-statement instruction bodies the one-liner table delegates to
// (branches, divides, string transfers) live here; their semantics follow
// the MPC601 User's Manual chapter-10 pages, including the POWER holdovers
// the 601 retains.

#include "ppc_ops.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>

LOG_USE_CATEGORY_NAME("ppc");

// === Temporary pc-trace (pm8500 604 beacon divergence hunt) =================
// Diagnostic scaffolding, armed only via environment; inert otherwise.
//   GS_PCTRACE_FILE=<path>    enable; per-instruction "pc iw" lines
//   GS_PCTRACE_TRIGGER=<hex>  start pc (default FF809A64, the beacon)
//   GS_PCTRACE_COUNT=<n>      instructions to log once triggered (default 30000)
//   GS_PCTRACE_SKIP=<n>       trigger hits to ignore before tracing (default 0)
//   GS_PCTRACE_HITS=1         instead log one context line per trigger hit
//   GS_PCTRACE_REG=<n>        append gpr[n] to each line (-1 = off)
//   GS_PCTRACE_WATCH=<hex>    watch mode: log only changes of the word at
//                             this physical address (with pc), no pc lines
//   GS_PCTRACE_ONLYPC=<hex>   filter: within the window log only this pc
//                             (plus a ROUND marker per trigger hit and the
//                             $300/$600 vector entries); with REG set the
//                             line also carries the byte at gpr[REG], read
//                             through the boot-era static alias (arena
//                             below 16 MB 1:1, kernel $FF8xxxxx → phys
//                             $4xxxxx)
static FILE *g_trace_file;
static int g_trace_reg = -1;
static uint32_t g_trace_trigger, g_trace_count, g_trace_skip;
static uint32_t g_trace_active; // instructions left in the triggered window
static uint32_t g_trace_hitno; // trigger hits seen so far
static uint64_t g_trace_seen; // instructions executed since arm (approximates instr_count)
static uint32_t g_trace_watch; // physical word address to watch (0 = off)
static uint32_t g_trace_watch_val; // last observed value at the watch address
static uint32_t g_trace_onlypc; // filter: log only this pc within the window (0 = off)
static int g_trace_hits_mode = -1; // -1 = env not parsed yet

// One-time env parse; leaves g_trace_file NULL (trace off) unless armed.
static void ppc_trace_init(void) {
    g_trace_hits_mode = 0;
    const char *path = getenv("GS_PCTRACE_FILE");
    if (!path)
        return;
    g_trace_file = fopen(path, "w");
    if (!g_trace_file)
        return;
    const char *s;
    g_trace_trigger = (s = getenv("GS_PCTRACE_TRIGGER")) ? (uint32_t)strtoul(s, NULL, 16) : 0xFF809A64u;
    g_trace_count = (s = getenv("GS_PCTRACE_COUNT")) ? (uint32_t)strtoul(s, NULL, 0) : 30000u;
    g_trace_skip = (s = getenv("GS_PCTRACE_SKIP")) ? (uint32_t)strtoul(s, NULL, 0) : 0u;
    g_trace_hits_mode = ((s = getenv("GS_PCTRACE_HITS")) && *s == '1');
    g_trace_reg = (s = getenv("GS_PCTRACE_REG")) ? atoi(s) : -1;
    g_trace_watch = (s = getenv("GS_PCTRACE_WATCH")) ? (uint32_t)strtoul(s, NULL, 16) : 0u;
    g_trace_watch_val = g_trace_watch ? memory_read_uint32(g_trace_watch) : 0u;
    g_trace_onlypc = (s = getenv("GS_PCTRACE_ONLYPC")) ? (uint32_t)strtoul(s, NULL, 16) : 0u;
}

// Boot-era static virtual→physical alias for diagnostic byte peeks: the 68k
// arena is identity-mapped below 16 MB and the kernel image at virtual
// $FF800000 is phys $00400000.  Returns $FF for anything else (matches the
// untranslated-peek convention).
static uint8_t ppc_trace_peek8(uint32_t vaddr) {
    if (vaddr < 0x01000000u)
        return memory_read_uint8(vaddr);
    if (vaddr >= 0xFF800000u && vaddr < 0xFFC00000u)
        return memory_read_uint8(vaddr - 0xFF800000u + 0x00400000u);
    return 0xFF;
}

// Per-instruction hook (called only while g_trace_file is set): waits for
// the trigger pc, then logs the instruction stream (or just hit contexts).
static void ppc_trace(ppc_t *p, uint32_t iw) {
    g_trace_seen++;
    if (p->instruction_pc == g_trace_trigger)
        g_trace_hitno++; // counted even inside an active window (ONLYPC round markers)
    if (g_trace_active == 0) {
        if (p->instruction_pc != g_trace_trigger)
            return;
        if (g_trace_hits_mode) {
            fprintf(g_trace_file, "HIT %u seen=%llu lr=%08X ctr=%08X msr=%08X r2=%08X r25=%08X\n", g_trace_hitno,
                    (unsigned long long)g_trace_seen, p->lr, p->ctr, p->msr, p->gpr[2], p->gpr[25]);
            fflush(g_trace_file);
            return;
        }
        if (g_trace_hitno <= g_trace_skip)
            return;
        fprintf(g_trace_file, "TRIG %u seen=%llu lr=%08X ctr=%08X msr=%08X\n", g_trace_hitno,
                (unsigned long long)g_trace_seen, p->lr, p->ctr, p->msr);
        g_trace_active = g_trace_count;
    }
    if (g_trace_watch) { // watch mode: log only value changes (prev instruction wrote it)
        uint32_t v = memory_read_uint32(g_trace_watch);
        if (v != g_trace_watch_val) {
            fprintf(g_trace_file, "WATCH seen=%llu %08X->%08X next_pc=%08X\n", (unsigned long long)g_trace_seen,
                    g_trace_watch_val, v, p->instruction_pc);
            fflush(g_trace_file);
            g_trace_watch_val = v;
        }
        return; // watch mode never expires its window
    }
    if (p->instruction_pc == 0xFFF00300u || p->instruction_pc == 0xFFF00600u) // vector entry: fault details
        fprintf(g_trace_file, "%08X %08X VEC dar=%08X dsisr=%08X srr0=%08X srr1=%08X\n", p->instruction_pc, iw, p->dar,
                p->dsisr, p->srr0, p->srr1);
    else if (g_trace_onlypc) { // filtered mode: round markers + the one pc of interest
        if (p->instruction_pc == g_trace_trigger) {
            fprintf(g_trace_file, "ROUND %u seen=%llu\n", g_trace_hitno, (unsigned long long)g_trace_seen);
            fflush(g_trace_file); // survive a daemon kill mid-window
        } else if (p->instruction_pc == g_trace_onlypc) {
            if (g_trace_reg >= 0) {
                uint32_t a = p->gpr[g_trace_reg & 31];
                fprintf(g_trace_file, "%08X %08X %08X %02X\n", p->instruction_pc, iw, a, ppc_trace_peek8(a));
            } else
                fprintf(g_trace_file, "%08X %08X\n", p->instruction_pc, iw);
        }
    } else if (g_trace_reg >= 0)
        fprintf(g_trace_file, "%08X %08X %08X\n", p->instruction_pc, iw, p->gpr[g_trace_reg & 31]);
    else
        fprintf(g_trace_file, "%08X %08X\n", p->instruction_pc, iw);
    if (--g_trace_active == 0) { // window done; disarm (one window per run)
        fclose(g_trace_file);
        g_trace_file = NULL;
    }
}

// 601 branch-folding flag for the sprint loop: the four branch handlers
// (and, for bit-exactness, illegal branch forms) record here whether the
// just-executed instruction folds — b/bc/bclr/bcctr issue to the branch
// unit in parallel and retire in zero cycles, EXCEPT a taken branch to
// itself (a pure spin must still burn budget slots).  Purely
// intra-instruction state (set in the handler, consumed by the loop right
// after ppc_execute returns), so it lives outside ppc_t and is never
// checkpointed.  Keeping the classification here instead of re-deriving
// `iw >> 26` + a compare chain in the loop takes the cost off the 100%
// path and puts it on the ~15% of instructions that are branches.
static uint32_t g_ppc_fold = 0;

// The fold predicate shared by the handlers: everything except a redirect
// back onto the instruction's own address.
static inline void ppc_record_fold(ppc_t *p) {
    g_ppc_fold = (p->pc != p->instruction_pc);
}

// Illegal-instruction program exception (also the documented path for
// PowerPC instructions the 601 does not implement — mftb, tlbia, the
// 64-bit set (601UM §10.3 Tables 10-6/10-8) — and for invalid forms the
// shared decode tree routes to OP_ILLEGAL).
void ppc_illegal_op(ppc_t *p, uint32_t iw) {
    LOG(5, "illegal instruction $%08X at $%08X", iw, p->instruction_pc);
    ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_ILLEGAL, p->instruction_pc);
    // Bit-exactness with the loop's former post-hoc opcode test: an illegal
    // FORM of a branch opcode (invalid BO) classified as folded there, so it
    // must keep doing so (pc now points at the program vector, != the
    // faulting address, so the predicate holds).
    uint32_t op = iw >> 26;
    if (op == 18 || op == 16 || (op == 19 && ((iw & 0x7FEu) == 0x20u || (iw & 0x7FEu) == 0x420u)))
        ppc_record_fold(p);
}

// === Branches ===============================================================

static inline void ppc_do_b(ppc_t *p, uint32_t iw) {
    int32_t li = (int32_t)(iw << 6) >> 6; // sign-extend the 26-bit field
    li &= ~3;
    uint32_t target = (iw & 2u) ? (uint32_t)li : p->instruction_pc + (uint32_t)li;
    if (iw & 1u)
        p->lr = p->pc;
    p->pc = target;
    ppc_record_fold(p);
}

static inline void ppc_do_bc(ppc_t *p, uint32_t iw) {
    bool taken = ppc_branch_taken(p, PPC_RT(iw), PPC_RA(iw), true);
    int32_t bd = (int32_t)(int16_t)(iw & 0xFFFCu);
    uint32_t target = (iw & 2u) ? (uint32_t)bd : p->instruction_pc + (uint32_t)bd;
    if (iw & 1u)
        p->lr = p->pc;
    if (taken)
        p->pc = target;
    ppc_record_fold(p);
}

static inline void ppc_do_bclr(ppc_t *p, uint32_t iw) {
    bool taken = ppc_branch_taken(p, PPC_RT(iw), PPC_RA(iw), true);
    uint32_t target = p->lr & ~3u;
    if (PPC_RC(iw))
        p->lr = p->pc;
    if (taken)
        p->pc = target;
    ppc_record_fold(p);
}

// bcctr: BO[2] = 0 (decrement CTR) is an invalid form, and the 601's
// documented behavior for it (601UM Table 3-31) is peculiar enough to be
// worth spelling out — the count register IS decremented and tested, and
// the branch is resolved on that test, but instruction fetch is directed to
// the NON-decremented value.  Hence the target snapshot before the test.
static inline void ppc_do_bcctr(ppc_t *p, uint32_t iw) {
    uint32_t target = p->ctr & ~3u;
    bool taken = ppc_branch_taken(p, PPC_RT(iw), PPC_RA(iw), true);
    if (PPC_RC(iw))
        p->lr = p->pc;
    if (taken)
        p->pc = target;
    ppc_record_fold(p);
}

// === Divides (architecturally-undefined results fixed for determinism) ======

void ppc_do_divw(ppc_t *p, uint32_t iw) {
    uint32_t a = p->gpr[PPC_RA(iw)], b = p->gpr[PPC_RB(iw)], r;
    if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) {
        r = 0; // undefined; fixed
        if (PPC_OE(iw))
            ppc_set_ov(p, 1);
    } else {
        r = (uint32_t)((int32_t)a / (int32_t)b);
        if (PPC_OE(iw))
            ppc_set_ov(p, 0);
    }
    p->gpr[PPC_RT(iw)] = r;
    if (PPC_RC(iw))
        ppc_record_cr0(p, r);
}

void ppc_do_divwu(ppc_t *p, uint32_t iw) {
    uint32_t b = p->gpr[PPC_RB(iw)], r;
    if (b == 0) {
        r = 0;
        if (PPC_OE(iw))
            ppc_set_ov(p, 1);
    } else {
        r = p->gpr[PPC_RA(iw)] / b;
        if (PPC_OE(iw))
            ppc_set_ov(p, 0);
    }
    p->gpr[PPC_RT(iw)] = r;
    if (PPC_RC(iw))
        ppc_record_cr0(p, r);
}

// === POWER arithmetic holdovers =============================================

// doz: rD = 0 when rA > rB algebraically, else rB - rA; OV only on the
// positive overflows (601UM doz page).
void ppc_do_doz(ppc_t *p, uint32_t iw) {
    uint32_t a = p->gpr[PPC_RA(iw)], b = p->gpr[PPC_RB(iw)], r;
    if ((int32_t)a > (int32_t)b) {
        r = 0;
        if (PPC_OE(iw))
            ppc_set_ov(p, 0);
    } else {
        r = b - a;
        if (PPC_OE(iw))
            ppc_set_ov(p, (int)(((a ^ b) & (r ^ b)) >> 31));
    }
    p->gpr[PPC_RT(iw)] = r;
    if (PPC_RC(iw))
        ppc_record_cr0(p, r);
}

// mul: rD = high 32 of the product, MQ = low 32; CR0 reflects MQ.
void ppc_do_mul(ppc_t *p, uint32_t iw) {
    int64_t prod = (int64_t)(int32_t)p->gpr[PPC_RA(iw)] * (int32_t)p->gpr[PPC_RB(iw)];
    p->mq = (uint32_t)prod;
    p->gpr[PPC_RT(iw)] = (uint32_t)((uint64_t)prod >> 32);
    if (PPC_OE(iw))
        ppc_set_ov(p, prod != (int64_t)(int32_t)p->mq);
    if (PPC_RC(iw))
        ppc_record_cr0(p, p->mq);
}

// div: (rA||MQ) / rB → rD, remainder → MQ (sign follows the dividend);
// CR0 reflects the remainder.  The documented -2^31/-1 case yields
// rD=$80000000/MQ=0; the other unrepresentable quotients are undefined —
// the same deterministic values are chosen.
void ppc_do_div(ppc_t *p, uint32_t iw) {
    uint32_t a = p->gpr[PPC_RA(iw)], b = p->gpr[PPC_RB(iw)], r;
    int64_t dd = ((int64_t)a << 32) | p->mq;
    if (b == 0) {
        r = 0;
        p->mq = 0;
        if (PPC_OE(iw))
            ppc_set_ov(p, 1);
    } else {
        int64_t q = dd / (int32_t)b; // C99: truncation toward zero,
        int64_t rem = dd % (int32_t)b; // remainder sign follows dividend
        if (q != (int64_t)(int32_t)q) {
            r = 0x80000000u;
            p->mq = 0;
            if (PPC_OE(iw))
                ppc_set_ov(p, 1);
        } else {
            r = (uint32_t)q;
            p->mq = (uint32_t)rem;
            if (PPC_OE(iw))
                ppc_set_ov(p, 0);
        }
    }
    p->gpr[PPC_RT(iw)] = r;
    if (PPC_RC(iw))
        ppc_record_cr0(p, p->mq);
}

// divs: rA / rB → rD, remainder → MQ; CR0 reflects the remainder.
void ppc_do_divs(ppc_t *p, uint32_t iw) {
    uint32_t a = p->gpr[PPC_RA(iw)], b = p->gpr[PPC_RB(iw)], r;
    if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) {
        r = (b != 0) ? 0x80000000u : 0u;
        p->mq = 0;
        if (PPC_OE(iw))
            ppc_set_ov(p, 1);
    } else {
        r = (uint32_t)((int32_t)a / (int32_t)b);
        p->mq = (uint32_t)((int32_t)a % (int32_t)b);
        if (PPC_OE(iw))
            ppc_set_ov(p, 0);
    }
    p->gpr[PPC_RT(iw)] = r;
    if (PPC_RC(iw))
        ppc_record_cr0(p, p->mq);
}

// sraw: rB bit 26 selects the >=32 case (fill with sign bits); CA set when
// 1-bits shift out of a negative value.
void ppc_do_sraw(ppc_t *p, uint32_t iw) {
    uint32_t s = p->gpr[PPC_RT(iw)], b = p->gpr[PPC_RB(iw)], n = b & 31u, r;
    if (b & 0x20u) {
        r = (uint32_t)((int32_t)s >> 31);
        ppc_set_ca(p, ((int32_t)s < 0) && s != r);
    } else {
        r = (uint32_t)((int32_t)s >> n);
        ppc_set_ca(p, ((int32_t)s < 0) && n != 0 && (s & ((1u << n) - 1u)) != 0);
    }
    p->gpr[PPC_RA(iw)] = r;
    if (PPC_RC(iw))
        ppc_record_cr0(p, r);
}

// === The scalar access gate =================================================

// 604UM §4.5.6 word-alignment classes routed through CHKA_*: the FP
// loads/stores and lwarx (stwcx./eciwx/ecowx check at their own sites).
static bool iw_requires_word_align_604(uint32_t iw) {
    if (ppc_iw_is_fp_ls(iw))
        return true;
    return PPC_OPCD(iw) == 31 && PPC_XO10(iw) == 20; // lwarx
}

// The 604's hardware-split page-crossing access, modeled byte-wise so
// discontiguously translated pages behave (the string-transfer precedent).
// A mid-transfer fault abandons with the store partially done — the 604UM
// documents exactly that ("the instruction may complete partially").
// Returns true when an exception was raised.
static bool ppc_split_access(ppc_t *p, uint32_t iw, uint32_t ea, uint32_t size, bool store, uint64_t *val) {
    if (store) {
        for (uint32_t i = 0; i < size; i++) {
            uint32_t xa = ea + i;
            if (ppc_dxlate(p, iw, &xa, true))
                return true;
            memory_write_uint8(xa, (uint8_t)(*val >> (8u * (size - 1u - i))));
        }
    } else {
        uint64_t v = 0;
        for (uint32_t i = 0; i < size; i++) {
            uint32_t xa = ea + i;
            if (ppc_dxlate(p, iw, &xa, false))
                return true;
            v = (v << 8) | memory_read_uint8(xa);
        }
        *val = v;
    }
    return false;
}

// See ppc_internal.h for the contract (-1 exception / 0 proceed at *addr /
// +1 completed byte-wise).  The 601 path is the classic §5.4.6 boundary
// check plus one translation; the 604 path is the word-alignment rule for
// its fault-prone classes plus the hardware split.
int ppc_scalar_gate(ppc_t *p, uint32_t iw, uint32_t *addr, uint32_t size, bool store, uint64_t *val) {
    uint32_t ea = *addr;
    if (!ppc_is_604(p)) {
        if (ppc_check_align_scalar(p, iw, ea, size))
            return -1;
        return ppc_dxlate(p, iw, addr, store) ? -1 : 0;
    }
    if ((ea & 3u) && iw_requires_word_align_604(iw)) {
        ppc_align_exception(p, iw, ea);
        return -1;
    }
    // With translation off the identity map is contiguous and the normal
    // access handles any misalignment; translated page crossings go
    // byte-wise (pages need not be physically adjacent).
    if (size > 1 && (p->msr & PPC_MSR_DT) && ppc_crosses_page(ea, size))
        return ppc_split_access(p, iw, ea, size, store, val) ? -1 : 1;
    return ppc_dxlate(p, iw, addr, store) ? -1 : 0;
}

// === Multiples and strings ==================================================

void ppc_do_lmw(ppc_t *p, uint32_t iw) {
    uint32_t rt = PPC_RT(iw), ra = PPC_RA(iw);
    uint32_t ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
    // 604: multiples must be word-aligned (604UM §4.5.6); aligned ones are
    // split per word below, so no further boundary rule applies.
    if (ppc_is_604(p)) {
        if (ea & 3u) {
            ppc_align_exception(p, iw, ea);
            return;
        }
    } else if (ppc_check_align_string(p, iw, ea, 4u * (32u - rt), false))
        return;
    // Word-aligned multiples may cross pages, and HTAB pages need not be
    // physically contiguous: translate per word.  A mid-transfer fault
    // abandons with registers partially loaded (restartable — rA in
    // range is skipped, so the base survives for the re-execution).
    for (uint32_t reg = rt; reg < 32; reg++, ea += 4) {
        uint32_t xa = ea;
        if (ppc_dxlate(p, iw, &xa, false))
            return;
        if (reg != ra || ra == 0) // rA in range is skipped (kept as base)
            p->gpr[reg] = memory_read_uint32(xa);
    }
}

void ppc_do_stmw(ppc_t *p, uint32_t iw) {
    uint32_t rt = PPC_RT(iw), ra = PPC_RA(iw);
    uint32_t ea = (ra ? p->gpr[ra] : 0u) + (uint32_t)PPC_SIMM(iw);
    if (ppc_is_604(p)) {
        if (ea & 3u) {
            ppc_align_exception(p, iw, ea);
            return;
        }
    } else if (ppc_check_align_string(p, iw, ea, 4u * (32u - rt), false))
        return;
    for (uint32_t reg = rt; reg < 32; reg++, ea += 4) {
        uint32_t xa = ea;
        if (ppc_dxlate(p, iw, &xa, true))
            return;
        memory_write_uint32(xa, p->gpr[reg]);
    }
}

// Load n bytes at ea into registers rt.. (wrapping, left-to-right per
// register, partial last register zero-filled).  Registers named by the
// EA-forming fields are skipped (601UM lswx page).  skip_a/skip_b are -1
// when not applicable.  Byte-wise translation: unaligned strings that
// would cross a page have already taken the alignment exception, but
// aligned ones may span pages that translate discontiguously.
static void ppc_load_string(ppc_t *p, uint32_t iw, uint32_t ea, uint32_t rt, uint32_t n, int skip_a, int skip_b) {
    uint32_t r = rt;
    while (n > 0) {
        uint32_t word = 0;
        uint32_t take = n < 4 ? n : 4;
        for (uint32_t i = 0; i < take; i++) {
            uint32_t xa = ea + i;
            if (ppc_dxlate(p, iw, &xa, false))
                return;
            word |= (uint32_t)memory_read_uint8(xa) << (24 - 8 * i);
        }
        if ((int)r != skip_a && (int)r != skip_b)
            p->gpr[r] = word;
        ea += take;
        n -= take;
        r = (r + 1) & 31;
    }
}

static void ppc_store_string(ppc_t *p, uint32_t iw, uint32_t ea, uint32_t rs, uint32_t n) {
    uint32_t r = rs;
    while (n > 0) {
        uint32_t word = p->gpr[r];
        uint32_t take = n < 4 ? n : 4;
        for (uint32_t i = 0; i < take; i++) {
            uint32_t xa = ea + i;
            if (ppc_dxlate(p, iw, &xa, true))
                return;
            memory_write_uint8(xa, (uint8_t)(word >> (24 - 8 * i)));
        }
        ea += take;
        n -= take;
        r = (r + 1) & 31;
    }
}

void ppc_do_lswi(ppc_t *p, uint32_t iw) {
    uint32_t ra = PPC_RA(iw);
    uint32_t n = PPC_RB(iw) ? PPC_RB(iw) : 32u;
    uint32_t ea = ra ? p->gpr[ra] : 0u;
    if (ppc_check_align_string(p, iw, ea, n, false))
        return;
    ppc_load_string(p, iw, ea, PPC_RT(iw), n, ra ? (int)ra : -1, -1);
}

void ppc_do_lswx(ppc_t *p, uint32_t iw) {
    uint32_t ra = PPC_RA(iw), rb = PPC_RB(iw);
    uint32_t n = p->xer & PPC_XER_BYTES;
    uint32_t ea = (ra ? p->gpr[ra] : 0u) + p->gpr[rb];
    if (n == 0)
        return;
    if (ppc_check_align_string(p, iw, ea, n, false))
        return;
    ppc_load_string(p, iw, ea, PPC_RT(iw), n, ra ? (int)ra : -1, (int)rb);
}

void ppc_do_stswi(ppc_t *p, uint32_t iw) {
    uint32_t ra = PPC_RA(iw);
    uint32_t n = PPC_RB(iw) ? PPC_RB(iw) : 32u;
    uint32_t ea = ra ? p->gpr[ra] : 0u;
    if (ppc_check_align_string(p, iw, ea, n, false))
        return;
    ppc_store_string(p, iw, ea, PPC_RT(iw), n);
}

void ppc_do_stswx(ppc_t *p, uint32_t iw) {
    uint32_t ra = PPC_RA(iw);
    uint32_t n = p->xer & PPC_XER_BYTES;
    uint32_t ea = (ra ? p->gpr[ra] : 0u) + p->gpr[PPC_RB(iw)];
    if (n == 0)
        return;
    if (ppc_check_align_string(p, iw, ea, n, false))
        return;
    ppc_store_string(p, iw, ea, PPC_RT(iw), n);
}

// lscbx: load string, stopping at the XER compare byte (601UM lscbx page).
// XER[25-31] is updated to the bytes-loaded count only when a match is
// found; CR0 (Rc=1) = 0b00 || match || SO.
void ppc_do_lscbx(ppc_t *p, uint32_t iw) {
    uint32_t rt = PPC_RT(iw), ra = PPC_RA(iw), rb = PPC_RB(iw);
    uint32_t n = p->xer & PPC_XER_BYTES;
    uint32_t match_byte = (p->xer >> 8) & 0xFFu;
    uint32_t ea = (ra ? p->gpr[ra] : 0u) + p->gpr[rb];
    if (n == 0)
        return; // rD undefined; leave untouched (deterministic)
    if (ppc_check_align_string(p, iw, ea, n, true))
        return;
    uint32_t reg = rt, word = 0, loaded = 0;
    int shift = 24;
    bool matched = false;
    for (uint32_t i = 0; i < n && !matched; i++) {
        uint32_t xa = ea + i;
        if (ppc_dxlate(p, iw, &xa, false))
            return;
        uint8_t byte = memory_read_uint8(xa);
        word |= (uint32_t)byte << shift;
        loaded++;
        matched = (byte == match_byte);
        shift -= 8;
        if (shift < 0 || matched || i + 1 == n) {
            bool skip = ((int)reg == (ra ? (int)ra : -1)) || (reg == rb);
            if (!skip)
                p->gpr[reg] = word;
            reg = (reg + 1) & 31;
            word = 0;
            shift = 24;
        }
    }
    if (matched) // count of bytes loaded incl. the match (else unchanged)
        p->xer = (p->xer & ~PPC_XER_BYTES) | loaded;
    if (PPC_RC(iw))
        ppc_set_cr_field(p, 0, (matched ? 2u : 0u) | ((p->xer & PPC_XER_SO) ? 1u : 0u));
}

// === Atomics / external control =============================================

void ppc_do_stwcx(ppc_t *p, uint32_t iw) {
    uint32_t ea = (PPC_RA(iw) ? p->gpr[PPC_RA(iw)] : 0u) + p->gpr[PPC_RB(iw)];
    if (ppc_is_604(p) && (ea & 3u)) { // 604: word alignment required (604UM §4.5.6)
        ppc_align_exception(p, iw, ea);
        return;
    }
    if (ppc_check_align_scalar(p, iw, ea, 4)) // 601: misalignment alone is not a fault; see OP_LWARX
        return;
    uint32_t xa = ea;
    if (ppc_dxlate(p, iw, &xa, true))
        return;
    if (p->reserve) {
        memory_write_uint32(xa, p->gpr[PPC_RT(iw)]);
        ppc_set_cr_field(p, 0, 2u | ((p->xer & PPC_XER_SO) ? 1u : 0u));
    } else {
        ppc_set_cr_field(p, 0, (p->xer & PPC_XER_SO) ? 1u : 0u);
    }
    p->reserve = 0;
}

// eciwx/ecowx with EAR[E]=0: DSI with DSISR[11] set (601UM Table 5-10).
void ppc_ecx_fault(ppc_t *p, uint32_t ea, bool store) {
    p->dar = ea;
    p->dsisr = 0x00100000u | (store ? PPC_DSISR_STORE : 0u);
    ppc_exception(p, PPC_VEC_DSI, 0, p->instruction_pc);
}

// === Instruction fetch ======================================================

// Fetch the instruction word at p->pc.  The one-page window in
// g_ppc_fetch caches the host mapping (identity or translated —
// ppc_mmu.c owns the refill, including ISI delivery); fetch never goes
// through the mode-dependent g_active maps.  Returns false when the
// fetch raised ISI (pc has been redirected to the vector).
static inline bool ppc_fetch(ppc_t *p, uint32_t *iw) {
    uint32_t pc = p->pc;
    if (__builtin_expect(pc - g_ppc_fetch.lo < g_ppc_fetch.span, 1)) {
        *iw = LOAD_BE32((uint8_t *)(g_ppc_fetch.host_adjust + pc));
        return true;
    }
    return ppc_fetch_fill(p, pc, iw);
}

// === The generated decoder ==================================================

// Single call site in the sprint loop below: force the dispatch switch
// inline into the loop (the function is past -O2's size heuristics, so a
// bare `static` stays out of line), dropping the per-instruction
// call/return and the register spills it forces.
#define PPC_DECODER_NAME        ppc_execute
#define PPC_DECODER_RETURN_TYPE static inline __attribute__((always_inline)) void
#define PPC_DECODER_ARGS        ppc_t *restrict p, uint32_t iw
#define PPC_DECODER_PROLOGUE    (void)0
#define PPC_DECODER_EPILOGUE    (void)0

#include "ppc_decode.h"

// === The sprint loop (main-CPU seam ABI) ====================================

void ppc_run(ppc_t *restrict p, uint32_t *instructions) {
    // A memory-layer fault zeroes the burndown through this pointer so the
    // sprint exits and the epilogue delivers the machine check (the 68030
    // decoder precedent in cpu_68000.c/cpu_68030.c).
    g_bus_error_instr_ptr = instructions;
    g_ppc_fold = 0; // no stale classification from a prior sprint
    if (__builtin_expect(g_trace_hits_mode < 0, 0))
        ppc_trace_init(); // temporary pc-trace: parse env once
    ppc_poll_interrupt(p);
    while (*instructions > 0) {
        // Level-sensitive interrupt inputs re-checked at every boundary —
        // this is what makes the "loop until all flags clear" dispatch and
        // post-rfi redelivery work (proposal §4.6).
        if ((p->ext_irq | p->dec_pending) && (p->msr & PPC_MSR_EE))
            ppc_poll_interrupt(p);
        p->instruction_pc = p->pc;
        uint32_t iw;
        if (!ppc_fetch(p, &iw))
            continue; // ISI raised; pc now at the vector
        if (__builtin_expect(g_bus_error_pending, 0))
            break; // fetch faulted; delivered below
        if (__builtin_expect(g_trace_file != NULL, 0))
            ppc_trace(p, iw); // temporary pc-trace (inert unless env-armed)
        p->pc += 4;
        ppc_execute(p, iw);
        // 601 branch folding: b/bc/bclr/bcctr issue to the branch unit in
        // parallel and retire in zero cycles — the reason HWInit's timed
        // 8-addi + bdnz measurement loop really runs at CPI 1.0 (proposal
        // §5.2).  The handlers record the classification in g_ppc_fold
        // (self-branch excluded there), keeping the opcode test off the
        // every-instruction path.  The last budget slot never folds — a
        // folded branch there would run the branch AND its target in one
        // nominal instruction, which breaks single-step and makes PC
        // breakpoints/logpoints skip branch targets.
        if (__builtin_expect(g_ppc_fold != 0, 0)) {
            g_ppc_fold = 0;
            if (*instructions > 1)
                continue; // folded: no budget slot consumed
        }
        if (*instructions > 0) // saturating (I/O penalty may have zeroed it)
            (*instructions)--;
    }
    // Deferred data/fetch fault → machine check (601UM §5.4.2 / 604UM
    // §4.5.2: the TEA path; the bus errors of the PDM family's AMIC/BART —
    // and the TNT family's Bandit windows — arrive this way).
    if (__builtin_expect(g_bus_error_pending, 0)) {
        g_bus_error_pending = false;
        p->dar = g_bus_error_address;
        LOG(3, "machine check: addr $%08X pc $%08X r24 $%08X", g_bus_error_address, p->instruction_pc, p->gpr[24]);
        if (ppc_is_604(p)) {
            // 604UM Table 4-8: SRR1[13] flags the TEA cause, SRR1[30] (the
            // RI copy) reads zero, and the exception clears MSR[ME].
            ppc_exception(p, PPC_VEC_MCHECK, 0x00040000u, p->instruction_pc);
            p->srr1 &= ~PPC_MSR_RI;
            p->msr &= ~PPC_MSR_ME;
        } else {
            ppc_exception(p, PPC_VEC_MCHECK, 0, p->instruction_pc);
        }
    }
    *instructions = 0;
}
