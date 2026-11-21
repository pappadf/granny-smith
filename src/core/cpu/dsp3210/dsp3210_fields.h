// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dsp3210_fields.h
// Instruction-word field helpers and the CAU register-code table of the AT&T
// DSP3210, shared by the interpreter (dsp3210.c) and the disassembler
// (dsp3210_disasm.c) so both read the encoding one way.

#ifndef DSP3210_FIELDS_H
#define DSP3210_FIELDS_H

#include <stdint.h>

// Extracts bits hi..lo of an instruction word, right-aligned
static inline uint32_t bits(uint32_t w, int hi, int lo) {
    return (w >> lo) & ((hi - lo == 31) ? 0xFFFFFFFFu : ((1u << (hi - lo + 1)) - 1u));
}

// Sign-extends the low 16 bits
static inline int32_t sext16(uint32_t v) {
    return (int32_t)(int16_t)(v & 0xFFFFu);
}

// What a 5-bit CAU register code names
enum { RK_ZERO, RK_REG, RK_PC, RK_PCSH, RK_PLUS, RK_MINUS, RK_RES };

// One CAU register code (IM Table 10-2): its kind, the register index for
// RK_REG, and the disassembler's name.  The map is deliberately
// discontinuous: pc = 15, r15-r19 = 17-21, -n/+n = 22/23, r20-r22 = 24-26,
// pcsh = 30; 16, 27-29 and 31 are reserved.
typedef struct {
    uint8_t kind;
    uint8_t idx;
    const char *name;
} dsp_rc_t;

static const dsp_rc_t k_dsp_rc[32] = {
    {RK_ZERO,  0,  "r0"  },
    {RK_REG,   1,  "r1"  },
    {RK_REG,   2,  "r2"  },
    {RK_REG,   3,  "r3"  },
    {RK_REG,   4,  "r4"  },
    {RK_REG,   5,  "r5"  },
    {RK_REG,   6,  "r6"  },
    {RK_REG,   7,  "r7"  },
    {RK_REG,   8,  "r8"  },
    {RK_REG,   9,  "r9"  },
    {RK_REG,   10, "r10" },
    {RK_REG,   11, "r11" },
    {RK_REG,   12, "r12" },
    {RK_REG,   13, "r13" },
    {RK_REG,   14, "r14" },
    {RK_PC,    0,  "pc"  },
    {RK_RES,   0,  "?r16"},
    {RK_REG,   15, "r15" },
    {RK_REG,   16, "r16" },
    {RK_REG,   17, "r17" },
    {RK_REG,   18, "r18" },
    {RK_REG,   19, "r19" },
    {RK_MINUS, 0,  "-n"  },
    {RK_PLUS,  0,  "+n"  },
    {RK_REG,   20, "r20" },
    {RK_REG,   21, "r21" },
    {RK_REG,   22, "r22" },
    {RK_RES,   0,  "?r27"},
    {RK_RES,   0,  "?r28"},
    {RK_RES,   0,  "?r29"},
    {RK_PCSH,  0,  "pcsh"},
    {RK_RES,   0,  "?r31"},
};

#endif // DSP3210_FIELDS_H
