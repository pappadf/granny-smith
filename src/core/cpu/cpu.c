// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu.c
// CPU lifecycle, public API, and runtime dispatch for Motorola 68000.

#include "cpu_internal.h"

#include "log.h"
#include "system.h"
LOG_USE_CATEGORY_NAME("cpu");

// Declare decoder functions (defined in cpu_68000.c and cpu_68030.c)
void cpu_run_68000(cpu_t *restrict cpu, uint32_t *instructions);
void cpu_run_68030(cpu_t *restrict cpu, uint32_t *instructions);

// === Public Accessors ===

// Get the value of address register An (n=0-7)
uint32_t cpu_get_an(cpu_t *restrict cpu, int n) {
    assert(n >= 0 && n < 8);
    return cpu->a[n];
}

// Get the value of data register Dn (n=0-7)
uint32_t cpu_get_dn(cpu_t *restrict cpu, int n) {
    assert(n >= 0 && n < 8);
    return cpu->d[n];
}

// Get the program counter value
uint32_t cpu_get_pc(cpu_t *restrict cpu) {
    return cpu->pc;
}

// Set the value of address register An (n=0-7)
void cpu_set_an(cpu_t *restrict cpu, int n, uint32_t value) {
    assert(n >= 0 && n < 8);
    cpu->a[n] = value;
}

// Set the value of data register Dn (n=0-7)
void cpu_set_dn(cpu_t *restrict cpu, int n, uint32_t value) {
    assert(n >= 0 && n < 8);
    cpu->d[n] = value;
}

// Set the program counter value
void cpu_set_pc(cpu_t *restrict cpu, uint32_t value) {
    cpu->pc = value;
}

// Get the supervisor stack pointer value
uint32_t cpu_get_ssp(cpu_t *restrict cpu) {
    return cpu->ssp;
}

// Set the supervisor stack pointer value
void cpu_set_ssp(cpu_t *restrict cpu, uint32_t value) {
    cpu->ssp = value;
}

// Get the user stack pointer value
uint32_t cpu_get_usp(cpu_t *restrict cpu) {
    return cpu->usp;
}

// Set the user stack pointer value
void cpu_set_usp(cpu_t *restrict cpu, uint32_t value) {
    cpu->usp = value;
}

// Get the interrupt priority level
uint32_t cpu_get_ipl(cpu_t *restrict cpu) {
    return cpu->ipl;
}

// Set the interrupt priority level
void cpu_set_ipl(cpu_t *restrict cpu, uint32_t value) {
    cpu->ipl = value;
}

// Get the complete status register (includes CCR and system byte).
// On 68030, includes M bit (bit 12) and T0 bit (bit 14).
uint16_t cpu_get_sr(cpu_t *restrict cpu) {
    uint16_t sr = read_ccr(cpu);

    if (cpu->cpu_model == CPU_MODEL_68030) {
        sr |= (cpu->trace >> 1 & 1) << 15; // T1
        sr |= (cpu->trace & 1) << 14; // T0
        if (cpu->m)
            sr |= 1 << 12;
    } else {
        if (cpu->trace)
            sr |= 1 << 15; // T1 only
    }
    if (cpu->supervisor)
        sr |= 1 << 13;
    sr |= cpu->interrupt_mask << 8;

    return sr;
}

// Set the complete status register
void cpu_set_sr(cpu_t *restrict cpu, uint16_t sr) {
    write_sr(cpu, sr);
}

// === Lifecycle ===

// Create and initialize a CPU instance for the specified model.
extern cpu_t *cpu_init(int cpu_model, checkpoint_t *checkpoint) {

    cpu_t *cpu = (cpu_t *)malloc(sizeof(cpu_t));
    if (!cpu)
        return NULL;

    memset(cpu, 0, sizeof(cpu_t));

    // Load from checkpoint if provided
    if (checkpoint) {
        // Read contiguous plain-data portion of cpu_t (everything before the first pointer)
        system_read_checkpoint_data(checkpoint, cpu, sizeof(cpu_t));
    } else {
        cpu->cpu_model = cpu_model;
        cpu->pc = 0x0040002a;
        cpu->a[7] = 0x4d1f8172;
        cpu->supervisor = 1;
        cpu->interrupt_mask = 7;
        // 68030-specific registers default to zero (VBR=0, CACR=0, etc.)
    }

    return cpu;
}

// Free resources associated with a CPU instance
void cpu_delete(cpu_t *cpu) {
    if (!cpu)
        return;
    free(cpu);
}

// Save CPU state to a checkpoint
void cpu_checkpoint(cpu_t *restrict cpu, checkpoint_t *checkpoint) {
    if (!cpu || !checkpoint)
        return;
    // Write contiguous plain-data portion of cpu_t in one operation
    system_write_checkpoint_data(checkpoint, cpu, sizeof(cpu_t));
}

// === Runtime Dispatch ===

// Run the appropriate decoder for the CPU model
void cpu_run_sprint(cpu_t *restrict cpu, uint32_t *instructions) {
    if (cpu->cpu_model == CPU_MODEL_68030)
        cpu_run_68030(cpu, instructions);
    else
        cpu_run_68000(cpu, instructions);
}
