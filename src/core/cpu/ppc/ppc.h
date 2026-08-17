// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc.h
// Public interface for the PowerPC main-CPU core (MPC601).
//
// The module is named `ppc`, not `ppc601`: the decode tree and register file
// are architectural 32-bit PowerPC, with 601-specific behavior (POWER
// holdovers, MQ, RTC-instead-of-timebase, 601 BAT format, HID SPRs, the
// 601-only exception vectors) carried behind cpu_model discrimination —
// a future 603/604 would be a second model here, not a second module.
// Nothing beyond the 601 is implemented.
//
// Unlike the auxiliary DSP3210 core, this is a MAIN CPU (cores.md): it reads
// and writes guest memory through the global fast-path accessors
// (memory.h g_active_read/write), owns the supervisor/user SoA switch on
// MSR[PR] transitions, and registers `machine.cpu` and the `$` register
// aliases when instantiated by a machine.
//
// Source of truth: Motorola/IBM, "PowerPC 601 RISC Microprocessor User's
// Manual", 1995 (MPC601UM/AD) — chapter/table references in comments below
// cite that document.

#ifndef GS_CPU_PPC_H
#define GS_CPU_PPC_H

#include "common.h"
#include "debug.h" // cpu_debug_if_t
#include "scheduler.h" // sched_cpu_if_t

#include <stdbool.h>
#include <stdint.h>

struct ppc;
typedef struct ppc ppc_t;

// === Lifecycle ===

// Create a 601 core in the hard-reset state (601UM Table 5-8): all register
// files zeroed, MSR = $00001040 (ME + EP), PVR = $00010001, HID0 = $80010080.
// If `checkpoint` is non-NULL, state is restored from the stream instead.
// Registers the `machine.cpu` object node and `$` register aliases (the
// main-CPU privilege per docs/core/cpu/cores.md).
ppc_t *ppc_init(checkpoint_t *checkpoint);

void ppc_delete(ppc_t *p);

void ppc_checkpoint(ppc_t *restrict p, checkpoint_t *checkpoint);

// Hard reset (power-on): 601UM Table 5-8 register state; execution resumes
// at $FFF00100 (MSR[EP]=1 vectors the reset exception high).
void ppc_reset(ppc_t *p);

// === Execution ===

// Burn-down sprint execution (the main-CPU seam ABI, scheduler.h):
// executes until *instructions reaches 0.  A memory-layer fault zeroes the
// counter through g_bus_error_instr_ptr; the epilogue delivers it as a
// machine check (601UM §5.4.2 — the TEA path).
void ppc_run(ppc_t *restrict p, uint32_t *instructions);

// Scheduler adapter (multi-cpu seam).  is_stopped is always false: the 601
// has no STOP-equivalent the Mac uses (guest idles in loops).
sched_cpu_if_t ppc_sched_if(ppc_t *p);

// Bind the RTC/DEC time source (§3.7): RTCU/RTCL/DEC are derived from
// scheduler_cpu_cycles at exactly 7.8336 MHz-equivalent via the reduced
// rational 7,833,600/freq_hz — the dossier's hard constraint.  Registers the
// "ppc.dec" event type, so call before scheduler_start.  Unbound (unit
// tests), the RTC/DEC SPRs are static state.
void ppc_bind_time(ppc_t *p, struct scheduler *s, uint32_t freq_hz);

// Debugger adapter (PPC proposal §3.9b): PC access, pc-based disassembly,
// logical→physical translation.
cpu_debug_if_t ppc_debug_if(ppc_t *p);

// === External interrupt line ===

// Level of the external-interrupt input (PDM: AMIC's CpuInt*).  Level-
// sensitive: while high and MSR[EE]=1 the core takes the $00500 exception,
// including immediately after rfi/mtmsr re-enable (the family recomputes and
// re-asserts after every flag/enable write, proposal §4.6).
void ppc_set_ext_irq(ppc_t *p, bool level);

// Re-evaluate pending interrupts now (the sched-if poll hook): takes the
// external or decrementer exception if one is pending and MSR[EE] allows.
void ppc_poll_interrupt(ppc_t *p);

// === Register access (tests / glue; the shell reads via machine.cpu.*) ===

uint32_t ppc_get_pc(ppc_t *restrict p);
void ppc_set_pc(ppc_t *restrict p, uint32_t pc);
uint32_t ppc_get_gpr(ppc_t *restrict p, int n);
void ppc_set_gpr(ppc_t *restrict p, int n, uint32_t value);
uint32_t ppc_get_msr(ppc_t *restrict p);
void ppc_set_msr(ppc_t *restrict p, uint32_t value);
bool ppc_is_supervisor(ppc_t *restrict p);

#endif // GS_CPU_PPC_H
