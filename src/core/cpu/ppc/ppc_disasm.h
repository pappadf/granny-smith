// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_disasm.h
// Dependency-free disassembler for the PPC (MPC601/MPC604) core, per the
// core-module contract (docs/core/cpu/cores.md): raw word + pc in, text
// out; linkable standalone (tools/disasm --arch ppc / ppc604).  Covers
// the union of both models' instruction sets — the 601's POWER holdovers
// and MQ/RTC SPR moves flagged `is_power`, the 604-only encodings (mftb,
// tlbsync, stfiwx, fsel, fres, frsqrte) flagged `is_604` — with
// ppc_disassemble_model() applying one model's validity view.
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
    PPC_DIS_OK = 0, // valid instruction (on some model — see the flags)
    PPC_DIS_INVALID = 1, // no model implements it (takes the program exception)
};

typedef struct {
    uint32_t word; // raw instruction word
    uint32_t addr; // address the word was fetched from
    int status; // PPC_DIS_OK / PPC_DIS_INVALID
    int is_power; // 1 for a POWER-architecture holdover / MQ/RTC SPR move (601-only)
    int is_604; // 1 for a 604-only encoding (mftb/tlbsync/stfiwx/fsel/fres/frsqrte)
    int is_branch; // 1 for any control transfer (b/bc/bclr/bcctr/rfi/sc)
    int has_target; // 1 if `target` holds a resolved branch target
    uint32_t target; // absolute branch target when has_target
    char text[96]; // "mnemonic\toperands" (tab-separated, NUL-terminated)
} ppc_insn;

// Disassemble one instruction word against the union of both models.
// `addr` resolves pc-relative branch targets.  `out` is filled in
// completely; returns out->status.
int ppc_disassemble(uint32_t word, uint32_t addr, ppc_insn *out);

// Model-filtered disassembly: `model` is 601 or 604 (numerically equal to
// the machine_profile.h CPU_MODEL_PPC* ids; plain ints keep this TU
// dependency-free).  Encodings the model rejects at runtime — the POWER
// holdovers on the 604, the 604-only group on the 601 — come back
// PPC_DIS_INVALID with `.long` text, matching the model's program
// exception and the objdump -m powerpc:<model> oracle.
int ppc_disassemble_model(uint32_t word, uint32_t addr, int model, ppc_insn *out);

#ifdef __cplusplus
}
#endif

#endif // GS_CPU_PPC_DISASM_H
