// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// annotate_disasm.h
// Disassemble + annotate a 68k code buffer to a file.  Calls cpu_disasm
// per instruction and decorates the output with branch-destination
// resolution and (when the flag is set) trap-name comments via
// macos_atrap_name().  Both the standalone tools/disasm tool and the `re`
// orchestrator link against this; the proposal calls out factoring out
// the duplicated branch annotator that previously lived inside
// tools/disasm.c.

#pragma once

#ifndef GS_RE_ANNOTATE_DISASM_H
#define GS_RE_ANNOTATE_DISASM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Bit flags steering which annotation passes are emitted.
#define RE_DISASM_ANNOTATE_BRANCH 0x01u // append "; -> $addr" to PC-relative ops
#define RE_DISASM_ANNOTATE_TRAPS  0x02u // append "; <_TrapName>" to Axxx opcodes
#define RE_DISASM_ALL             (RE_DISASM_ANNOTATE_BRANCH | RE_DISASM_ANNOTATE_TRAPS)

// Append the destination annotation (e.g. "\t; -> $00000034") to `buf`
// for any operand string containing "*+$N" or "*-$N".  Exposed so the
// standalone tools/disasm/disasm tool can use the same helper.
void re_annotate_branch_destination(char *buf, size_t buf_size, const char *mnemonic, const char *operands_text,
                                    uint32_t instr_addr);

// Disassemble `bytes`/`bytes_len` (a contiguous run of 68k machine code in
// big-endian bytes — typically the body of a CODE segment with its 4- or
// 16-byte header skipped) and stream the listing to `out`.  `base_addr`
// is the address printed alongside each instruction; supply 0 to make
// addresses relative to the start of the buffer.  `flags` is a bitmask
// of RE_DISASM_ANNOTATE_*.  Returns the number of instructions emitted.
size_t re_annotate_disasm_write(FILE *out, const uint8_t *bytes, size_t bytes_len, uint32_t base_addr, uint32_t flags);

#endif // GS_RE_ANNOTATE_DISASM_H
