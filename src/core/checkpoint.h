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

// Reads a data block from the checkpoint with size validation
void system_read_checkpoint_data_loc(checkpoint_t *checkpoint, void *data, size_t size, const char *file, int line);

// Writes a data block to the checkpoint with size header
void system_write_checkpoint_data_loc(checkpoint_t *checkpoint, const void *data, size_t size, const char *file,
                                      int line);

// Convenience macros that inject __FILE__ and __LINE__ automatically
#define system_read_checkpoint_data(cp, data, size)                                                                    \
    system_read_checkpoint_data_loc((cp), (data), (size), __FILE__, __LINE__)
#define system_write_checkpoint_data(cp, data, size)                                                                   \
    system_write_checkpoint_data_loc((cp), (data), (size), __FILE__, __LINE__)

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
extern const struct class_desc checkpoint_class;

void checkpoint_init(void);
void checkpoint_delete(void);

#endif // CHECKPOINT_H
