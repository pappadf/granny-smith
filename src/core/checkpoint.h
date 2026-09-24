// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint.h
// Checkpoint file I/O interface for save/restore state snapshots.

#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Checkpoint kind: quick (auto-save) vs consolidated (full export)
typedef enum {
    CHECKPOINT_KIND_QUICK = 0,
    CHECKPOINT_KIND_CONSOLIDATED = 1,
} checkpoint_kind_t;

// === Checkpoint Handle Management ===

// Opens a checkpoint file for reading
checkpoint_t *checkpoint_open_read(const char *filename);

// Opens a checkpoint file for writing with the specified kind, machine model ID, and RAM size
checkpoint_t *checkpoint_open_write(const char *filename, checkpoint_kind_t kind, const char *model_id,
                                    uint32_t ram_size_kb);

// Closes a checkpoint file and frees resources
void checkpoint_close(checkpoint_t *checkpoint);

// Returns true if the checkpoint has encountered an error
bool checkpoint_has_error(checkpoint_t *checkpoint);

// Flag the checkpoint as having encountered an error
void checkpoint_set_error(checkpoint_t *checkpoint);

// Returns the kind of an open checkpoint
checkpoint_kind_t checkpoint_get_kind(checkpoint_t *checkpoint);

// Returns the machine model ID stored in the checkpoint header (e.g. "plus", "se30")
const char *checkpoint_get_model_id(checkpoint_t *checkpoint);

// Returns the RAM size (in KB) stored in the checkpoint header (0 = use machine default)
uint32_t checkpoint_get_ram_size_kb(checkpoint_t *checkpoint);

// === Block I/O (with file:line metadata for diagnostics) ===

// Reads a data block from the checkpoint with size and tag validation
void system_read_checkpoint_data_loc(checkpoint_t *checkpoint, void *data, size_t size, const char *tag,
                                     const char *file, int line);

// Writes a data block to the checkpoint with size and tag header
void system_write_checkpoint_data_loc(checkpoint_t *checkpoint, const void *data, size_t size, const char *tag,
                                      const char *file, int line);

// === Block tags ===
//
// The stream is positional: subsystem N's state is whatever bytes sit between
// N-1's and N+1's, and integrity rests entirely on the save and restore
// functions visiting subsystems in the same order.  Nothing checks that they
// do.  When they diverge, what happens depends only on whether the block
// sizes happen to match: different sizes surface as a confusing mismatch
// several blocks later, pointing at an innocent bystander; **equal sizes are
// not detected at all, in either format**, and each subsystem silently
// restores the other's state.  F-20 was exactly this -- the IIfx saved
// ASC -> ADB -> floppy and restored ASC -> floppy -> ADB.
//
// So every block carries a 32-bit tag beside its size.  Pass an optional
// fourth argument naming the block, on BOTH paths:
//
//     system_write_checkpoint_data(cp, &s->regs, sizeof(s->regs), "adb");
//     system_read_checkpoint_data(cp, &s->regs, sizeof(s->regs), "adb");
//
// Tag it inside the subsystem, next to the data it names -- not at the
// machine's call site.  One edit in adb.c then protects every machine that
// saves ADB, because a machine restoring floppy where adb was saved reads the
// wrong name and fails AT the swap.
//
// The three-argument form still works everywhere and writes tag 0, meaning
// "unchecked".  A comparison passes whenever either side is 0, so a block may
// gain a tag on the write side and the read side independently without the
// stream ever desynchronising -- the field is ALWAYS present, so the layout
// never depends on whether a caller chose to name its block.
//
// A source location cannot serve as the tag, which is why the stored
// __FILE__/__LINE__ is a diagnostic and not a check: the writer and the
// reader sit at different lines by construction.
#define CP_SELECT_4(_1, _2, _3, _4, NAME, ...) NAME
#define CP_READ_TAGGED(cp, data, size, tag)                                                                            \
    system_read_checkpoint_data_loc((cp), (data), (size), (tag), __FILE__, __LINE__)
#define CP_READ_PLAIN(cp, data, size) system_read_checkpoint_data_loc((cp), (data), (size), NULL, __FILE__, __LINE__)
#define CP_WRITE_TAGGED(cp, data, size, tag)                                                                           \
    system_write_checkpoint_data_loc((cp), (data), (size), (tag), __FILE__, __LINE__)
#define CP_WRITE_PLAIN(cp, data, size) system_write_checkpoint_data_loc((cp), (data), (size), NULL, __FILE__, __LINE__)

#define system_read_checkpoint_data(...)  CP_SELECT_4(__VA_ARGS__, CP_READ_TAGGED, CP_READ_PLAIN)(__VA_ARGS__)
#define system_write_checkpoint_data(...) CP_SELECT_4(__VA_ARGS__, CP_WRITE_TAGGED, CP_WRITE_PLAIN)(__VA_ARGS__)

// === Bounded reads from an untrusted stream ===
//
// A checkpoint is a user-supplied file and the build-ID gate is not a defence
// (the ID is in the file).  Restore paths read counts and strings through
// these rather than trusting the writer, so an on-disk length cannot drive an
// allocation or a loop bound.  See 08-core-infra F-22/F-23/F-24.

// Longest path a restore may claim for an image or its delta directory.
#define CHECKPOINT_MAX_PATH 4096u

// Read a uint32 count, refusing and flagging the checkpoint when it exceeds
// `max`.  `what` names the items for the diagnostic ("images", "events").
// Returns false with *out = 0 when the value is rejected or the read failed.
bool checkpoint_read_count(checkpoint_t *checkpoint, uint32_t *out, uint32_t max, const char *what);

// Read a `uint32 length + bytes` string, bounded by `max` and ALWAYS
// NUL-terminated regardless of what the file claimed.  Returns NULL for an
// empty string and for a refused one; check checkpoint_has_error() to tell
// them apart.  Caller frees.
char *checkpoint_read_string(checkpoint_t *checkpoint, uint32_t max, const char *what);

// The writer checkpoint_read_string reads: `uint32 length + bytes`, the length
// counting the NUL.  NULL and "" both write length 0 (and read back as NULL).
void checkpoint_write_string(checkpoint_t *checkpoint, const char *s);

// === File Serialization (content or reference mode) ===

// Writes a file to the checkpoint (either embedded or as reference)
void checkpoint_write_file_loc(checkpoint_t *checkpoint, const char *path, const char *file, int line);

// Reads a file from the checkpoint into a buffer
size_t checkpoint_read_file_loc(checkpoint_t *checkpoint, uint8_t *dest, size_t capacity, char **out_path,
                                const char *file, int line);

// Convenience macros
#define checkpoint_write_file(cp, path) checkpoint_write_file_loc((cp), (path), __FILE__, __LINE__)
#define checkpoint_read_file(cp, dest, cap, out_path)                                                                  \
    checkpoint_read_file_loc((cp), (dest), (cap), (out_path), __FILE__, __LINE__)

// === File-as-reference mode control ===

// Sets whether files should be stored as references (true) or embedded (false)
void checkpoint_set_files_as_refs(bool refs);

// Returns the current file-as-reference mode setting
bool checkpoint_get_files_as_refs(void);

// Validate that a checkpoint file's build ID matches the current build.
// Opens the file, reads magic + build ID, compares with current build.
// Returns true if the build IDs match, false on mismatch or error.
bool checkpoint_validate_build_id(const char *filename);

// === Object-model class descriptor =========================================
//
// `checkpoint` is a process-singleton namespace registered at shell_init
// (alongside rom / vrom / machine). It exposes save / load / clear /
// probe / snapshot methods plus the auto_checkpoint attribute so
// callers can drive the checkpoint subsystem without going through the
// legacy `checkpoint --foo` shell-form parser.

struct class_desc;

void checkpoint_init(void);
void checkpoint_delete(void);

#endif // CHECKPOINT_H
