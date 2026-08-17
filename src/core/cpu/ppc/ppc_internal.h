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

    // --- instruction-fetch translation cache (MSR[IT] BAT path) ---
    // While fetch EA is in [fetch_lo, fetch_lo+fetch_span), physical fetch
    // address = EA + fetch_delta.  span == 0 means invalid; refilled by
    // ppc_fetch_fill, invalidated on MSR/BAT writes.
    uint32_t fetch_lo, fetch_span, fetch_delta;

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

// Keep the SoA fast-path maps in sync with MSR[PR] (the one global
// obligation of a main CPU — proposal §3.5).  Called on every MSR write,
// exception entry, and rfi.  Doubles as the fetch-translation cache
// invalidation point: every MSR[IT] change routes through here.
static inline void ppc_update_active_maps(ppc_t *p) {
    p->fetch_span = 0;
    if (p->msr & PPC_MSR_PR) {
        g_active_read = g_user_read;
        g_active_write = g_user_write;
    } else {
        g_active_read = g_supervisor_read;
        g_active_write = g_supervisor_write;
    }
}

// === Address translation, Phase-C subset (601UM Ch. 6) ======================
// BAT match (601 format, unified I/D) + the T=1 memory-forced I/O-controller
// segments HWInit runs on.  T=0 hashed-table translation is Phase D and
// raises a loud DSI/ISI so a premature dependence is visible, not silent.

// 601 BAT match (Tables 6-11/6-12): BATU = BLPI[0-14]|WIM|Ks/Ku|PP,
// BATL = PBN[0-14]|V(bit 25)|BSM[26-31].  BSM is a ones-mask selecting the
// block size (000000 = 128 KB ... 111111 = 8 MB).
static inline bool ppc_bat_xlate(ppc_t *p, uint32_t ea, uint32_t *pa) {
    for (int i = 0; i < 4; i++) {
        uint32_t bl = p->batl[i];
        if (!(bl & 0x40u))
            continue; // V
        uint32_t cmp_mask = ~(((bl & 0x3Fu) << 17) | 0x1FFFFu); // bits above the block
        if ((ea & cmp_mask) != (p->batu[i] & cmp_mask))
            continue;
        *pa = ((bl & 0xFFFE0000u) & cmp_mask) | (ea & ~cmp_mask);
        return true;
    }
    return false;
}

// Data-access translation when MSR[DT] is on (ppc.c).  Returns true when the
// access faulted (exception raised — abandon the instruction); otherwise *ea
// has been rewritten to the physical address.  Store-ness (for the DSISR
// image) is derived from the instruction word.
bool ppc_dxlate_slow(ppc_t *p, uint32_t iw, uint32_t *ea);

static inline bool ppc_dxlate(ppc_t *p, uint32_t iw, uint32_t *ea) {
    if (!(p->msr & PPC_MSR_DT))
        return false;
    return ppc_dxlate_slow(p, iw, ea);
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
// (WASM/native byte determinism, proposal §3.6), compares, mcrfs, and the
// Phase-E backstop for the arithmetic datapath.
uint64_t ppc_f32_to_f64(uint32_t s);
uint32_t ppc_f64_to_f32(uint64_t d);
void ppc_fcmp(ppc_t *p, uint32_t iw, bool ordered);
void ppc_do_mcrfs(ppc_t *p, uint32_t iw);
void ppc_fpu_unimpl(ppc_t *p, uint32_t iw);

// The interpreter proper (ppc_run.c, generated from ppc_decode.h):
// execute one instruction word.
void ppc_execute(ppc_t *restrict p, uint32_t iw);

#endif // GS_CPU_PPC_INTERNAL_H
