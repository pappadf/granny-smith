// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fpu.h
// Motorola 68882 FPU emulation: state struct, public API, and helpers.
// The FPU is accessed by the 68030 via F-line coprocessor instructions (CpID=1).

#ifndef FPU_H
#define FPU_H

#include <stdbool.h>
#include <stdint.h>

// Forward declarations
typedef struct cpu cpu_t;

// FPU condition code bits (FPSR bits 27:24)
#define FPCC_N   (1u << 27) // negative
#define FPCC_Z   (1u << 26) // zero
#define FPCC_I   (1u << 25) // infinity
#define FPCC_NAN (1u << 24) // not-a-number

// FPU exception status bits (FPSR bits 15:8)
#define FPEXC_BSUN  (1u << 15) // branch/set on unordered
#define FPEXC_SNAN  (1u << 14) // signaling NaN
#define FPEXC_OPERR (1u << 13) // operand error
#define FPEXC_OVFL  (1u << 12) // overflow
#define FPEXC_UNFL  (1u << 11) // underflow
#define FPEXC_DZ    (1u << 10) // divide by zero
#define FPEXC_INEX2 (1u << 9) // inexact result (operation)
#define FPEXC_INEX1 (1u << 8) // inexact result (decimal input)

// FPU accrued exception bits (FPSR bits 7:3)
#define FPACC_IOP  (1u << 7) // invalid operation
#define FPACC_OVFL (1u << 6) // overflow
#define FPACC_UNFL (1u << 5) // underflow
#define FPACC_DZ   (1u << 4) // divide by zero
#define FPACC_INEX (1u << 3) // inexact

// FPU state structure
typedef struct fpu_state {
    double fp[8]; // FP0-FP7 data registers (stored as host doubles)
    uint32_t fpcr; // FPU control register
    uint32_t fpsr; // FPU status register
    uint32_t fpiar; // FPU instruction address register
} fpu_state_t;

// Initialize FPU state (called from cpu_init for 68030 model)
fpu_state_t *fpu_init(void);

// Free FPU state
void fpu_free(fpu_state_t *fpu);

// General FPU operation (type=0): arithmetic, FMOVE, FMOVEM, FMOVECR.
// Decodes ext_word, reads source operand, executes op, writes result.
void fpu_general_op(cpu_t *cpu, fpu_state_t *fpu, uint16_t opcode, uint16_t ext_word);

// Evaluate FPU condition predicate (used by FBcc, FScc, FDBcc, FTRAPcc).
// Returns true if the condition is satisfied. Sets BSUN if needed.
bool fpu_test_condition(fpu_state_t *fpu, unsigned predicate);

#endif // FPU_H
