// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fpu.c
// Motorola 68882 FPU emulation — soft-float core using native 80-bit register
// type (float80_reg_t) and 128-bit mantissa unpacked format (fpu_unpacked_t).

#include "fpu.h"
#include "cpu_internal.h"
#include "fpu_internal.h"
#include "memory.h"

#include "log.h"
LOG_USE_CATEGORY_NAME("fpu");

#include <stdlib.h>
#include <string.h>

// ============================================================================
// FPU exception vector offsets (vector_number * 4)
// ============================================================================

#define FPVEC_BSUN  0xC0 // vector 48: branch/set on unordered
#define FPVEC_INEX  0xC4 // vector 49: inexact result
#define FPVEC_DZ    0xC8 // vector 50: divide by zero
#define FPVEC_UNFL  0xCC // vector 51: underflow
#define FPVEC_OPERR 0xD0 // vector 52: operand error
#define FPVEC_OVFL  0xD4 // vector 53: overflow
#define FPVEC_SNAN  0xD8 // vector 54: signaling NaN

// ============================================================================
// Lifecycle
// ============================================================================

// Allocate and zero-init FPU state
fpu_state_t *fpu_init(void) {
    fpu_state_t *fpu = (fpu_state_t *)malloc(sizeof(fpu_state_t));
    if (!fpu)
        return NULL;
    memset(fpu, 0, sizeof(fpu_state_t));
    // Hardware reset: data registers are non-signaling NaNs (MC68882UM §2.2)
    for (int i = 0; i < 8; i++)
        fpu->fp[i] = (float80_reg_t){0x7FFF, 0xFFFFFFFFFFFFFFFFULL};
    return fpu;
}

// Free FPU state
void fpu_free(fpu_state_t *fpu) {
    free(fpu);
}

// ============================================================================
// 128-bit integer primitives
// ============================================================================

// 128-bit unsigned add: (hi:lo) = (a_hi:a_lo) + (b_hi:b_lo)
static inline void uint128_add(uint64_t *hi, uint64_t *lo, uint64_t a_hi, uint64_t a_lo, uint64_t b_hi, uint64_t b_lo) {
    uint64_t lo_sum = a_lo + b_lo;
    uint64_t carry = (lo_sum < a_lo) ? 1 : 0;
    *lo = lo_sum;
    *hi = a_hi + b_hi + carry;
}

// 128-bit unsigned subtract: (hi:lo) = (a_hi:a_lo) - (b_hi:b_lo)
static inline void uint128_sub(uint64_t *hi, uint64_t *lo, uint64_t a_hi, uint64_t a_lo, uint64_t b_hi, uint64_t b_lo) {
    uint64_t borrow = (a_lo < b_lo) ? 1 : 0;
    *lo = a_lo - b_lo;
    *hi = a_hi - b_hi - borrow;
}

// 64x64 -> 128 unsigned multiply
static inline void uint64_mul128(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
#ifdef __SIZEOF_INT128__
    // Native 128-bit support (GCC/Clang on 64-bit hosts)
    __uint128_t r = (__uint128_t)a * b;
    *hi = (uint64_t)(r >> 64);
    *lo = (uint64_t)r;
#else
    // Portable four-32x32 fallback for Emscripten/WASM
    uint64_t a_hi32 = a >> 32, a_lo32 = a & 0xFFFFFFFF;
    uint64_t b_hi32 = b >> 32, b_lo32 = b & 0xFFFFFFFF;

    uint64_t p0 = a_lo32 * b_lo32;
    uint64_t p1 = a_lo32 * b_hi32;
    uint64_t p2 = a_hi32 * b_lo32;
    uint64_t p3 = a_hi32 * b_hi32;

    // Combine partial products
    uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFF) + (p2 & 0xFFFFFFFF);
    *lo = (p0 & 0xFFFFFFFF) | (mid << 32);
    *hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
#endif
}

// 128-bit right shift with sticky bit: shifted-out nonzero bits OR into lsb
static inline void uint128_shr_sticky(uint64_t *hi, uint64_t *lo, int n) {
    if (n == 0)
        return;
    if (n >= 128) {
        uint64_t sticky = (*hi | *lo) ? 1 : 0;
        *hi = 0;
        *lo = sticky;
        return;
    }
    // Compute sticky from bits that will be shifted out
    uint64_t sticky = 0;
    if (n >= 64) {
        // All of lo plus lower (n-64) bits of hi are lost
        sticky = *lo;
        if (n > 64)
            sticky |= *hi & ((1ULL << (n - 64)) - 1);
        *lo = *hi >> (n - 64);
        *hi = 0;
    } else {
        // Lower n bits of lo are lost
        sticky = *lo & ((1ULL << n) - 1);
        *lo = (*lo >> n) | (*hi << (64 - n));
        *hi >>= n;
    }
    if (sticky)
        *lo |= 1;
}

// ============================================================================
// Pack / Unpack conversions
// ============================================================================

// Unpack: float80_reg_t -> fpu_unpacked_t (lossless)
fpu_unpacked_t fpu_unpack(float80_reg_t reg) {
    fpu_unpacked_t r;
    r.sign = fp80_sign(reg) != 0;
    r.mantissa_lo = 0;
    uint16_t exp = fp80_exp(reg);

    if (exp == 0) {
        if (reg.mantissa == 0) {
            // Zero
            r.exponent = FPU_EXP_ZERO;
            r.mantissa_hi = 0;
        } else if (reg.mantissa & 0x8000000000000000ULL) {
            // Pseudo-denormal: j-bit set with biased exponent 0.
            // M68882 extended has explicit j-bit, so literal exponent = 0 - bias.
            r.exponent = -FPU_EXP_BIAS; // 0 - 16383 = -16383
            r.mantissa_hi = reg.mantissa;
        } else {
            // True denormalized: exponent = 0 - bias = -16383 (explicit j-bit)
            r.exponent = -FPU_EXP_BIAS;
            r.mantissa_hi = reg.mantissa;
            // Normalize it
            int shift = clz64(r.mantissa_hi);
            r.mantissa_hi <<= shift;
            r.exponent -= shift;
        }
    } else if (exp == 0x7FFF) {
        // Infinity or NaN
        r.exponent = FPU_EXP_INF;
        r.mantissa_hi = reg.mantissa;
    } else {
        // Normal number: unbiased exponent
        r.exponent = (int32_t)exp - FPU_EXP_BIAS;
        r.mantissa_hi = reg.mantissa;
    }
    return r;
}

// Normalize: shift mantissa left until bit 63 of mantissa_hi is set
void fpu_normalize(fpu_unpacked_t *v) {
    if (v->mantissa_hi == 0 && v->mantissa_lo == 0) {
        v->exponent = FPU_EXP_ZERO;
        return;
    }
    if (v->mantissa_hi == 0) {
        // High word is empty, shift low into high
        int shift = clz64(v->mantissa_lo);
        v->mantissa_hi = (shift < 64) ? (v->mantissa_lo << shift) : 0;
        v->mantissa_lo = 0;
        v->exponent -= 64 + shift;
    } else {
        int shift = clz64(v->mantissa_hi);
        if (shift > 0) {
            v->mantissa_hi = (v->mantissa_hi << shift) | (v->mantissa_lo >> (64 - shift));
            v->mantissa_lo <<= shift;
            v->exponent -= shift;
        }
    }
}

// Round mantissa to a specific number of significant bits
static void fpu_round_mantissa(fpu_state_t *fpu, fpu_unpacked_t *val, int prec_bits) {
    if (val->exponent == FPU_EXP_INF || val->exponent == FPU_EXP_ZERO)
        return;
    if (prec_bits >= 64)
        return;
    int discard = 64 - prec_bits;
    uint64_t half = 1ULL << (discard - 1);
    uint64_t mask = (half << 1) - 1;
    uint64_t round_bits = val->mantissa_hi & mask;
    bool has_lo = (val->mantissa_lo != 0);
    if (round_bits != 0 || has_lo)
        fpu->fpsr |= FPEXC_INEX2;
    unsigned rmode = (fpu->fpcr >> 4) & 3;
    bool round_up = false;
    switch (rmode) {
    case 0:
        if (round_bits > half || (round_bits == half && !has_lo && (val->mantissa_hi & (1ULL << discard))))
            round_up = true;
        else if (round_bits == half && has_lo)
            round_up = true;
        break;
    case 1:
        break;
    case 2:
        if (val->sign && (round_bits || has_lo))
            round_up = true;
        break;
    case 3:
        if (!val->sign && (round_bits || has_lo))
            round_up = true;
        break;
    }
    val->mantissa_hi &= ~mask;
    val->mantissa_lo = 0;
    if (round_up) {
        val->mantissa_hi += (1ULL << discard);
        if (val->mantissa_hi == 0) {
            val->mantissa_hi = 0x8000000000000000ULL;
            val->exponent++;
        }
    }
}

// Round the 128-bit mantissa to target precision and pack into float80_reg_t
float80_reg_t fpu_pack(fpu_state_t *fpu, fpu_unpacked_t val) {
    // Handle special cases
    if (val.exponent == FPU_EXP_ZERO) {
        return fp80_make(val.sign, 0, 0);
    }
    if (val.exponent == FPU_EXP_INF) {
        if (val.mantissa_hi == 0)
            return fp80_make(val.sign, 0x7FFF, 0); // infinity
        // NaN: detect SNAN and set exception, then quiet
        if (!(val.mantissa_hi & 0x4000000000000000ULL))
            fpu->fpsr |= FPEXC_SNAN;
        uint64_t mant = val.mantissa_hi | 0x4000000000000000ULL;
        return fp80_make(val.sign, 0x7FFF, mant);
    }

    // Determine target precision from FPCR bits 7:6
    int prec_bits;
    unsigned prec = (fpu->fpcr >> 6) & 3;
    switch (prec) {
    case 1:
        prec_bits = 24;
        break; // single
    case 2:
    case 3:
        prec_bits = 53;
        break; // double (11 acts as double on 68882)
    default:
        prec_bits = 64;
        break; // extended
    }

    // Determine rounding mode from FPCR bits 5:4
    unsigned rmode = (fpu->fpcr >> 4) & 3;

    // Determine max/min exponents for target precision
    int32_t max_exp, min_exp;
    switch (prec) {
    case 1:
        max_exp = 127;
        min_exp = -126;
        break; // single
    case 2:
    case 3:
        max_exp = 1023;
        min_exp = -1022;
        break; // double
    default:
        max_exp = 16383;
        min_exp = -16383;
        break; // extended (explicit j-bit, literal 0-bias)
    }

    // Normalize mantissa so j-bit (bit 63) is set
    fpu_normalize(&val);
    if (val.exponent == FPU_EXP_ZERO) {
        return fp80_make(val.sign, 0, 0);
    }

    // Pre-denormalize if underflow (shift mantissa right before rounding)
    if (val.exponent < min_exp) {
        int32_t shift = min_exp - val.exponent;
        // Shift 128-bit mantissa right, preserving lost bits as sticky
        // (uint128_shr_sticky handles all shift amounts including >= 128)
        uint128_shr_sticky(&val.mantissa_hi, &val.mantissa_lo, shift);
        val.exponent = min_exp;
        // Signal underflow
        fpu->fpsr |= FPEXC_UNFL;
    }

    // Round mantissa to target precision
    if (prec_bits < 64) {
        // Bits to discard from mantissa_hi
        int discard = 64 - prec_bits;
        uint64_t half = 1ULL << (discard - 1);
        uint64_t mask = (half << 1) - 1; // mask for discarded bits
        uint64_t round_bits = val.mantissa_hi & mask;
        bool has_lo = (val.mantissa_lo != 0);

        // Check if any bits are lost
        if (round_bits != 0 || has_lo) {
            fpu->fpsr |= FPEXC_INEX2;
        }

        // Apply rounding
        bool round_up = false;
        switch (rmode) {
        case 0: // round to nearest (ties to even)
            if (round_bits > half || (round_bits == half && !has_lo && (val.mantissa_hi & (1ULL << discard)))) {
                round_up = true;
            } else if (round_bits == half && has_lo) {
                round_up = true;
            }
            break;
        case 1: // round to zero
            break;
        case 2: // round toward -inf
            if (val.sign && (round_bits || has_lo))
                round_up = true;
            break;
        case 3: // round toward +inf
            if (!val.sign && (round_bits || has_lo))
                round_up = true;
            break;
        }

        // Clear discarded bits and apply round
        val.mantissa_hi &= ~mask;
        val.mantissa_lo = 0;
        if (round_up) {
            val.mantissa_hi += (1ULL << discard);
            if (val.mantissa_hi == 0) {
                // Overflow from rounding: renormalize
                val.mantissa_hi = 0x8000000000000000ULL;
                val.exponent++;
            }
        }
    } else {
        // Extended precision: round based on mantissa_lo
        if (val.mantissa_lo != 0) {
            fpu->fpsr |= FPEXC_INEX2;

            bool round_up = false;
            uint64_t half_lo = 0x8000000000000000ULL;
            switch (rmode) {
            case 0: // nearest
                if (val.mantissa_lo > half_lo || (val.mantissa_lo == half_lo && (val.mantissa_hi & 1)))
                    round_up = true;
                break;
            case 1: // zero
                break;
            case 2: // -inf
                if (val.sign)
                    round_up = true;
                break;
            case 3: // +inf
                if (!val.sign)
                    round_up = true;
                break;
            }

            val.mantissa_lo = 0;
            if (round_up) {
                val.mantissa_hi++;
                if (val.mantissa_hi == 0) {
                    val.mantissa_hi = 0x8000000000000000ULL;
                    val.exponent++;
                }
            }
        }
    }

    // Check for overflow
    if (val.exponent > max_exp) {
        // Rounding mode determines overflow result
        bool to_inf = false;
        switch (rmode) {
        case 0:
            to_inf = true;
            break; // nearest → infinity
        case 1:
            to_inf = false;
            break; // toward zero → max finite
        case 2:
            to_inf = val.sign;
            break; // toward -inf: neg→-inf, pos→+max
        case 3:
            to_inf = !val.sign;
            break; // toward +inf: pos→+inf, neg→-max
        }
        // OVFL always set on overflow; INEX2 already set above if rounding lost bits
        fpu->fpsr |= FPEXC_OVFL;
        if (to_inf) {
            return fp80_make(val.sign, 0x7FFF, 0);
        }
        // Return max finite value for target precision
        uint64_t max_mant;
        switch (prec) {
        case 1:
            max_mant = 0xFFFFFF0000000000ULL;
            break; // 24 bits
        case 2:
        case 3:
            max_mant = 0xFFFFFFFFFFFFF800ULL;
            break; // 53 bits
        default:
            max_mant = 0xFFFFFFFFFFFFFFFFULL;
            break; // 64 bits
        }
        uint16_t max_biased = (uint16_t)(max_exp + FPU_EXP_BIAS);
        return fp80_make(val.sign, max_biased, max_mant);
    }

    int32_t biased = val.exponent + FPU_EXP_BIAS;
    return fp80_make(val.sign, (uint16_t)biased, val.mantissa_hi);
}

// ============================================================================
// Condition code helpers
// ============================================================================

// Update FPSR condition codes from a float80 register value
static void fpu_update_cc(fpu_state_t *fpu, float80_reg_t val) {
    uint32_t cc = 0;
    if (fp80_is_nan(val))
        cc |= FPCC_NAN | (fp80_sign(val) ? FPCC_N : 0);
    else if (fp80_is_inf(val))
        cc |= FPCC_I | (fp80_sign(val) ? FPCC_N : 0);
    else if (fp80_is_zero(val))
        cc |= FPCC_Z | (fp80_sign(val) ? FPCC_N : 0);
    else if (fp80_sign(val))
        cc |= FPCC_N;
    fpu->fpsr = (fpu->fpsr & ~(FPCC_N | FPCC_Z | FPCC_I | FPCC_NAN)) | cc;
}

// Evaluate FPU condition predicate from FPSR condition codes
bool fpu_test_condition(fpu_state_t *fpu, unsigned predicate) {
    uint32_t cc = fpu->fpsr;
    bool n = (cc & FPCC_N) != 0;
    bool z = (cc & FPCC_Z) != 0;
    bool nan_bit = (cc & FPCC_NAN) != 0;

    // IEEE-aware predicates (0x10-0x1F) set BSUN if NaN.  Accrued IOP is set
    // here directly rather than through fpu_update_accrued(): that helper
    // re-derives every accrued bit from the whole exception byte, which
    // would also re-accrue stale status bits left by an earlier instruction.
    if ((predicate & 0x10) && nan_bit)
        fpu->fpsr |= FPEXC_BSUN | FPACC_IOP;

    switch (predicate & 0x0F) {
    case 0x00:
        return false; // F/SF
    case 0x01:
        return z; // EQ/SEQ
    case 0x02:
        return !(nan_bit || z || n); // OGT/GT
    case 0x03:
        return z || !(nan_bit || n); // OGE/GE
    case 0x04:
        return n && !(nan_bit || z); // OLT/LT
    case 0x05:
        return z || (n && !nan_bit); // OLE/LE
    case 0x06:
        return !(nan_bit || z); // OGL/GL
    case 0x07:
        return !nan_bit || z; // OR/GLE — hardware: Z overrides NAN for "ordered" check
    case 0x08:
        return nan_bit; // UN/NGLE
    case 0x09:
        return nan_bit || z; // UEQ/NGL
    case 0x0A:
        return nan_bit || !(n || z); // UGT/NGE
    case 0x0B:
        return nan_bit || z || !n; // UGE/NLT
    case 0x0C:
        return nan_bit || (n && !z); // ULT/NGT
    case 0x0D:
        return nan_bit || z || n; // ULE/NLE
    case 0x0E:
        return !z || nan_bit; // NE/SNE — hardware: NAN implies not-equal
    case 0x0F:
        return true; // T/ST
    }
    return false;
}

// ============================================================================
// FSAVE / FRESTORE — coprocessor state frame save and restore
// ============================================================================

// MC68882 FSAVE/FRESTORE state frame constants (MC68882UM §6.4.2, Table 6-6)
//
// Format word layout: [version:8][size:8][reserved:16]
//   version = $1F for MC68882 initial production
//   size    = payload bytes (excluding format word itself)
//
// MC68882 idle frame layout (Figure 6-5, 56 bytes payload):
//   +$00: Format word ($1F380000)
//   +$04: Command/Condition register (4 bytes)
//   +$08: CU internal registers (32 bytes)
//   +$28: Exceptional operand (12 bytes, extended precision)
//   +$34: Operand register (4 bytes)
//   +$38: BIU flags (4 bytes)
//
// We store emulator-specific state in the CU internal registers area:
//   +$08: pre_exc_mask (4 bytes) — which exceptions already fired pre-instruction

#define FSAVE_VERSION 0x1F
// FSAVE_IDLE_SIZE defined in fpu.h ($38 = 56 bytes)
#define FSAVE_BUSY_SIZE 0xD4 // MC68882 busy frame: 212 bytes payload (not generated)

// BIU flags bit definitions
#define BIU_EXC_PEND (1u << 0) // exception pending

// Write FSAVE state frame to memory. Returns total frame size in bytes.
int fpu_fsave(fpu_state_t *fpu, uint32_t addr) {
    if (!fpu->initialized) {
        // Null frame: FPU has not been used since reset (§6.4.2.1)
        memory_write_uint32(addr, 0x00000000);
        return 4;
    }

    // +$00: Format word — version=$1F, size=$38 (MC68882 idle)
    uint32_t header = ((uint32_t)FSAVE_VERSION << 24) | ((uint32_t)FSAVE_IDLE_SIZE << 16);
    memory_write_uint32(addr, header);

    // +$04: Command/Condition register (internal, reserved bits — zero for idle)
    memory_write_uint32(addr + 0x04, 0x00000000);

    // +$08: CU internal registers (32 bytes)
    // Store pre_exc_mask at the start; remainder is zero
    memory_write_uint32(addr + 0x08, fpu->pre_exc_mask);
    for (uint32_t off = addr + 0x0C; off < addr + 0x28; off += 4)
        memory_write_uint32(off, 0x00000000);

    // +$28: Exceptional operand (12 bytes, extended precision)
    // Store the last exceptional operand for exception handler use
    uint32_t w0, w1, w2;
    fpu_to_extended(fpu->exceptional_operand, &w0, &w1, &w2);
    memory_write_uint32(addr + 0x28, w0);
    memory_write_uint32(addr + 0x2C, w1);
    memory_write_uint32(addr + 0x30, w2);

    // +$34: Operand register (internal — zero for idle)
    memory_write_uint32(addr + 0x34, 0x00000000);

    // +$38: BIU flags
    uint32_t biu = 0;
    uint32_t exc = fpu->fpsr & 0xFF00; // exception status bits
    uint32_t ena = fpu->fpcr & 0xFF00; // exception enable bits
    if (exc & ena)
        biu |= BIU_EXC_PEND;
    memory_write_uint32(addr + 0x38, biu);

    // After FSAVE, the FPU transitions to null state (§6.4.3.2)
    // The programmer model is NOT affected — it remains valid for FMOVEM
    fpu->initialized = false;
    fpu->pre_exc_mask = 0;

    return 4 + FSAVE_IDLE_SIZE;
}

// Read FRESTORE state frame from memory. Returns total frame size in bytes.
int fpu_frestore(fpu_state_t *fpu, uint32_t addr) {
    uint32_t header = memory_read_uint32(addr);
    uint32_t size = (header >> 16) & 0xFF;

    if (size == 0) {
        // Null frame: reset FPU to hardware-reset state (§6.4.2.1)
        // When already in null state (e.g. after FSAVE), the programmer model
        // was preserved by FSAVE and there is no pending operation to abort —
        // only clear internal bookkeeping.  A full reset (data regs → NaN,
        // control regs → 0) is only needed when aborting from an initialized
        // (idle/busy) state.
        if (fpu->initialized) {
            for (int i = 0; i < 8; i++)
                fpu->fp[i] = (float80_reg_t){0x7FFF, 0xFFFFFFFFFFFFFFFFULL};
            fpu->fpcr = 0;
            fpu->fpsr = 0;
            fpu->fpiar = 0;
        }
        fpu->pre_exc_mask = 0;
        fpu->exceptional_operand = FP80_ZERO;
        fpu->initialized = false;
        return 4;
    }

    if (size == FSAVE_IDLE_SIZE) {
        // MC68882 idle frame ($38 = 56 bytes payload)
        // Restore emulator state from CU internal registers area
        fpu->pre_exc_mask = memory_read_uint32(addr + 0x08);

        // Restore exceptional operand
        uint32_t w0 = memory_read_uint32(addr + 0x28);
        uint32_t w1 = memory_read_uint32(addr + 0x2C);
        uint32_t w2 = memory_read_uint32(addr + 0x30);
        fpu->exceptional_operand = fpu_from_extended(w0, w1, w2);
    }

    // Idle or busy frame: mark FPU as initialized
    fpu->initialized = true;
    return 4 + size;
}

// MC68040 FSAVE: the on-chip FPU's frames are a single status longword — NULL
// (version $00) when the FPU is in the reset state, IDLE (version $41, size
// byte $00) otherwise.  The larger UNIMP/BUSY frames only exist mid-exception
// on real silicon; this functional model completes every FP operation
// synchronously, so there is never a mid-instruction state to dump.  Frame
// sizes cross-checked against the FPSP equates (fpsp.h: IDLE_SIZE=4,
// UNIMP_41_SIZE=52, BUSY_SIZE=100).
int fpu_fsave040(fpu_state_t *fpu, uint32_t addr) {
    memory_write_uint32(addr, fpu->initialized ? 0x41000000u : 0x00000000u);
    // After FSAVE the FPU is in the null state; the programmer model
    // (FP0-FP7, control registers) is preserved, as on the 6888x path.
    fpu->initialized = false;
    fpu->pre_exc_mask = 0;
    return 4;
}

// MC68040 FRESTORE: a NULL frame resets the FPU; IDLE/UNIMP/BUSY frames
// are consumed by their size byte (bits 23:16 = $00/$30/$60; total frame
// is size+4) and re-arm the idle state.
int fpu_frestore040(fpu_state_t *fpu, uint32_t addr) {
    uint32_t header = memory_read_uint32(addr);
    uint32_t version = header >> 24;
    uint32_t size = (header >> 16) & 0xFF;

    if (version == 0) {
        // Null frame: reset to hardware-reset state (same discipline as the
        // 6888x path — a full reset only when aborting an initialized state).
        if (fpu->initialized) {
            for (int i = 0; i < 8; i++)
                fpu->fp[i] = (float80_reg_t){0x7FFF, 0xFFFFFFFFFFFFFFFFULL};
            fpu->fpcr = 0;
            fpu->fpsr = 0;
            fpu->fpiar = 0;
        }
        fpu->pre_exc_mask = 0;
        fpu->exceptional_operand = FP80_ZERO;
        fpu->initialized = false;
        return 4;
    }

    fpu->initialized = true;
    return 4 + (int)size;
}

// ============================================================================
// FMOVECR - load FPU ROM constant
// ============================================================================

// FMOVECR ROM constants, indexed by offset.  Transcendental constants and
// the large powers of ten carry extra bits in mantissa_lo, so fpu_pack can
// round them per FPCR mode.  Offsets with no entry (mantissa_hi == 0) are
// undefined ROM locations and read as +0.0, as does 0x0F (zero by design).
static const fpu_unpacked_t fpu_rom_table[0x40] = {
    [0x00] = {false, 1,     0xC90FDAA22168C234ULL, 0xC4C6628B80DC1CD1ULL}, // pi
    [0x01] = {false, 2,     0xFE00068200000000ULL, 0                    }, // undocumented 68882 ROM offset 0x01
    [0x0B] = {false, -2,    0x9A209A84FBCFF798ULL, 0x8F8959AC0B7C9178ULL}, // log10(2)
    [0x0C] = {false, 1,     0xADF85458A2BB4A9AULL, 0                    }, // e (68882 RN value; math lo would over-round)
    [0x0D] = {false, 0,     0xB8AA3B295C17F0BBULL, 0xBE87FED0691D3E88ULL}, // log2(e)
    [0x0E] = {false, -2,    0xDE5BD8A937287195ULL, 0x355BAAAFAD33DC32ULL}, // log10(e)
    [0x30] = {false, -1,    0xB17217F7D1CF79ABULL, 0xC9E3B39803F2F6AFULL}, // ln(2)
    [0x31] = {false, 1,     0x935D8DDDAAA8AC16ULL, 0xEA56D62B82D30A28ULL}, // ln(10)
    [0x32] = {false, 0,     0x8000000000000000ULL, 0                    }, // 10^0 = 1.0
    [0x33] = {false, 3,     0xA000000000000000ULL, 0                    }, // 10^1
    [0x34] = {false, 6,     0xC800000000000000ULL, 0                    }, // 10^2
    [0x35] = {false, 13,    0x9C40000000000000ULL, 0                    }, // 10^4
    [0x36] = {false, 26,    0xBEBC200000000000ULL, 0                    }, // 10^8
    [0x37] = {false, 53,    0x8E1BC9BF04000000ULL, 0                    }, // 10^16
    [0x38] = {false, 106,   0x9DC5ADA82B70B59DULL, 0xF020000000000000ULL}, // 10^32
    [0x39] = {false, 212,   0xC2781F49FFCFA6D5ULL, 0x3CBF6B71C76B25FBULL}, // 10^64
    [0x3A] = {false, 425,   0x93BA47C980E98CDFULL, 0xC66F336C36B10137ULL}, // 10^128
    [0x3B] = {false, 850,   0xAA7EEBFB9DF9DE8DULL, 0xDDBB901B98FEEAB7ULL}, // 10^256
    [0x3C] = {false, 1700,  0xE319A0AEA60E91C6ULL, 0xCC655C54BC5058F8ULL}, // 10^512
    [0x3D] = {false, 3401,  0xC976758681750C17ULL, 0x650D3D28F18B50CEULL}, // 10^1024
    [0x3E] = {false, 6803,  0x9E8B3B5DC53D5DE4ULL, 0xA74D28CE329ACE52ULL}, // 10^2048
    [0x3F] = {false, 13606, 0xC46052028A20979AULL, 0                    }, // 10^4096 (68882 RN value; math lo would over-round)
};

fpu_unpacked_t fpu_rom_constant(unsigned offset) {
    if (offset < 0x40 && fpu_rom_table[offset].mantissa_hi != 0)
        return fpu_rom_table[offset];
    return (fpu_unpacked_t){false, FPU_EXP_ZERO, 0, 0};
}

// ============================================================================
// Source operand loading from EA
// ============================================================================

// Load a value from the effective address in the given format
static float80_reg_t fpu_load_ea(cpu_t *cpu, uint16_t opcode, unsigned format) {
    unsigned ea_mode = (opcode >> 3) & 7;
    unsigned ea_reg = opcode & 7;

    switch (format) {
    case 0: { // Long integer (4 bytes)
        int32_t v = (int32_t)read_ea_32(cpu, opcode, true);
        return fpu_from_int32(v);
    }
    case 1: { // Single (4 bytes)
        uint32_t bits = read_ea_32(cpu, opcode, true);
        return fpu_from_single(bits);
    }
    case 2: { // Extended (12 bytes)
        uint32_t ea = calculate_ea(cpu, 12, ea_mode, ea_reg, true);
        uint32_t w0 = memory_read_uint32(ea);
        uint32_t w1 = memory_read_uint32(ea + 4);
        uint32_t w2 = memory_read_uint32(ea + 8);
        return fpu_from_extended(w0, w1, w2);
    }
    case 4: { // Word integer (2 bytes)
        int16_t v = (int16_t)read_ea_16(cpu, opcode, true);
        return fpu_from_int16(v);
    }
    case 5: { // Double (8 bytes)
        uint32_t ea = calculate_ea(cpu, 8, ea_mode, ea_reg, true);
        uint64_t bits = ((uint64_t)memory_read_uint32(ea) << 32) | memory_read_uint32(ea + 4);
        return fpu_from_double(bits);
    }
    case 6: { // Byte integer (1 byte)
        int8_t v = (int8_t)read_ea_8(cpu, opcode, true);
        return fpu_from_int8(v);
    }
    case 3: // Packed decimal (12 bytes)
    case 7: {
        uint32_t ea = calculate_ea(cpu, 12, ea_mode, ea_reg, true);
        uint32_t w0 = memory_read_uint32(ea);
        uint32_t w1 = memory_read_uint32(ea + 4);
        uint32_t w2 = memory_read_uint32(ea + 8);
        return fpu_from_packed(cpu->fpu, w0, w1, w2);
    }
    default:
        return FP80_ZERO;
    }
}

// ============================================================================
// Store result to EA (FMOVE FPn -> memory)
// ============================================================================

// Store an FP register value to the EA in the given format
static void fpu_store_ea(cpu_t *cpu, fpu_state_t *fpu, uint16_t opcode, float80_reg_t val, unsigned format) {
    unsigned ea_mode = (opcode >> 3) & 7;
    unsigned ea_reg = opcode & 7;

    switch (format) {
    case 0: { // Long integer
        int32_t v = fpu_to_int32(fpu, val);
        write_ea_32(cpu, ea_mode, ea_reg, (uint32_t)v);
        break;
    }
    case 1: { // Single
        uint32_t bits = fpu_to_single(fpu, val);
        write_ea_32(cpu, ea_mode, ea_reg, bits);
        break;
    }
    case 2: { // Extended (12 bytes)
        // For FMOVE.X to memory, the 68882 normalizes abnormal representations
        // (pseudo-denormals, unnormals) through the internal pipeline, applying
        // FPCR precision/rounding. Normal values are written directly.
        float80_reg_t store_val = val;
        uint16_t bexp = fp80_exp(val);
        bool j_bit = (val.mantissa >> 63) & 1;
        bool is_abnormal = false;
        if (bexp == 0 && j_bit) {
            is_abnormal = true; // pseudo-denormal
        } else if (bexp != 0 && bexp != 0x7FFF && !j_bit && val.mantissa != 0) {
            is_abnormal = true; // unnormal
        }
        if (is_abnormal) {
            store_val = fpu_pack(fpu, fpu_unpack(val));
        }
        uint32_t w0, w1, w2;
        fpu_to_extended(store_val, &w0, &w1, &w2);
        uint32_t ea = calculate_ea(cpu, 12, ea_mode, ea_reg, true);
        memory_write_uint32(ea, w0);
        memory_write_uint32(ea + 4, w1);
        memory_write_uint32(ea + 8, w2);
        break;
    }
    case 4: { // Word integer
        int16_t v = fpu_to_int16(fpu, val);
        write_ea_16(cpu, ea_mode, ea_reg, (uint16_t)v);
        break;
    }
    case 5: { // Double (8 bytes)
        uint64_t bits = fpu_to_double(fpu, val);
        uint32_t ea = calculate_ea(cpu, 8, ea_mode, ea_reg, true);
        memory_write_uint32(ea, (uint32_t)(bits >> 32));
        memory_write_uint32(ea + 4, (uint32_t)(bits & 0xFFFFFFFF));
        break;
    }
    case 6: { // Byte integer
        int8_t v = fpu_to_int8(fpu, val);
        write_ea_8(cpu, ea_mode, ea_reg, (uint8_t)v);
        break;
    }
    default: // Packed decimal handled separately in fpu_general_op case 3
        break;
    }
}

// ============================================================================
// FMOVEM - save/restore FP data registers
// ============================================================================

// Size of each FP register in memory (extended = 12 bytes)
#define FP_REG_SIZE 12

// Check if EA mode/reg is valid for FMOVEM data register save or restore.
// Returns true if valid, false if the instruction should take an F-line exception.
static bool fpu_movem_data_ea_valid(unsigned ea_mode, unsigned ea_reg, unsigned dir) {
    if (dir) {
        // Restore (mem→reg): (An)+, or control modes
        if (ea_mode == 3)
            return true; // (An)+ postincrement
        if (ea_mode == 2)
            return true; // (An) indirect
        if (ea_mode == 5)
            return true; // d16(An)
        if (ea_mode == 6)
            return true; // d8(An,Xn)
        if (ea_mode == 7 && ea_reg <= 3)
            return true; // abs.W, abs.L, d16(PC), d8(PC,Xn)
        return false;
    }
    // Save (reg→mem): -(An), or control alterable modes
    if (ea_mode == 4)
        return true; // -(An) predecrement
    if (ea_mode == 2)
        return true; // (An) indirect
    if (ea_mode == 5)
        return true; // d16(An)
    if (ea_mode == 6)
        return true; // d8(An,Xn)
    if (ea_mode == 7 && ea_reg <= 1)
        return true; // abs.W, abs.L
    return false;
}

// FMOVEM data register list save/restore.
// Returns false if the EA mode is invalid (caller should take F-line exception).
static bool fpu_movem_data(cpu_t *cpu, fpu_state_t *fpu, uint16_t opcode, uint16_t ext) {
    unsigned ea_mode = (opcode >> 3) & 7;
    unsigned ea_reg = opcode & 7;

    // 68882 FMOVEM data register encoding (MC68882 User Manual §6.1.5):
    //   Bits 15-13 encode type: 110 (dr=0) = memory→register (restore),
    //   111 (dr=1) = register→memory (save)
    //   Bit 11 encodes mode: 0 = static register list (bits 7-0), 1 = dynamic (Dn in bits 6-4)
    // For (An)+ and -(An) addressing, the direction is implicit from the EA mode.
    unsigned dir;
    if (ea_mode == 3) // (An)+: always restore (memory → register)
        dir = 1;
    else if (ea_mode == 4) // -(An): always save (register → memory)
        dir = 0;
    else
        dir = ((ext >> 13) & 1) ^ 1; // control modes: dr=1 is save, so dir(restore)=1 when dr=0

    // Validate EA mode before decoding register list
    if (!fpu_movem_data_ea_valid(ea_mode, ea_reg, dir))
        return false;

    unsigned mode_d = (ext >> 11) & 1; // bit 11: 0=static list, 1=dynamic (Dn)
    uint8_t reglist;

    // Get register list: static from ext word, or dynamic from Dn
    if (mode_d)
        reglist = (uint8_t)cpu->d[(ext >> 4) & 7];
    else
        reglist = (uint8_t)(ext & 0xFF);

    if (dir) {
        // Direction: memory -> FPn (restore)
        if (ea_mode == 3) {
            // (An)+ postincrement
            uint32_t addr = cpu->a[ea_reg];
            for (int i = 0; i < 8; i++) {
                if (reglist & (1 << (7 - i))) {
                    uint32_t w0 = memory_read_uint32(addr);
                    uint32_t w1 = memory_read_uint32(addr + 4);
                    uint32_t w2 = memory_read_uint32(addr + 8);
                    fpu->fp[i] = fpu_from_extended(w0, w1, w2);
                    addr += FP_REG_SIZE;
                }
            }
            cpu->a[ea_reg] = addr;
        } else {
            // Other EA modes (control alterable)
            uint32_t addr = calculate_ea(cpu, 0, ea_mode, ea_reg, true);
            for (int i = 0; i < 8; i++) {
                if (reglist & (1 << (7 - i))) {
                    uint32_t w0 = memory_read_uint32(addr);
                    uint32_t w1 = memory_read_uint32(addr + 4);
                    uint32_t w2 = memory_read_uint32(addr + 8);
                    fpu->fp[i] = fpu_from_extended(w0, w1, w2);
                    addr += FP_REG_SIZE;
                }
            }
        }
    } else {
        // Direction: FPn -> memory (save)
        if (ea_mode == 4) {
            // -(An) predecrement: reversed bit order (bit i = FPi)
            uint32_t addr = cpu->a[ea_reg];
            for (int i = 7; i >= 0; i--) {
                if (reglist & (1 << i)) {
                    addr -= FP_REG_SIZE;
                    uint32_t w0, w1, w2;
                    fpu_to_extended(fpu->fp[i], &w0, &w1, &w2);
                    memory_write_uint32(addr, w0);
                    memory_write_uint32(addr + 4, w1);
                    memory_write_uint32(addr + 8, w2);
                }
            }
            cpu->a[ea_reg] = addr;
        } else {
            // Other EA modes (control alterable)
            uint32_t addr = calculate_ea(cpu, 0, ea_mode, ea_reg, true);
            for (int i = 0; i < 8; i++) {
                if (reglist & (1 << (7 - i))) {
                    uint32_t w0, w1, w2;
                    fpu_to_extended(fpu->fp[i], &w0, &w1, &w2);
                    memory_write_uint32(addr, w0);
                    memory_write_uint32(addr + 4, w1);
                    memory_write_uint32(addr + 8, w2);
                    addr += FP_REG_SIZE;
                }
            }
        }
    }
    return true;
}

// FMOVEM control register save/restore.
// The 68882 masks undefined bits on write to FPCR and FPSR.
// Returns false if the encoding is invalid (caller takes F-line exception).
#define FPCR_MASK 0x0000FFF0 // bits 15:4 are defined
#define FPSR_MASK 0x0FFFFFF8 // bits 27:3 are defined

static bool fpu_movem_control(cpu_t *cpu, fpu_state_t *fpu, uint16_t opcode, uint16_t ext) {
    unsigned ea_mode = (opcode >> 3) & 7;
    unsigned ea_reg = opcode & 7;
    unsigned regsel = (ext >> 10) & 7; // bits 12:10: which control regs

    // 68882: regsel=0 defaults to FPIAR transfer
    if (regsel == 0)
        regsel = 1;

    // Count how many control registers are in the list
    int count = ((regsel >> 2) & 1) + ((regsel >> 1) & 1) + (regsel & 1);
    int total_size = count * 4;

    // Dn mode: only single register allowed (count <= 1)
    if (ea_mode == 0 && count > 1)
        return false;

    if ((ext >> 13) & 1) {
        // Control registers -> EA (save)
        if (ea_mode == 0) {
            // Dn - single register only
            if (regsel & 4)
                cpu->d[ea_reg] = fpu->fpcr;
            if (regsel & 2)
                cpu->d[ea_reg] = fpu->fpsr;
            if (regsel & 1)
                cpu->d[ea_reg] = fpu->fpiar;
        } else if (ea_mode == 4) {
            // -(An) predecrement
            uint32_t addr = cpu->a[ea_reg];
            // Write in reverse order: FPIAR, FPSR, FPCR
            if (regsel & 1) {
                addr -= 4;
                memory_write_uint32(addr, fpu->fpiar);
            }
            if (regsel & 2) {
                addr -= 4;
                memory_write_uint32(addr, fpu->fpsr);
            }
            if (regsel & 4) {
                addr -= 4;
                memory_write_uint32(addr, fpu->fpcr);
            }
            cpu->a[ea_reg] = addr;
        } else {
            uint32_t addr = calculate_ea(cpu, (uint32_t)total_size, ea_mode, ea_reg, true);
            if (regsel & 4) {
                memory_write_uint32(addr, fpu->fpcr);
                addr += 4;
            }
            if (regsel & 2) {
                memory_write_uint32(addr, fpu->fpsr);
                addr += 4;
            }
            if (regsel & 1) {
                memory_write_uint32(addr, fpu->fpiar);
                addr += 4;
            }
        }
    } else {
        // EA -> control registers (restore)
        if (ea_mode == 3) {
            // (An)+ postincrement
            uint32_t addr = cpu->a[ea_reg];
            if (regsel & 4) {
                fpu->fpcr = memory_read_uint32(addr) & FPCR_MASK;
                addr += 4;
            }
            if (regsel & 2) {
                fpu->fpsr = memory_read_uint32(addr) & FPSR_MASK;
                addr += 4;
            }
            if (regsel & 1) {
                fpu->fpiar = memory_read_uint32(addr);
                addr += 4;
            }
            cpu->a[ea_reg] = addr;
        } else if (ea_mode == 0) {
            // Dn - single register only
            uint32_t val = cpu->d[ea_reg];
            if (regsel & 4)
                fpu->fpcr = val & FPCR_MASK;
            if (regsel & 2)
                fpu->fpsr = val & FPSR_MASK;
            if (regsel & 1)
                fpu->fpiar = val;
        } else {
            uint32_t addr = calculate_ea(cpu, (uint32_t)total_size, ea_mode, ea_reg, true);
            if (regsel & 4) {
                fpu->fpcr = memory_read_uint32(addr) & FPCR_MASK;
                addr += 4;
            }
            if (regsel & 2) {
                fpu->fpsr = memory_read_uint32(addr) & FPSR_MASK;
                addr += 4;
            }
            if (regsel & 1) {
                fpu->fpiar = memory_read_uint32(addr);
                addr += 4;
            }
        }
    }
    return true;
}

// ============================================================================
// Soft-float arithmetic operations
// ============================================================================

// NaN propagation helper: return appropriate NaN for dyadic ops
static fpu_unpacked_t fpu_propagate_nan(fpu_state_t *fpu, fpu_unpacked_t a, fpu_unpacked_t b) {
    // Check for SNaN first
    bool a_snan = (a.exponent == FPU_EXP_INF && a.mantissa_hi != 0 && !(a.mantissa_hi & 0x4000000000000000ULL));
    bool b_snan = (b.exponent == FPU_EXP_INF && b.mantissa_hi != 0 && !(b.mantissa_hi & 0x4000000000000000ULL));
    if (a_snan || b_snan)
        fpu->fpsr |= FPEXC_SNAN;

    // 68882 rule: propagate destination NaN if both are NaN
    bool a_nan = (a.exponent == FPU_EXP_INF && a.mantissa_hi != 0);
    bool b_nan = (b.exponent == FPU_EXP_INF && b.mantissa_hi != 0);

    fpu_unpacked_t result;
    if (a_nan && b_nan)
        result = a; // propagate destination
    else if (a_nan)
        result = a;
    else
        result = b;
    // Ensure quiet NaN
    result.mantissa_hi |= 0x4000000000000000ULL;
    return result;
}

// Add magnitudes |a| + |b| (operands of the same sign); result has a's sign.
// Post-condition: the only possible denormalization is a carry out of the
// 128-bit sum, which is shifted back in here, so the result is normalized
// whenever both inputs were — no fpu_normalize() pass needed.
static fpu_unpacked_t fpu_op_add_mag(fpu_unpacked_t a, fpu_unpacked_t b) {
    // Align exponents: shift the smaller one right (sticky preserves lost bits)
    int32_t diff = a.exponent - b.exponent;
    if (diff > 0) {
        uint128_shr_sticky(&b.mantissa_hi, &b.mantissa_lo, diff);
        b.exponent = a.exponent;
    } else if (diff < 0) {
        diff = -diff;
        uint128_shr_sticky(&a.mantissa_hi, &a.mantissa_lo, diff);
        a.exponent = b.exponent;
    }

    fpu_unpacked_t r;
    r.sign = a.sign;
    r.exponent = a.exponent;
    uint128_add(&r.mantissa_hi, &r.mantissa_lo, a.mantissa_hi, a.mantissa_lo, b.mantissa_hi, b.mantissa_lo);

    // Check for carry (mantissa overflowed 128 bits)
    if (r.mantissa_hi < a.mantissa_hi || (r.mantissa_hi == a.mantissa_hi && r.mantissa_lo < a.mantissa_lo)) {
        r.mantissa_lo = (r.mantissa_lo >> 1) | (r.mantissa_hi << 63);
        r.mantissa_hi = (r.mantissa_hi >> 1) | 0x8000000000000000ULL;
        r.exponent++;
    }
    return r;
}

// Subtract magnitudes: |a| - |b| (assumes |a| > |b|), signed `result_sign`.
// Post-condition: cancellation can clear any number of leading bits, so the
// difference is always passed through fpu_normalize() before returning.
static fpu_unpacked_t fpu_op_sub_mag(fpu_unpacked_t a, fpu_unpacked_t b, bool result_sign) {
    // Align exponents (sticky preserves lost bits)
    int32_t diff = a.exponent - b.exponent;
    if (diff > 0) {
        uint128_shr_sticky(&b.mantissa_hi, &b.mantissa_lo, diff);
        b.exponent = a.exponent;
    }

    fpu_unpacked_t r;
    r.sign = result_sign;
    r.exponent = a.exponent;
    uint128_sub(&r.mantissa_hi, &r.mantissa_lo, a.mantissa_hi, a.mantissa_lo, b.mantissa_hi, b.mantissa_lo);

    // Normalize result
    fpu_normalize(&r);
    return r;
}

// Compare 128-bit magnitudes: 1 if a>b, -1 if a<b, 0 if equal
static int cmp128(uint64_t a_hi, uint64_t a_lo, uint64_t b_hi, uint64_t b_lo) {
    if (a_hi > b_hi)
        return 1;
    if (a_hi < b_hi)
        return -1;
    if (a_lo > b_lo)
        return 1;
    if (a_lo < b_lo)
        return -1;
    return 0;
}

// Core add operation (handles signs)
fpu_unpacked_t fpu_op_add(fpu_state_t *fpu, fpu_unpacked_t a, fpu_unpacked_t b) {
    // Handle special cases
    bool a_nan = (a.exponent == FPU_EXP_INF && a.mantissa_hi != 0);
    bool b_nan = (b.exponent == FPU_EXP_INF && b.mantissa_hi != 0);
    if (a_nan || b_nan)
        return fpu_propagate_nan(fpu, a, b);

    bool a_inf = (a.exponent == FPU_EXP_INF && a.mantissa_hi == 0);
    bool b_inf = (b.exponent == FPU_EXP_INF && b.mantissa_hi == 0);
    if (a_inf && b_inf) {
        if (a.sign != b.sign) {
            // +inf + (-inf) = NaN (OPERR)
            fpu->fpsr |= FPEXC_OPERR;
            fpu_unpacked_t nan = {false, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
            return nan;
        }
        return a; // inf + inf = inf
    }
    if (a_inf)
        return a;
    if (b_inf)
        return b;

    bool a_zero = (a.exponent == FPU_EXP_ZERO);
    bool b_zero = (b.exponent == FPU_EXP_ZERO);
    if (a_zero && b_zero) {
        // 0 + 0: IEEE 754 sign rules
        fpu_unpacked_t r = {false, FPU_EXP_ZERO, 0, 0};
        if (a.sign && b.sign)
            r.sign = true; // both negative -> negative zero
        else if (a.sign != b.sign) {
            // Different signs: +0 unless round toward -inf
            unsigned rmode = (fpu->fpcr >> 4) & 3;
            if (rmode == 2)
                r.sign = true; // round toward -inf -> -0
        }
        return r;
    }
    if (a_zero)
        return b;
    if (b_zero)
        return a;

    // Normal addition/subtraction
    if (a.sign == b.sign) {
        return fpu_op_add_mag(a, b);
    } else {
        // Different signs: subtract magnitudes
        int cmp = (a.exponent > b.exponent)   ? 1
                  : (a.exponent < b.exponent) ? -1
                                              : cmp128(a.mantissa_hi, a.mantissa_lo, b.mantissa_hi, b.mantissa_lo);
        if (cmp == 0) {
            // Equal magnitudes, different signs: result is zero
            unsigned rmode = (fpu->fpcr >> 4) & 3;
            fpu_unpacked_t r = {false, FPU_EXP_ZERO, 0, 0};
            if (rmode == 2)
                r.sign = true; // round toward -inf -> -0
            return r;
        }
        // The result takes the sign of the larger magnitude
        if (cmp > 0)
            return fpu_op_sub_mag(a, b, a.sign);
        return fpu_op_sub_mag(b, a, b.sign);
    }
}

// Subtract: a - b = a + (-b)
fpu_unpacked_t fpu_op_sub(fpu_state_t *fpu, fpu_unpacked_t a, fpu_unpacked_t b) {
    // Propagate NaN before sign negation to preserve original NaN sign
    bool a_nan = (a.exponent == FPU_EXP_INF && a.mantissa_hi != 0);
    bool b_nan = (b.exponent == FPU_EXP_INF && b.mantissa_hi != 0);
    if (a_nan || b_nan)
        return fpu_propagate_nan(fpu, a, b);
    b.sign = !b.sign;
    return fpu_op_add(fpu, a, b);
}

// Multiply
fpu_unpacked_t fpu_op_mul(fpu_state_t *fpu, fpu_unpacked_t a, fpu_unpacked_t b) {
    bool a_nan = (a.exponent == FPU_EXP_INF && a.mantissa_hi != 0);
    bool b_nan = (b.exponent == FPU_EXP_INF && b.mantissa_hi != 0);
    if (a_nan || b_nan)
        return fpu_propagate_nan(fpu, a, b);

    bool result_sign = a.sign ^ b.sign;
    bool a_inf = (a.exponent == FPU_EXP_INF);
    bool b_inf = (b.exponent == FPU_EXP_INF);
    bool a_zero = (a.exponent == FPU_EXP_ZERO);
    bool b_zero = (b.exponent == FPU_EXP_ZERO);

    if ((a_inf && b_zero) || (b_inf && a_zero)) {
        // inf * 0 = NaN (OPERR)
        fpu->fpsr |= FPEXC_OPERR;
        fpu_unpacked_t nan = {false, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
        return nan;
    }
    if (a_inf || b_inf) {
        fpu_unpacked_t r = {result_sign, FPU_EXP_INF, 0, 0};
        return r;
    }
    if (a_zero || b_zero) {
        fpu_unpacked_t r = {result_sign, FPU_EXP_ZERO, 0, 0};
        return r;
    }

    // Normal multiply: 64x64 -> 128
    fpu_unpacked_t r;
    r.sign = result_sign;
    r.exponent = a.exponent + b.exponent;

    // Full 128x128 -> 256-bit product of {mantissa_hi, mantissa_lo}, as four
    // limbs w3:w2:w1:w0 (w3 most significant):
    //   HH*2^128 + (HL + LH)*2^64 + LL
    // The top 128 bits become the result; the bottom 128 fold into sticky.
    uint64_t hh_hi, hh_lo, hl_hi, hl_lo, lh_hi, lh_lo, ll_hi, ll_lo;
    uint64_mul128(a.mantissa_hi, b.mantissa_hi, &hh_hi, &hh_lo);
    uint64_mul128(a.mantissa_hi, b.mantissa_lo, &hl_hi, &hl_lo);
    uint64_mul128(a.mantissa_lo, b.mantissa_hi, &lh_hi, &lh_lo);
    uint64_mul128(a.mantissa_lo, b.mantissa_lo, &ll_hi, &ll_lo);

    uint64_t w0 = ll_lo;
    uint64_t w1 = ll_hi;
    uint64_t w2 = hh_lo;
    uint64_t w3 = hh_hi;
    // Add HL and LH (each spanning w2:w1) with carries into w3:w2.  The
    // full product is below 2^256, so w3 never overflows.
    w1 += hl_lo;
    uint128_add(&w3, &w2, w3, w2, 0, w1 < hl_lo);
    uint128_add(&w3, &w2, w3, w2, 0, hl_hi);
    w1 += lh_lo;
    uint128_add(&w3, &w2, w3, w2, 0, w1 < lh_lo);
    uint128_add(&w3, &w2, w3, w2, 0, lh_hi);

    r.mantissa_hi = w3;
    r.mantissa_lo = w2;
    if (w1 || w0)
        r.mantissa_lo |= 1; // sticky

    // Normalize and adjust exponent for implicit integer bit
    if (r.mantissa_hi == 0 && r.mantissa_lo == 0) {
        r.exponent = FPU_EXP_ZERO;
    } else {
        fpu_normalize(&r);
        r.exponent += 1; // account for double-counting of integer bit
    }

    return r;
}

// Divide: a / b
fpu_unpacked_t fpu_op_div(fpu_state_t *fpu, fpu_unpacked_t a, fpu_unpacked_t b) {
    bool a_nan = (a.exponent == FPU_EXP_INF && a.mantissa_hi != 0);
    bool b_nan = (b.exponent == FPU_EXP_INF && b.mantissa_hi != 0);
    if (a_nan || b_nan)
        return fpu_propagate_nan(fpu, a, b);

    bool result_sign = a.sign ^ b.sign;
    bool a_inf = (a.exponent == FPU_EXP_INF);
    bool b_inf = (b.exponent == FPU_EXP_INF);
    bool a_zero = (a.exponent == FPU_EXP_ZERO);
    bool b_zero = (b.exponent == FPU_EXP_ZERO);

    if (a_inf && b_inf) {
        fpu->fpsr |= FPEXC_OPERR;
        fpu_unpacked_t nan = {false, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
        return nan;
    }
    if (a_zero && b_zero) {
        // 0 / 0 = NaN (OPERR)
        fpu->fpsr |= FPEXC_OPERR;
        fpu_unpacked_t nan = {false, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
        return nan;
    }
    if (a_inf) {
        fpu_unpacked_t r = {result_sign, FPU_EXP_INF, 0, 0};
        return r;
    }
    if (b_zero) {
        // x / 0 = infinity (DZ exception)
        fpu->fpsr |= FPEXC_DZ;
        fpu_unpacked_t r = {result_sign, FPU_EXP_INF, 0, 0};
        return r;
    }
    if (a_zero || b_inf) {
        fpu_unpacked_t r = {result_sign, FPU_EXP_ZERO, 0, 0};
        return r;
    }

    // The long division below needs both mantissas normalized (J-bit set):
    // an unnormal divisor would overflow the 64-bit quotient.  Normalizing
    // keeps the value, so do it for unnormal operands as the 68882 does.
    if (!(a.mantissa_hi & 0x8000000000000000ULL)) {
        fpu_normalize(&a);
        if (a.exponent == FPU_EXP_ZERO)
            return (fpu_unpacked_t){result_sign, FPU_EXP_ZERO, 0, 0};
    }
    if (!(b.mantissa_hi & 0x8000000000000000ULL)) {
        fpu_normalize(&b);
        if (b.exponent == FPU_EXP_ZERO) {
            fpu->fpsr |= FPEXC_DZ;
            return (fpu_unpacked_t){result_sign, FPU_EXP_INF, 0, 0};
        }
    }

    // Shift-and-subtract division for 64+64 quotient bits
    fpu_unpacked_t r;
    r.sign = result_sign;
    r.exponent = a.exponent - b.exponent;

    uint64_t dividend_hi = a.mantissa_hi;
    uint64_t dividend_lo = a.mantissa_lo;
    uint64_t divisor = b.mantissa_hi;

    // Long division: produce 64 bits of quotient in q_hi
    uint64_t q_hi = 0;
    bool carry = false;
    for (int i = 63; i >= 0; i--) {
        if (carry || dividend_hi >= divisor) {
            q_hi |= (1ULL << i);
            dividend_hi -= divisor;
        }
        carry = (dividend_hi >> 63) != 0;
        dividend_hi = (dividend_hi << 1) | (dividend_lo >> 63);
        dividend_lo <<= 1;
    }

    // Continue for 64 more bits (guard/round/sticky)
    uint64_t q_lo = 0;
    for (int i = 63; i >= 0; i--) {
        if (carry || dividend_hi >= divisor) {
            q_lo |= (1ULL << i);
            dividend_hi -= divisor;
        }
        carry = (dividend_hi >> 63) != 0;
        dividend_hi = (dividend_hi << 1) | (dividend_lo >> 63);
        dividend_lo <<= 1;
    }

    // Sticky bit for remainder
    if (dividend_hi != 0 || dividend_lo != 0)
        q_lo |= 1;

    r.mantissa_hi = q_hi;
    r.mantissa_lo = q_lo;

    fpu_normalize(&r);
    return r;
}

// Square root.
//
// Uses native `__uint128_t` arithmetic for the Newton-Raphson refinement
// loops below. The 128-bit divide is the load-bearing step we have no
// portable fallback for, so pin the assumption at compile time.
#ifndef __SIZEOF_INT128__
#error "fpu_op_sqrt requires a compiler with __uint128_t (gcc/clang/emcc)"
#endif
fpu_unpacked_t fpu_op_sqrt(fpu_state_t *fpu, fpu_unpacked_t a) {
    // Handle specials
    if (a.exponent == FPU_EXP_INF && a.mantissa_hi != 0) {
        // NaN: propagate (make quiet)
        if (!(a.mantissa_hi & 0x4000000000000000ULL))
            fpu->fpsr |= FPEXC_SNAN;
        a.mantissa_hi |= 0x4000000000000000ULL;
        return a;
    }
    if (a.exponent == FPU_EXP_ZERO)
        return a; // sqrt(+-0) = +-0
    if (a.sign) {
        // sqrt(negative) = NaN (OPERR)
        fpu->fpsr |= FPEXC_OPERR;
        fpu_unpacked_t nan = {false, FPU_EXP_INF, 0xFFFFFFFFFFFFFFFFULL, 0};
        return nan;
    }
    if (a.exponent == FPU_EXP_INF)
        return a; // sqrt(+inf) = +inf
    if (!(a.mantissa_hi & 0x8000000000000000ULL)) {
        // Unnormal (J-bit clear): normalize first, as the 68882 does
        fpu_normalize(&a);
        if (a.exponent == FPU_EXP_ZERO)
            return a; // unnormal zero: sqrt(+-0) = +-0
    }

    // Compute sqrt using exact integer arithmetic for IEEE 754 correct rounding.
    // For even exp: sqrt(M * 2^exp) = sqrt(M) * 2^(exp/2), where q²=S/2
    // For odd exp:  sqrt(M * 2^exp) = sqrt(2M) * 2^((exp-1)/2), where q²=S
    // S = {mantissa_hi, mantissa_lo} as 128-bit integer.
    int32_t exp = a.exponent;
    bool odd_exp = (exp & 1) != 0;
    if (odd_exp)
        exp -= 1;
    int32_t result_exp = exp / 2;

    // q = floor(sqrt(target)) by integer Newton-Raphson, where target = S
    // (odd exponent) or S/2 (even), S = {mantissa_hi, mantissa_lo} as a
    // 128-bit integer.  Pure integer arithmetic keeps the result independent
    // of the host libm.  The seed 2^ceil(bits/2) is >= sqrt(target), and from
    // above the iteration x' = (x + target/x) / 2 decreases monotonically to
    // floor(sqrt(target)); it stops at the first step that does not decrease.
    // target >= 2^126 (normalized mantissa), so q lands in [2^63, 2^64).
    __uint128_t S = ((__uint128_t)a.mantissa_hi << 64) | a.mantissa_lo;
    __uint128_t target = odd_exp ? S : (S >> 1);
    uint64_t target_hi = (uint64_t)(target >> 64);
    int target_bits = target_hi ? 128 - clz64(target_hi) : 64 - clz64((uint64_t)target);
    __uint128_t q128 = (__uint128_t)1 << ((target_bits + 1) / 2);
    for (;;) {
        __uint128_t next = (q128 + target / q128) >> 1;
        if (next >= q128)
            break;
        q128 = next;
    }
    uint64_t q = (uint64_t)q128;

    // Check if result is exact; encode remainder for correct rounding.
    // The true sqrt lies between q and q+1. The rounding "halfway" point
    // corresponds to sqrt(S) = q + 0.5, i.e. S = q² + q + 0.25.
    // Since S is always integer, this can never be exact:
    //   remainder = q   → sqrt < q+0.5 (just below halfway)
    //   remainder = q+1 → sqrt > q+0.5 (just above halfway)
    __uint128_t q_sq = q128 * q128;
    __uint128_t remainder = target - q_sq;

    fpu_unpacked_t r;
    r.sign = false;
    r.exponent = result_exp;
    r.mantissa_hi = q;
    if (remainder == 0) {
        // Exact: check for lost bit from S>>1 for even exponent
        r.mantissa_lo = (!odd_exp && (S & 1)) ? 1 : 0;
    } else if (remainder > q128) {
        // Above halfway: encode > 0.5 ULP for round-up
        r.mantissa_lo = 0xC000000000000000ULL;
    } else {
        // Below halfway (includes remainder == q): sticky bit only
        r.mantissa_lo = 1;
    }

    fpu_normalize(&r);
    return r;
}

// ============================================================================
// Arithmetic operation dispatch
// ============================================================================

// Clear per-operation exception status bits (bits 15:8), preserve accrued (bits 7:3)
// Also clear the pre-instruction exception mask since a new operation is starting.
static void fpu_clear_status(fpu_state_t *fpu) {
    fpu->fpsr &= ~0xFF00u;
    fpu->pre_exc_mask = 0;
}

// Propagate exception status bits (15:8) to accrued exception bits (7:3)
// Per MC68881/68882 manual:
// - OVFL always sets accrued OVFL and accrued INEX
// - UNFL sets accrued UNFL only when INEX2 is also set
// - INEX1/INEX2 set accrued INEX
static void fpu_update_accrued(fpu_state_t *fpu) {
    uint32_t exc = fpu->fpsr & 0xFF00u;
    if (exc & (FPEXC_BSUN | FPEXC_SNAN | FPEXC_OPERR))
        fpu->fpsr |= FPACC_IOP;
    if (exc & FPEXC_OVFL)
        fpu->fpsr |= FPACC_OVFL | FPACC_INEX;
    if ((exc & FPEXC_UNFL) && (exc & FPEXC_INEX2))
        fpu->fpsr |= FPACC_UNFL;
    if (exc & FPEXC_DZ)
        fpu->fpsr |= FPACC_DZ;
    if (exc & (FPEXC_INEX2 | FPEXC_INEX1))
        fpu->fpsr |= FPACC_INEX;
}

// Check FPSR exception status against FPCR enables; take exception if match
bool fpu_check_exceptions(cpu_t *cpu, fpu_state_t *fpu) {
    // FPCR bits 15:8 enable; FPSR bits 15:8 status — same positions
    uint32_t enabled = fpu->fpsr & fpu->fpcr & 0xFF00u;
    if (!enabled)
        return false;

    // MC68882UM §6.1.2: UNFL post-instruction exception only fires if INEX2
    // is also set. If UNFL is enabled but result is exact, UNFL status stays
    // in FPSR but no post-instruction trap occurs (deferred to pre-instruction).
    if (enabled == FPEXC_UNFL && !(fpu->fpsr & FPEXC_INEX2))
        return false;

    // Select highest-priority exception vector
    uint32_t vector;
    if (enabled & FPEXC_BSUN)
        vector = FPVEC_BSUN;
    else if (enabled & FPEXC_SNAN)
        vector = FPVEC_SNAN;
    else if (enabled & FPEXC_OPERR)
        vector = FPVEC_OPERR;
    else if (enabled & FPEXC_OVFL)
        vector = FPVEC_OVFL;
    else if (enabled & FPEXC_UNFL)
        vector = FPVEC_UNFL;
    else if (enabled & FPEXC_DZ)
        vector = FPVEC_DZ;
    else
        vector = FPVEC_INEX;

    LOG(3, "fpu exception: vector=$%02X fpsr=$%08X fpcr=$%08X", vector, fpu->fpsr, fpu->fpcr);

    // Post-instruction exception: PC already points to next instruction
    exception(cpu, vector, cpu->pc, cpu_get_sr(cpu));
    return true;
}

// Pre-instruction exception check (MC68882UM §6.1.4):
// Fires before any FPU instruction (except FSAVE/FRESTORE) if FPSR & FPCR
// have matching enabled exception bits that haven't already been handled.
// On real 68882, the handler acknowledges the exception via FSAVE/BSET/FRESTORE;
// we track this with pre_exc_mask to prevent re-firing on the instruction retry.
// FPSR status bits are NOT cleared — they remain set for the retried instruction.
bool fpu_pre_instruction_check(cpu_t *cpu, fpu_state_t *fpu, bool conditional) {
    // Exclude exceptions already acknowledged (prevented from re-firing)
    uint32_t enabled = (fpu->fpsr & fpu->fpcr & 0xFF00u) & ~fpu->pre_exc_mask;
    if (!enabled)
        return false;

    // For non-conditional instructions (operational, system-control), UNFL
    // without INEX2 does not generate a pre-instruction exception — matching
    // the post-instruction UNFL+INEX2 rule. Conditional instructions (FBcc,
    // FScc, etc.) always trigger UNFL pre-instruction regardless of INEX2.
    if (!conditional && enabled == FPEXC_UNFL && !(fpu->fpsr & FPEXC_INEX2))
        return false;

    // Select highest-priority exception vector
    uint32_t vector;
    if (enabled & FPEXC_BSUN)
        vector = FPVEC_BSUN;
    else if (enabled & FPEXC_SNAN)
        vector = FPVEC_SNAN;
    else if (enabled & FPEXC_OPERR)
        vector = FPVEC_OPERR;
    else if (enabled & FPEXC_OVFL)
        vector = FPVEC_OVFL;
    else if (enabled & FPEXC_UNFL)
        vector = FPVEC_UNFL;
    else if (enabled & FPEXC_DZ)
        vector = FPVEC_DZ;
    else
        vector = FPVEC_INEX;

    LOG(3, "fpu pre-instruction exception: vector=$%02X fpsr=$%08X fpcr=$%08X", vector, fpu->fpsr, fpu->fpcr);

    // Mark these exception bits as acknowledged so the retried instruction
    // after the handler's RTE won't re-trigger the same exception.
    fpu->pre_exc_mask |= enabled;

    // Pre-instruction exception: PC points to the FPU instruction itself
    exception(cpu, vector, cpu->instruction_pc, cpu_get_sr(cpu));
    return true;
}

// Execute an FPU operation with source and destination register
static void fpu_execute_op(fpu_state_t *fpu, unsigned op, float80_reg_t src, unsigned dst) {
    fpu_unpacked_t a, b, result;

    // SNAN detection (MC68882UM §3.2.1): FPSR SNAN status bit is ALWAYS set
    // when a signaling NaN is encountered, regardless of FPCR enable bit.
    // If SNAN exception is enabled, abort the operation without storing result.
    // If not enabled, quiet the NaN and continue (except FMOVE to extended).
    // Dyadic operations also check the destination register.  Opcodes
    // (MC68882UM table 4-5): FDIV (0x20), FMOD (0x21), FADD (0x22),
    // FMUL (0x23), FSGLDIV (0x24), FREM (0x25), FSCALE (0x26),
    // FSGLMUL (0x27), FSUB (0x28), FCMP (0x38).
    // FPIAR has already been set by the caller: that is intended, an enabled
    // SNAN trap reports this instruction's address in FPIAR.
    {
        bool dyadic = (op >= 0x20 && op <= 0x28) || op == 0x38;
        bool src_snan = fp80_is_snan(src);
        bool dst_snan = dyadic && fp80_is_snan(fpu->fp[dst]);
        if (src_snan || dst_snan) {
            fpu->fpsr |= FPEXC_SNAN;
            if (fpu->fpcr & FPEXC_SNAN) {
                fpu_update_cc(fpu, src_snan ? src : fpu->fp[dst]);
                return;
            }
        }
    }

    switch (op) {
    case 0x00: // FMOVE
    {
        // NaN: detect SNaN, quieten, and pass through
        if (fp80_is_nan(src)) {
            if (fp80_is_snan(src)) {
                fpu->fpsr |= FPEXC_SNAN;
                src.mantissa |= 0x4000000000000000ULL;
            }
            fpu->fp[dst] = src;
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        // Round to FPCR-specified precision
        fpu_unpacked_t uv = fpu_unpack(src);
        fpu->fp[dst] = fpu_pack(fpu, uv);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    case 0x01: // FINT (round to integer per FPCR mode)
    case 0x03: // FINTRZ (round to integer toward zero)
    {
        fpu_unpacked_t uv = fpu_unpack(src);
        // NaN: detect SNaN, quieten, and pass through
        if (uv.exponent == FPU_EXP_INF && uv.mantissa_hi != 0) {
            if (!(uv.mantissa_hi & 0x4000000000000000ULL)) {
                // SNaN → QNaN: set quiet bit, raise SNAN + IOP
                src.mantissa |= 0x4000000000000000ULL;
                fpu->fpsr |= FPEXC_SNAN | FPACC_IOP;
            }
            fpu->fp[dst] = src;
            fpu_update_cc(fpu, src);
            return;
        }
        // Infinity and zero pass through unchanged
        if (uv.exponent == FPU_EXP_INF || uv.exponent == FPU_EXP_ZERO) {
            fpu->fp[dst] = src;
            fpu_update_cc(fpu, src);
            return;
        }
        // Normalize unnormal inputs (J=0 with non-zero exponent) so every
        // path below sees, and stores, a normalized value.  An unnormal with
        // an all-zero mantissa is a zero.
        if (!(uv.mantissa_hi & 0x8000000000000000ULL)) {
            fpu_normalize(&uv);
            if (uv.exponent == FPU_EXP_ZERO) {
                fpu->fp[dst] = uv.sign ? FP80_NEG_ZERO : FP80_ZERO;
                fpu_update_cc(fpu, fpu->fp[dst]);
                return;
            }
        }
        // Already an integer if exponent >= 63 (all mantissa bits are integer)
        if (uv.exponent >= 63) {
            // Store directly without precision rounding
            int32_t biased = uv.exponent + FPU_EXP_BIAS;
            fpu->fp[dst] = fp80_make(uv.sign, (uint16_t)biased, uv.mantissa_hi);
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        // If exponent < 0, |value| < 1 — result depends on rounding mode
        if (uv.exponent < 0) {
            unsigned rmode = (op == 0x03) ? 1 : ((fpu->fpcr >> 4) & 3);
            bool round_up = false;
            if (uv.mantissa_hi != 0)
                fpu->fpsr |= FPEXC_INEX2;
            switch (rmode) {
            case 0: // nearest: round up if |val| > 0.5 or exactly 0.5 with odd
                if (uv.exponent == -1 && uv.mantissa_hi > 0x8000000000000000ULL)
                    round_up = true;
                else if (uv.exponent == -1 && uv.mantissa_hi == 0x8000000000000000ULL && uv.mantissa_lo != 0)
                    round_up = true;
                // ties: round to even -> 0 (even) is correct here
                break;
            case 1:
                break; // toward zero: always 0
            case 2: // toward -inf: round up if negative
                if (uv.sign && uv.mantissa_hi != 0)
                    round_up = true;
                break;
            case 3: // toward +inf: round up if positive
                if (!uv.sign && uv.mantissa_hi != 0)
                    round_up = true;
                break;
            }
            if (round_up) {
                // Result is +-1.0 (directly, no FPCR precision rounding)
                fpu->fp[dst] = fp80_make(uv.sign, FPU_EXP_BIAS, 0x8000000000000000ULL);
            } else {
                fpu->fp[dst] = uv.sign ? FP80_NEG_ZERO : FP80_ZERO;
            }
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        // 0 <= exponent < 63: mask off fractional bits
        int frac_bits = 63 - uv.exponent; // bits to discard (1..63)
        uint64_t frac_mask = (1ULL << frac_bits) - 1;
        uint64_t frac = uv.mantissa_hi & frac_mask;
        bool has_lo = (uv.mantissa_lo != 0);
        if (frac != 0 || has_lo)
            fpu->fpsr |= FPEXC_INEX2;
        // Truncate fractional bits
        uv.mantissa_hi &= ~frac_mask;
        uv.mantissa_lo = 0;
        // Apply rounding
        unsigned rmode = (op == 0x03) ? 1 : ((fpu->fpcr >> 4) & 3);
        bool round_up = false;
        uint64_t half = 1ULL << (frac_bits - 1);
        switch (rmode) {
        case 0: // nearest (ties to even)
            if (frac > half || (frac == half && has_lo))
                round_up = true;
            else if (frac == half && !has_lo && (uv.mantissa_hi & (1ULL << frac_bits)))
                round_up = true; // tie: round to even
            break;
        case 1:
            break; // toward zero
        case 2: // toward -inf
            if (uv.sign && (frac || has_lo))
                round_up = true;
            break;
        case 3: // toward +inf
            if (!uv.sign && (frac || has_lo))
                round_up = true;
            break;
        }
        if (round_up) {
            uv.mantissa_hi += (1ULL << frac_bits);
            if (uv.mantissa_hi == 0) {
                // Carry: mantissa wrapped around
                uv.mantissa_hi = 0x8000000000000000ULL;
                uv.exponent++;
            }
        }
        // Store result directly — FINT/FINTRZ bypass FPCR precision rounding
        if (uv.mantissa_hi == 0) {
            fpu->fp[dst] = uv.sign ? FP80_NEG_ZERO : FP80_ZERO;
        } else {
            int32_t biased = uv.exponent + FPU_EXP_BIAS;
            fpu->fp[dst] = fp80_make(uv.sign, (uint16_t)biased, uv.mantissa_hi);
        }
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    case 0x04: // FSQRT
        a = fpu_unpack(src);
        result = fpu_op_sqrt(fpu, a);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x18: // FABS
    {
        // NaN: propagate with sign preserved per 68882
        if (fp80_is_nan(src)) {
            if (fp80_is_snan(src)) {
                src.mantissa |= 0x4000000000000000ULL;
                fpu->fpsr |= FPEXC_SNAN;
            }
            fpu->fp[dst] = src;
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        // Non-NaN: clear sign and round to FPCR-specified precision
        fpu_unpacked_t uv = fpu_unpack(src);
        uv.sign = false;
        fpu->fp[dst] = fpu_pack(fpu, uv);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    case 0x1A: // FNEG
    {
        // NaN: propagate with sign preserved per 68882
        if (fp80_is_nan(src)) {
            if (fp80_is_snan(src)) {
                src.mantissa |= 0x4000000000000000ULL;
                fpu->fpsr |= FPEXC_SNAN;
            }
            fpu->fp[dst] = src;
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        // Non-NaN: flip sign and round to FPCR-specified precision
        fpu_unpacked_t uv = fpu_unpack(src);
        uv.sign = !uv.sign;
        fpu->fp[dst] = fpu_pack(fpu, uv);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    case 0x1E: // FGETEXP: extract unbiased exponent as FP value
    {
        fpu_unpacked_t uv = fpu_unpack(src);
        if (uv.exponent == FPU_EXP_INF && uv.mantissa_hi != 0) {
            // NaN: propagate
            if (!(uv.mantissa_hi & 0x4000000000000000ULL))
                fpu->fpsr |= FPEXC_SNAN;
            uv.mantissa_hi |= 0x4000000000000000ULL;
            fpu->fp[dst] = fpu_pack(fpu, uv);
        } else if (uv.exponent == FPU_EXP_INF) {
            // Infinity: OPERR, return NaN
            fpu->fpsr |= FPEXC_OPERR;
            fpu->fp[dst] = FP80_QNAN;
        } else if (uv.exponent == FPU_EXP_ZERO) {
            // Zero: result is zero (preserve sign)
            fpu->fp[dst] = fp80_sign(src) ? FP80_NEG_ZERO : FP80_ZERO;
        } else {
            // Normal: the unbiased exponent as an FP integer.  |exponent| is
            // below 2^15, so the conversion is exact in every FPCR precision
            // and needs no rounding pass through fpu_pack.
            fpu->fp[dst] = fpu_from_int32(uv.exponent);
        }
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    case 0x1F: // FGETMAN: extract mantissa with exponent = 0
    {
        fpu_unpacked_t uv = fpu_unpack(src);
        if (uv.exponent == FPU_EXP_INF && uv.mantissa_hi != 0) {
            // NaN: propagate
            if (!(uv.mantissa_hi & 0x4000000000000000ULL))
                fpu->fpsr |= FPEXC_SNAN;
            uv.mantissa_hi |= 0x4000000000000000ULL;
            fpu->fp[dst] = fpu_pack(fpu, uv);
        } else if (uv.exponent == FPU_EXP_INF) {
            // Infinity: OPERR, return NaN
            fpu->fpsr |= FPEXC_OPERR;
            fpu->fp[dst] = FP80_QNAN;
        } else if (uv.exponent == FPU_EXP_ZERO) {
            // Zero: result is zero (preserve sign)
            fpu->fp[dst] = fp80_sign(src) ? FP80_NEG_ZERO : FP80_ZERO;
        } else {
            // Normal: set biased exponent to 3FFF, keep raw mantissa+sign
            // Preserves unnormalized mantissa bits (no normalization shift)
            fpu->fp[dst] = fp80_make(fp80_sign(src), 0x3FFF, src.mantissa);
        }
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    // Binary operations: FPd OP src -> FPd
    case 0x20: // FDIV: FPd / src
        a = fpu_unpack(fpu->fp[dst]);
        b = fpu_unpack(src);
        result = fpu_op_div(fpu, a, b);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x21: // FMOD: FPd mod src (IEEE modulo, quotient toward zero)
    case 0x25: // FREM: FPd rem src (IEEE remainder, quotient to nearest)
    {
        a = fpu_unpack(fpu->fp[dst]);
        b = fpu_unpack(src);
        // Handle special cases
        bool a_nan = (a.exponent == FPU_EXP_INF && a.mantissa_hi != 0);
        bool b_nan = (b.exponent == FPU_EXP_INF && b.mantissa_hi != 0);
        if (a_nan || b_nan) {
            result = fpu_propagate_nan(fpu, a, b);
            fpu->fp[dst] = fpu_pack(fpu, result);
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        bool a_inf = (a.exponent == FPU_EXP_INF);
        bool b_zero = (b.exponent == FPU_EXP_ZERO);
        if (a_inf || b_zero) {
            // Inf rem x or x rem 0: OPERR, return NaN
            fpu->fpsr |= FPEXC_OPERR;
            fpu->fp[dst] = FP80_QNAN;
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        bool a_zero = (a.exponent == FPU_EXP_ZERO);
        bool b_inf = (b.exponent == FPU_EXP_INF);
        if (a_zero || b_inf) {
            // 0 rem x = 0, x rem inf = x
            fpu->fp[dst] = fpu_pack(fpu, a);
            // Set quotient to 0 with correct sign (sign of dividend XOR divisor)
            unsigned q_sign_bit = (a.sign ^ b.sign) ? 0x80u : 0;
            fpu->fpsr = (fpu->fpsr & ~0x00FF0000u) | ((uint32_t)q_sign_bit << 16);
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        // FPSP iterative shift-and-subtract algorithm for FMOD/FREM
        // Step 1: strip signs
        bool sign_x = a.sign;
        bool sign_q = a.sign ^ b.sign;
        fpu_unpacked_t R = a;
        R.sign = false;
        fpu_unpacked_t Y = b;
        Y.sign = false;

        // Step 2: L = expo(X) - expo(Y), iterate to compute Q and R
        int32_t L = R.exponent - Y.exponent;
        uint32_t Q = 0;

        if (L >= 0) {
            // Work with 128-bit integer mantissas at the same exponent
            __uint128_t r_m = ((__uint128_t)R.mantissa_hi << 64) | R.mantissa_lo;
            __uint128_t y_m = ((__uint128_t)Y.mantissa_hi << 64) | Y.mantissa_lo;

            // Step 3: iterative MOD (shift-and-subtract)
            // Track a carry bit for when r_m <<= 1 overflows 128 bits,
            // following the same approach as Motorola's FPSP (srem_mod.S).
            bool carry = false;
            for (int32_t j = L; j >= 0; j--) {
                Q <<= 1;
                // carry means real R = r_m + 2^128 > y_m, so always subtract
                if (carry || r_m >= y_m) {
                    Q++;
                    r_m -= y_m; // unsigned wrap gives correct result when carry set
                    carry = false;
                    if (r_m == 0) {
                        R.exponent = FPU_EXP_ZERO;
                        R.mantissa_hi = 0;
                        R.mantissa_lo = 0;
                        // Account for remaining j iterations (each would Q <<= 1).
                        // Cap the shift at 32 since `j` can be much larger and
                        // `uint32_t <<= 32+` is UB. After 32 shifts Q is already
                        // zero, and the 7-bit mask further down preserves that.
                        Q = (j >= 32) ? 0u : (Q << j);
                        break;
                    }
                }
                if (j > 0) {
                    carry = (r_m >> 127) != 0; // MSB set means shift will overflow
                    r_m <<= 1; // R = 2R (may truncate, carry tracks overflow)
                }
            }

            // Convert back to unpacked and normalize
            if (R.exponent != FPU_EXP_ZERO) {
                R.mantissa_hi = (uint64_t)(r_m >> 64);
                R.mantissa_lo = (uint64_t)r_m;
                R.exponent = Y.exponent;
                if (R.mantissa_hi == 0 && R.mantissa_lo == 0) {
                    R.exponent = FPU_EXP_ZERO;
                } else {
                    // Normalize
                    if (R.mantissa_hi == 0) {
                        R.mantissa_hi = R.mantissa_lo;
                        R.mantissa_lo = 0;
                        R.exponent -= 64;
                    }
                    int lz = clz64(R.mantissa_hi);
                    if (lz > 0) {
                        R.mantissa_hi = (R.mantissa_hi << lz) | (R.mantissa_lo >> (64 - lz));
                        R.mantissa_lo <<= lz;
                        R.exponent -= lz;
                    }
                }
            }
        }
        // else: L < 0 means |X| < |Y|, R = X, Q = 0

        // Step 5: FREM adjustment (round quotient to nearest)
        bool last_subtract = false;
        if (op == 0x25 && R.exponent != FPU_EXP_ZERO) {
            // Compare R with Y/2. Y/2 = Y with exponent decremented by 1.
            int cmp;
            int32_t yhalf_exp = Y.exponent - 1;
            if (R.exponent > yhalf_exp)
                cmp = 1;
            else if (R.exponent < yhalf_exp)
                cmp = -1;
            else {
                // Same exponent: compare mantissas directly
                if (R.mantissa_hi > Y.mantissa_hi)
                    cmp = 1;
                else if (R.mantissa_hi < Y.mantissa_hi)
                    cmp = -1;
                else if (R.mantissa_lo > Y.mantissa_lo)
                    cmp = 1;
                else if (R.mantissa_lo < Y.mantissa_lo)
                    cmp = -1;
                else
                    cmp = 0;
            }
            if (cmp > 0) {
                // R > Y/2: adjust
                last_subtract = true;
                Q++;
            } else if (cmp == 0 && (Q & 1)) {
                // R == Y/2 and Q is odd: round to even
                last_subtract = true;
                Q++;
            }
        }

        // Step 7: subtract |Y| if last_subtract (before sign application)
        // Following FPSP: first adjust the unsigned remainder, then apply sign.
        if (last_subtract) {
            // Y.sign stays false from step 1 — subtract unsigned |Y|
            R = fpu_op_sub(fpu, R, Y);
            fpu->fpsr &= ~0xFF00u;
        }

        // Step 6: apply sign of dividend (FPSP fnegx if sign_x negative)
        if (sign_x)
            R.sign = !R.sign;

        // Special: if result is zero, sign = sign of dividend
        if (R.exponent == FPU_EXP_ZERO)
            R.sign = sign_x;

        // Step 8: set quotient byte
        unsigned q_abs = Q & 0x7F;
        unsigned q_sign_val = sign_q ? 0x80u : 0;
        fpu->fpsr = (fpu->fpsr & ~0x00FF0000u) | ((uint32_t)(q_sign_val | q_abs) << 16);
        fpu->fp[dst] = fpu_pack(fpu, R);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    case 0x22: // FADD: FPd + src
        a = fpu_unpack(fpu->fp[dst]);
        b = fpu_unpack(src);
        result = fpu_op_add(fpu, a, b);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x23: // FMUL: FPd * src
        a = fpu_unpack(fpu->fp[dst]);
        b = fpu_unpack(src);
        result = fpu_op_mul(fpu, a, b);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x24: // FSGLDIV: FPd / src, result rounded to single precision
    {
        a = fpu_unpack(fpu->fp[dst]);
        b = fpu_unpack(src);
        // Truncate both operands to single precision (24-bit mantissa)
        a.mantissa_hi &= 0xFFFFFF0000000000ULL;
        a.mantissa_lo = 0;
        b.mantissa_hi &= 0xFFFFFF0000000000ULL;
        b.mantissa_lo = 0;
        result = fpu_op_div(fpu, a, b);
        // Round result mantissa to single precision (24 bits)
        fpu_round_mantissa(fpu, &result, 24);
        // FSGLDIV uses extended exponent range regardless of FPCR precision
        uint32_t saved_fpcr = fpu->fpcr;
        fpu->fpcr &= ~0x00C0u;
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu->fpcr = saved_fpcr;
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    case 0x26: // FSCALE: FPd * 2^(integer part of src)
    {
        a = fpu_unpack(fpu->fp[dst]);
        b = fpu_unpack(src);
        // Handle specials
        if (a.exponent == FPU_EXP_INF && a.mantissa_hi != 0) {
            // dst is NaN
            if (b.exponent == FPU_EXP_INF && b.mantissa_hi != 0) {
                result = fpu_propagate_nan(fpu, a, b);
            } else {
                if (!(a.mantissa_hi & 0x4000000000000000ULL))
                    fpu->fpsr |= FPEXC_SNAN;
                a.mantissa_hi |= 0x4000000000000000ULL;
                result = a;
            }
            fpu->fp[dst] = fpu_pack(fpu, result);
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        if (b.exponent == FPU_EXP_INF && b.mantissa_hi != 0) {
            // src is NaN (dst is not NaN)
            if (!(b.mantissa_hi & 0x4000000000000000ULL))
                fpu->fpsr |= FPEXC_SNAN;
            b.mantissa_hi |= 0x4000000000000000ULL;
            fpu->fp[dst] = fpu_pack(fpu, b);
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        if (a.exponent == FPU_EXP_ZERO) {
            // 0 * 2^n = 0
            fpu->fp[dst] = fpu_pack(fpu, a);
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        if (a.exponent == FPU_EXP_INF) {
            // inf * 2^n = inf (but inf * 2^inf_neg = OPERR)
            if (b.exponent == FPU_EXP_INF && b.sign) {
                fpu->fpsr |= FPEXC_OPERR;
                fpu->fp[dst] = FP80_QNAN;
            } else {
                fpu->fp[dst] = fpu_pack(fpu, a);
            }
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        if (b.exponent == FPU_EXP_INF) {
            // finite * 2^(+/-inf)
            if (b.sign) {
                fpu->fp[dst] = a.sign ? FP80_NEG_ZERO : FP80_ZERO;
            } else {
                fpu_unpacked_t inf = {a.sign, FPU_EXP_INF, 0, 0};
                fpu->fp[dst] = fpu_pack(fpu, inf);
            }
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        if (b.exponent == FPU_EXP_ZERO) {
            // scale by 0: no change
            fpu->fp[dst] = fpu_pack(fpu, a);
            fpu_update_cc(fpu, fpu->fp[dst]);
            return;
        }
        // Extract integer from source (truncate toward zero)
        int32_t scale;
        if (b.exponent >= 31) {
            scale = b.sign ? INT32_MIN : INT32_MAX;
        } else if (b.exponent < 0) {
            scale = 0;
        } else {
            int shift = 63 - b.exponent;
            // Build the magnitude as uint32_t so the sign flip can't trip
            // signed-overflow UB (`-INT32_MIN` is UB; `-(uint32_t)magnitude`
            // produces the two's-complement bit pattern).
            uint32_t magnitude = (uint32_t)(b.mantissa_hi >> shift);
            scale = b.sign ? (int32_t)(-magnitude) : (int32_t)magnitude;
        }
        // Add scale to exponent in 64 bits, then clamp the sum: anything
        // beyond +-20000 already over/underflows in fpu_pack (even for an
        // unnormal mantissa), and clamping keeps clear of int32 overflow and
        // of the FPU_EXP_ZERO/FPU_EXP_INF sentinel values.
        int64_t new_exp = (int64_t)a.exponent + scale;
        if (new_exp > 20000)
            new_exp = 20000;
        else if (new_exp < -20000)
            new_exp = -20000;
        a.exponent = (int32_t)new_exp;
        fpu->fp[dst] = fpu_pack(fpu, a);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    case 0x27: // FSGLMUL: FPd * src, result rounded to single precision
    {
        a = fpu_unpack(fpu->fp[dst]);
        b = fpu_unpack(src);
        // Truncate both operands to single precision (24-bit mantissa)
        a.mantissa_hi &= 0xFFFFFF0000000000ULL;
        a.mantissa_lo = 0;
        b.mantissa_hi &= 0xFFFFFF0000000000ULL;
        b.mantissa_lo = 0;
        result = fpu_op_mul(fpu, a, b);
        // Round result mantissa to single precision (24 bits)
        fpu_round_mantissa(fpu, &result, 24);
        // FSGLMUL uses extended exponent range regardless of FPCR precision
        uint32_t saved_fpcr = fpu->fpcr;
        fpu->fpcr &= ~0x00C0u;
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu->fpcr = saved_fpcr;
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    case 0x28: // FSUB: FPd - src
        a = fpu_unpack(fpu->fp[dst]);
        b = fpu_unpack(src);
        result = fpu_op_sub(fpu, a, b);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    // Comparison: FCMP sets condition codes only, no write-back
    // Does not set any exception status bits per 68882; rounding mode not applied
    case 0x38: {
        a = fpu_unpack(fpu->fp[dst]);
        b = fpu_unpack(src);
        // NaN: 68882 FCMP always sets just NAN=1 (N=Z=I=0) when unordered
        bool a_nan = (a.exponent == FPU_EXP_INF && a.mantissa_hi != 0);
        bool b_nan = (b.exponent == FPU_EXP_INF && b.mantissa_hi != 0);
        if (a_nan || b_nan) {
            uint32_t saved_exc = fpu->fpsr & 0xFF00u;
            fpu_propagate_nan(fpu, a, b);
            // Preserve only SNAN exception, restore the rest
            uint32_t snan_bit = fpu->fpsr & FPEXC_SNAN;
            fpu->fpsr = (fpu->fpsr & ~0xFF00u) | saved_exc | snan_bit;
            // Unordered: NAN=1, clear N/Z/I
            fpu->fpsr = (fpu->fpsr & ~(FPCC_N | FPCC_Z | FPCC_I | FPCC_NAN)) | FPCC_NAN;
            return;
        }
        // Save FPSR exception status, override rounding to RN extended
        uint32_t saved_exc = fpu->fpsr & 0xFF00u;
        uint32_t saved_fpcr = fpu->fpcr;
        fpu->fpcr = (fpu->fpcr & ~0x00F0u); // RN, extended precision
        fpu_unpacked_t diff = fpu_op_sub(fpu, a, b);
        float80_reg_t diff80 = fpu_pack(fpu, diff);
        fpu->fpcr = saved_fpcr;
        // Restore exception status (FCMP must not modify exception bits)
        fpu->fpsr = (fpu->fpsr & ~0xFF00u) | saved_exc;
        fpu_update_cc(fpu, diff80);
        return;
    }

    // Test: FTST sets condition codes from source, no write-back
    case 0x3A:
        // SNaN: signal but do not quieten (no writeback)
        if (fp80_is_snan(src))
            fpu->fpsr |= FPEXC_SNAN;
        fpu_update_cc(fpu, src);
        return;

        // === Transcendental functions (fpu_transc.c) ===

    case 0x14: // FLOGN: natural logarithm
        a = fpu_unpack(src);
        result = fpu_op_logn(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x15: // FLOG10: base-10 logarithm
        a = fpu_unpack(src);
        result = fpu_op_log10(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x16: // FLOG2: base-2 logarithm
        a = fpu_unpack(src);
        result = fpu_op_log2(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x06: // FLOGNP1: log(1+X)
        a = fpu_unpack(src);
        result = fpu_op_lognp1(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x10: // FETOX: e^X
        a = fpu_unpack(src);
        result = fpu_op_etox(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x08: // FETOXM1: e^X - 1
        a = fpu_unpack(src);
        result = fpu_op_etoxm1(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x11: // FTWOTOX: 2^X
        a = fpu_unpack(src);
        result = fpu_op_twotox(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        // 68882 suppresses INEX2 when the result exceeds the *extended*
        // range.  The test deliberately reads the pre-pack `result`: an
        // overflow that only fpu_pack's single/double precision control
        // produced (exponent still <= FPU_EXP_BIAS) keeps its INEX2.
        if ((fpu->fpsr & FPEXC_OVFL) && (result.exponent == FPU_EXP_INF || result.exponent > FPU_EXP_BIAS))
            fpu->fpsr &= ~FPEXC_INEX2;
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x12: // FTENTOX: 10^X
        a = fpu_unpack(src);
        result = fpu_op_tentox(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        // Same extended-range INEX2 suppression as FTWOTOX (pre-pack test)
        if ((fpu->fpsr & FPEXC_OVFL) && (result.exponent == FPU_EXP_INF || result.exponent > FPU_EXP_BIAS))
            fpu->fpsr &= ~FPEXC_INEX2;
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x0A: // FATAN: arctangent
        a = fpu_unpack(src);
        result = fpu_op_atan(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x0E: // FSIN: sine
        a = fpu_unpack(src);
        result = fpu_op_sin(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x0F: // FTAN: tangent
        a = fpu_unpack(src);
        result = fpu_op_tan(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x1D: // FCOS: cosine
        a = fpu_unpack(src);
        result = fpu_op_cos(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33: // FSINCOS
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37: {
        int cos_reg = op & 7;
        a = fpu_unpack(src);
        result = fpu_op_sincos(fpu, a, src, cos_reg);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;
    }

    case 0x02: // FSINH: hyperbolic sine
        a = fpu_unpack(src);
        result = fpu_op_sinh(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x19: // FCOSH: hyperbolic cosine
        a = fpu_unpack(src);
        result = fpu_op_cosh(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x09: // FTANH: hyperbolic tangent
        a = fpu_unpack(src);
        result = fpu_op_tanh(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x0C: // FASIN: arcsine
        a = fpu_unpack(src);
        result = fpu_op_asin(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x1C: // FACOS: arccosine
        a = fpu_unpack(src);
        result = fpu_op_acos(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    case 0x0D: // FATANH: inverse hyperbolic tangent
        a = fpu_unpack(src);
        result = fpu_op_atanh(fpu, a, src);
        fpu->fp[dst] = fpu_pack(fpu, result);
        fpu_update_cc(fpu, fpu->fp[dst]);
        return;

    default:
        // Unimplemented operation
        LOG(1, "fpu: unimplemented op $%02X", op);
        fpu->fpsr |= FPEXC_OPERR;
        return;
    }
}

// ============================================================================
// General FPU operation dispatcher (type=0)
// ============================================================================

void fpu_general_op(cpu_t *cpu, fpu_state_t *fpu, uint16_t opcode, uint16_t ext_word) {
    LOG(4, "fpu op PC=%08X opcode=%04X ext=%04X", cpu->instruction_pc, opcode, ext_word);

    // Mark FPU as initialized (for FSAVE idle vs null frame)
    fpu->initialized = true;

    // Decode the top 3 bits of the extension word
    unsigned top3 = (ext_word >> 13) & 7;

    // Pre-instruction exception check (MC68882UM §6.1.4):
    // Fires before any FPU instruction except FSAVE/FRESTORE (handled in
    // cpu_ops.h). Uses pre_exc_mask to avoid re-firing after handler acks.
    if (fpu_pre_instruction_check(cpu, fpu, false))
        return;

    unsigned src_spec = (ext_word >> 10) & 7;
    unsigned dst_reg = (ext_word >> 7) & 7;
    unsigned op = ext_word & 0x7F;

    // Update FPIAR for exception-generating operations (top3 < 4)
    // FMOVEM (top3 4-7) and FSAVE/FRESTORE do not update FPIAR
    if (top3 < 4)
        fpu->fpiar = cpu->instruction_pc;

    // Clear per-operation exception status bits (MC68882UM §3.3.1):
    // exception status byte is cleared before each arithmetic/data instruction.
    // System control instructions (FMOVEM control regs, top3=4/5) and
    // FMOVEM data registers (top3=6/7) do NOT clear the exception status.
    if (top3 < 4)
        fpu_clear_status(fpu);

    switch (top3) {
    case 0: // 000: FPn->FPn operation (register to register)
    {
        float80_reg_t src_val = fpu->fp[src_spec];
        fpu_execute_op(fpu, op, src_val, dst_reg);
        break;
    }

    case 1: // 001: FMOVECR (bit 13 set, rm=0 dir=0)
    {
        fpu_unpacked_t val = fpu_rom_constant(op);
        fpu->fp[dst_reg] = fpu_pack(fpu, val);
        fpu_update_cc(fpu, fpu->fp[dst_reg]);
        // Transcendental and large-power constants are inexact in extended
        if (op == 0x00 || (op >= 0x0B && op <= 0x0E) || op == 0x30 || op == 0x31 || op >= 0x38)
            fpu->fpsr |= FPEXC_INEX2;
        break;
    }

    case 2: // 010: EA->FPn operation (memory source) OR FMOVECR
    {
        // Bug 2 fix: detect FMOVECR when src_spec=7 and dir bit (14)=1
        // FMOVECR encoding: ext_word bits [15:10] = 010111
        unsigned dir = (ext_word >> 14) & 1;
        if (dir && src_spec == 7) {
            // FMOVECR: load ROM constant, apply FPCR rounding
            fpu_unpacked_t val = fpu_rom_constant(op);
            fpu->fp[dst_reg] = fpu_pack(fpu, val);
            fpu_update_cc(fpu, fpu->fp[dst_reg]);
            // Transcendental and large-power constants are inexact in extended
            if (op == 0x00 || (op >= 0x0B && op <= 0x0E) || op == 0x30 || op == 0x31 || op >= 0x38)
                fpu->fpsr |= FPEXC_INEX2;
        } else {
            // An direct (mode 1) is never valid for FPU data operations
            unsigned ea_mode = (opcode >> 3) & 7;
            if (ea_mode == 1) {
                cpu->pc = cpu->instruction_pc + 2;
                f_trap(cpu, opcode);
                return;
            }
            // Multi-word formats (extended/packed/double) with Dn:
            // the 68882 protocol can't transfer >4 bytes from Dn, so trap
            if ((src_spec == 2 || src_spec == 3 || src_spec == 5 || src_spec == 7) && ea_mode == 0) {
                cpu->pc = cpu->instruction_pc + 2;
                f_trap(cpu, opcode);
                return;
            }
            float80_reg_t src_val = fpu_load_ea(cpu, opcode, src_spec);
            fpu_execute_op(fpu, op, src_val, dst_reg);
        }
        break;
    }

    case 3: // 011: FMOVE FPn->EA (register to memory)
    {
        unsigned format = src_spec;
        unsigned src_fpn = dst_reg;
        // An direct (mode 1) is never valid for FPU store operations
        unsigned ea_mode_w = (opcode >> 3) & 7;
        if (ea_mode_w == 1) {
            cpu->pc = cpu->instruction_pc + 2;
            f_trap(cpu, opcode);
            return;
        }
        // Multi-word formats with Dn: can't transfer >4 bytes, so trap
        if ((format == 2 || format == 3 || format == 5 || format == 7) && ea_mode_w == 0) {
            cpu->pc = cpu->instruction_pc + 2;
            f_trap(cpu, opcode);
            return;
        }
        if (format == 3 || format == 7) {
            // Packed decimal: extract k-factor and write 12-byte BCD
            int k_factor;
            if (format == 3) {
                // Static k-factor from ext_word bits 6:0 (signed 7-bit)
                int raw = ext_word & 0x7F;
                k_factor = (raw & 0x40) ? (raw | ~0x7F) : raw;
            } else {
                // Dynamic k-factor from Dn (register in ext_word bits 6:4)
                unsigned dn = (ext_word >> 4) & 7;
                int raw = cpu->d[dn] & 0x7F;
                k_factor = (raw & 0x40) ? (raw | ~0x7F) : raw;
            }
            unsigned ea_mode = (opcode >> 3) & 7;
            unsigned ea_reg = opcode & 7;
            uint32_t w0, w1, w2;
            fpu_to_packed(fpu, fpu->fp[src_fpn], k_factor, &w0, &w1, &w2);
            uint32_t ea = calculate_ea(cpu, 12, ea_mode, ea_reg, true);
            memory_write_uint32(ea, w0);
            memory_write_uint32(ea + 4, w1);
            memory_write_uint32(ea + 8, w2);
        } else {
            fpu_store_ea(cpu, fpu, opcode, fpu->fp[src_fpn], format);
        }
        break;
    }

    case 4: // 100: FMOVEM control registers (EA -> CR)
    case 5: // 101: FMOVEM control registers (CR -> EA)
        if (!fpu_movem_control(cpu, fpu, opcode, ext_word)) {
            // Invalid encoding (e.g. multi-register with Dn) → F-line exception
            exception(cpu, 0x2C, cpu->instruction_pc, cpu_get_sr(cpu));
            return;
        }
        // FMOVEM does not update FPIAR or accrued exception bits
        return;

    case 6: // 110: FMOVEM data registers
    case 7: // 111: FMOVEM data registers
        if (!fpu_movem_data(cpu, fpu, opcode, ext_word)) {
            // Invalid EA mode for FMOVEM data → F-line exception
            exception(cpu, 0x2C, cpu->instruction_pc, cpu_get_sr(cpu));
            return;
        }
        // FMOVEM does not update FPIAR or accrued exception bits
        return;
    }

    // Propagate exception status to accrued exception bits
    fpu_update_accrued(fpu);

    // Post-instruction exception check: only FMOVE to memory (top3=3) fires
    // post-instruction exceptions. Arithmetic/register operations (top3=0-2)
    // defer exceptions to pre-instruction check of the NEXT FPU instruction.
    if (top3 == 3)
        fpu_check_exceptions(cpu, fpu);
}
