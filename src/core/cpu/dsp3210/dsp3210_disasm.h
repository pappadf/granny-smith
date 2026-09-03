// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

/*
 * dsp3210_dis.h — reference disassembler for the AT&T DSP3210
 *
 * Sources of truth:
 *   - AT&T "DSP3210 Information Manual", chapters 4 (instruction set) and
 *     10 (instruction and register encodings), Table 10-1..10-3.
 *   - Cross-checked against the ROM-verified encoding notes in the
 *     840av_660av dossier (docs/dsp3210.md §1.5).
 *
 * Every DSP3210 instruction is exactly one 32-bit word on a 4-byte
 * boundary.  The disassembler is a pure function of (word, address); the
 * address is only used to resolve pc-relative branch targets (the pc value
 * observable by an instruction is the address of the instruction after the
 * delay slot, i.e. insn_addr + 8).
 *
 * Portable C99: no I/O, no allocation, no global state.
 */

#ifndef DSP3210_DIS_H
#define DSP3210_DIS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode status. */
enum {
    DSP3210_OK = 0, /* valid instruction */
    DSP3210_ILLEGAL = 1, /* one of the seven illegal opcodes (IM 7.5.3.2):
                            000000 000001 000010 001111 010110 010111
                            100010 — raises the illegal-opcode error */
    DSP3210_RESERVED = 2 /* opcode is legal but the word uses an encoding
                            the manual documents as reserved/not allowed */
};

/* Instruction class. */
enum {
    DSP3210_CLASS_CA = 0, /* control arithmetic (integer/branch/move) */
    DSP3210_CLASS_DA = 1 /* data arithmetic (floating point) */
};

typedef struct {
    uint32_t word; /* raw instruction word */
    uint32_t addr; /* address the word was fetched from */
    int status; /* DSP3210_OK / _ILLEGAL / _RESERVED */
    int klass; /* DSP3210_CLASS_CA / _DA */
    int is_branch; /* 1 for any control transfer: goto, conditional
                      goto, call, loop branch, return, ireturn */
    int no_delay_slot; /* 1 only for `ireturn`, the one control transfer
                         that does NOT execute the following word: the
                         instruction shadow register is replayed in its
                         place [IM IRETURN "Latency"].  A tracer that
                         keys delay-slot handling off is_branch alone
                         will mishandle it. */
    int has_target; /* 1 if `target` holds a resolved branch target */
    uint32_t target; /* absolute branch target when has_target */
    char text[128]; /* nul-terminated assembler text */
} dsp3210_insn;

/*
 * Disassemble one instruction word.
 *   word — the 32-bit instruction (host byte order; use the helpers below
 *          to read from a byte stream)
 *   addr — address of the instruction (used for pc-relative targets)
 *   out  — filled in completely on return (never left uninitialised)
 * Returns the status (same value as out->status).
 */
int dsp3210_disassemble(uint32_t word, uint32_t addr, dsp3210_insn *out);

/* Byte-stream helpers.  The DSP3210 in the AV Macs runs big-endian. */
uint32_t dsp3210_read_be32(const void *p);
uint32_t dsp3210_read_le32(const void *p);

/* Human-readable name for a status code. */
const char *dsp3210_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif /* DSP3210_DIS_H */
