// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_internal.h
// CPU struct definition and shared helper functions for decoder instantiation.
// Included by cpu.c (lifecycle), cpu_68000.c (68000 decoder), and future
// cpu_68030.c (68030 decoder). All helpers are static inline to ensure
// inlining within each translation unit.

#ifndef CPU_INTERNAL_H
#define CPU_INTERNAL_H

#include "cpu.h"
#include "memory.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// CPU internal state
struct cpu {

    uint32_t pc;

    uint32_t d[8];
    uint32_t a[8];

    uint32_t ssp;
    uint32_t usp;

    // status register
    uint32_t trace;
    uint32_t supervisor;
    uint32_t interrupt_mask;
    uint32_t extend;
    uint32_t negative;
    uint32_t zero;
    uint32_t carry;
    uint32_t overflow;

    // Interrupt priority level (from PAL to CPU)
    uint32_t ipl;

    // CPU model (CPU_MODEL_68000 or CPU_MODEL_68030)
    int cpu_model;

    // 68030-specific control registers (zero/unused on 68000)
    uint32_t vbr; // Vector Base Register
    uint32_t cacr; // Cache Control Register
    uint32_t caar; // Cache Address Register
    uint32_t sfc; // Source Function Code
    uint32_t dfc; // Destination Function Code
    uint32_t msp; // Master Stack Pointer (M=1 supervisor stack)
    uint32_t m; // M bit: 0=ISP active, 1=MSP active (supervisor only)
    uint32_t instruction_pc; // address of current instruction (for Format $2 frames)

    // 68030-specific: MMU state pointer (NULL for 68000)
    void *mmu;
};

// Read the condition code register from CPU flags
static inline uint8_t read_ccr(cpu_t *restrict cpu) {
    uint8_t ccr = 0;

    if (cpu->extend)
        ccr |= 1 << 4;
    if (cpu->negative)
        ccr |= 1 << 3;
    if (cpu->zero)
        ccr |= 1 << 2;
    if (cpu->overflow)
        ccr |= 1 << 1;
    if (cpu->carry)
        ccr |= 1;

    return ccr;
}

// Update condition code register flags from 16-bit value
static inline void write_ccr(cpu_t *restrict cpu, uint16_t ccr) {
    cpu->extend = ccr >> 4 & 1;
    cpu->negative = ccr >> 3 & 1;
    cpu->zero = ccr >> 2 & 1;
    cpu->overflow = ccr >> 1 & 1;
    cpu->carry = ccr & 1;
}

// Fetch a 16-bit word from PC, optionally advancing PC
static inline uint16_t fetch_16(cpu_t *restrict cpu, bool increment) {
    uint16_t w = memory_read_uint16(cpu->pc);
    if (increment)
        cpu->pc += 2;
    return w;
}

// Fetch a 32-bit long word from PC, optionally advancing PC
static inline uint32_t fetch_32(cpu_t *restrict cpu, bool increment) {
    uint32_t l = memory_read_uint32(cpu->pc);
    if (increment)
        cpu->pc += 4;
    return l;
}

// Helper: extract and sign-extend index register value for indexed modes
static inline uint32_t ea_index_value(cpu_t *restrict cpu, uint16_t ext_word) {
    uint32_t v = (ext_word & 0x8000) ? cpu->a[(ext_word >> 12) & 7] : cpu->d[(ext_word >> 12) & 7];
    if ((ext_word & 0x0800) == 0) // word-sized index
        v = (int32_t)(int16_t)v; // sign extend
    // Apply scale factor (68020+): bits 10-9 encode shift count 0-3
    if (cpu->cpu_model >= CPU_MODEL_68030) {
        int scale = (ext_word >> 9) & 3;
        v <<= scale;
    }
    return v;
}

// 68020+ full extension word EA calculation (bit 8 = 1)
static inline uint32_t calculate_ea_full(cpu_t *restrict cpu, uint16_t ext, uint32_t base_reg, bool increment) {
    // The extension word (ext) was already fetched by the caller via fetch_16(cpu, increment).
    // With increment=true, cpu->pc now points past the ext word (correct for BD/OD reads).
    // With increment=false, cpu->pc still points AT the ext word (not yet advanced).
    // Use a local 'pos' to sequence through BD and OD bytes correctly in both cases.
    uint32_t pos = cpu->pc;
    if (!increment)
        pos += 2; // skip the ext word we already read without advancing cpu->pc
    // Base register suppressed? (bit 7)
    uint32_t base = (ext & 0x0080) ? 0 : base_reg;
    // Index suppressed? (bit 6)
    uint32_t index = (ext & 0x0040) ? 0 : ea_index_value(cpu, ext);
    // Base displacement size (bits 5-4): 00=reserved, 01=null, 10=word, 11=long
    int32_t bd = 0;
    int bd_size = (ext >> 4) & 3;
    if (bd_size == 2) {
        bd = (int32_t)(int16_t)memory_read_uint16(pos);
        pos += 2;
    } else if (bd_size == 3) {
        bd = (int32_t)memory_read_uint32(pos);
        pos += 4;
    }
    // I/IS (bits 2-0 + bit 6): determine indirect mode
    int iis = ext & 7;
    uint32_t result;
    if ((ext & 0x0040) == 0 && iis == 0) {
        // No memory indirect: (bd,An,Xn)
        result = base + index + (uint32_t)bd;
    } else if (ext & 0x0040) {
        // Index suppressed: memory indirect without index
        uint32_t intermediate = base + (uint32_t)bd;
        if (iis >= 1 && iis <= 3) {
            // Memory indirect: ([bd,An])
            intermediate = memory_read_uint32(intermediate);
        }
        // Outer displacement
        int32_t od = 0;
        if (iis == 2) {
            od = (int32_t)(int16_t)memory_read_uint16(pos);
            pos += 2;
        } else if (iis == 3) {
            od = (int32_t)memory_read_uint32(pos);
            pos += 4;
        }
        result = intermediate + (uint32_t)od;
    } else {
        // Index not suppressed
        if (iis >= 1 && iis <= 3) {
            // Pre-indexed: ([bd,An,Xn],od)
            uint32_t inner_addr = base + index + (uint32_t)bd;
            uint32_t intermediate = memory_read_uint32(inner_addr);
            int32_t od = 0;
            if (iis == 2) {
                od = (int32_t)(int16_t)memory_read_uint16(pos);
                pos += 2;
            } else if (iis == 3) {
                od = (int32_t)memory_read_uint32(pos);
                pos += 4;
            }
            result = intermediate + (uint32_t)od;
        } else if (iis >= 5 && iis <= 7) {
            // Post-indexed: ([bd,An],Xn,od)
            uint32_t intermediate = memory_read_uint32(base + (uint32_t)bd);
            int32_t od = 0;
            if (iis == 6) {
                od = (int32_t)(int16_t)memory_read_uint16(pos);
                pos += 2;
            } else if (iis == 7) {
                od = (int32_t)memory_read_uint32(pos);
                pos += 4;
            }
            result = intermediate + index + (uint32_t)od;
        } else {
            // iis=4 (reserved): treat as no-memory-indirect
            result = base + index + (uint32_t)bd;
        }
    }
    // Commit the new PC only if the caller wants increment
    if (increment)
        cpu->pc = pos;
    return result;
}

// Calculate the effective address for an operand based on mode and register
static inline uint32_t calculate_ea(cpu_t *restrict cpu, int size, int mode, int reg, bool increment) {
    switch (mode) {
    case 2: // (An)
        return cpu->a[reg];
    case 3: // (An)+
    {
        uint32_t ea = cpu->a[reg];
        if (increment)
            cpu->a[reg] += (reg == 7 && size == 1) ? 2 : size; // stack byte adjust
        return ea;
    }
    case 4: // -(An)
    {
        uint32_t ea = cpu->a[reg] - ((reg == 7 && size == 1) ? 2 : size);
        if (increment)
            cpu->a[reg] = ea;
        return ea;
    }
    case 5: // (d16,An)
    {
        int16_t disp = fetch_16(cpu, increment);
        return cpu->a[reg] + (int32_t)disp;
    }
    case 6: // (d8,An,Xn) or 68020+ full extension word
    {
        uint16_t ext = fetch_16(cpu, increment);
        if ((ext & 0x0100) && cpu->cpu_model >= CPU_MODEL_68030) {
            // Full extension word (bit 8 = 1): 68020+ complex addressing
            return calculate_ea_full(cpu, ext, cpu->a[reg], increment);
        }
        // Brief extension word: (d8,An,Xn) with optional scale on 68020+
        int32_t disp = (int32_t)(int8_t)(ext & 0xFF);
        return cpu->a[reg] + disp + ea_index_value(cpu, ext);
    }
    case 7:
        switch (reg) {
        case 0: // (xxx).W
            return (int32_t)(int16_t)fetch_16(cpu, increment);
        case 1: // (xxx).L
            return fetch_32(cpu, increment);
        case 2: // (d16,PC)
        {
            uint32_t pc = cpu->pc;
            int16_t disp = fetch_16(cpu, increment);
            return pc + (int32_t)disp;
        }
        case 3: // (d8,PC,Xn) or 68020+ full extension word
        {
            uint32_t pc = cpu->pc;
            uint16_t ext = fetch_16(cpu, increment);
            if ((ext & 0x0100) && cpu->cpu_model >= CPU_MODEL_68030) {
                // Full extension word (bit 8 = 1): 68020+ PC-relative complex addressing
                return calculate_ea_full(cpu, ext, pc, increment);
            }
            // Brief extension word: (d8,PC,Xn) with optional scale on 68020+
            int32_t disp = (int32_t)(int8_t)(ext & 0xFF);
            return pc + disp + ea_index_value(cpu, ext);
        }
        case 4: // immediate (#<data>)
        {
            uint32_t ea = (size == 1) ? cpu->pc + 1 : cpu->pc;
            if (increment)
                cpu->pc += (size == 1) ? 2 : size;
            return ea;
        }
        default:
            assert(0);
            return 0;
        }
    default:
        break;
    }
    return (uint32_t)-1;
}

// Read an 8-bit value from the effective address specified in the opcode
static inline uint8_t read_ea_8(cpu_t *restrict cpu, uint16_t opcode, bool increment) {
    uint16_t mode = opcode >> 3 & 7;
    uint16_t reg = opcode & 7;

    if (mode == 0)
        return cpu->d[opcode & 7];
    else if (mode == 1)
        return cpu->a[opcode & 7];
    else
        return memory_read_uint8(calculate_ea(cpu, 1, mode, reg, increment));
}

// Read a 16-bit value from the effective address specified in the opcode
static inline uint16_t read_ea_16(cpu_t *restrict cpu, uint16_t opcode, bool increment) {
    uint16_t mode = opcode >> 3 & 7;
    uint16_t reg = opcode & 7;

    if (mode == 0)
        return cpu->d[opcode & 7];
    else if (mode == 1)
        return cpu->a[opcode & 7];
    else
        return memory_read_uint16(calculate_ea(cpu, 2, mode, reg, increment));
}

// Read a 32-bit value from the effective address specified in the opcode
static inline uint32_t read_ea_32(cpu_t *restrict cpu, uint16_t opcode, bool increment) {
    uint16_t mode = opcode >> 3 & 7;
    uint16_t reg = opcode & 7;

    if (mode == 0)
        return cpu->d[opcode & 7];
    else if (mode == 1)
        return cpu->a[opcode & 7];
    else
        return memory_read_uint32(calculate_ea(cpu, 4, mode, reg, increment));
}

// Write an 8-bit value to the effective address specified by mode and register
static inline void write_ea_8(cpu_t *restrict cpu, uint16_t mode, uint16_t reg, uint8_t value) {
    if (mode == 0)
        cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | value;
    else if (mode == 1)
        cpu->a[reg] = (cpu->a[reg] & 0xFFFFFF00) | value;
    else
        memory_write_uint8(calculate_ea(cpu, 1, mode, reg, true), value);
}

// Write a 16-bit value to the effective address specified by mode and register
static inline void write_ea_16(cpu_t *restrict cpu, uint16_t mode, uint16_t reg, uint16_t value) {
    if (mode == 0)
        cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | value;
    else if (mode == 1)
        cpu->a[reg] = (cpu->a[reg] & 0xFFFF0000) | value;
    else
        memory_write_uint16(calculate_ea(cpu, 2, mode, reg, true), value);
}

// Write a 32-bit value to the effective address specified by mode and register
static inline void write_ea_32(cpu_t *restrict cpu, uint16_t mode, uint16_t reg, uint32_t value) {
    if (mode == 0)
        cpu->d[reg] = value;
    else if (mode == 1)
        cpu->a[reg] = value;
    else
        memory_write_uint32(calculate_ea(cpu, 4, mode, reg, true), value);
}

// Execute MOVEM instruction: load multiple registers from memory
static inline void movem_to_register(cpu_t *restrict cpu, uint16_t opcode, int bits) {
    int i;
    uint16_t register_mask = fetch_16(cpu, true);
    uint32_t ea = calculate_ea(cpu, 4, opcode >> 3 & 7, opcode & 7, true);

    for (i = 0; i < 8; i++)
        if (register_mask & (1 << i)) {
            cpu->d[i] = bits == 16 ? (int32_t)(int16_t)memory_read_uint16(ea) : memory_read_uint32(ea);
            ea += bits >> 3;
        }
    for (i = 0; i < 8; i++)
        if (register_mask & (0x100 << i)) {
            cpu->a[i] = bits == 16 ? (int32_t)(int16_t)memory_read_uint16(ea) : memory_read_uint32(ea);
            ea += bits >> 3;
        }

    if ((opcode & 0x38) == 0x18) // post-increment mode updates address register after MOVEM
        cpu->a[opcode & 7] = ea;
}

// Execute MOVEM instruction: store multiple registers to memory
static inline void movem_from_register(cpu_t *restrict cpu, uint16_t opcode, int bits) {
    int i;
    uint16_t register_mask = fetch_16(cpu, true);
    int step = bits >> 3; // 2 for 16-bit, 4 for 32-bit

    if ((opcode & 0x38) == 0x20) // predecrement mode: -(An)
    {
        int an = opcode & 7; // base address register number
        uint32_t addr = cpu->a[an];
        for (i = 0; i < 8; i++)
            if (register_mask & (1 << i)) {
                addr -= step;
                // 68030 stores An-step when An is the predecrement base register
                uint32_t val = cpu->a[7 - i];
                if (cpu->cpu_model >= CPU_MODEL_68030 && (7 - i) == an)
                    val -= step;
                if (bits == 16)
                    memory_write_uint16(addr, val);
                else
                    memory_write_uint32(addr, val);
            }
        for (i = 0; i < 8; i++)
            if (register_mask & (0x100 << i)) {
                addr -= step;
                if (bits == 16)
                    memory_write_uint16(addr, cpu->d[7 - i]);
                else
                    memory_write_uint32(addr, cpu->d[7 - i]);
            }
        cpu->a[an] = addr;
    } else {
        uint32_t ea = calculate_ea(cpu, 4, opcode >> 3 & 7, opcode & 7, true);
        for (i = 0; i < 8; i++)
            if (register_mask & (1 << i)) {
                if (bits == 16)
                    memory_write_uint16(ea, cpu->d[i]);
                else
                    memory_write_uint32(ea, cpu->d[i]);
                ea += step;
            }
        for (i = 0; i < 8; i++)
            if (register_mask & (0x100 << i)) {
                if (bits == 16)
                    memory_write_uint16(ea, cpu->a[i]);
                else
                    memory_write_uint32(ea, cpu->a[i]);
                ea += step;
            }
    }
}

// ABCD: add decimal with extend (based on research at https://gendev.spritesmind.net/forum/viewtopic.php?t=1964)
static inline uint8_t abcd(cpu_t *restrict cpu, uint8_t xx, uint8_t yy) {
    uint8_t ss = xx + yy + !!cpu->extend;
    uint8_t dc = (ss + 0x66 ^ ss) >> 1;
    uint8_t bc = (xx & yy) | ((xx | yy) & ~ss);
    uint8_t corr = (bc | dc) & 0x88;
    uint8_t rr = ss + corr - (corr >> 2);
    // V is "undefined" per PRM; 68030+ clears it, 68000 computes from result (Spritesmind formula)
    cpu->overflow = (cpu->cpu_model >= CPU_MODEL_68030) ? 0 : (~ss & rr & 0x80);
    cpu->extend = cpu->carry = (bc | (ss & ~rr)) & 0x80;
    cpu->negative = rr & 0x80;
    cpu->zero &= rr == 0;
    return rr;
}

// SBCD: subtract decimal with extend (based on research at https://gendev.spritesmind.net/forum/viewtopic.php?t=1964)
static inline uint8_t sbcd(cpu_t *restrict cpu, uint8_t xx, uint8_t yy) {
    uint8_t dd = xx - yy - !!cpu->extend;
    uint8_t bc = ((~xx & yy) | ((~xx | yy) & dd)) & 0x88;
    uint8_t rr = dd - bc + (bc >> 2);
    // V is "undefined" per PRM; 68030+ clears it, 68000 computes from result (Spritesmind formula)
    cpu->overflow = (cpu->cpu_model >= CPU_MODEL_68030) ? 0 : (dd & ~rr & 0x80);
    cpu->extend = cpu->carry = (bc | (~dd & rr)) & 0x80;
    cpu->negative = rr & 0x80;
    cpu->zero &= rr == 0;
    return rr;
}

// Test CPU condition codes (M68000PRM table 3-19)
static inline bool conditional_test(cpu_t *restrict cpu, uint8_t test) {
    assert(test < 16);

    switch (test) {
    case 0x0:
        return true;
    case 0x1:
        return false;
    case 0x2:
        return !cpu->carry && !cpu->zero;
    case 0x3:
        return cpu->carry || cpu->zero;
    case 0x4:
        return !cpu->carry;
    case 0x5:
        return cpu->carry;
    case 0x6:
        return !cpu->zero;
    case 0x7:
        return cpu->zero;
    case 0x8:
        return !cpu->overflow;
    case 0x9:
        return cpu->overflow;
    case 0xA:
        return !cpu->negative;
    case 0xB:
        return cpu->negative;
    case 0xC:
        return cpu->negative && cpu->overflow || !cpu->negative && !cpu->overflow;
    case 0xD:
        return cpu->negative && !cpu->overflow || !cpu->negative && cpu->overflow;
    case 0xE:
        return cpu->negative && cpu->overflow && !cpu->zero || !cpu->negative && !cpu->overflow && !cpu->zero;
    case 0xF:
        return cpu->zero || cpu->negative && !cpu->overflow || !cpu->negative && cpu->overflow;
    }

    return 0;
}

// Raise a CPU exception by pushing state and loading exception vector.
// On 68030, determines frame format from vector number: vectors 5 (divide by
// zero), 6 (CHK/CHK2), 7 (TRAPV/TRAPcc), and 9 (trace) use Format $2 (adds
// instruction address); all others use Format $0. Uses VBR on 68030.
static inline void exception(cpu_t *restrict cpu, uint32_t vector, uint32_t pc, uint16_t sr) {
    if (!cpu->supervisor) {
        cpu->usp = cpu->a[7];
        cpu->a[7] = (cpu->m && cpu->cpu_model == CPU_MODEL_68030) ? cpu->msp : cpu->ssp;
        cpu->supervisor = 1;
        cpu->m = 0; // exceptions always switch to ISP (M=0)
    } else if (cpu->m && cpu->cpu_model == CPU_MODEL_68030) {
        // In supervisor+master mode: switch to ISP for the exception frame
        cpu->msp = cpu->a[7];
        cpu->a[7] = cpu->ssp;
        cpu->m = 0;
    }

    if (cpu->cpu_model == CPU_MODEL_68030) {
        // Determine frame format from exception vector number
        uint32_t vec_num = vector / 4;
        int format = 0;
        // Vectors 5,6,7,9 use Format $2 on 68020+
        if (vec_num == 5 || vec_num == 6 || vec_num == 7 || vec_num == 9) {
            format = 2;
        }

        if (format == 2) {
            // Format $2: push instruction address (4 bytes) before format word
            cpu->a[7] -= 4;
            memory_write_uint32(cpu->a[7], cpu->instruction_pc);
        }

        // Format/vector word with format code in upper nibble
        cpu->a[7] -= 2;
        memory_write_uint16(cpu->a[7], (uint16_t)((format << 12) | (vector & 0x0FFF)));
        cpu->a[7] -= 4;
        memory_write_uint32(cpu->a[7], pc);
        cpu->a[7] -= 2;
        memory_write_uint16(cpu->a[7], sr);
        cpu->pc = memory_read_uint32(cpu->vbr + vector);
        // Exception processing clears T1/T0 per M68000 PRM
        cpu->trace = 0;
    } else {
        // 68000 frame: PC then SR (6 bytes, no format word)
        cpu->a[7] -= 4;
        memory_write_uint32(cpu->a[7], pc);
        cpu->a[7] -= 2;
        memory_write_uint16(cpu->a[7], sr);
        cpu->pc = memory_read_uint32(vector);
        cpu->trace = 0;
    }
}

// Check if a pending interrupt should be serviced
static inline void cpu_check_interrupt(cpu_t *restrict cpu) {
    if (cpu->ipl > cpu->interrupt_mask) {
        uint16_t sr = cpu_get_sr(cpu);
        cpu->interrupt_mask = cpu->ipl;
        exception(cpu, 0x60 + 4 * cpu->ipl, cpu->pc, sr); // vector includes VBR on 68030
    }
}

// Update full status register including supervisor mode, M bit, and interrupt mask.
// On 68030, handles ISP/MSP switching when M bit changes.
static inline void write_sr(cpu_t *restrict cpu, uint16_t sr) {
    bool new_s = (sr >> 13) & 1;

    if (cpu->cpu_model == CPU_MODEL_68030) {
        bool new_m = (sr >> 12) & 1;
        if (cpu->supervisor) {
            // Save current A7 to the active supervisor stack register
            if (cpu->m)
                cpu->msp = cpu->a[7];
            else
                cpu->ssp = cpu->a[7];
            if (new_s) {
                // Staying in supervisor: load the newly-selected stack
                if (new_m)
                    cpu->a[7] = cpu->msp;
                else
                    cpu->a[7] = cpu->ssp;
            } else {
                cpu->a[7] = cpu->usp; // leaving supervisor
            }
        } else if (new_s) {
            cpu->usp = cpu->a[7]; // save USP
            if (new_m)
                cpu->a[7] = cpu->msp;
            else
                cpu->a[7] = cpu->ssp;
        }
        cpu->m = new_m;
        cpu->trace = ((sr >> 14) & 3); // T1 in bit 1, T0 in bit 0
    } else {
        // 68000: no M bit, no T0
        if (new_s && !cpu->supervisor) {
            cpu->usp = cpu->a[7];
            cpu->a[7] = cpu->ssp;
        } else if (!new_s && cpu->supervisor) {
            cpu->ssp = cpu->a[7];
            cpu->a[7] = cpu->usp;
        }
        cpu->trace = (sr >> 15) & 1; // only T1
    }

    cpu->supervisor = new_s;
    cpu->interrupt_mask = (sr >> 8) & 7;
    write_ccr(cpu, sr);
    cpu_check_interrupt(cpu);
}

// Exception helpers
static inline void exception_divide_by_zero(cpu_t *restrict cpu) {
    exception(cpu, 0x14, cpu->pc, cpu_get_sr(cpu));
}
static inline void chk_exception(cpu_t *restrict cpu) {
    exception(cpu, 0x18, cpu->pc, cpu_get_sr(cpu));
}
static inline void trapv(cpu_t *restrict cpu) {
    exception(cpu, 0x1c, cpu->pc, cpu_get_sr(cpu));
}
static inline void privilege_violation(cpu_t *restrict cpu) {
    exception(cpu, 0x020, cpu->pc - 2, cpu_get_sr(cpu));
}
static inline void trap(cpu_t *restrict cpu, int trap_num) {
    exception(cpu, 0x080 + trap_num * 4, cpu->pc, cpu_get_sr(cpu));
}
static inline void a_trap(cpu_t *restrict cpu) {
    exception(cpu, 0x28, cpu->pc - 2, cpu_get_sr(cpu));
}
static inline void f_trap(cpu_t *restrict cpu) {
    exception(cpu, 0x2C, cpu->pc - 2, cpu_get_sr(cpu));
}

// Advance cpu->pc past EA extension words (68030 exception path only).
// Called when an invalid EA mode is detected, to ensure the exception PC
// reflects the words that real hardware would have prefetched.
// op_size is the operand size in bytes (only needed for #imm; 0 = default word).
static inline void skip_ea_extension_words(cpu_t *restrict cpu, int mode, int reg, int op_size) {
    if (mode == 5 || (mode == 7 && reg == 2)) {
        // (d16,An) or (d16,PC): word displacement
        cpu->pc += 2;
    } else if (mode == 6 || (mode == 7 && reg == 3)) {
        // (d8,An,Xn) or (d8,PC,Xn): brief or full extension word
        uint16_t ext = memory_read_uint16(cpu->pc);
        cpu->pc += 2;
        if (ext & 0x0100) {
            // Full extension word — skip base and outer displacements
            int bd_size = (ext >> 4) & 3;
            if (bd_size == 2)
                cpu->pc += 2; // word BD
            else if (bd_size == 3)
                cpu->pc += 4; // long BD
            int iis = ext & 7;
            if (!(ext & 0x0040)) {
                // Index not suppressed
                if (iis == 2 || iis == 6)
                    cpu->pc += 2; // word OD
                else if (iis == 3 || iis == 7)
                    cpu->pc += 4; // long OD
            } else {
                // Index suppressed
                if (iis == 2)
                    cpu->pc += 2; // word OD
                else if (iis == 3)
                    cpu->pc += 4; // long OD
            }
        }
    } else if (mode == 7 && reg == 0) {
        // (xxx).W: word absolute address
        cpu->pc += 2;
    } else if (mode == 7 && reg == 1) {
        // (xxx).L: long absolute address
        cpu->pc += 4;
    } else if (mode == 7 && reg == 4) {
        // #imm: byte/word = 1 word, long = 2 words
        cpu->pc += (op_size <= 2) ? 2 : 4;
    }
    // Modes 0-4 (Dn, An, (An), (An)+, -(An)): no extension words
}

// Raise illegal instruction exception (vector 4).
// Stacked PC points to the first word of the illegal instruction (per M68000 PRM).
static inline void illegal_instruction(cpu_t *restrict cpu) {
    uint32_t pc = (cpu->cpu_model >= CPU_MODEL_68030) ? cpu->instruction_pc : (cpu->pc - 2);
    exception(cpu, 0x010, pc, cpu_get_sr(cpu));
}

#endif // CPU_INTERNAL_H
