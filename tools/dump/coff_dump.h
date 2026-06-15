// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// coff_dump.h
// re.dump path for A/UX COFF executables.  Called from re_dump_with_flags
// when the input file begins with the 0x0150 COFF magic.  Layout of the
// produced dump directory:
//
//   data.bin            — the entire COFF file, verbatim (input round-trip)
//   manifest.json       — schema_version, source identity, sections,
//                          symbol summary, A/UX entry-point info
//   README.md           — human-readable TOC (analogous to the Mac dump)
//   sections/<name>     — raw bytes of each section (one file per section,
//                          .bss-style "no raw data" sections show as a 0-
//                          byte placeholder)
//   symbols.txt         — every defined symbol from the COFF symbol table
//                          (vaddr, section, storage class, name)
//   disasm/<name>.s     — annotated 68k disassembly of every executable
//                          (STYP_TEXT) section

#pragma once

#ifndef GS_RE_COFF_DUMP_H
#define GS_RE_COFF_DUMP_H

#include <stddef.h>
#include <stdint.h>

// Dump a COFF binary into dst_dir.  `bytes`/`len` is the file content
// (already read into memory by the caller).  `vfs_path` is the source
// path string used in manifest.json + README.  Returns 0 on success,
// negative errno on failure.
int re_coff_dump(const uint8_t *bytes, size_t len, const char *vfs_path, const char *dst_dir);

#endif // GS_RE_COFF_DUMP_H
