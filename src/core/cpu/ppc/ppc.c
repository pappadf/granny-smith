// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc.c
// PPC (MPC601/MPC604) core: lifecycle, exception machinery, SPR file,
// scheduler and debugger adapters, object class.  The interpreter lives in
// ppc_run.c; model-specific behavior is discriminated on ppc_t.cpu_model.

#include "ppc_internal.h"
#include "ppc_softfp.h"

#include <stdlib.h> // getenv (TEMP diagnostic PVR override)

#include "alias.h"
#include "log.h"
#include "machine_profile.h"
#include "object.h"
#include "ppc_disasm.h"
#include "scheduler.h"
#include "system.h"
#include "value.h"

LOG_USE_CATEGORY_NAME("ppc");

// Forward declaration — class descriptor is at the bottom of the file.
extern const class_desc_t ppc_cpu_class;

// === Exception machinery ====================================================

// Publish the BAT registers to the fetch side (ppc_t.ibatu_cs).
//
// A BAT write does not steer instruction fetching until the processor
// context-synchronizes: the instructions after the mtspr were fetched and
// translated before it retired.  Software depends on that.  MkLinux DR3's
// start.s is the case that forced the model — on the 601 it writes
//
//     mtibatu 0,r7    ; BLPI=0, PP=2
//     mtibatl 0,r8    ; PBN=0, V=1, BSM=8M
//
// with only a trailing sync/isync.  Between the two writes the pair reads
// as "EA 0-8 MB -> the PBN the MacOS loader left behind", which relocates
// the kernel's own text off the end of RAM; applying that to the fetch of
// the very next instruction executes whatever the fill pattern decodes to.
// Data accesses are NOT deferred: they issue after the mtspr retires, so
// they legitimately see the new value.
void ppc_context_sync(ppc_t *p) {
    bool changed = false;
    for (int i = 0; i < 4; i++) {
        if (p->ibatu_cs[i] != p->batu[i] || p->ibatl_cs[i] != p->batl[i])
            changed = true;
        p->ibatu_cs[i] = p->batu[i];
        p->ibatl_cs[i] = p->batl[i];
    }
    // Only a real change costs a fetch-cache flush; guests re-write
    // identical BAT values constantly (the nanokernel's SetSpace).
    if (changed)
        ppc_mmu_flush_fetch();
}

// Raise an exception (601UM §5.4 / 604UM §4.3, per-exception Register
// Settings tables): SRR0 = resume_pc; SRR1 = exception-specific high bits
// | MSR[16-31]; MSR keeps ME/EP (+PM on the 604) and clears everything
// else — which covers the 604's POW/BE/RI-cleared rows too; PC = vector,
// prefixed $FFF00000 when MSR[EP] is set.
void ppc_exception(ppc_t *p, uint32_t vector, uint32_t srr1_hi, uint32_t resume_pc) {
    ppc_context_sync(p); // taking an exception is context-synchronizing
    p->srr0 = resume_pc;
    p->srr1 = (srr1_hi & 0xFFFF0000u) | (p->msr & 0x0000FFFFu);
    p->msr &= ppc_msr_exception_keep(p);
    ppc_update_active_maps(p);
    p->pc = ((p->msr & PPC_MSR_EP) ? 0xFFF00000u : 0u) + vector;
    // Record in the shared exception trace ring (§3.9c field mapping:
    // vbr slot = MSR, format_frame = vector offset, fault_addr = DAR).
    exc_trace_record(vector, resume_pc, p->srr0, p->dar, 0, p->msr, 0, (uint16_t)vector, 0);
}

// Take a pending external/decrementer interrupt when MSR[EE] allows.
// Level-sensitive: called before each instruction and from the sched-if
// poll hook (the just-re-enabled case after rfi/mtmsr, proposal §4.6).
void ppc_poll_interrupt(ppc_t *p) {
    if (!(p->msr & PPC_MSR_EE))
        return;
    if (p->ext_irq) {
        ppc_exception(p, PPC_VEC_EXTERNAL, 0, p->pc);
    } else if (p->dec_pending) {
        p->dec_pending = 0;
        ppc_exception(p, PPC_VEC_DEC, 0, p->pc);
    }
}

void ppc_set_ext_irq(ppc_t *p, bool level) {
    uint32_t next = level ? 1u : 0u;
    if (next != p->ext_irq) // temporary diagnostics for the 604 boot wall
        LOG(3, "ext_irq %u->%u pc=$%08X msr=$%08X", p->ext_irq, next, p->pc, p->msr);
    p->ext_irq = next;
}

// === RTC/TB/DEC time derivation (§3.7; TNT proposal §4.4) ===================

// Exact rational cycles→ticks: q*mul + r*mul/div never overflows (r < div,
// both 32-bit after reduction) and is exact over any interval.
uint64_t ppc_ticks_now(ppc_t *p) {
    if (!p->tick_mul || !p->scheduler)
        return 0;
    uint64_t cycles = scheduler_cpu_cycles(p->scheduler);
    return (cycles / p->tick_div) * p->tick_mul + (cycles % p->tick_div) * p->tick_mul / p->tick_div;
}

// RTCL advances 128 units per 7.8336 MHz tick and rolls into RTCU at 10^9
// (601UM §2.3.3.4); RTCU/RTCL hold their rebase-instant values.
static void ppc_rtc_now(ppc_t *p, uint32_t *rtcu, uint32_t *rtcl) {
    if (!p->tick_mul) {
        *rtcu = p->rtcu;
        *rtcl = p->rtcl;
        return;
    }
    uint64_t elapsed = ppc_ticks_now(p) - p->rtc_base_ticks;
    uint64_t units = p->rtcl + elapsed * 128u;
    *rtcu = p->rtcu + (uint32_t)(units / 1000000000u);
    *rtcl = (uint32_t)(units % 1000000000u) & 0x3FFFFF80u;
}

uint32_t ppc_rtcu_now(ppc_t *p) {
    uint32_t u, l;
    ppc_rtc_now(p, &u, &l);
    return u;
}

uint32_t ppc_rtcl_now(ppc_t *p) {
    uint32_t u, l;
    ppc_rtc_now(p, &u, &l);
    return l;
}

// 604 timebase: a plain 64-bit counter advancing one unit per tick (604UM
// §1.3.2.2 — the tick rate is bus/4, bound by ppc_bind_time).  rtcu/rtcl
// hold TBU/TBL at the rebase instant.
uint64_t ppc_tb_now(ppc_t *p) {
    uint64_t base = ((uint64_t)p->rtcu << 32) | p->rtcl;
    if (!p->tick_mul)
        return base;
    return base + (ppc_ticks_now(p) - p->rtc_base_ticks);
}

// DEC units per tick, as a shift: the 601 decrements 128 RTCL-units per
// 7.8336 MHz tick (DecClockRateHz = 1,002,700,800 — the constant HWInit
// hard-codes); the 604 decrements once per timebase tick (604UM §4.5.9).
static inline int ppc_dec_shift(const ppc_t *p) {
    return ppc_is_604(p) ? 0 : 7;
}

uint32_t ppc_dec_now(ppc_t *p) {
    if (!p->tick_mul)
        return p->dec;
    uint64_t elapsed = ppc_ticks_now(p) - p->dec_base_ticks;
    return p->dec - (uint32_t)(elapsed << ppc_dec_shift(p));
}

// DEC expiry event: latch the exception request (taken when MSR[EE] allows)
// and re-arm for the next wrap-around transition (~4.3 s away).
static void ppc_dec_event(void *source, uint64_t data) {
    (void)data;
    ppc_t *p = (ppc_t *)source;
    p->dec_pending = 1;
    ppc_dec_arm(p);
}

// Schedule the next 0→negative transition of DEC as a scheduler event.
void ppc_dec_arm(ppc_t *p) {
    if (!p->tick_mul || !p->scheduler)
        return;
    remove_event(p->scheduler, ppc_dec_event, p);
    uint32_t dec = ppc_dec_now(p);
    // Ticks until the sign transition: a non-negative DEC crosses below zero
    // after floor(dec/units_per_tick)+1 ticks; an already-negative DEC
    // transitions again only after wrapping through zero.
    int shift = ppc_dec_shift(p);
    uint64_t ticks_until;
    if ((int32_t)dec >= 0)
        ticks_until = (dec >> shift) + 1u;
    else
        ticks_until = (((uint64_t)dec + 0x100000000ull) >> shift) + 1u;
    // ticks→cycles, rounded up so the event never fires early.
    uint64_t cycles = (ticks_until * p->tick_div + p->tick_mul - 1) / p->tick_mul;
    scheduler_new_cpu_event(p->scheduler, ppc_dec_event, p, 0, cycles, 0);
}

void ppc_bind_time(ppc_t *p, struct scheduler *s, uint32_t freq_hz, uint32_t tick_hz) {
    // Reduce tick_hz/freq_hz once; all derivations use the reduced pair.
    uint32_t a = tick_hz, b = freq_hz;
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    p->scheduler = s;
    p->tick_mul = tick_hz / a;
    p->tick_div = freq_hz / a;
    scheduler_new_event_type(s, "ppc", p, "dec", ppc_dec_event);
    // No rebase: the stored base ticks are in the scheduler-cycle-derived
    // tick domain, which checkpoint restore reproduces exactly (cold init
    // starts both at zero).  Only the expiry event needs re-arming.
    ppc_dec_arm(p);
}

// === SPR file (mfspr/mtspr Tables 10-4/10-5) ================================

// Common privilege gate: a supervisor-level SPR touched from user mode takes
// the privileged-instruction program exception.
static bool spr_priv_fault(ppc_t *p, uint32_t iw) {
    if (p->msr & PPC_MSR_PR) {
        ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_PRIV, p->instruction_pc);
        return true;
    }
    (void)iw;
    return false;
}

// SPR number: the two 5-bit instruction halves are swapped (601UM mfspr page)
static inline uint32_t spr_number(uint32_t iw) {
    return (((iw) >> 16) & 0x1Fu) | ((((iw) >> 11) & 0x1Fu) << 5);
}

// SPR number not defined for the active model.  Both models: SPR[0]=1 from
// user mode is the privileged program exception (bit 4 of the swapped `n`).
// Past that they diverge: the 601 treats the access as a no-op (601UM
// mfspr page "treated as a no-op"); the 604 fully decodes the SPR field
// and raises the illegal-instruction program exception (604UM §4.5.7).
// Returns true when an exception was raised.
static bool spr_undefined(ppc_t *p, uint32_t n, bool is_read) {
    (void)is_read; // only the (compiled-out-in-tests) log consumes it
    if ((n & 0x10u) && (p->msr & PPC_MSR_PR)) {
        ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_PRIV, p->instruction_pc);
        return true;
    }
    if (ppc_is_604(p)) {
        ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_ILLEGAL, p->instruction_pc);
        return true;
    }
    LOG(5, "%s %u: unimplemented SPR (no-op)", is_read ? "mfspr" : "mtspr", n);
    return false;
}

bool ppc_mfspr(ppc_t *p, uint32_t iw) {
    uint32_t n = spr_number(iw);
    uint32_t d = PPC_RT(iw);
    uint32_t v;
    switch (n) {
    case 0: // MQ: 601-only (the 604 rejects the POWER SPRs — 604UM §2.3)
        if (ppc_is_604(p))
            goto undefined;
        v = p->mq;
        break;
    case 1:
        v = p->xer;
        break;
    case 4: // RTC reads use SPR 4/5 in EVERY mode (601 asymmetry); no RTC on the 604
        if (ppc_is_604(p))
            goto undefined;
        v = ppc_rtcu_now(p);
        break;
    case 5:
        if (ppc_is_604(p))
            goto undefined;
        v = ppc_rtcl_now(p);
        break;
    case 6: // POWER user-level DEC read, 601-supported (601UM Table 10-4 note 3)
        if (ppc_is_604(p))
            goto undefined;
        v = ppc_dec_now(p);
        break;
    case 8:
        v = p->lr;
        break;
    case 9:
        v = p->ctr;
        break;
    case 18:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->dsisr;
        break;
    case 19:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->dar;
        break;
    case 22:
        if (spr_priv_fault(p, iw))
            return false;
        v = ppc_dec_now(p);
        break;
    case 25:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->sdr1;
        break;
    case 26:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->srr0;
        break;
    case 27:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->srr1;
        break;
    case 272:
    case 273:
    case 274:
    case 275:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->sprg[n - 272];
        break;
    case 282:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->ear;
        break;
    case 287:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->pvr;
        break;
    case 528:
    case 529:
    case 530:
    case 531:
    case 532:
    case 533:
    case 534:
    case 535: { // 601 unified BATs / 604 IBATs — same storage
        if (spr_priv_fault(p, iw))
            return false;
        uint32_t pair = (n - 528) >> 1;
        v = (n & 1) ? p->batl[pair] : p->batu[pair];
        break;
    }
    case 536:
    case 537:
    case 538:
    case 539:
    case 540:
    case 541:
    case 542:
    case 543: { // DBATs: 604 only (PEM Table 2-8)
        if (!ppc_is_604(p))
            goto undefined;
        if (spr_priv_fault(p, iw))
            return false;
        uint32_t pair = (n - 536) >> 1;
        v = (n & 1) ? p->dbatl[pair] : p->dbatu[pair];
        break;
    }
    case 952: // MMCR0 — 604 performance monitor group: read-zero stubs
    case 953: // PMC1     (TNT proposal §4.2; the $00F00 interrupt never fires)
    case 954: // PMC2
    case 955: // SIA
    case 959: // SDA
        if (!ppc_is_604(p))
            goto undefined;
        if (spr_priv_fault(p, iw))
            return false;
        v = 0;
        break;
    case 1008:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->hid0;
        break;
    case 1009: // HID1: 601 only (the 604 defines no HID1)
        if (ppc_is_604(p))
            goto undefined;
        if (spr_priv_fault(p, iw))
            return false;
        v = p->hid1;
        break;
    case 1010:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->iabr;
        break;
    case 1013:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->dabr;
        break;
    case 1023:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->pir;
        break;
    default:
    undefined:
        return !spr_undefined(p, n, true);
    }
    p->gpr[d] = v;
    return true;
}

// mftb/mftbu (opcode 31 xo 371, 604-only — the 601 traps the opcode
// before reaching here).  User-readable in every mode (PEM §2.2.1); the
// TBR field uses the same swapped halves as an SPR number, and only 268
// (TBL) / 269 (TBU) are defined — anything else is an invalid form
// taking the illegal-instruction program exception (PEM mftb page).
void ppc_do_mftb(ppc_t *p, uint32_t iw) {
    uint32_t n = spr_number(iw);
    uint64_t tb = ppc_tb_now(p);
    if (n == 268)
        p->gpr[PPC_RT(iw)] = (uint32_t)tb;
    else if (n == 269)
        p->gpr[PPC_RT(iw)] = (uint32_t)(tb >> 32);
    else
        ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_ILLEGAL, p->instruction_pc);
}

bool ppc_mtspr(ppc_t *p, uint32_t iw) {
    uint32_t n = spr_number(iw);
    uint32_t v = p->gpr[PPC_RT(iw)];
    switch (n) {
    case 0: // MQ: 601-only
        if (ppc_is_604(p))
            goto undefined;
        p->mq = v;
        break;
    case 1:
        p->xer = v;
        break;
    case 8:
        p->lr = v;
        break;
    case 9:
        p->ctr = v;
        break;
    case 18:
        if (spr_priv_fault(p, iw))
            return false;
        p->dsisr = v;
        break;
    case 19:
        if (spr_priv_fault(p, iw))
            return false;
        p->dar = v;
        break;
    case 20: // RTC writes use SPR 20/21 (supervisor); reads use 4/5; 601-only
        if (ppc_is_604(p))
            goto undefined;
        if (spr_priv_fault(p, iw))
            return false;
        p->rtcl = ppc_rtcl_now(p); // keep RTCL's live phase across the rebase
        p->rtcu = v;
        p->rtc_base_ticks = ppc_ticks_now(p);
        break;
    case 21:
        if (ppc_is_604(p))
            goto undefined;
        if (spr_priv_fault(p, iw))
            return false;
        p->rtcu = ppc_rtcu_now(p);
        p->rtcl = v & 0x3FFFFF80u; // RTCL: bits 25-31 and 0-1 read as zero
        p->rtc_base_ticks = ppc_ticks_now(p);
        break;
    case 22:
        if (spr_priv_fault(p, iw))
            return false;
        p->dec = v;
        p->dec_base_ticks = ppc_ticks_now(p);
        p->dec_pending = 0; // re-arming clears the latched expiry
        ppc_dec_arm(p);
        break;
    case 25:
        if (spr_priv_fault(p, iw))
            return false;
        if (p->sdr1 != v) {
            p->sdr1 = v;
            ppc_mmu_invalidate_all(p); // the whole HTAB moved
        }
        break;
    case 26:
        if (spr_priv_fault(p, iw))
            return false;
        p->srr0 = v;
        break;
    case 27:
        if (spr_priv_fault(p, iw))
            return false;
        p->srr1 = v;
        break;
    case 272:
    case 273:
    case 274:
    case 275:
        if (spr_priv_fault(p, iw))
            return false;
        p->sprg[n - 272] = v;
        break;
    case 282:
        if (spr_priv_fault(p, iw))
            return false;
        p->ear = v;
        break;
    case 284: // TBL write (604; reads go through mftb — PEM §2.3.1)
        if (!ppc_is_604(p))
            goto undefined;
        if (spr_priv_fault(p, iw))
            return false;
        p->rtcu = (uint32_t)(ppc_tb_now(p) >> 32); // TBU keeps counting across the rebase
        p->rtcl = v;
        p->rtc_base_ticks = ppc_ticks_now(p);
        break;
    case 285: // TBU write (604)
        if (!ppc_is_604(p))
            goto undefined;
        if (spr_priv_fault(p, iw))
            return false;
        p->rtcl = (uint32_t)ppc_tb_now(p);
        p->rtcu = v;
        p->rtc_base_ticks = ppc_ticks_now(p);
        break;
    case 528:
    case 529:
    case 530:
    case 531:
    case 532:
    case 533:
    case 534:
    case 535: { // 601 unified BATs / 604 IBATs — same storage
        if (spr_priv_fault(p, iw))
            return false;
        uint32_t pair = (n - 528) >> 1;
        uint32_t *slot = (n & 1) ? &p->batl[pair] : &p->batu[pair];
        if (*slot != v) {
            *slot = v;
            // Change-triggered only: the nanokernel rewrites identical
            // BAT values on every 601 space reload (SetSpace).
            ppc_mmu_invalidate_all(p);
        }
        break;
    }
    case 536:
    case 537:
    case 538:
    case 539:
    case 540:
    case 541:
    case 542:
    case 543: { // DBATs: 604 only
        if (!ppc_is_604(p))
            goto undefined;
        if (spr_priv_fault(p, iw))
            return false;
        uint32_t pair = (n - 536) >> 1;
        uint32_t *slot = (n & 1) ? &p->dbatl[pair] : &p->dbatu[pair];
        if (*slot != v) {
            *slot = v;
            ppc_mmu_invalidate_all(p);
        }
        break;
    }
    case 952: // 604 performance monitor group: write-ignore stubs
    case 953:
    case 954:
    case 955:
    case 959:
        if (!ppc_is_604(p))
            goto undefined;
        if (spr_priv_fault(p, iw))
            return false;
        break;
    case 1008:
        if (spr_priv_fault(p, iw))
            return false;
        p->hid0 = v;
        break;
    case 1009: // HID1: 601 only
        if (ppc_is_604(p))
            goto undefined;
        if (spr_priv_fault(p, iw))
            return false;
        p->hid1 = v;
        break;
    case 1010:
        if (spr_priv_fault(p, iw))
            return false;
        p->iabr = v;
        break;
    case 1013:
        if (spr_priv_fault(p, iw))
            return false;
        p->dabr = v;
        break;
    case 1023:
        if (spr_priv_fault(p, iw))
            return false;
        p->pir = v;
        break;
    default:
    undefined:
        return !spr_undefined(p, n, false);
    }
    return true;
}

// === Public register accessors ==============================================

uint32_t ppc_get_pc(ppc_t *restrict p) {
    return p->pc;
}

void ppc_set_pc(ppc_t *restrict p, uint32_t pc) {
    p->pc = pc;
}

uint32_t ppc_get_gpr(ppc_t *restrict p, int n) {
    assert(n >= 0 && n < 32);
    return p->gpr[n];
}

void ppc_set_gpr(ppc_t *restrict p, int n, uint32_t value) {
    assert(n >= 0 && n < 32);
    p->gpr[n] = value;
}

uint32_t ppc_get_msr(ppc_t *restrict p) {
    return p->msr;
}

void ppc_set_msr(ppc_t *restrict p, uint32_t value) {
    p->msr = value & ppc_msr_mask(p);
    ppc_update_active_maps(p);
}

bool ppc_is_supervisor(ppc_t *restrict p) {
    return !(p->msr & PPC_MSR_PR);
}

// === Lifecycle ==============================================================

// Hard-reset register state per model (601UM Table 5-8; 604UM §8.8.4 —
// HRESET sets only MSR[IP], and the 604's HID0 comes up all-zero with the
// caches and BHT disabled).  The cpu_model itself survives the reset.
void ppc_reset(ppc_t *p) {
    struct object *keep_cpu = p->cpu_object;
    struct object *keep_fpu = p->fpu_object;
    struct object *keep_mmu = p->mmu_object;
    int keep_model = p->cpu_model;
    memset(p, 0, sizeof(*p));
    p->cpu_object = keep_cpu;
    p->fpu_object = keep_fpu;
    p->mmu_object = keep_mmu;
    p->cpu_model = keep_model;
    if (ppc_is_604(p)) {
        p->msr = PPC_MSR_EP; // $00000040 (IP only)
        p->pvr = 0x00040103u; // 604, revision 1.3 (chosen constant; the kernel keys on the $0004 half)
    } else {
        p->msr = PPC_MSR_ME | PPC_MSR_EP; // $00001040
        p->pvr = 0x00010001u;
        p->hid0 = 0x80010080u;
    }
    // TEMP diagnostic (604 boot-wall hunt): let a run present a foreign PVR so
    // the ROM/kernel select the other CPU's personality against this model's
    // semantics.  Env-gated, inert otherwise.
    {
        const char *s = getenv("GS_PVR_OVERRIDE");
        if (s)
            p->pvr = (uint32_t)strtoul(s, NULL, 16);
    }
    p->pc = 0xFFF00100u; // reset vector, MSR[EP]=1 on both models
    p->instruction_pc = p->pc;
    ppc_mmu_invalidate_all(p); // translation state gone with the SRs/BATs
    ppc_context_sync(p); // the fetch-side BAT view starts out cleared too
}

// === `$reg` aliases (main-CPU privilege per cores.md) =======================

static void register_alias_or_warn(const char *name, const char *path) {
    char err[160];
    if (alias_register_builtin(name, path, err, sizeof(err)) < 0)
        LOG(0, "ppc: built-in alias '$%s' → '%s' rejected: %s", name, path, err);
}

static void register_ppc_aliases(void) {
    register_alias_or_warn("pc", "machine.cpu.pc");
    register_alias_or_warn("lr", "machine.cpu.lr");
    register_alias_or_warn("ctr", "machine.cpu.ctr");
    register_alias_or_warn("cr", "machine.cpu.cr");
    register_alias_or_warn("msr", "machine.cpu.msr");
    register_alias_or_warn("xer", "machine.cpu.xer");
    for (int i = 0; i < 32; i++) {
        char name[8], path[24];
        snprintf(name, sizeof(name), "r%d", i);
        snprintf(path, sizeof(path), "machine.cpu.r%d", i);
        register_alias_or_warn(name, path);
    }
}

// Memory-logpoint installs reshape the SoA fast path behind the MMU's
// back; drop every cache that could bypass the slow path (the xtlb's
// physical rewrites included — see ppc_mmu_logpoints_changed).
static void ppc_fastpath_changed(void) {
    ppc_mmu_logpoints_changed();
}

// The instance behind the parameterless memory hooks below (one main CPU
// per machine; rebound on every ppc_init).
static ppc_t *g_hook_ppc;

// g_mem_logical_xlate: current-data-context logical→physical for the
// memory slow path's logpoint resolution.  Side-effect-free.
static uint32_t ppc_hook_logical_xlate(uint32_t addr, bool *ok) {
    if (!g_hook_ppc) {
        *ok = false;
        return addr;
    }
    return ppc_mmu_translate_debug(g_hook_ppc, addr, true, ok);
}

ppc_t *ppc_init(checkpoint_t *checkpoint, int cpu_model) {
    ppc_t *p = (ppc_t *)malloc(sizeof(ppc_t));
    if (!p)
        return NULL;
    assert(cpu_model == CPU_MODEL_PPC601 || cpu_model == CPU_MODEL_PPC604);

    // The user SoA arrays carry this MMU's logical fills — the generic
    // identity-restore paths must leave them alone (memory.h).
    g_user_soa_reserved = true;
    g_mem_fastpath_changed = ppc_fastpath_changed;
    g_hook_ppc = p;
    g_mem_logical_xlate = ppc_hook_logical_xlate;

    if (checkpoint) {
        // The stream carries the whole struct including save-time pointers;
        // null them so the bindings below are rebuilt for THIS machine
        // (the cpu.c same-process-restore double-free precedent).
        system_read_checkpoint_data(checkpoint, p, sizeof(ppc_t));
        p->cpu_object = NULL;
        p->fpu_object = NULL;
        p->mmu_object = NULL;
        p->scheduler = NULL; // re-planted by ppc_bind_time
        p->tick_mul = p->tick_div = 0;
        // The MMU caches are derived state and refill lazily; the T-bit
        // mask is derived from the restored SRs.
        ppc_recompute_sr_t_mask(p);
        ppc_mmu_invalidate_all(p);
        ppc_update_active_maps(p);
        ppc_context_sync(p); // restoring a checkpoint is context-synchronizing
    } else {
        memset(p, 0, sizeof(ppc_t));
        p->cpu_model = cpu_model; // ppc_reset keeps the model
        ppc_reset(p);
    }

    // Object-tree binding: the main CPU owns `machine.cpu`.
    p->cpu_object = object_new(&ppc_cpu_class, p, "cpu");
    if (p->cpu_object) {
        object_set_label(p->cpu_object, "CPU");
        object_set_order(p->cpu_object, 10);
        object_attach(machine_object(), p->cpu_object);
        // machine.cpu.mmu: the translation debug window (§3.9d).
        extern const class_desc_t ppc_mmu_class;
        p->mmu_object = object_new(&ppc_mmu_class, p, "mmu");
        if (p->mmu_object) {
            object_set_label(p->mmu_object, "MMU");
            object_attach(p->cpu_object, p->mmu_object);
        }
        // machine.cpu.fpu: the FPR file + FPSCR (Phase E, §3.9d).
        extern const class_desc_t ppc_fpu_class;
        p->fpu_object = object_new(&ppc_fpu_class, p, "fpu");
        if (p->fpu_object) {
            object_set_label(p->fpu_object, "FPU");
            object_attach(p->cpu_object, p->fpu_object);
        }
    }

    // `$pc`, `$r0`... — the 68K `$d0`-style aliases simply don't exist on a
    // PPC machine (§3.9d); registration is idempotent.
    register_ppc_aliases();

    return p;
}

void ppc_delete(ppc_t *p) {
    if (!p)
        return;
    // Drop the parameterless-hook binding if it is ours (memory_map_init
    // also clears the function pointers on machine swap).
    if (g_hook_ppc == p) {
        g_hook_ppc = NULL;
        g_mem_logical_xlate = NULL;
    }
    if (p->fpu_object) {
        object_detach(p->fpu_object);
        object_delete(p->fpu_object);
        p->fpu_object = NULL;
    }
    if (p->mmu_object) {
        object_detach(p->mmu_object);
        object_delete(p->mmu_object);
        p->mmu_object = NULL;
    }
    if (p->cpu_object) {
        object_detach(p->cpu_object);
        object_delete(p->cpu_object);
        p->cpu_object = NULL;
    }
    free(p);
}

void ppc_checkpoint(ppc_t *restrict p, checkpoint_t *checkpoint) {
    if (!p || !checkpoint)
        return;
    // One POD blob; pointers are nulled on restore (§3.9f).
    system_write_checkpoint_data(checkpoint, p, sizeof(ppc_t));
}

// === Scheduler adapter (the main-CPU seam) ==================================

static void ppc_if_run_sprint(void *ctx, uint32_t *instructions) {
    ppc_run((ppc_t *)ctx, instructions);
}

// The 601 never parks: PowerPC has no STOP-equivalent the Mac uses — the
// guest idles in loops, exactly as the real machine burns its CPU (§3.7).
static bool ppc_if_is_stopped(void *ctx) {
    (void)ctx;
    return false;
}

static void ppc_if_poll_interrupt(void *ctx) {
    ppc_poll_interrupt((ppc_t *)ctx);
}

sched_cpu_if_t ppc_sched_if(ppc_t *p) {
    sched_cpu_if_t cif = {p, ppc_if_run_sprint, ppc_if_is_stopped, ppc_if_poll_interrupt};
    return cif;
}

// === Debugger adapter (§3.9b) ===============================================

static uint32_t ppc_dbgif_get_pc(void *ctx) {
    return ((ppc_t *)ctx)->pc;
}

static void ppc_dbgif_set_pc(void *ctx, uint32_t pc) {
    ((ppc_t *)ctx)->pc = pc;
}

// One instruction at pc through the debug memory view; always 4 bytes.
// The pc is translated with the fetch rules so disassembly through
// translated pages shows the bytes the CPU would execute.
static int ppc_dbgif_disasm(void *ctx, uint32_t pc, char *buf) {
    ppc_t *p = (ppc_t *)ctx;
    bool ok;
    uint32_t pa = ppc_mmu_translate_debug(p, pc, false, &ok);
    ppc_insn ins;
    ppc_disassemble_model(ok ? memory_debug_read_uint32(pa) : 0, pc, p->cpu_model, &ins);
    // debug.c splits on '\t'; ppc_disasm emits "mnemonic\toperands" already.
    snprintf(buf, 100, "%s", ins.text);
    return 4;
}

// Data-side logical→physical for the shared debug paths (debug.mac reads
// the 68k world through this hook).  Side-effect-free.
static uint32_t ppc_dbgif_translate(void *ctx, uint32_t logical, bool *ok) {
    return ppc_mmu_translate_debug((ppc_t *)ctx, logical, true, ok);
}

// The mac world on PDM is the user data context (the 68k emulator runs in
// user mode); debug.mac resolves through it whatever the stop context.
static uint32_t ppc_dbgif_translate_mac(void *ctx, uint32_t logical, bool *ok) {
    return ppc_mmu_translate_mac((ppc_t *)ctx, logical, ok);
}

cpu_debug_if_t ppc_debug_if(ppc_t *p) {
    cpu_debug_if_t dif = {
        p, ppc_dbgif_get_pc, ppc_dbgif_set_pc, ppc_dbgif_disasm, ppc_dbgif_translate, ppc_dbgif_translate_mac};
    return dif;
}

// === Object-model class (§3.9d) =============================================

static ppc_t *ppc_from(struct object *self) {
    return (ppc_t *)object_data(self);
}

// Uniform hex attribute plumbing: user_data selects the field.
enum ppc_attr_id {
    PA_PC = 0,
    PA_MSR,
    PA_CR,
    PA_XER,
    PA_LR,
    PA_CTR,
    PA_MQ,
    PA_SRR0,
    PA_SRR1,
    PA_DEC,
    PA_RTCU,
    PA_RTCL,
    PA_SDR1,
    PA_FPSCR,
    PA_DAR,
    PA_DSISR,
    PA_GPR0 = 0x100, // ..0x11F
    PA_SR0 = 0x200, // ..0x20F
    PA_BAT0U = 0x300, // U/L interleaved ..0x307 (601 unified / 604 IBATs)
    PA_DBAT0U = 0x400, // U/L interleaved ..0x407 (604 DBATs)
};

static uint32_t *ppc_attr_slot(ppc_t *p, int id) {
    if (id >= PA_GPR0 && id < PA_GPR0 + 32)
        return &p->gpr[id - PA_GPR0];
    if (id >= PA_SR0 && id < PA_SR0 + 16)
        return &p->sr[id - PA_SR0];
    if (id >= PA_BAT0U && id < PA_BAT0U + 8) {
        int i = id - PA_BAT0U;
        return (i & 1) ? &p->batl[i >> 1] : &p->batu[i >> 1];
    }
    if (id >= PA_DBAT0U && id < PA_DBAT0U + 8) {
        int i = id - PA_DBAT0U;
        return (i & 1) ? &p->dbatl[i >> 1] : &p->dbatu[i >> 1];
    }
    switch (id) {
    case PA_PC:
        return &p->pc;
    case PA_MSR:
        return &p->msr;
    case PA_CR:
        return &p->cr;
    case PA_XER:
        return &p->xer;
    case PA_LR:
        return &p->lr;
    case PA_CTR:
        return &p->ctr;
    case PA_MQ:
        return &p->mq;
    case PA_SRR0:
        return &p->srr0;
    case PA_SRR1:
        return &p->srr1;
    case PA_DEC:
        return &p->dec;
    case PA_RTCU:
        return &p->rtcu;
    case PA_RTCL:
        return &p->rtcl;
    case PA_SDR1:
        return &p->sdr1;
    case PA_FPSCR:
        return &p->fpscr;
    case PA_DAR:
        return &p->dar;
    case PA_DSISR:
        return &p->dsisr;
    }
    return NULL;
}

static value_t attr_ppc_get(struct object *self, const member_t *m) {
    ppc_t *p = ppc_from(self);
    if (!p)
        return val_err("cpu not initialised");
    int id = (int)(uintptr_t)m->attr.user_data;
    uint32_t raw;
    // The time-derived SPRs read live, exactly as mfspr/mftb do; on the
    // 604 the rtcu/rtcl slots ARE the timebase halves (tbu/tbl aliases).
    if (id == PA_RTCU)
        raw = ppc_is_604(p) ? (uint32_t)(ppc_tb_now(p) >> 32) : ppc_rtcu_now(p);
    else if (id == PA_RTCL)
        raw = ppc_is_604(p) ? (uint32_t)ppc_tb_now(p) : ppc_rtcl_now(p);
    else if (id == PA_DEC)
        raw = ppc_dec_now(p);
    else {
        uint32_t *slot = ppc_attr_slot(p, id);
        if (!slot)
            return val_err("bad register id");
        raw = *slot;
    }
    value_t v = val_uint(4, raw);
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_ppc_set(struct object *self, const member_t *m, value_t in) {
    ppc_t *p = ppc_from(self);
    if (!p)
        return val_err("cpu not initialised");
    int id = (int)(uintptr_t)m->attr.user_data;
    uint32_t *slot = ppc_attr_slot(p, id);
    if (!slot)
        return val_err("bad register id");
    *slot = (uint32_t)in.u;
    // An MSR poke must keep the SoA maps coherent (the §3.5 discipline);
    // SR/BAT/SDR1 pokes invalidate the translation caches like their
    // instruction-level counterparts do.
    if (id == PA_MSR) {
        p->msr &= ppc_msr_mask(p);
        ppc_update_active_maps(p);
    } else if ((id >= PA_SR0 && id < PA_SR0 + 16) || (id >= PA_BAT0U && id < PA_BAT0U + 8) ||
               (id >= PA_DBAT0U && id < PA_DBAT0U + 8) || id == PA_SDR1) {
        ppc_recompute_sr_t_mask(p);
        ppc_mmu_invalidate_all(p);
        // A poke is an operator action, not guest code: publish it to the
        // fetch side at once rather than waiting for the guest to isync.
        ppc_context_sync(p);
    }
    return val_none();
}

#define PPC_ATTR(name_, id_)                                                                                           \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .attr = {                                                                       \
            .type = V_UINT,                                                                                            \
            .presentation_flags = VAL_HEX,                                                                             \
            .get = attr_ppc_get,                                                                                       \
            .set = attr_ppc_set,                                                                                       \
            .user_data = (const void *)(uintptr_t)(id_)                                                                \
        }                                                                                                              \
    }

// clang-format off
static const member_t ppc_members[] = {
    PPC_ATTR("pc", PA_PC),       PPC_ATTR("msr", PA_MSR),   PPC_ATTR("cr", PA_CR),     PPC_ATTR("xer", PA_XER),
    PPC_ATTR("lr", PA_LR),       PPC_ATTR("ctr", PA_CTR),   PPC_ATTR("mq", PA_MQ),     PPC_ATTR("srr0", PA_SRR0),
    PPC_ATTR("srr1", PA_SRR1),   PPC_ATTR("dec", PA_DEC),   PPC_ATTR("rtcu", PA_RTCU), PPC_ATTR("rtcl", PA_RTCL),
    PPC_ATTR("sdr1", PA_SDR1),   PPC_ATTR("fpscr", PA_FPSCR), PPC_ATTR("dar", PA_DAR),   PPC_ATTR("dsisr", PA_DSISR),
    PPC_ATTR("r0", PA_GPR0 + 0),   PPC_ATTR("r1", PA_GPR0 + 1),   PPC_ATTR("r2", PA_GPR0 + 2),
    PPC_ATTR("r3", PA_GPR0 + 3),   PPC_ATTR("r4", PA_GPR0 + 4),   PPC_ATTR("r5", PA_GPR0 + 5),
    PPC_ATTR("r6", PA_GPR0 + 6),   PPC_ATTR("r7", PA_GPR0 + 7),   PPC_ATTR("r8", PA_GPR0 + 8),
    PPC_ATTR("r9", PA_GPR0 + 9),   PPC_ATTR("r10", PA_GPR0 + 10), PPC_ATTR("r11", PA_GPR0 + 11),
    PPC_ATTR("r12", PA_GPR0 + 12), PPC_ATTR("r13", PA_GPR0 + 13), PPC_ATTR("r14", PA_GPR0 + 14),
    PPC_ATTR("r15", PA_GPR0 + 15), PPC_ATTR("r16", PA_GPR0 + 16), PPC_ATTR("r17", PA_GPR0 + 17),
    PPC_ATTR("r18", PA_GPR0 + 18), PPC_ATTR("r19", PA_GPR0 + 19), PPC_ATTR("r20", PA_GPR0 + 20),
    PPC_ATTR("r21", PA_GPR0 + 21), PPC_ATTR("r22", PA_GPR0 + 22), PPC_ATTR("r23", PA_GPR0 + 23),
    PPC_ATTR("r24", PA_GPR0 + 24), PPC_ATTR("r25", PA_GPR0 + 25), PPC_ATTR("r26", PA_GPR0 + 26),
    PPC_ATTR("r27", PA_GPR0 + 27), PPC_ATTR("r28", PA_GPR0 + 28), PPC_ATTR("r29", PA_GPR0 + 29),
    PPC_ATTR("r30", PA_GPR0 + 30), PPC_ATTR("r31", PA_GPR0 + 31),
    PPC_ATTR("sr0", PA_SR0 + 0),   PPC_ATTR("sr1", PA_SR0 + 1),   PPC_ATTR("sr2", PA_SR0 + 2),
    PPC_ATTR("sr3", PA_SR0 + 3),   PPC_ATTR("sr4", PA_SR0 + 4),   PPC_ATTR("sr5", PA_SR0 + 5),
    PPC_ATTR("sr6", PA_SR0 + 6),   PPC_ATTR("sr7", PA_SR0 + 7),   PPC_ATTR("sr8", PA_SR0 + 8),
    PPC_ATTR("sr9", PA_SR0 + 9),   PPC_ATTR("sr10", PA_SR0 + 10), PPC_ATTR("sr11", PA_SR0 + 11),
    PPC_ATTR("sr12", PA_SR0 + 12), PPC_ATTR("sr13", PA_SR0 + 13), PPC_ATTR("sr14", PA_SR0 + 14),
    PPC_ATTR("sr15", PA_SR0 + 15),
    PPC_ATTR("bat0u", PA_BAT0U + 0), PPC_ATTR("bat0l", PA_BAT0U + 1), PPC_ATTR("bat1u", PA_BAT0U + 2),
    PPC_ATTR("bat1l", PA_BAT0U + 3), PPC_ATTR("bat2u", PA_BAT0U + 4), PPC_ATTR("bat2l", PA_BAT0U + 5),
    PPC_ATTR("bat3u", PA_BAT0U + 6), PPC_ATTR("bat3l", PA_BAT0U + 7),
    // 604 additions: the DBAT file, and tbu/tbl as aliases of the rtcu/rtcl
    // storage (which holds the timebase halves on that model).  Present on
    // both models — a static member table — and simply inert on the 601.
    PPC_ATTR("dbat0u", PA_DBAT0U + 0), PPC_ATTR("dbat0l", PA_DBAT0U + 1), PPC_ATTR("dbat1u", PA_DBAT0U + 2),
    PPC_ATTR("dbat1l", PA_DBAT0U + 3), PPC_ATTR("dbat2u", PA_DBAT0U + 4), PPC_ATTR("dbat2l", PA_DBAT0U + 5),
    PPC_ATTR("dbat3u", PA_DBAT0U + 6), PPC_ATTR("dbat3l", PA_DBAT0U + 7),
    PPC_ATTR("tbu", PA_RTCU),          PPC_ATTR("tbl", PA_RTCL),
};
// clang-format on

const class_desc_t ppc_cpu_class = {
    .name = "ppc",
    .members = ppc_members,
    .n_members = sizeof(ppc_members) / sizeof(ppc_members[0]),
};

// === machine.cpu.mmu (§3.9d) ================================================
// Debug window into the 601 translation: side-effect-free logical→physical
// and a translated peek — the way tests and debugging reach the 68k
// world's logical memory without knowing the HTAB layout.

static value_t mmu_method_translate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    ppc_t *p = (ppc_t *)object_data(self);
    if (!p)
        return val_err("cpu not initialised");
    bool ok;
    uint32_t pa = ppc_mmu_translate_debug(p, (uint32_t)argv[0].u, true, &ok);
    if (!ok)
        return val_err("no translation for $%08X", (uint32_t)argv[0].u);
    value_t v = val_uint(4, pa);
    v.flags |= VAL_HEX;
    return v;
}

static value_t mmu_method_peek(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    ppc_t *p = (ppc_t *)object_data(self);
    if (!p)
        return val_err("cpu not initialised");
    uint32_t size = (argc >= 2) ? (uint32_t)argv[1].u : 4u;
    if (size != 1 && size != 2 && size != 4)
        return val_err("size must be 1, 2 or 4");
    uint32_t raw = 0;
    for (uint32_t i = 0; i < size; i++) {
        bool ok;
        uint32_t pa = ppc_mmu_translate_debug(p, (uint32_t)argv[0].u + i, true, &ok);
        if (!ok)
            return val_err("no translation for $%08X", (uint32_t)argv[0].u + i);
        raw = (raw << 8) | memory_debug_read_uint8(pa);
    }
    value_t v = val_uint((int)size, raw);
    v.flags |= VAL_HEX;
    return v;
}

static const arg_decl_t mmu_translate_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "effective (logical) address"},
};
static const arg_decl_t mmu_peek_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX,        .doc = "effective (logical) address"},
    {.name = "size", .kind = V_UINT, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "1, 2 or 4 bytes (default 4)"},
};

static const member_t ppc_mmu_members[] = {
    {.kind = M_METHOD,
     .name = "translate",
     .doc = "Translate a data-side effective address (current MSR context, no side effects)",
     .method = {.args = mmu_translate_args, .nargs = 1, .result = V_UINT, .fn = mmu_method_translate}},
    {.kind = M_METHOD,
     .name = "peek",
     .doc = "Read guest memory through the current translation (side-effect-free)",
     .method = {.args = mmu_peek_args, .nargs = 2, .result = V_UINT, .fn = mmu_method_peek}          },
};

const class_desc_t ppc_mmu_class = {
    .name = "ppc_mmu",
    .members = ppc_mmu_members,
    .n_members = sizeof(ppc_mmu_members) / sizeof(ppc_mmu_members[0]),
};

// === machine.cpu.fpu (§3.9d) — the FPR file and FPSCR =======================
// Registered by Phase E alongside the arithmetic datapath; its existence is
// also what flips the capability probe's `fpu` bit for the PDM machines.

static value_t attr_fpr_get(struct object *self, const member_t *m) {
    ppc_t *p = ppc_from(self);
    if (!p)
        return val_err("cpu not initialised");
    int idx = (int)(uintptr_t)m->attr.user_data;
    value_t v = (idx == 32) ? val_uint(4, p->fpscr) : val_uint(8, p->fpr[idx]);
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_fpr_set(struct object *self, const member_t *m, value_t in) {
    ppc_t *p = ppc_from(self);
    if (!p)
        return val_err("cpu not initialised");
    int idx = (int)(uintptr_t)m->attr.user_data;
    if (idx == 32)
        p->fpscr = ppc_fpscr_derive((uint32_t)in.u); // FEX/VX stay derived
    else
        p->fpr[idx] = in.u;
    return val_none();
}

#define PPC_FPR_ATTR(name_, id_)                                                                                       \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .attr = {                                                                       \
            .type = V_UINT,                                                                                            \
            .presentation_flags = VAL_HEX,                                                                             \
            .get = attr_fpr_get,                                                                                       \
            .set = attr_fpr_set,                                                                                       \
            .user_data = (const void *)(uintptr_t)(id_)                                                                \
        }                                                                                                              \
    }

// clang-format off
static const member_t ppc_fpu_members[] = {
    PPC_FPR_ATTR("fpscr", 32),
    PPC_FPR_ATTR("fpr0", 0),   PPC_FPR_ATTR("fpr1", 1),   PPC_FPR_ATTR("fpr2", 2),   PPC_FPR_ATTR("fpr3", 3),
    PPC_FPR_ATTR("fpr4", 4),   PPC_FPR_ATTR("fpr5", 5),   PPC_FPR_ATTR("fpr6", 6),   PPC_FPR_ATTR("fpr7", 7),
    PPC_FPR_ATTR("fpr8", 8),   PPC_FPR_ATTR("fpr9", 9),   PPC_FPR_ATTR("fpr10", 10), PPC_FPR_ATTR("fpr11", 11),
    PPC_FPR_ATTR("fpr12", 12), PPC_FPR_ATTR("fpr13", 13), PPC_FPR_ATTR("fpr14", 14), PPC_FPR_ATTR("fpr15", 15),
    PPC_FPR_ATTR("fpr16", 16), PPC_FPR_ATTR("fpr17", 17), PPC_FPR_ATTR("fpr18", 18), PPC_FPR_ATTR("fpr19", 19),
    PPC_FPR_ATTR("fpr20", 20), PPC_FPR_ATTR("fpr21", 21), PPC_FPR_ATTR("fpr22", 22), PPC_FPR_ATTR("fpr23", 23),
    PPC_FPR_ATTR("fpr24", 24), PPC_FPR_ATTR("fpr25", 25), PPC_FPR_ATTR("fpr26", 26), PPC_FPR_ATTR("fpr27", 27),
    PPC_FPR_ATTR("fpr28", 28), PPC_FPR_ATTR("fpr29", 29), PPC_FPR_ATTR("fpr30", 30), PPC_FPR_ATTR("fpr31", 31),
};
// clang-format on

const class_desc_t ppc_fpu_class = {
    .name = "ppc_fpu",
    .members = ppc_fpu_members,
    .n_members = sizeof(ppc_fpu_members) / sizeof(ppc_fpu_members[0]),
};
