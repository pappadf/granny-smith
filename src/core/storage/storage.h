// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage.h
// Public API for the delta-file storage engine.
//
// Each disk image is backed by three files:
//   base   — the original image, opened read-only, never modified
//   delta  — header (magic + bitmaps) + block data area for modified blocks
//   journal — append-only preimage log for crash recovery
//
// Reads check a bitmap: bit set → read from delta, bit clear → read from base.
// Writes go to the delta; the bitmap is updated in memory and flushed at
// checkpoint time.  A preimage journal captures old data before overwriting
// committed blocks, enabling crash recovery without a full sync step.

#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fixed block size in bytes.  All offsets must be aligned to this value.
#define STORAGE_BLOCK_SIZE 512

// Opaque handle to a storage instance.
typedef struct storage_t storage_t;

// Configuration passed to storage_new().
typedef struct {
    const char *base_path; // Path to original image (read-only)
    const char *delta_path; // Path to delta file (read-write, created if missing)
    const char *journal_path; // Path to preimage journal (created if missing)
    uint64_t block_count; // Number of 512-byte logical blocks
    uint32_t block_size; // Must be 512
    size_t base_data_offset; // Byte offset to data in base file (e.g. DiskCopy header)
} storage_config_t;

// Callback signatures for streaming block data.
typedef int (*storage_write_callback_t)(void *context, const void *data, size_t size);
typedef int (*storage_read_callback_t)(void *context, void *data, size_t size);

// === Lifecycle ===

// Creates or opens a delta-file storage instance.
// If the delta file exists, reads its header and bitmaps.
// If the journal is non-empty, it is loaded (but NOT replayed automatically —
// call storage_apply_rollback() to replay before normal use if no checkpoint
// will be loaded).
int storage_new(const storage_config_t *config, storage_t **out_storage);

// Releases all resources (closes file handles, frees memory).
int storage_delete(storage_t *storage);

// === Checkpointing ===

// Serializes storage metadata into a checkpoint stream.
// Quick checkpoints: writes the current bitmap.
// Consolidated checkpoints: streams all block data via storage_save_state().
// When checkpoint is NULL, behaves as a flush (clears rollback).
int storage_checkpoint(storage_t *storage, checkpoint_t *checkpoint);

// Restores storage state from a checkpoint stream.
// Quick checkpoints: reads bitmap, sets as current, clears journal.
// Consolidated checkpoints: loads all block data via storage_load_state().
// If storage is NULL, the serialized data is consumed and discarded.
int storage_restore_from_checkpoint(storage_t *storage, checkpoint_t *checkpoint);

// === Block I/O ===

// Reads one 512-byte block at the given byte offset.
int storage_read_block(storage_t *storage, size_t offset, void *buffer);

// Writes one 512-byte block at the given byte offset.
int storage_write_block(storage_t *storage, size_t offset, const void *buffer);

// === Rollback ===

// Replays the preimage journal: restores committed blocks in the delta,
// sets current bitmap = committed bitmap, truncates journal.
int storage_apply_rollback(storage_t *storage);

// Marks current state as committed: copies current bitmap to committed,
// flushes both bitmaps to delta header, truncates journal.
int storage_clear_rollback(storage_t *storage);

// === Streaming (consolidated checkpoints / export) ===

// Streams the entire logical disk (block_count blocks) to write_cb.
int storage_save_state(storage_t *storage, void *context, storage_write_callback_t write_cb);

// Replaces all storage data from read_cb, sets all bitmap bits, commits.
int storage_load_state(storage_t *storage, void *context, storage_read_callback_t read_cb);

// === Maintenance ===

// No-op (consolidation is not needed with the delta model).
int storage_tick(storage_t *storage);

// Object-model lifecycle hooks for storage.images indexed children.
// Called by gs_classes_install / gs_classes_uninstall.
struct config;
void storage_object_classes_init(struct config *cfg);
void storage_object_classes_teardown(void);

#ifdef __cplusplus
}
#endif

#endif // STORAGE_H
