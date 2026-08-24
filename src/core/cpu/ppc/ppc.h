// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc.h
// Public interface for the PowerPC main-CPU core (MPC601 / MPC604).
//
// The module is named `ppc`, not `ppc601`: the decode tree and register file
// are architectural 32-bit PowerPC, with model-specific behavior carried
// behind cpu_model discrimination the way cpu.c discriminates 68000/030/040.
// Two models exist:
//   CPU_MODEL_PPC601 — POWER holdovers, MQ, RTC-instead-of-timebase, the
//     601 unified BAT format, the $00A00/$02000 vectors;
//   CPU_MODEL_PPC604 — timebase/mftb, architected split I/D BATs, per-class
//     tlbie + tlbsync, POW/BE/RI/PM MSR bits, the optional FP group
//     (fsel/fres/frsqrte/stfiwx), holdover rejection, trace at $00D00.
//
// Unlike the auxiliary DSP3210 core, this is a MAIN CPU (cores.md): it reads
// and writes guest memory through the global fast-path accessors
// (memory.h g_active_read/write), owns the supervisor/user SoA switch on
// MSR[PR] transitions, and registers `machine.cpu` and the `$` register
// aliases when instantiated by a machine.
//
// Sources of truth: Motorola/IBM, "PowerPC 601 RISC Microprocessor User's
// Manual", 1995 (MPC601UM/AD) for the 601 model, and "PowerPC 604 RISC
// Microprocessor User's Manual", 1994 (MPC604UM/AD) plus "PowerPC
// Microprocessor Family: The Programming Environments" (MPCFPE32B) for the
// 604 — chapter/table references in comments cite those documents.

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

// Create a core of `cpu_model` (CPU_MODEL_PPC601 / CPU_MODEL_PPC604) in the
// hard-reset state: 601 per 601UM Table 5-8 (MSR = $00001040 (ME + EP),
// PVR = $00010001, HID0 = $80010080); 604 per 604UM §8.8.4 (MSR = $00000040
// — HRESET sets only IP — PVR = $00040103, HID0 = 0).
// If `checkpoint` is non-NULL, state (including the model) is restored from
// the stream instead and `cpu_model` is ignored.
// Registers the `machine.cpu` object node and `$` register aliases (the
// main-CPU privilege per docs/core/cpu/cores.md).
ppc_t *ppc_init(checkpoint_t *checkpoint, int cpu_model);

void ppc_delete(ppc_t *p);

void ppc_checkpoint(ppc_t *restrict p, checkpoint_t *checkpoint);

// Hard reset (power-on): per-model register state (see ppc_init); the
// cpu_model itself survives.  Execution resumes at $FFF00100 (MSR[EP]=1
// vectors the reset exception high on both models).
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

// Bind the RTC/TB/DEC time source (601 proposal §3.7, generalized per the
// TNT proposal §4.4): the time SPRs are derived from scheduler_cpu_cycles
// via the reduced rational tick_hz/freq_hz.  On the 601 `tick_hz` is the
// 7.8336 MHz RTC input (RTCL advances 128 ns-units, DEC decrements 128
// units per tick — the dossier's hard constraint); on the 604 it is the
// timebase rate (bus clock / 4 — 604UM §1.3.2.2), with TB incrementing and
// DEC decrementing once per tick.  Registers the "ppc.dec" event type, so
// call before scheduler_start.  Unbound (unit tests), the time SPRs are
// static state.
void ppc_bind_time(ppc_t *p, struct scheduler *s, uint32_t freq_hz, uint32_t tick_hz);

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

// === MMU (Phase D) ===

// Drop every cached translation (user-SoA fills, translation TLB, fetch
// window).  The family calls this when the PHYSICAL map changes under
// the MMU's feet — the HMC bank remap — mirroring what mtsr/BAT/SDR1
// writes do from guest code.
void ppc_mmu_invalidate_all(ppc_t *p);

// === Register access (tests / glue; the shell reads via machine.cpu.*) ===

uint32_t ppc_get_pc(ppc_t *restrict p);
void ppc_set_pc(ppc_t *restrict p, uint32_t pc);
uint32_t ppc_get_gpr(ppc_t *restrict p, int n);
void ppc_set_gpr(ppc_t *restrict p, int n, uint32_t value);
uint32_t ppc_get_msr(ppc_t *restrict p);
void ppc_set_msr(ppc_t *restrict p, uint32_t value);
bool ppc_is_supervisor(ppc_t *restrict p);

#endif // GS_CPU_PPC_H
