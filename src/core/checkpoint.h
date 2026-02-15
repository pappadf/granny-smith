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

// Opens a checkpoint file for writing with the specified kind
checkpoint_t *checkpoint_open_write(const char *filename, checkpoint_kind_t kind);

// Closes a checkpoint file and frees resources
void checkpoint_close(checkpoint_t *checkpoint);

// Returns true if the checkpoint has encountered an error
bool checkpoint_has_error(checkpoint_t *checkpoint);

// Flag the checkpoint as having encountered an error
void checkpoint_set_error(checkpoint_t *checkpoint);

// Returns the kind of an open checkpoint
checkpoint_kind_t checkpoint_get_kind(checkpoint_t *checkpoint);

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

#endif // CHECKPOINT_H
