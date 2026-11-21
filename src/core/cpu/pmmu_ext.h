// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pmmu_ext.h
// Field decode of the 68030 PMMU extension word (F-line CpID 0, type 0:
// PMOVE / PFLUSH / PLOAD / PTEST), shared by the executor (cpu_68030.c) and
// the disassembler (cpu_disasm.c) so both read the encoding one way.

#ifndef PMMU_EXT_H
#define PMMU_EXT_H

#include <stdint.h>

// The 68030 PMMU operation classes in extension bits 15:13 (MC68030UM 9.x,
// M68000PRM section 6)
enum {
    PMMU_OP_PMOVE_TT = 0, // PMOVE TT0/TT1
    PMMU_OP_PLOAD_PFLUSH = 1, // PLOAD, PFLUSHA, PFLUSH FC,#mask[,<ea>]
    PMMU_OP_PMOVE_ROOT = 2, // PMOVE TC/SRP/CRP
    PMMU_OP_PMOVE_MMUSR = 3, // PMOVE MMUSR
    PMMU_OP_PTEST = 4, // PTESTR / PTESTW
};

// The fields of one PMMU extension word.  Bits 12:10 are the register
// select (PMOVE), the mode (PLOAD/PFLUSH) or the level (PTEST); bit 8 is FD
// (PMOVE) or the A bit (PTEST); bits 7:5 are the PFLUSH mask or the PTEST
// address register.
typedef struct {
    unsigned op; // bits 15:13, PMMU_OP_*
    unsigned sel; // bits 12:10: preg / mode / level
    unsigned rw; // bit 9: PMOVE 1 = register to <ea>; PLOAD/PTEST 1 = read
    unsigned fd; // bit 8: PMOVE FD, PTEST A
    unsigned mask; // bits 7:5: PFLUSH mask, PTEST An
    unsigned fc5; // bits 4:0: the function-code specifier
} pmmu_ext_t;

// Splits a PMMU extension word into its fields
static inline pmmu_ext_t pmmu_ext_decode(uint16_t ext) {
    pmmu_ext_t x = {
        .op = (ext >> 13) & 7u,
        .sel = (ext >> 10) & 7u,
        .rw = (ext >> 9) & 1u,
        .fd = (ext >> 8) & 1u,
        .mask = (ext >> 5) & 7u,
        .fc5 = ext & 0x1Fu,
    };
    return x;
}

#endif // PMMU_EXT_H
