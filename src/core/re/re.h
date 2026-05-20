// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// re.h
// Reverse-engineering orchestrator for classic-Mac 68k applications.
// `re` is a process-singleton root object that *consumes* the
// resource-fork-as-VFS-paths convention (see resource_fork.{c,h} and
// image_vfs.c) to produce self-contained, disassembled, decoded dumps.
// Stateless path-based methods, attached at shell_init alongside
// archive / vfs / storage.

#pragma once

#ifndef GS_RE_H
#define GS_RE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct class_desc;

// === Object-model class descriptor =========================================

extern const struct class_desc re_class;

void re_init(void);
void re_delete(void);

// === C-level entry points ==================================================
//
// The object-model methods are thin wrappers around these.  Tooling that
// already lives in C (integration tests, future inspector backends) can
// call them directly to skip the value_t round-trip.

// Print a one-line identify summary to stdout.  Returns true if the path
// resolves to a forked file we recognise, false otherwise.
bool re_identify(const char *vfs_path);

// Dump the forked file at `vfs_path` into `dst_dir`.  Creates dst_dir if
// missing; writes `data.bin`, `finder.json`, a `resources/<TYPE>/<id>` +
// matching `.info` sidecar for every resource, and (when CODE 0 +
// non-empty CODE N segments are present) per-segment disassembly under
// `disasm/CODE-XXXX.s` plus `disasm/jump-table.s`.  Returns 0 on success
// or a negative errno-style code on failure (with details on stderr).
int re_dump(const char *vfs_path, const char *dst_dir);

// Disassemble one CODE resource and stream the listing to either
// `dst_file` (a host path that is created or truncated) or stdout when
// `dst_file` is NULL/empty.  Returns 0 on success, negative on failure.
int re_disasm_code(const char *vfs_path, int code_id, const char *dst_file);

// Run the per-type decoder for one resource and emit its JSON to
// `dst_file` (or stdout when NULL/empty).  Returns 0 on success, -ENOENT
// when no decoder is registered for the type, -EIO on read/parse error.
int re_decode_one(const char *vfs_path, const char *type_str, int id, const char *dst_file);

// Flags steering re_dump's behaviour.  Each maps to a `--no-X` rest-arg
// at the shell layer.
#define RE_DUMP_NO_DECODE 0x01u // skip per-type decoders + decoded/ output
#define RE_DUMP_NO_DISASM 0x02u // skip CODE disassembly + symbols.txt
#define RE_DUMP_FORCE     0x04u // overwrite even when dst_dir is non-empty

// Like re_dump but with a flag mask.  Equivalent to re_dump when flags = 0.
int re_dump_with_flags(const char *vfs_path, const char *dst_dir, uint32_t flags);

// Read a whole VFS path into a freshly malloc'd buffer.  Returns NULL on
// failure; on success `*out_len` is the byte count.  Caller frees with
// free().  Exposed here because both re_identify and re_dump use it and
// later PRs (re_disasm_code) will too.
uint8_t *re_read_vfs_file(const char *vfs_path, size_t *out_len);

#endif // GS_RE_H
