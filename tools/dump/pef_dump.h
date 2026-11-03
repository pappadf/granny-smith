// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pef_dump.h
// re.dump path for PEF code fragments (PowerPC applications, shared
// libraries, 'ndrv' drivers).  Called from dump_run when the data fork —
// or a fragment a 'cfrg' member points at — starts with 'Joy!peff'.
// Layout of the produced directory (dst_dir is typically <dump>/pef):
//
//   README.md          — human-readable summary and directory tour
//   manifest.json      — container header, sections, entry points,
//                        imports, exports, relocation and function counts
//   summary.txt        — the same information as plain text
//   sections/NN-<kind>.bin
//                      — each instantiated section in memory form
//                        (pattern-initialized data unpacked, zero tail)
//   relocations.txt    — every relocated word, symbolically resolved
//                        ("data+0x0040 -> import RegistryEntrySearch")
//   symbols.txt        — every label the passes named: exports, entry
//                        points, traceback-table names, glue, branch targets
//   disasm/NN-code.s   — annotated PowerPC disassembly of each code section

#pragma once

#ifndef GS_TOOLS_PEF_DUMP_H
#define GS_TOOLS_PEF_DUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Dump one PEF fragment into dst_dir.  `bytes`/`len` is the fragment;
// `src_label` names its origin in the README and manifest.  When `disasm`
// is false the disasm/ pass is skipped.  On success returns the number of
// code sections disassembled (>= 0); on failure a negative errno.
int re_pef_dump(const uint8_t *bytes, size_t len, const char *src_label, const char *dst_dir, bool disasm);

#endif // GS_TOOLS_PEF_DUMP_H
