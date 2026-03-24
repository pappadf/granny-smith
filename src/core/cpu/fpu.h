// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fpu.h
// Motorola 68882 FPU emulation: state struct, public API, and helpers.
// The FPU is accessed by the 68030 via F-line coprocessor instructions (CpID=1).
// Registers use native 80-bit extended precision (float80_reg_t).
// Arithmetic uses an unpacked 128-bit mantissa format (fpu_unpacked_t).

#ifndef FPU_H
#define FPU_H

#include <stdbool.h>
#include <stdint.h>

// Forward declarations
typedef struct cpu cpu_t;

// ============================================================================
// 80-bit extended precision register type (storage format)
// ============================================================================

// Maps directly to the 68882 80-bit extended precision layout
typedef struct {
    uint16_t exponent; // 15-bit biased exponent (bias=16383) + sign in bit 15
    uint64_t mantissa; // 64-bit mantissa (bit 63 = explicit integer/J bit)
} float80_reg_t;

// Sign bit is stored in bit 15 of the exponent field
#define FP80_SIGN(f) (((f).exponent >> 15) & 1)
#define FP80_EXP(f)  ((f).exponent & 0x7FFF)

// Predicate helpers for classifying float80 values
static inline bool fp80_is_zero(float80_reg_t f) {
    return FP80_EXP(f) == 0 && f.mantissa == 0;
}
static inline bool fp80_is_inf(float80_reg_t f) {
    return FP80_EXP(f) == 0x7FFF && f.mantissa == 0;
}
static inline bool fp80_is_nan(float80_reg_t f) {
    return FP80_EXP(f) == 0x7FFF && f.mantissa != 0;
}
static inline bool fp80_is_snan(float80_reg_t f) {
    return FP80_EXP(f) == 0x7FFF && (f.mantissa & 0x4000000000000000ULL) == 0 && f.mantissa != 0;
}
static inline bool fp80_is_denormal(float80_reg_t f) {
    return FP80_EXP(f) == 0 && f.mantissa != 0;
}
static inline bool fp80_is_negative(float80_reg_t f) {
    return FP80_SIGN(f) && !fp80_is_nan(f);
}

// Construct a float80_reg_t from components
static inline float80_reg_t fp80_make(int sign, uint16_t exp, uint64_t mant) {
    float80_reg_t r;
    r.exponent = (uint16_t)(((sign & 1) << 15) | (exp & 0x7FFF));
    r.mantissa = mant;
    return r;
}

// Common constants
#define FP80_ZERO     ((float80_reg_t){0x0000, 0})
#define FP80_NEG_ZERO ((float80_reg_t){0x8000, 0})
#define FP80_INF      ((float80_reg_t){0x7FFF, 0})
#define FP80_NEG_INF  ((float80_reg_t){0xFFFF, 0})
#define FP80_QNAN     ((float80_reg_t){0x7FFF, 0xFFFFFFFFFFFFFFFFULL})

// ============================================================================
// Unpacked internal calculation format (128-bit mantissa)
// ============================================================================

// Sentinel exponent values
#define FPU_EXP_ZERO (-32768) // zero (mantissa==0) or denormal (mantissa!=0)
#define FPU_EXP_INF  (32767) // infinity (mantissa==0) or NaN (mantissa!=0)
#define FPU_EXP_BIAS 16383 // 68882 extended-precision exponent bias

// Wider format used during arithmetic for extra guard/round/sticky bits
typedef struct {
    bool sign; // sign bit
    int32_t exponent; // signed, unbiased exponent
    uint64_t mantissa_hi; // upper 64 bits (bit 63 = integer/J bit)
    uint64_t mantissa_lo; // lower 64 bits (guard + round + sticky)
} fpu_unpacked_t;

// ============================================================================
// FPU condition code bits (FPSR bits 27:24)
// ============================================================================

#define FPCC_N   (1u << 27) // negative
#define FPCC_Z   (1u << 26) // zero
#define FPCC_I   (1u << 25) // infinity
#define FPCC_NAN (1u << 24) // not-a-number

// ============================================================================
// FPU exception status bits (FPSR bits 15:8)
// ============================================================================

#define FPEXC_BSUN  (1u << 15) // branch/set on unordered
#define FPEXC_SNAN  (1u << 14) // signaling NaN
#define FPEXC_OPERR (1u << 13) // operand error
#define FPEXC_OVFL  (1u << 12) // overflow
#define FPEXC_UNFL  (1u << 11) // underflow
#define FPEXC_DZ    (1u << 10) // divide by zero
#define FPEXC_INEX2 (1u << 9) // inexact result (operation)
#define FPEXC_INEX1 (1u << 8) // inexact result (decimal input)

// ============================================================================
// FPU accrued exception bits (FPSR bits 7:3)
// ============================================================================

#define FPACC_IOP  (1u << 7) // invalid operation
#define FPACC_OVFL (1u << 6) // overflow
#define FPACC_UNFL (1u << 5) // underflow
#define FPACC_DZ   (1u << 4) // divide by zero
#define FPACC_INEX (1u << 3) // inexact

// ============================================================================
// FPU state structure
// ============================================================================

typedef struct fpu_state {
    float80_reg_t fp[8]; // FP0-FP7 data registers (80-bit extended)
    uint32_t fpcr; // FPU control register
    uint32_t fpsr; // FPU status register
    uint32_t fpiar; // FPU instruction address register
    bool initialized; // true once any FPU operation has executed (for FSAVE)
} fpu_state_t;

// ============================================================================
// Public API
// ============================================================================

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

// Check FPSR exception status against FPCR enables; take exception if any match.
// Returns true if an exception was vectored, false otherwise.
bool fpu_check_exceptions(cpu_t *cpu, fpu_state_t *fpu);

// FSAVE: write state frame to memory at addr. Returns frame size in bytes.
// Writes idle frame (28 bytes) if FPU has been used, null frame (4 bytes) otherwise.
int fpu_fsave(fpu_state_t *fpu, uint32_t addr);

// FRESTORE: read state frame from memory at addr. Returns frame size in bytes.
// Null frame resets FPU. Idle/busy frames restore internal state.
int fpu_frestore(fpu_state_t *fpu, uint32_t addr);

// ============================================================================
// Conversion helpers (used by test harness and FMOVEM)
// ============================================================================

// Pack: fpu_unpacked_t → float80_reg_t (with rounding per FPCR)
float80_reg_t fpu_pack(fpu_state_t *fpu, fpu_unpacked_t val);

// Unpack: float80_reg_t → fpu_unpacked_t (lossless)
fpu_unpacked_t fpu_unpack(float80_reg_t reg);

#endif // FPU_H
