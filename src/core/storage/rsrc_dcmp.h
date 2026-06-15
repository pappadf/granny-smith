// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rsrc_dcmp.h
// Decompression for Apple System 7 compressed resources (the format
// signalled by the 0xA89F6572 signature in the resource bytes).
//
// Supports the two stock Apple decompressors that ship with System 7
// (and the two header layouts they pair with):
//   - header version 8 + dcmp 0 — DonnBits (Donn Denman, 1990-1991)
//   - header version 9 + dcmp 2 — GreggyBits (Greg Marriott, 1990-1991)
//
// Algorithms ported from Apple's open-sourced assembly in
//   Patches/DeCompressDefProc.a + Patches/DeCompressCommon.A  (dcmp 0)
//   Patches/GreggyBitsDefProc.a                                (dcmp 2)
// (© 1990-1991 Apple, released as part of the Mac OS / System 7 source
// drops).  Constant-word emit tables, opcode dispatch, var-table
// mechanics, and the 256-word GreggyBits static table are reproduced
// verbatim; the assembly's `Bsr` / `Bra` flow is rewritten as a plain
// C switch.
//
// Other dcmp ids (1, 3, 7, 8, custom dcmps shipped in extensions) are
// not implemented and produce an "unsupported dcmp" error so callers
// can fall through to the raw bytes rather than feed garbage to a
// disassembler.

#pragma once

#ifndef GS_RSRC_DCMP_H
#define GS_RSRC_DCMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Magic value at offset 0 of every System-7-compressed resource.
// `0xA89F` is the 68k `_Unimplemented` trap — chosen as the sentinel
// because real code never starts with it.  The trailing `0x6572` is
// the literal ASCII pair 'er' (probably "extended resource", not
// formally documented anywhere).
#define RSRC_DCMP_MAGIC 0xA89F6572u

// Quick test: returns true iff `bytes` begins with the compressed-
// resource signature.  Cheap (4-byte comparison); safe to call on
// arbitrarily short buffers.
bool rsrc_dcmp_is_compressed(const uint8_t *bytes, size_t len);

// Decompress `compressed_len` bytes from `compressed` (must begin with
// the dcmp magic).  On success returns a freshly malloc'd buffer of
// exactly `*out_len` bytes (which equals the `actualSize` field from
// the header).  On failure returns NULL and sets `*errmsg` to a static
// string.  Caller free()s the returned buffer.
//
// Failure modes (errmsg values):
//   - "not compressed"          — magic doesn't match
//   - "truncated header"        — header_length goes past the buffer
//   - "unsupported version"     — only header version 8 is implemented
//   - "unsupported dcmp"        — dcmp_id != 0 (no GreggyBits yet)
//   - "corrupt stream"          — opcode walk fell off the input
//   - "out of memory"
uint8_t *rsrc_dcmp_decompress(const uint8_t *compressed, size_t compressed_len, size_t *out_len, const char **errmsg);

#endif // GS_RSRC_DCMP_H
