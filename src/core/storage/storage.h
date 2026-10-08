// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage.h
// Public API for the delta-file storage engine.
//
// Each disk image is backed by a base and two files:
//   base   — the original image, a read-only byte source (source.h), never
//            modified: a host file, or anything an adapter makes one
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

#include "checkpoint.h"
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Default block size in bytes.  Most disks (Mac SCSI HD, floppy data) use this;
// it is also the value every existing delta/checkpoint was written with.  A
// storage instance honours its own runtime block_size (see storage_config_t),
// so this is the default, not a universal truth.
#define STORAGE_BLOCK_SIZE 512

// Upper bound on a storage block.  Bounds the stack buffers used for journal
// preimages and block streaming so a variable block_size stays stack-safe.
// 512 (flat disks) and 532 (Lisa ProFile: 512 data + 20 inline tag) both fit.
#define STORAGE_MAX_BLOCK_SIZE 1024

// Opaque handle to a storage instance.
typedef struct storage_t storage_t;

// A byte source (source.h); storage reads its base through one.
struct peel_source;

// Configuration passed to storage_new().
typedef struct {
    // The original image, read-only: any byte source -- a host file, a view
    // past a DiskCopy header, an NDIF chunk map, a member of an archive.
    // storage_new takes its own reference.  NULL for a blank disk.
    struct peel_source *base;
    const char *delta_path; // Path to delta file (read-write, created if missing)
    const char *journal_path; // Path to preimage journal (created if missing)
    uint64_t block_count; // Number of logical blocks
    uint32_t block_size; // Bytes per block: a multiple of 4 in [512, STORAGE_MAX_BLOCK_SIZE] (512 default, 532 ProFile)
} storage_config_t;

// Callback signatures for streaming block data.
typedef int (*storage_write_callback_t)(void *context, const void *data, size_t size);

// === Exporting off the emulator thread ===
//
// An export view is a snapshot of the storage's READ side taken on the
// emulator thread -- its own handles on the base and delta files and a copy
// of the modification bitmap -- that another thread streams from with
// storage_export_view_write while the guest keeps reading the disk.  The
// storage is write-locked meanwhile: a guest write to it fails (what a
// drive being copied does), and storage_export_view_end lifts the lock
// (a storage deleted in between is recognised and skipped).
typedef struct storage_export_view storage_export_view_t;
storage_export_view_t *storage_export_view_begin(storage_t *storage);
// Any thread: streams every block (one callback per block, the checkpoint
// record shape), reporting progress and stopping on cancel (io_worker.h).
// GS_SUCCESS, GS_ERROR, or -ECANCELED.
int storage_export_view_write(storage_export_view_t *v, void *context, storage_write_callback_t write_cb);
// Emulator thread: closes the view's handles and lifts the lock.
void storage_export_view_end(storage_export_view_t *v);
// True while an export of this storage is in flight.
bool storage_export_locked(const storage_t *storage);
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

// Serializes storage metadata into a checkpoint stream, then commits (as
// storage_clear_rollback does).
// Quick checkpoints: writes the current bitmap and cluster table.
// Consolidated checkpoints: streams all block data via storage_save_state().
// A NULL checkpoint is an error; to commit without one, call
// storage_clear_rollback().
int storage_checkpoint(storage_t *storage, checkpoint_t *checkpoint);

// Restores storage state from a checkpoint stream.
// Quick checkpoints: reads bitmap, sets as current, clears journal.
// Consolidated checkpoints: loads all block data via storage_load_state().
// If storage is NULL, the serialized data is consumed and discarded.
int storage_restore_from_checkpoint(storage_t *storage, checkpoint_t *checkpoint);

// === Block I/O ===

// Reads one block (block_size bytes) at the given byte offset.  GS_ERROR
// (buffer zeroed) when the block cannot be read, from the delta or the base
// alike; a block past the end of a base shorter than the geometry reads as
// zeros.
int storage_read_block(storage_t *storage, size_t offset, void *buffer);

// Writes one block (block_size bytes) at the given byte offset.
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

// The `files` process singleton, created at shell init.  It registers the
// per-machine `files.images` collection with root_install.
void files_init(void);

#ifdef __cplusplus
}
#endif

#endif // STORAGE_H
