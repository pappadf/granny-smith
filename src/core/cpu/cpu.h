// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu.h
// Public interface for Motorola 68000 CPU emulation.

#ifndef CPU_H
#define CPU_H

// === Includes ===
#include "common.h"
#include "platform.h"

#include <stdbool.h>
#include <stdint.h>

// === Constants ===

// CPU model identifiers (also defined in cpu_internal.h for decoder use)
#define CPU_MODEL_68000 68000
#define CPU_MODEL_68030 68030

// Condition Code Register (CCR) bit masks
typedef enum {
    cpu_ccr_c = 1 << 0,
    cpu_ccr_v = 1 << 1,
    cpu_ccr_z = 1 << 2,
    cpu_ccr_n = 1 << 3,
    cpu_ccr_x = 1 << 4,
    cpu_ccr_mask = (1 << 5) - 1,
} cpu_ccr_bit_t;

// Status Register (SR) bit masks (includes CCR bits)
typedef enum {
    cpu_sr_c = cpu_ccr_c,
    cpu_sr_v = cpu_ccr_v,
    cpu_sr_z = cpu_ccr_z,
    cpu_sr_n = cpu_ccr_n,
    cpu_sr_x = cpu_ccr_x,

    cpu_sr_ipl0 = 1 << 8,
    cpu_sr_ipl1 = 1 << 9,
    cpu_sr_ipl2 = 1 << 10,
    cpu_sr_ipl = cpu_sr_ipl0 | cpu_sr_ipl1 | cpu_sr_ipl2,

    cpu_sr_rsvd11 = 1 << 11,
    cpu_sr_m = 1 << 12, // unused on 68000, present on later models
    cpu_sr_s = 1 << 13,
    cpu_sr_t0 = 1 << 14, // unused on 68000 (T0 on later CPUs)
    cpu_sr_t1 = 1 << 15,
} cpu_sr_bit_t;

typedef enum {

    ea_dn = 0x00001,
    ea_an = 0x00002,
    ea_an_mem = 0x00004,
    ea_an_plus = 0x00008,
    ea_min_an = 0x00010,
    ea_d16_an = 0x00020,
    ea_d8_an_xn = 0x00040,
    ea_xxx_w = 0x00080,
    ea_xxx_l = 0x00100,
    ea_d16_pc = 0x00200,
    ea_d8_pc_xn = 0x00400,
    ea_xxx = 0x00800,

    ea_any = 0x00FFF,
    ea_data = ea_any & ~ea_an,
    ea_memory = ea_data & ~ea_dn,
    ea_control = ea_memory & ~(ea_an_plus + ea_min_an + ea_xxx), // 0x1FFE4,
    ea_alterable = ea_any & ~(ea_d16_pc + ea_d8_pc_xn + ea_xxx), // 0x1E3FF,

} ea_mode_t;

// === Type Definitions ===
struct cpu;
typedef struct cpu cpu_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

extern cpu_t *cpu_init(int cpu_model, checkpoint_t *checkpoint);

void cpu_delete(cpu_t *cpu);

void cpu_checkpoint(cpu_t *restrict cpu, checkpoint_t *checkpoint);

// === Operations ===

extern void cpu_run_sprint(cpu_t *restrict cpu, uint32_t *instructions);

extern int cpu_disasm(uint16_t *instr, char *buf);

uint32_t cpu_get_an(cpu_t *restrict cpu, int n);

uint32_t cpu_get_dn(cpu_t *restrict cpu, int n);

uint32_t cpu_get_pc(cpu_t *restrict cpu);

uint32_t cpu_get_ssp(cpu_t *restrict cpu);

uint32_t cpu_get_msp(cpu_t *restrict cpu);

uint32_t cpu_get_usp(cpu_t *restrict cpu);

uint16_t cpu_get_sr(cpu_t *restrict cpu);

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

// Get/set vector base register (68010+)
uint32_t cpu_get_vbr(cpu_t *restrict cpu);

void cpu_set_vbr(cpu_t *restrict cpu, uint32_t value);

#endif // CPU_H
