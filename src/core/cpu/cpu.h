// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu.h
// Public interface for Motorola 68000 CPU emulation.

#ifndef CPU_H
#define CPU_H

// === Includes ===
#include "common.h"
#include "debug.h" // cpu_debug_if_t (the main-CPU debug seam)
#include "platform.h"

#include <stdbool.h>
#include <stdint.h>

// === Constants ===

// CPU model identifiers (the sole definition; decoders reach it via cpu_internal.h)
#define CPU_MODEL_68000 68000
#define CPU_MODEL_68030 68030
#define CPU_MODEL_68040 68040

// True if the given CPU model carries (or is paired with) a floating-point
// unit in the machines we emulate: the 68000 compacts have none; every
// 68030 Mac we model ships an FPU (68882), and the 68040 integrates one
// on-chip.  This is the single source of truth the machine capability
// probe derives `fpu` from.  Implemented in cpu.c as an explicit 68K model
// switch (not a `>=` compare) so a non-68K cpu_model — e.g. the PPC 601 —
// can never satisfy it and allocate a 68881-format fpu_t by accident; the
// PPC core answers FPU presence in its own module.
bool cpu_has_fpu(int cpu_model);

// Condition Code Register (CCR) bit masks
typedef enum {
    CPU_CCR_CARRY = 1 << 0,
    CPU_CCR_OVERFLOW = 1 << 1,
    CPU_CCR_ZERO = 1 << 2,
    CPU_CCR_NEGATIVE = 1 << 3,
    CPU_CCR_EXTEND = 1 << 4,
    CPU_CCR_MASK = (1 << 5) - 1,
} cpu_ccr_bit_t;

// Status Register (SR) bit masks (includes CCR bits)
typedef enum {
    CPU_SR_CARRY = CPU_CCR_CARRY,
    CPU_SR_OVERFLOW = CPU_CCR_OVERFLOW,
    CPU_SR_ZERO = CPU_CCR_ZERO,
    CPU_SR_NEGATIVE = CPU_CCR_NEGATIVE,
    CPU_SR_EXTEND = CPU_CCR_EXTEND,

    CPU_SR_IPL0 = 1 << 8,
    CPU_SR_IPL1 = 1 << 9,
    CPU_SR_IPL2 = 1 << 10,
    CPU_SR_INTERRUPT_MASK = CPU_SR_IPL0 | CPU_SR_IPL1 | CPU_SR_IPL2,

    CPU_SR_RESERVED11 = 1 << 11,
    CPU_SR_MASTER = 1 << 12, // M: unused on 68000, present on later models
    CPU_SR_SUPERVISOR = 1 << 13,
    CPU_SR_TRACE0 = 1 << 14, // T0: unused on 68000 (68020+ only)
    CPU_SR_TRACE1 = 1 << 15,
} cpu_sr_bit_t;

// === Type Definitions ===
struct cpu;
typedef struct cpu cpu_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Create a CPU of the given CPU_MODEL_* (restoring its state from `checkpoint`
// when non-NULL) and bind its `machine.cpu` object node.
cpu_t *cpu_init(int cpu_model, checkpoint_t *checkpoint);

// The CPU half of a reset: everything inside the package, PC and SSP reloaded
// from the vectors at $0/$4.  The BUS half must already have run -- those
// vectors are ROM only while the overlay is armed.  Level 2 (machine.reset(),
// the reset button, Cuda CMD_RESET) is bus_reset plus one of these.
void cpu_reset_to_vector_68030(cpu_t *restrict cpu);
void cpu_reset_to_vector_68040(cpu_t *restrict cpu);

// Free a CPU created by cpu_init, with its object nodes and owned FPU/040 MMU.
void cpu_delete(cpu_t *cpu);

// Save CPU state (register file, 040 MMU, FPU) to a checkpoint.
void cpu_checkpoint(cpu_t *restrict cpu, checkpoint_t *checkpoint);

// Attach an MMU instance to the CPU.  Sets `cpu->mmu` and creates a `cpu.mmu`
// child object so debug probes can read TC/CRP/SRP/TT0/TT1.  Called by
// machine setup code after both CPU and MMU instances exist.  Idempotent.
void cpu_attach_mmu(cpu_t *cpu, void *mmu);

// Bind a `cpu.mmu` child node of class `cls` (an MMU kind cpu.c does not
// model, e.g. the Lisa's segment MMU).  Idempotent.
struct class_desc;
void cpu_attach_mmu_node(cpu_t *cpu, const struct class_desc *cls, void *data);

// === Operations ===

// Run the model's decoder until the burn-down counter *instructions reaches 0.
void cpu_run_sprint(cpu_t *restrict cpu, uint32_t *instructions);

// Disassemble the instruction in instr[] into buf; returns its length in words.
int cpu_disasm(uint16_t *instr, char *buf);

uint32_t cpu_get_an(cpu_t *restrict cpu, int n);

uint32_t cpu_get_dn(cpu_t *restrict cpu, int n);

uint32_t cpu_get_pc(cpu_t *restrict cpu);

uint32_t cpu_get_ssp(cpu_t *restrict cpu);

uint32_t cpu_get_msp(cpu_t *restrict cpu);

uint32_t cpu_get_usp(cpu_t *restrict cpu);

uint16_t cpu_get_sr(cpu_t *restrict cpu);

bool cpu_is_supervisor(cpu_t *restrict cpu);

void cpu_set_an(cpu_t *restrict cpu, int n, uint32_t value);

void cpu_set_dn(cpu_t *restrict cpu, int n, uint32_t value);

void cpu_set_pc(cpu_t *restrict cpu, uint32_t value);

void cpu_set_ssp(cpu_t *restrict cpu, uint32_t value);

void cpu_set_msp(cpu_t *restrict cpu, uint32_t value);

void cpu_set_usp(cpu_t *restrict cpu, uint32_t value);

void cpu_set_sr(cpu_t *restrict cpu, uint16_t sr);

// Get/set interrupt priority level
uint32_t cpu_get_ipl(cpu_t *restrict cpu);

void cpu_set_ipl(cpu_t *restrict cpu, uint32_t value);

// STOP-halt support: query the halt state and let the scheduler service an
// interrupt that should wake a halted CPU (see cpu.c).
bool cpu_is_stopped(cpu_t *restrict cpu);
void cpu_poll_interrupt(cpu_t *restrict cpu);

// Get/set vector base register (68010+)
uint32_t cpu_get_vbr(cpu_t *restrict cpu);

void cpu_set_vbr(cpu_t *restrict cpu, uint32_t value);

// 68K adapter for the main-CPU debug seam: PC access, pc-based disassembly
// through the memory system, and logical→physical translation.  system_create
// stores the result in config_t.cpu_dbg.
cpu_debug_if_t cpu_debug_if(cpu_t *cpu);

#endif // CPU_H
