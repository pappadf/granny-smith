// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dump.h
// Standalone reverse-engineering tool for classic-Mac forked files and
// A/UX COFF binaries.  Takes resource-fork / data-fork / COFF blobs on
// the host filesystem (typically extracted from an HFS image via the
// emulator's `storage.cp` and the `/rsrc/_raw` VFS path) and produces a
// self-contained dump directory with raw resources, per-segment
// disassembly, per-type decoded JSON, a manifest and a README.
//
// Inputs are plain `uint8_t *` buffers, not VFS paths — see main() in
// dump.c for the CLI that loads the inputs from host files.

#pragma once

#ifndef DUMP_H
#define DUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Flags steering the full-file dump.  Each maps to a CLI flag.
#define DUMP_NO_DECODE 0x01u // skip per-type decoders + decoded/ output
#define DUMP_NO_DISASM 0x02u // skip CODE disassembly + symbols.txt
#define DUMP_FORCE     0x04u // overwrite even when dst_dir is non-empty

// Identify a buffer as a Mac forked file or A/UX COFF binary and print
// a one-line summary to stdout.  `data_bytes`/`data_len` are the data
// fork (may be NULL/0 for a forked file with no data fork); `rsrc_bytes`
// is the resource fork (may be NULL/0 for COFF or data-fork-only).
// Returns true on a successful identify, false if neither the COFF
// magic nor a parseable resource fork is present.
bool dump_identify(const uint8_t *data_bytes, size_t data_len, const uint8_t *rsrc_bytes, size_t rsrc_len,
                   const char *label);

// Full dump.  `data_bytes` is optional (NULL/0 means "no data fork");
// `rsrc_bytes` is the resource fork (may be NULL/0 if data_bytes is a
// COFF binary, which is auto-detected).  `finf_bytes` is the optional
// 32-byte Finder info blob.  `src_label` appears in headers/manifest
// (typically the original host path).  Returns 0 on success or a
// negated errno on failure (details on stderr).
int dump_run(const uint8_t *data_bytes, size_t data_len, const uint8_t *rsrc_bytes, size_t rsrc_len,
             const uint8_t *finf_bytes, size_t finf_len, const char *src_label, const char *dst_dir, uint32_t flags);

// Disassemble one CODE resource from `rsrc_bytes` and stream the listing
// to either `dst_file` (a host path created or truncated) or stdout when
// `dst_file` is NULL/empty.  Returns 0 on success, negative on failure.
int dump_disasm_code(const uint8_t *rsrc_bytes, size_t rsrc_len, int code_id, const char *dst_file);

// Run the per-type decoder for one resource and emit its JSON to
// `dst_file` (or stdout when NULL/empty).  Returns 0 on success, -ENOENT
// when no decoder is registered for the type, -EIO on read/parse error.
int dump_decode_one(const uint8_t *rsrc_bytes, size_t rsrc_len, const char *type_str, int id, const char *dst_file);

#endif // DUMP_H
