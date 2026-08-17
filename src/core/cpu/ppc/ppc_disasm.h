// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_disasm.h
// Dependency-free disassembler for the PPC (MPC601) core, per the
// core-module contract (docs/core/cpu/cores.md): raw word + pc in, text
// out; linkable standalone (tools/disasm --arch ppc).  Covers the full
// 601 instruction set including the POWER holdovers, flagging 601-only
// SPR names and POWER-architecture mnemonics.
//
// Portable C99: no I/O, no allocation, no global state.

#ifndef GS_CPU_PPC_DISASM_H
#define GS_CPU_PPC_DISASM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decode status
enum {
    PPC_DIS_OK = 0, // valid 601 instruction
    PPC_DIS_INVALID = 1, // not a 601 instruction (takes the program exception)
};

typedef struct {
    uint32_t word; // raw instruction word
    uint32_t addr; // address the word was fetched from
    int status; // PPC_DIS_OK / PPC_DIS_INVALID
    int is_power; // 1 for a POWER-architecture holdover (601-specific)
    int is_branch; // 1 for any control transfer (b/bc/bclr/bcctr/rfi/sc)
    int has_target; // 1 if `target` holds a resolved branch target
    uint32_t target; // absolute branch target when has_target
    char text[96]; // "mnemonic\toperands" (tab-separated, NUL-terminated)
} ppc_insn;

// Disassemble one instruction word.  `addr` resolves pc-relative branch
// targets.  `out` is filled in completely; returns out->status.
int ppc_disassemble(uint32_t word, uint32_t addr, ppc_insn *out);

#ifdef __cplusplus
}
#endif

#endif // GS_CPU_PPC_DISASM_H
