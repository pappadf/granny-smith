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

#include "code_segment.h"
#include "symbols.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Bit flags steering which annotation passes are emitted.
#define RE_DISASM_ANNOTATE_BRANCH   0x01u // append "; -> $addr" to PC-relative ops
#define RE_DISASM_ANNOTATE_TRAPS    0x02u // append "; trap <name>" to Axxx opcodes
#define RE_DISASM_ANNOTATE_GLOBALS  0x04u // map abs addrs < 0x800 to low-mem names
#define RE_DISASM_ANNOTATE_JTXREF   0x08u // resolve (d16,A5) into JT[i] = CODE s:offset
#define RE_DISASM_ANNOTATE_MACSBUG  0x10u // detect MacsBug name trailers after RTS
#define RE_DISASM_ANNOTATE_LABELS   0x20u // emit "name:" lines at known symbol addrs
#define RE_DISASM_ANNOTATE_AUX_SYSC 0x40u // A/UX: resolve "TRAP #0" via preceding D0=N
#define RE_DISASM_ALL                                                                                                  \
    (RE_DISASM_ANNOTATE_BRANCH | RE_DISASM_ANNOTATE_TRAPS | RE_DISASM_ANNOTATE_GLOBALS | RE_DISASM_ANNOTATE_JTXREF |   \
     RE_DISASM_ANNOTATE_MACSBUG | RE_DISASM_ANNOTATE_LABELS)
// Annotation set tuned for A/UX COFF binaries: drops the Mac-toolbox
// passes (no $Axxx traps, no jump-table xrefs, no low-mem globals) and
// adds the TRAP #0 syscall-name pass.  Labels stay on so the symbol-
// table-derived names show up at every function entry.
#define RE_DISASM_AUX (RE_DISASM_ANNOTATE_BRANCH | RE_DISASM_ANNOTATE_LABELS | RE_DISASM_ANNOTATE_AUX_SYSC)

// Context for the richer per-segment annotator.  All fields are optional;
// passing NULL for any of them skips the matching annotation pass.
typedef struct re_annotate_ctx {
    const re_jt_table_t *jt; // CODE 0 jump table (for A5-relative xrefs)
    int16_t code_id; // owning CODE resource id (for symbols and labels)
    re_symbols_t *symbols; // mutable: pass populates MacsBug + boundary syms
} re_annotate_ctx_t;

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
// of RE_DISASM_ANNOTATE_*.  `ctx` is optional — when present, the pass
// resolves JT-xrefs / globals / MacsBug-name labels using it.  Returns
// the number of instructions emitted.
size_t re_annotate_disasm_write(FILE *out, const uint8_t *bytes, size_t bytes_len, uint32_t base_addr, uint32_t flags,
                                re_annotate_ctx_t *ctx);

#endif // GS_RE_ANNOTATE_DISASM_H
