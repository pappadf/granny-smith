// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fpu.c
// Motorola 68882 FPU emulation — arithmetic via host double, FMOVE, FMOVEM,
// FSAVE/FRESTORE null frames, FBcc condition evaluation.
// Approximative: all arithmetic is performed by casting to/from C double.

#include "fpu.h"
#include "cpu_internal.h"
#include "memory.h"

#include "log.h"
LOG_USE_CATEGORY_NAME("fpu");

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Lifecycle
// ============================================================================

// Allocate and zero-init FPU state
fpu_state_t *fpu_init(void) {
    fpu_state_t *fpu = (fpu_state_t *)malloc(sizeof(fpu_state_t));
    if (!fpu)
        return NULL;
    memset(fpu, 0, sizeof(fpu_state_t));
    return fpu;
}

// Free FPU state
void fpu_free(fpu_state_t *fpu) {
    free(fpu);
}

// ============================================================================
// Condition code helpers
// ============================================================================

// Update FPSR condition codes from a double result
static void fpu_update_cc(fpu_state_t *fpu, double val) {
    uint32_t cc = 0;
    if (isnan(val))
        cc |= FPCC_NAN;
    else if (isinf(val))
        cc |= FPCC_I | (val < 0 ? FPCC_N : 0);
    else if (val == 0.0)
        cc |= FPCC_Z;
    else if (val < 0.0)
        cc |= FPCC_N;
    fpu->fpsr = (fpu->fpsr & ~(FPCC_N | FPCC_Z | FPCC_I | FPCC_NAN)) | cc;
}

// Evaluate FPU condition predicate from FPSR condition codes
bool fpu_test_condition(fpu_state_t *fpu, unsigned predicate) {
    uint32_t cc = fpu->fpsr;
    bool n = (cc & FPCC_N) != 0;
    bool z = (cc & FPCC_Z) != 0;
    bool nan_bit = (cc & FPCC_NAN) != 0;

    // IEEE-aware predicates (0x10-0x1F) set BSUN if NaN
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
        return !nan_bit; // OR/GLE
    case 0x08:
        return nan_bit; // UN/NGLE
    case 0x09:
        return nan_bit || z; // UEQ/NGL
    case 0x0A:
        return nan_bit || !(n || z); // UGT/NLE
    case 0x0B:
        return nan_bit || z || !n; // UGE/NLT
    case 0x0C:
        return nan_bit || (n && !z); // ULT/NGE
    case 0x0D:
        return nan_bit || z || n; // ULE/NGT
    case 0x0E:
        return !z; // NE/SNEQ
    case 0x0F:
        return true; // T/ST
    }
    return false;
}

// ============================================================================
// Data format conversions: memory ↔ double
// ============================================================================

// Convert IEEE 754 single (32-bit) to double
static double fpu_from_single(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return (double)f;
}

// Convert double to IEEE 754 single (32-bit)
static uint32_t fpu_to_single(double val) {
    float f = (float)val;
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// Convert IEEE 754 double (64-bit) to double
static double fpu_from_double(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

// Convert double to IEEE 754 double (64-bit)
static uint64_t fpu_to_double(double val) {
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    return bits;
}

// Convert 68882 extended (96-bit in memory: 16-bit exp+sign, 16-bit pad,
// 64-bit mantissa) to double
static double fpu_from_extended(uint32_t exp_sign_word, uint32_t mant_hi, uint32_t mant_lo) {
    // Extract sign and biased exponent from first 16 bits
    int sign = (exp_sign_word >> 15) & 1;
    int biased_exp = exp_sign_word & 0x7FFF;
    uint64_t mantissa = ((uint64_t)mant_hi << 32) | mant_lo;

    if (biased_exp == 0) {
        // Zero or denormal
        if (mantissa == 0)
            return sign ? -0.0 : 0.0;
        // Denormal: exponent is 1-16383 = -16382, explicit integer bit expected
        double result = ldexp((double)mantissa, -16382 - 63);
        return sign ? -result : result;
    }
    if (biased_exp == 0x7FFF) {
        // Infinity or NaN
        if (mantissa == 0)
            return sign ? -INFINITY : INFINITY;
        return NAN;
    }
    // Normal: unbiased exponent, explicit integer bit (bit 63 of mantissa)
    int exponent = biased_exp - 16383;
    double result = ldexp((double)mantissa, exponent - 63);
    return sign ? -result : result;
}

// Convert double to 68882 extended (96-bit): writes 3 x 32-bit words
static void fpu_to_extended(double val, uint32_t *word0, uint32_t *word1, uint32_t *word2) {
    uint16_t exp_sign = 0;

    if (isnan(val)) {
        // NaN
        exp_sign = 0x7FFF;
        *word0 = (uint32_t)exp_sign << 16; // sign=0 + exp=7FFF + 16-bit pad
        *word1 = 0xC0000000u; // quiet NaN: J-bit + QNaN bit
        *word2 = 0;
        return;
    }
    if (isinf(val)) {
        exp_sign = (val < 0 ? 0xFFFF : 0x7FFF);
        *word0 = (uint32_t)exp_sign << 16;
        *word1 = 0;
        *word2 = 0;
        return;
    }
    if (val == 0.0) {
        // Detect negative zero
        uint64_t bits;
        memcpy(&bits, &val, sizeof(bits));
        exp_sign = (bits >> 63) ? 0x8000 : 0;
        *word0 = (uint32_t)exp_sign << 16;
        *word1 = 0;
        *word2 = 0;
        return;
    }

    int sign = (val < 0) ? 1 : 0;
    double abs_val = fabs(val);

    // Extract exponent and mantissa using frexp
    int exp_val;
    double frac = frexp(abs_val, &exp_val);
    // frexp returns 0.5 <= frac < 1.0, exponent such that val = frac * 2^exp
    // Extended format: 1.mantissa * 2^(exp-1), biased exponent = exp-1 + 16383

    // Scale fraction to 64-bit integer mantissa with explicit integer bit
    // frac is in [0.5, 1.0), multiply by 2^64 to get integer mantissa
    uint64_t mantissa = (uint64_t)ldexp(frac, 64);

    int biased_exp = (exp_val - 1) + 16383;
    if (biased_exp <= 0) {
        // Underflow to denormal or zero — just store zero for simplicity
        biased_exp = 0;
        mantissa = 0;
    } else if (biased_exp >= 0x7FFF) {
        // Overflow to infinity
        exp_sign = (uint16_t)(sign ? 0xFFFF : 0x7FFF);
        *word0 = (uint32_t)exp_sign << 16;
        *word1 = 0;
        *word2 = 0;
        return;
    }

    exp_sign = (uint16_t)((sign << 15) | biased_exp);
    *word0 = (uint32_t)exp_sign << 16; // exp+sign in upper 16 bits, pad in lower 16
    *word1 = (uint32_t)(mantissa >> 32);
    *word2 = (uint32_t)(mantissa & 0xFFFFFFFF);
}

// Convert int32 to double
static double fpu_from_int32(int32_t v) {
    return (double)v;
}

// Convert int16 to double
static double fpu_from_int16(int16_t v) {
    return (double)v;
}

// Convert int8 to double
static double fpu_from_int8(int8_t v) {
    return (double)v;
}

// Convert double to int32, clamping on overflow
static int32_t fpu_to_int32(double val) {
    if (isnan(val))
        return 0;
    val = trunc(val);
    if (val > (double)INT32_MAX)
        return INT32_MAX;
    if (val < (double)INT32_MIN)
        return INT32_MIN;
    return (int32_t)val;
}

// Convert double to int16, clamping on overflow
static int16_t fpu_to_int16(double val) {
    if (isnan(val))
        return 0;
    val = trunc(val);
    if (val > (double)INT16_MAX)
        return INT16_MAX;
    if (val < (double)INT16_MIN)
        return INT16_MIN;
    return (int16_t)val;
}

// Convert double to int8, clamping on overflow
static int8_t fpu_to_int8(double val) {
    if (isnan(val))
        return 0;
    val = trunc(val);
    if (val > (double)INT8_MAX)
        return INT8_MAX;
    if (val < (double)INT8_MIN)
        return INT8_MIN;
    return (int8_t)val;
}

// ============================================================================
// FMOVECR — load FPU ROM constant
// ============================================================================

// ROM constant table (subset of 68882 constants)
static double fpu_rom_constant(unsigned offset) {
    switch (offset) {
    case 0x00:
        return 3.14159265358979323846; // pi
    case 0x0B:
        return 0.30102999566398119521; // log10(2)
    case 0x0C:
        return 2.71828182845904523536; // e
    case 0x0D:
        return 1.44269504088896340736; // log2(e)
    case 0x0E:
        return 0.43429448190325182765; // log10(e)
    case 0x0F:
        return 0.0; // zero
    case 0x30:
        return 0.69314718055994530942; // ln(2)
    case 0x31:
        return 2.30258509299404568402; // ln(10)
    case 0x32:
        return 1.0; // 10^0
    case 0x33:
        return 10.0; // 10^1
    case 0x34:
        return 100.0; // 10^2
    case 0x35:
        return 1.0e4; // 10^4
    case 0x36:
        return 1.0e8; // 10^8
    case 0x37:
        return 1.0e16; // 10^16
    case 0x38:
        return 1.0e32; // 10^32
    case 0x39:
        return 1.0e64; // 10^64
    case 0x3A:
        return 1.0e128; // 10^128
    case 0x3B:
        return 1.0e256; // 10^256
    default:
        return 0.0; // undefined → zero
    }
}

// ============================================================================
// Source operand loading from EA
// ============================================================================

// Load a value from the effective address in the given format.
// EA computation uses the opcode lower 6 bits (mode/reg).
// This function reads from memory at the address computed by the CPU.
static double fpu_load_ea(cpu_t *cpu, uint16_t opcode, unsigned format) {
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
        uint32_t w0 = memory_read_uint32(ea); // exp+sign + padding
        uint32_t w1 = memory_read_uint32(ea + 4); // mantissa high
        uint32_t w2 = memory_read_uint32(ea + 8); // mantissa low
        return fpu_from_extended(w0 >> 16, w1, w2);
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
    default: // Packed decimal (3, 7) — not yet supported
        return 0.0;
    }
}

// ============================================================================
// Store result to EA (FMOVE FPn → memory)
// ============================================================================

// Store an FP register value to the EA in the given format
static void fpu_store_ea(cpu_t *cpu, uint16_t opcode, double val, unsigned format) {
    unsigned ea_mode = (opcode >> 3) & 7;
    unsigned ea_reg = opcode & 7;

    switch (format) {
    case 0: { // Long integer
        int32_t v = fpu_to_int32(val);
        write_ea_32(cpu, ea_mode, ea_reg, (uint32_t)v);
        break;
    }
    case 1: { // Single
        uint32_t bits = fpu_to_single(val);
        write_ea_32(cpu, ea_mode, ea_reg, bits);
        break;
    }
    case 2: { // Extended (12 bytes)
        uint32_t w0, w1, w2;
        fpu_to_extended(val, &w0, &w1, &w2);
        uint32_t ea = calculate_ea(cpu, 12, ea_mode, ea_reg, true);
        memory_write_uint32(ea, w0);
        memory_write_uint32(ea + 4, w1);
        memory_write_uint32(ea + 8, w2);
        break;
    }
    case 4: { // Word integer
        int16_t v = fpu_to_int16(val);
        write_ea_16(cpu, ea_mode, ea_reg, (uint16_t)v);
        break;
    }
    case 5: { // Double (8 bytes)
        uint64_t bits = fpu_to_double(val);
        uint32_t ea = calculate_ea(cpu, 8, ea_mode, ea_reg, true);
        memory_write_uint32(ea, (uint32_t)(bits >> 32));
        memory_write_uint32(ea + 4, (uint32_t)(bits & 0xFFFFFFFF));
        break;
    }
    case 6: { // Byte integer
        int8_t v = fpu_to_int8(val);
        write_ea_8(cpu, ea_mode, ea_reg, (uint8_t)v);
        break;
    }
    default: // Packed decimal — not yet supported
        break;
    }
}

// ============================================================================
// FMOVEM — save/restore FP data registers
// ============================================================================

// Size of each FP register in memory (extended = 12 bytes)
#define FP_REG_SIZE 12

// FMOVEM data register list save/restore
static void fpu_movem_data(cpu_t *cpu, fpu_state_t *fpu, uint16_t opcode, uint16_t ext) {
    unsigned ea_mode = (opcode >> 3) & 7;
    unsigned ea_reg = opcode & 7;
    unsigned dir = (ext >> 13) & 1; // 0 = predecrement (reg→mem), 1 = postincrement (mem→reg)
    unsigned mode_d = (ext >> 11) & 1; // 0 = static list, 1 = dynamic (Dn)
    uint8_t reglist;

    // Get register list: static from ext word, or dynamic from Dn
    if (mode_d)
        reglist = (uint8_t)cpu->d[(ext >> 4) & 7];
    else
        reglist = (uint8_t)(ext & 0xFF);

    if ((ext >> 12) & 1) {
        // Direction: memory → FPn (restore)
        if (ea_mode == 3) {
            // (An)+ postincrement
            uint32_t addr = cpu->a[ea_reg];
            for (int i = 0; i < 8; i++) {
                if (reglist & (1 << (7 - i))) {
                    uint32_t w0 = memory_read_uint32(addr);
                    uint32_t w1 = memory_read_uint32(addr + 4);
                    uint32_t w2 = memory_read_uint32(addr + 8);
                    fpu->fp[i] = fpu_from_extended(w0 >> 16, w1, w2);
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
                    fpu->fp[i] = fpu_from_extended(w0 >> 16, w1, w2);
                    addr += FP_REG_SIZE;
                }
            }
        }
    } else {
        // Direction: FPn → memory (save)
        if (ea_mode == 4) {
            // -(An) predecrement: registers stored in reverse order
            uint32_t addr = cpu->a[ea_reg];
            for (int i = 7; i >= 0; i--) {
                if (reglist & (1 << (7 - i))) {
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
}

// FMOVEM control register save/restore
static void fpu_movem_control(cpu_t *cpu, fpu_state_t *fpu, uint16_t opcode, uint16_t ext) {
    unsigned ea_mode = (opcode >> 3) & 7;
    unsigned ea_reg = opcode & 7;
    unsigned dir = (ext >> 13) & 1; // bit 13: 0=EA→CR, 1=CR→EA (actually reversed)
    unsigned regsel = (ext >> 10) & 7; // bits 12:10: which control regs

    // Count how many control registers are in the list
    int count = ((regsel >> 2) & 1) + ((regsel >> 1) & 1) + (regsel & 1);
    int total_size = count * 4;

    if ((ext >> 13) & 1) {
        // Control registers → EA (save)
        if (ea_mode == 4) {
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
        // EA → control registers (restore)
        if (ea_mode == 3) {
            // (An)+ postincrement
            uint32_t addr = cpu->a[ea_reg];
            if (regsel & 4) {
                fpu->fpcr = memory_read_uint32(addr);
                addr += 4;
            }
            if (regsel & 2) {
                fpu->fpsr = memory_read_uint32(addr);
                addr += 4;
            }
            if (regsel & 1) {
                fpu->fpiar = memory_read_uint32(addr);
                addr += 4;
            }
            cpu->a[ea_reg] = addr;
        } else if (ea_mode == 0) {
            // Dn — single register only
            uint32_t val = cpu->d[ea_reg];
            if (regsel & 4)
                fpu->fpcr = val;
            if (regsel & 2)
                fpu->fpsr = val;
            if (regsel & 1)
                fpu->fpiar = val;
        } else {
            uint32_t addr = calculate_ea(cpu, (uint32_t)total_size, ea_mode, ea_reg, true);
            if (regsel & 4) {
                fpu->fpcr = memory_read_uint32(addr);
                addr += 4;
            }
            if (regsel & 2) {
                fpu->fpsr = memory_read_uint32(addr);
                addr += 4;
            }
            if (regsel & 1) {
                fpu->fpiar = memory_read_uint32(addr);
                addr += 4;
            }
        }
    }
}

// ============================================================================
// Arithmetic operation dispatch
// ============================================================================

// Execute an FPU operation with source value and destination register
static void fpu_execute_op(fpu_state_t *fpu, unsigned op, double src, unsigned dst) {
    double result;

    switch (op) {
    case 0x00: // FMOVE
        result = src;
        break;
    case 0x04: // FSQRT
        result = sqrt(src);
        break;
    case 0x18: // FABS
        result = fabs(src);
        break;
    case 0x1A: // FNEG
        result = -src;
        break;

    // Binary operations: FPd OP src → FPd
    case 0x20: // FDIV
        result = fpu->fp[dst] / src;
        break;
    case 0x22: // FADD
        result = fpu->fp[dst] + src;
        break;
    case 0x23: // FMUL
        result = fpu->fp[dst] * src;
        break;
    case 0x28: // FSUB
        result = fpu->fp[dst] - src;
        break;

    // Comparison: FCMP sets condition codes only, no write-back
    case 0x38: {
        double diff = fpu->fp[dst] - src;
        fpu_update_cc(fpu, diff);
        return;
    }

    // Test: FTST sets condition codes from source, no write-back
    case 0x3A:
        fpu_update_cc(fpu, src);
        return;

    default:
        // Unimplemented operation — log and set OPERR
        LOG(1, "fpu: unimplemented op $%02X", op);
        fpu->fpsr |= FPEXC_OPERR | FPACC_IOP;
        return;
    }

    // Store result and update condition codes
    fpu->fp[dst] = result;
    fpu_update_cc(fpu, result);
}

// ============================================================================
// General FPU operation dispatcher (type=0)
// ============================================================================

void fpu_general_op(cpu_t *cpu, fpu_state_t *fpu, uint16_t opcode, uint16_t ext_word) {
    // Update FPIAR with address of this FPU instruction
    fpu->fpiar = cpu->instruction_pc;

    LOG(1, "fpu op PC=%08X opcode=%04X ext=%04X", cpu->instruction_pc, opcode, ext_word);

    unsigned rm = (ext_word >> 15) & 1;
    unsigned dir = (ext_word >> 14) & 1;
    unsigned src13 = (ext_word >> 13) & 1;
    unsigned src_spec = (ext_word >> 10) & 7;
    unsigned dst_reg = (ext_word >> 7) & 7;
    unsigned op = ext_word & 0x7F;

    // Decode the top 3 bits of the extension word
    unsigned top3 = (ext_word >> 13) & 7;

    switch (top3) {
    case 0: // 000: FPn→FPn operation
        if (src13 == 0) {
            // Register-to-register: FOP.X FPs,FPd
            double src_val = fpu->fp[src_spec];
            fpu_execute_op(fpu, op, src_val, dst_reg);
        }
        break;

    case 1: // 001: FMOVECR or FPn→FPn with bit 13 set
        // FMOVECR: ext bits [15:10] = 010111 → already decoded as top3=2
        // Bit 13=1 in top3=0 area: actually (rm=0, dir=0, src13=1)
        // This is FMOVECR: load ROM constant
        {
            double val = fpu_rom_constant(op);
            fpu->fp[dst_reg] = val;
            fpu_update_cc(fpu, val);
        }
        break;

    case 2: // 010: EA→FPn operation (memory source)
    {
        double src_val = fpu_load_ea(cpu, opcode, src_spec);
        fpu_execute_op(fpu, op, src_val, dst_reg);
        break;
    }

    case 3: // 011: FMOVE FPn→EA (register to memory)
    {
        // src_spec is the format, dst_reg is actually the source FPn
        fpu_store_ea(cpu, opcode, fpu->fp[dst_reg], src_spec);
        break;
    }

    case 4: // 100: FMOVEM control registers (EA → CR)
    case 5: // 101: FMOVEM control registers (CR → EA)
        fpu_movem_control(cpu, fpu, opcode, ext_word);
        break;

    case 6: // 110: FMOVEM data registers (static list)
    case 7: // 111: FMOVEM data registers (dynamic list)
        fpu_movem_data(cpu, fpu, opcode, ext_word);
        break;
    }
}
