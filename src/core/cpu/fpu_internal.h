// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fpu_internal.h
// Private interface shared by the 68882 FPU translation units: fpu.c (soft-
// float core, dispatch, exceptions), fpu_format.c (integer / single /
// double / extended conversions) and fpu_packed.c (packed decimal).  Not for
// use outside src/core/cpu.

#ifndef FPU_INTERNAL_H
#define FPU_INTERNAL_H

#include "fpu.h"

#include <stdbool.h>
#include <stdint.h>

// Count leading zeros in 64-bit value
static inline int clz64(uint64_t v) {
    if (v == 0)
        return 64;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(v);
#else
    int n = 0;
    if (!(v & 0xFFFFFFFF00000000ULL)) {
        n += 32;
        v <<= 32;
    }
    if (!(v & 0xFFFF000000000000ULL)) {
        n += 16;
        v <<= 16;
    }
    if (!(v & 0xFF00000000000000ULL)) {
        n += 8;
        v <<= 8;
    }
    if (!(v & 0xF000000000000000ULL)) {
        n += 4;
        v <<= 4;
    }
    if (!(v & 0xC000000000000000ULL)) {
        n += 2;
        v <<= 2;
    }
    if (!(v & 0x8000000000000000ULL)) {
        n += 1;
    }
    return n;
#endif
}

// ============================================================================
// Format conversions (fpu_format.c)
// ============================================================================

// 68882 extended (three memory longwords) <-> float80_reg_t
float80_reg_t fpu_from_extended(uint32_t exp_sign_pad, uint32_t mant_hi, uint32_t mant_lo);
void fpu_to_extended(float80_reg_t val, uint32_t *word0, uint32_t *word1, uint32_t *word2);
// IEEE 754 single / double <-> float80_reg_t (stores round per FPCR)
float80_reg_t fpu_from_single(uint32_t bits);
uint32_t fpu_to_single(fpu_state_t *fpu, float80_reg_t val);
float80_reg_t fpu_from_double(uint64_t bits);
uint64_t fpu_to_double(fpu_state_t *fpu, float80_reg_t val);
// Integer <-> float80_reg_t (stores round per FPCR, clamp + OPERR on overflow)
float80_reg_t fpu_from_int32(int32_t v);
float80_reg_t fpu_from_int16(int16_t v);
float80_reg_t fpu_from_int8(int8_t v);
int32_t fpu_to_int32(fpu_state_t *fpu, float80_reg_t val);
int16_t fpu_to_int16(fpu_state_t *fpu, float80_reg_t val);
int8_t fpu_to_int8(fpu_state_t *fpu, float80_reg_t val);

// ============================================================================
// Packed decimal (fpu_packed.c)
// ============================================================================

// 12-byte packed BCD -> float80_reg_t (INEX1 on inexact decimal input)
float80_reg_t fpu_from_packed(fpu_state_t *fpu, uint32_t w0, uint32_t w1, uint32_t w2);
// float80_reg_t -> 12-byte packed BCD with the given k-factor
void fpu_to_packed(fpu_state_t *fpu, float80_reg_t val, int k_factor, uint32_t *w0, uint32_t *w1, uint32_t *w2);

#endif // FPU_INTERNAL_H
