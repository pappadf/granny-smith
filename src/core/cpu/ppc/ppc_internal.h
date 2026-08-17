// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_internal.h
// Internal state and shared helpers for the PPC (MPC601) core.
// All chapter/table citations refer to Motorola/IBM, "PowerPC 601 RISC
// Microprocessor User's Manual", 1995 (MPC601UM/AD).

#ifndef GS_CPU_PPC_INTERNAL_H
#define GS_CPU_PPC_INTERNAL_H

#include "ppc.h"

#include "memory.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// === MSR bits (Table 2-9; big-endian bit n = mask 1<<(31-n)) ===
#define PPC_MSR_EE  0x8000u // external interrupt enable (bit 16)
#define PPC_MSR_PR  0x4000u // privilege level: 1 = user (bit 17)
#define PPC_MSR_FP  0x2000u // floating-point available (bit 18)
#define PPC_MSR_ME  0x1000u // machine check enable (bit 19)
#define PPC_MSR_FE0 0x0800u // FP exception mode 0 (bit 20)
#define PPC_MSR_SE  0x0400u // single-step trace enable (bit 21)
#define PPC_MSR_FE1 0x0100u // FP exception mode 1 (bit 23)
#define PPC_MSR_EP  0x0040u // exception prefix: 1 = $FFFnnnnn (bit 25)
#define PPC_MSR_IT  0x0020u // instruction translation (bit 26)
#define PPC_MSR_DT  0x0010u // data translation (bit 27)
// All MSR bits the 601 implements — mtmsr/rfi writes are masked to this
// (601 has no POW/ILE/BE/RI; those bits read as zero).
#define PPC_MSR_MASK                                                                                                   \
    (PPC_MSR_EE | PPC_MSR_PR | PPC_MSR_FP | PPC_MSR_ME | PPC_MSR_FE0 | PPC_MSR_SE | PPC_MSR_FE1 | PPC_MSR_EP |         \
     PPC_MSR_IT | PPC_MSR_DT)

// === XER bits ===
#define PPC_XER_SO      0x80000000u
#define PPC_XER_OV      0x40000000u
#define PPC_XER_CA      0x20000000u
#define PPC_XER_BYTES   0x0000007Fu // string byte count, XER[25-31]
#define PPC_XER_CMPBYTE 0x0000FF00u // lscbx compare byte, XER[16-23]

// === Exception vector offsets (Table 5-2; §3.3 of the proposal) ===
#define PPC_VEC_RESET     0x00100u
#define PPC_VEC_MCHECK    0x00200u
#define PPC_VEC_DSI       0x00300u // data access
#define PPC_VEC_ISI       0x00400u // instruction access
#define PPC_VEC_EXTERNAL  0x00500u
#define PPC_VEC_ALIGNMENT 0x00600u
#define PPC_VEC_PROGRAM   0x00700u
#define PPC_VEC_FPUNAVAIL 0x00800u
#define PPC_VEC_DEC       0x00900u
#define PPC_VEC_IOERROR   0x00A00u // 601-only I/O controller interface error
#define PPC_VEC_SYSCALL   0x00C00u
#define PPC_VEC_TRACE     0x02000u // 601 run-mode/trace (not $00D00)

// SRR1 exception-specific bits for the program exception (Table 5-16)
#define PPC_SRR1_PROG_FPENABLED 0x00100000u // bit 11
#define PPC_SRR1_PROG_ILLEGAL   0x00080000u // bit 12
#define PPC_SRR1_PROG_PRIV      0x00040000u // bit 13
#define PPC_SRR1_PROG_TRAP      0x00020000u // bit 14

// DSISR bits for the data access exception (Table 5-10)
#define PPC_DSISR_NOTFOUND 0x40000000u // bit 1: no HTEG/BAT translation
#define PPC_DSISR_PROT     0x08000000u // bit 4: protection violation
#define PPC_DSISR_STORE    0x02000000u // bit 6: access was a store

// === Instruction state ===

// The 601 core state.  Plain data first; pointer fields last — the whole
// struct is written to the checkpoint stream and the pointers are nulled and
// re-planted on restore (the cpu.c double-free precedent).
struct ppc {
    // --- user register file ---
    uint32_t gpr[32];
    uint32_t pc; // address of the NEXT instruction to fetch
    uint32_t cr; // condition register (8 fields)
    uint32_t xer;
    uint32_t lr, ctr;
    uint32_t mq; // POWER MQ register (SPR 0)
    uint64_t fpr[32]; // raw IEEE-double bit patterns
    uint32_t fpscr;

    // --- supervisor register file ---
    uint32_t msr;
    uint32_t srr0, srr1;
    uint32_t sprg[4];
    uint32_t dsisr, dar;
    uint32_t dec; // decrementer value (time derivation lands with the PDM family)
    uint32_t sdr1;
    uint32_t ear;
    uint32_t pvr; // $00010001, read-only
    uint32_t hid0, hid1, iabr, dabr, pir; // 601 HID group: store-and-readback
    uint32_t rtcu, rtcl; // RTC pair (read SPR 4/5, write SPR 20/21)
    uint32_t batu[4], batl[4]; // 4 unified BAT pairs, 601 format
    uint32_t sr[16]; // segment registers
    // Which segments have T=1 (bit n = sr[n] bit 0) — data accesses
    // consult the segment's T bit even with MSR[DT]=0 (601UM §6.5.2),
    // and this mask keeps that check off the translation-off fast path.
    // Derived from sr[]; maintained by ppc_set_sr/ppc_recompute_sr_t_mask.
    uint32_t sr_t_mask;

    // --- execution state ---
    uint32_t instruction_pc; // address of the instruction being executed
    uint32_t reserve; // lwarx reservation held
    uint32_t reserve_addr;
    uint32_t ext_irq; // level of the external-interrupt line
    uint32_t dec_pending; // latched decrementer exception request
    int cpu_model; // CPU_MODEL_PPC601

    // --- time derivation (§3.7: exact-rational RTC/DEC) ---
    // ticks = cycles * tick_mul / tick_div, the reduced 7,833,600/freq
    // rational; tick_mul == 0 means unbound (unit tests: static SPRs).
    // rtcu/rtcl/dec above hold the values AT their rebase instant.
    uint32_t tick_mul, tick_div;
    uint64_t rtc_base_ticks; // RTC tick count when rtcu/rtcl were written
    uint64_t dec_base_ticks; // RTC tick count when dec was written

    // NOTE: the MMU/fetch translation caches live as file statics in
    // ppc_mmu.c, NOT here — they embed host pointers, and this struct is
    // written to the checkpoint stream verbatim (byte-determinism).

    // --- pointers (nulled on checkpoint restore, re-planted by owners) ---
    struct object *cpu_object; // machine.cpu node
    struct object *fpu_object; // machine.cpu.fpu (Phase E)
    struct object *mmu_object; // machine.cpu.mmu (Phase D)
    struct scheduler *scheduler; // time source (ppc_bind_time; NULL in tests)
};

// === Field extraction (BE bit numbering per 601UM Chapter 10 diagrams).
// Kept in sync with the #ifndef-guarded copy in ppc_decode.h (which serves
// the dependency-free disassembler TU).
#define PPC_OPCD(iw) ((iw) >> 26)
#define PPC_RT(iw)   (((iw) >> 21) & 31) // also RS, TO, BO, crfD<<2|..
#define PPC_RA(iw)   (((iw) >> 16) & 31) // also BI
#define PPC_RB(iw)   (((iw) >> 11) & 31) // also SH, NB
#define PPC_XO10(iw) (((iw) >> 1) & 0x3FF) // X/XL/XFX-form extended opcode
#define PPC_XO9(iw)  (((iw) >> 1) & 0x1FF) // XO-form (bit 21 = OE)
#define PPC_XO5(iw)  (((iw) >> 1) & 0x1F) // A-form (FP arithmetic)
#define PPC_OE(iw)   (((iw) >> 10) & 1)
#define PPC_RC(iw)   ((iw) & 1)
#define PPC_SIMM(iw) ((int32_t)(int16_t)(iw))
#define PPC_UIMM(iw) ((iw) & 0xFFFFu)
#define PPC_MB(iw)   (((iw) >> 6) & 31)
#define PPC_ME(iw)   (((iw) >> 1) & 31)
#define PPC_FRC(iw)  (((iw) >> 6) & 31) // A-form third operand
#define PPC_CRFD(iw) (((iw) >> 23) & 7)
#define PPC_CRFS(iw) (((iw) >> 18) & 7)

// (rA|0): a zero RA field reads as the value 0, not r0 (EA computation rule)
static inline uint32_t ppc_ra0(ppc_t *p, uint32_t iw) {
    uint32_t a = PPC_RA(iw);
    return a ? p->gpr[a] : 0;
}

// MASK(MB,ME): 1-bits from BE bit MB through BE bit ME, wrapping when
// MB > ME (601UM §10, rotate instructions).
static inline uint32_t ppc_mask(uint32_t mb, uint32_t me) {
    uint32_t m_begin = 0xFFFFFFFFu >> mb; // bits mb..31 set
    uint32_t m_end = (uint32_t)(0xFFFFFFFFu << (31 - me)); // bits 0..me set
    return (mb <= me) ? (m_begin & m_end) : (m_begin | m_end);
}

static inline uint32_t ppc_rotl(uint32_t v, uint32_t n) {
    n &= 31;
    return n ? ((v << n) | (v >> (32 - n))) : v;
}

// CR0 record form: LT/GT/EQ from the signed result, SO from XER[SO]
static inline void ppc_record_cr0(ppc_t *p, uint32_t result) {
    uint32_t f = ((int32_t)result < 0) ? 8u : (result == 0 ? 2u : 4u);
    if (p->xer & PPC_XER_SO)
        f |= 1u;
    p->cr = (p->cr & 0x0FFFFFFFu) | (f << 28);
}

// Set/clear XER[OV], keeping SO sticky on set
static inline void ppc_set_ov(ppc_t *p, int ov) {
    if (ov)
        p->xer |= PPC_XER_OV | PPC_XER_SO;
    else
        p->xer &= ~PPC_XER_OV;
}

static inline void ppc_set_ca(ppc_t *p, int ca) {
    if (ca)
        p->xer |= PPC_XER_CA;
    else
        p->xer &= ~PPC_XER_CA;
}

// === MMU front end (ppc_mmu.c, 601UM Ch. 6) =================================

// The one-page instruction-fetch window, refilled by ppc_fetch_fill and
// checked inline in the sprint loop.  host_adjust = host_base - lo, so
// LOAD_BE32(host_adjust + pc) fetches directly.  span == 0 = invalid.
typedef struct ppc_fetch_window {
    uint32_t lo, span;
    uintptr_t host_adjust;
} ppc_fetch_window_t;
extern ppc_fetch_window_t g_ppc_fetch;

// Refill the fetch window for pc (and return the word there via *iw).
// Returns false when the fetch raised ISI.
bool ppc_fetch_fill(ppc_t *p, uint32_t pc, uint32_t *iw);

// Full data translation (slow half of ppc_dxlate below).  On return
// false, *addr holds the address to access: the EA itself when the user
// SoA maps now cover it (logical fast path), else the physical address.
// True = exception raised (DSI / I/O-controller error) — abandon.
bool ppc_dxlate_slow(ppc_t *p, uint32_t iw, uint32_t *addr, bool store);

// dcbz's translated form: W/I alignment rule, T=1 no-op case.
// Returns 0 = proceed (zero at *addr), 1 = exception raised, 2 = no-op.
int ppc_dxlate_dcbz(ppc_t *p, uint32_t iw, uint32_t *addr);

// Rebuild sr_t_mask from sr[] (reset, checkpoint restore, shell pokes).
void ppc_recompute_sr_t_mask(ppc_t *p);

// Invalidation entry points.  invalidate_all: address-space change (SR/
// BAT/SDR1 value change, bank remap, checkpoint restore).  tlbie:
// congruence-class invalidation.  flush_fetch: MSR change (privilege or
// translation bits — fetch permissions are mode-dependent).
void ppc_mmu_invalidate_all(ppc_t *p);
void ppc_mmu_tlbie(ppc_t *p, uint32_t ea);
void ppc_mmu_flush_fetch(void);

// Segment-register write with change-triggered invalidation (the
// nanokernel reloads identical SR values wholesale on space touches).
void ppc_set_sr(ppc_t *p, uint32_t n, uint32_t v);

// Side-effect-free translation for the debug surfaces (no R/C update, no
// SoA fill, no exception).  data=true follows MSR[DT], else MSR[IT].
uint32_t ppc_mmu_translate_debug(ppc_t *p, uint32_t ea, bool data, bool *ok);

// The 68k world's view (user data context, translation forced on) for
// debug.mac — stable across supervisor/user stop contexts (§3.9e).
uint32_t ppc_mmu_translate_mac(ppc_t *p, uint32_t ea, bool *ok);

// Keep the SoA fast-path maps in sync with the (MSR[PR], MSR[DT]) pair —
// the one global obligation of a main CPU (proposal §3.5).  Called on
// every MSR write, exception entry, and rfi.  The user arrays hold the
// MMU's LOGICAL fills, so they are active only for translated user-mode
// data; every other mode runs on the machine's eager physical identity
// view in the supervisor arrays (supervisor-translated accesses rewrite
// their address in ppc_dxlate_slow before touching memory).
static inline void ppc_update_active_maps(ppc_t *p) {
    ppc_mmu_flush_fetch();
    if ((p->msr & (PPC_MSR_PR | PPC_MSR_DT)) == (PPC_MSR_PR | PPC_MSR_DT)) {
        g_active_read = g_user_read;
        g_active_write = g_user_write;
    } else {
        g_active_read = g_supervisor_read;
        g_active_write = g_supervisor_write;
    }
}

// Data-access translation gate, called with *addr = EA before any
// register writeback (a faulting access must leave the instruction
// abandoned with no side effects — the update forms depend on this).
// Fast paths: with translation off, only T=1 segments need the slow
// half (601UM §6.5.2 — SR[T] is consulted independent of MSR[DT]); with
// translation on in user mode the active maps hold logical fills, so a
// covered page needs no address rewrite at all.
static inline bool ppc_dxlate(ppc_t *p, uint32_t iw, uint32_t *addr, bool store) {
    if (!(p->msr & PPC_MSR_DT)) {
        if (__builtin_expect(!(p->sr_t_mask & (1u << (*addr >> 28))), 1))
            return false;
        return ppc_dxlate_slow(p, iw, addr, store);
    }
    if (p->msr & PPC_MSR_PR) {
        uintptr_t e = (store ? g_active_write : g_active_read)[*addr >> PAGE_SHIFT];
        if (__builtin_expect(e != 0, 1))
            return false;
    }
    return ppc_dxlate_slow(p, iw, addr, store);
}

// Live RTC/DEC derivation (§3.7).  With no time binding these return the
// stored SPR values unchanged.
uint64_t ppc_ticks_now(ppc_t *p);
uint32_t ppc_rtcu_now(ppc_t *p);
uint32_t ppc_rtcl_now(ppc_t *p);
uint32_t ppc_dec_now(ppc_t *p);
void ppc_dec_arm(ppc_t *p); // (re)schedule the DEC sign-transition event

// === Shared entry points (ppc.c) ===

// Raise an exception: SRR0 = resume_pc, SRR1 = srr1_hi | MSR[16-31], MSR
// mutated per the per-exception Register Settings tables (EE/PR/FP/FE0/SE/
// FE1/IT/DT cleared; ME and EP kept), PC vectored via MSR[EP].
void ppc_exception(ppc_t *p, uint32_t vector, uint32_t srr1_hi, uint32_t resume_pc);

// DSISR image for an alignment exception (601UM Table 5-13): opcode fields
// repacked so the handler can emulate the access without re-reading the
// instruction.  X-form (opcode 31) and D-form encode differently.
static inline uint32_t ppc_align_dsisr(uint32_t iw) {
    uint32_t dsisr = 0;
    if (PPC_OPCD(iw) == 31) {
        dsisr |= ((iw >> 2) & 1u) << 16 | ((iw >> 1) & 1u) << 15; // DSISR[15-16] = instr bits 29-30
        dsisr |= ((iw >> 6) & 1u) << 14; // DSISR[17] = instr bit 25
        dsisr |= ((iw >> 7) & 0xFu) << 10; // DSISR[18-21] = instr bits 21-24
    } else {
        dsisr |= ((iw >> 26) & 1u) << 14; // DSISR[17] = instr bit 5
        dsisr |= ((iw >> 27) & 0xFu) << 10; // DSISR[18-21] = instr bits 1-4
    }
    dsisr |= ((iw >> 21) & 0x1Fu) << 5; // DSISR[22-26] = source/destination
    dsisr |= (iw >> 16) & 0x1Fu; // DSISR[27-31] = rA
    return dsisr;
}

// Raise the alignment exception for the access described by iw/ea.
static inline void ppc_align_exception(ppc_t *p, uint32_t iw, uint32_t ea) {
    p->dar = ea;
    p->dsisr = ppc_align_dsisr(iw);
    ppc_exception(p, PPC_VEC_ALIGNMENT, 0, p->instruction_pc);
}

// mfspr/mtspr dispatch (601 SPR map incl. the RTC read/write asymmetry).
// Returns false if the SPR access raised an exception (privileged from user
// mode); a no-op for unimplemented SPR numbers per the 601UM mfspr/mtspr
// pages ("treated as a no-op").
bool ppc_mfspr(ppc_t *p, uint32_t iw);
bool ppc_mtspr(ppc_t *p, uint32_t iw);

// Exception-raise guards shared by the OP_ table (ppc_ops.h).  Each returns
// true when the fault was taken (the instruction body must abandon).
static inline bool ppc_priv_check(ppc_t *p) {
    if (p->msr & PPC_MSR_PR) {
        ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_PRIV, p->instruction_pc);
        return true;
    }
    return false;
}

// FP-availability gate for every FP opcode (601UM §5.4.8).
static inline bool ppc_fp_check(ppc_t *p) {
    if (!(p->msr & PPC_MSR_FP)) {
        ppc_exception(p, PPC_VEC_FPUNAVAIL, 0, p->instruction_pc);
        return true;
    }
    return false;
}

// Multi-statement instruction bodies (ppc_run.c) referenced by the OP_
// one-liner table: branches, divides with their deterministic-undefined
// results, string transfers, and the store-conditional.
void ppc_illegal_op(ppc_t *p, uint32_t iw);
void ppc_do_b(ppc_t *p, uint32_t iw);
void ppc_do_bc(ppc_t *p, uint32_t iw);
void ppc_do_bclr(ppc_t *p, uint32_t iw);
void ppc_do_bcctr(ppc_t *p, uint32_t iw);
void ppc_do_divw(ppc_t *p, uint32_t iw);
void ppc_do_divwu(ppc_t *p, uint32_t iw);
void ppc_do_doz(ppc_t *p, uint32_t iw);
void ppc_do_mul(ppc_t *p, uint32_t iw);
void ppc_do_div(ppc_t *p, uint32_t iw);
void ppc_do_divs(ppc_t *p, uint32_t iw);
void ppc_do_sraw(ppc_t *p, uint32_t iw);
void ppc_do_lmw(ppc_t *p, uint32_t iw);
void ppc_do_stmw(ppc_t *p, uint32_t iw);
void ppc_do_lswi(ppc_t *p, uint32_t iw);
void ppc_do_lswx(ppc_t *p, uint32_t iw);
void ppc_do_stswi(ppc_t *p, uint32_t iw);
void ppc_do_stswx(ppc_t *p, uint32_t iw);
void ppc_do_lscbx(ppc_t *p, uint32_t iw);
void ppc_do_stwcx(ppc_t *p, uint32_t iw);
void ppc_ecx_fault(ppc_t *p, uint32_t ea, bool store);

// FP surface (ppc_fpu.c): single<->double conversion in integer code
// (WASM/native byte determinism, proposal §3.6), compares, the FPSCR
// instruction semantics, and the Phase-E arithmetic wrappers over the
// integer-only kernel in ppc_softfp.c.
uint64_t ppc_f32_to_f64(uint32_t s);
uint32_t ppc_f64_to_f32(uint64_t d);
void ppc_fcmp(ppc_t *p, uint32_t iw, bool ordered);
void ppc_do_mcrfs(ppc_t *p, uint32_t iw);
void ppc_fp_arith(ppc_t *p, uint32_t iw, int op, bool single);
void ppc_do_frsp(ppc_t *p, uint32_t iw);
void ppc_do_fctiw(ppc_t *p, uint32_t iw, bool round_to_zero);
void ppc_do_mtfsf(ppc_t *p, uint32_t iw);
void ppc_do_mtfsfi(ppc_t *p, uint32_t iw);
void ppc_do_mtfsb(ppc_t *p, uint32_t iw, bool set);
void ppc_fp_trap_check(ppc_t *p);

// The interpreter proper (ppc_run.c, generated from ppc_decode.h):
// execute one instruction word.
void ppc_execute(ppc_t *restrict p, uint32_t iw);

#endif // GS_CPU_PPC_INTERNAL_H
