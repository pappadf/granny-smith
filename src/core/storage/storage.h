// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage.h
// Public API for the directory-of-blocks storage engine.

#ifndef STORAGE_H
#define STORAGE_H

// === Includes ===
#include <stddef.h>
#include <stdint.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// === Constants ===

// Fixed block size in bytes. All offsets must be aligned to this value.
#define STORAGE_BLOCK_SIZE 512

// === Type Definitions ===

// Opaque handle to a storage instance.
typedef struct storage_t storage_t;

// Configuration passed to storage_new().
// The engine persists every logical block as a standalone file inside
// `path_dir`. `block_count` defines the logical size of the disk in 512-byte
// blocks. `consolidations_per_tick` controls how many range merges may run
// during each storage_tick() call (set to <= 0 to disable automatic merges).
typedef struct {
    const char *path_dir; // Directory that owns all .dat files.
    uint64_t block_count; // Number of 512-byte logical blocks.
    uint32_t block_size; // Must be 512 today; reserved for future use.
    int consolidations_per_tick; // Max merge operations per tick.
} storage_config_t;

// Callback signature used by storage_save_state().
typedef int (*storage_write_callback_t)(void *context, const void *data, size_t size);

// Callback signature used by storage_load_state().
typedef int (*storage_read_callback_t)(void *context, void *data, size_t size);

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Creates or opens a directory-backed storage instance.
int storage_new(const storage_config_t *config, storage_t **out_storage);

// Releases all memory associated with a storage instance.
int storage_delete(storage_t *storage);

// Serializes storage state into a checkpoint stream.
// When checkpoint is NULL, this behaves like a flush no-op for compatibility.
int storage_checkpoint(storage_t *storage, checkpoint_t *checkpoint);

// Restores storage state previously serialized via storage_checkpoint().
// If storage is NULL, the serialized data is consumed and discarded.
int storage_restore_from_checkpoint(storage_t *storage, checkpoint_t *checkpoint);

// === Operations ===

// Reads one 512-byte block at the provided byte offset.
int storage_read_block(storage_t *storage, size_t offset, void *buffer);

// Writes one 512-byte block at the provided byte offset.
int storage_write_block(storage_t *storage, size_t offset, const void *buffer);

// Drives incremental maintenance tasks like consolidation.
int storage_tick(storage_t *storage);

// Replays pending rollback preimages so the on-disk data matches the
// last committed checkpoint.
int storage_apply_rollback(storage_t *storage);

// Deletes all rollback preimages and treats current data as committed.
int storage_clear_rollback(storage_t *storage);

// Streams the entire logical disk as a flat sequence of blocks.
int storage_save_state(storage_t *storage, void *context, storage_write_callback_t write_cb);

// Replaces all on-disk data with the blocks provided by read_cb().
int storage_load_state(storage_t *storage, void *context, storage_read_callback_t read_cb);

#ifdef __cplusplus
}
#endif

#endif // STORAGE_H
