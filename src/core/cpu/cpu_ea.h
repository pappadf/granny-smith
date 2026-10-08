// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_ea.h
// 68K effective-address mode bitmask, shared by the instruction templates
// (cpu_ops.h, via cpu_internal.h) and the disassembler (cpu_disasm.c).
// Internal to src/core/cpu/: the public cpu.h does not expose it.

#ifndef CPU_EA_H
#define CPU_EA_H

// ============================================================================
// Type Definitions
// ============================================================================

// One bit per addressing mode, indexed by `mode + (mode == 7 ? reg : 0)`:
// bits 0-6 are modes 0-6, bits 7-11 are mode 7 with reg 0-4.  An op's set of
// legal modes is the OR of these bits (or one of the PRM categories below).
typedef enum {

    ea_dn = 0x00001, // Dn
    ea_an = 0x00002, // An
    ea_an_mem = 0x00004, // (An)
    ea_an_plus = 0x00008, // (An)+
    ea_min_an = 0x00010, // -(An)
    ea_d16_an = 0x00020, // (d16,An)
    ea_d8_an_xn = 0x00040, // (d8,An,Xn) and the 68020+ full-extension forms
    ea_xxx_w = 0x00080, // (xxx).W
    ea_xxx_l = 0x00100, // (xxx).L
    ea_d16_pc = 0x00200, // (d16,PC)
    ea_d8_pc_xn = 0x00400, // (d8,PC,Xn) and the 68020+ full-extension forms
    ea_xxx = 0x00800, // #<data> (immediate)

    // M68000PRM table 2-4 addressing-mode categories
    ea_any = 0x00FFF,
    ea_data = ea_any & ~ea_an,
    ea_memory = ea_data & ~ea_dn,
    ea_control = ea_memory & ~(ea_an_plus | ea_min_an | ea_xxx),
    ea_alterable = ea_any & ~(ea_d16_pc | ea_d8_pc_xn | ea_xxx),

} ea_mode_t;

#endif // CPU_EA_H
