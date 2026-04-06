// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage.c
// Delta-file storage engine implementation.
//
// Layout of the delta file:
//   [0..23]                      Fixed header (magic, version, block_count, block_size)
//   [24 .. 24+bm-1]             Current bitmap (1 bit per block)
//   [24+bm .. 24+2*bm-1]        Committed bitmap
//   [24+2*bm .. EOF]             Block data area (block_count * 512 bytes, sparse)
//
// Journal format (append-only):
//   Each entry: [uint32_t LBA][512 bytes data] = 516 bytes per entry.

#include "storage.h"

#include "log.h"
#include "system.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// Constants
// ============================================================================

#define DELTA_MAGIC        "GSDL"
#define DELTA_MAGIC_SIZE   4
#define DELTA_VERSION      1
#define DELTA_HEADER_SIZE  24 // magic(4) + version(4) + block_count(8) + block_size(4) + reserved(4)
#define JOURNAL_ENTRY_SIZE (4 + STORAGE_BLOCK_SIZE) // LBA(4) + data(512) = 516

#define STORAGE_SNAPSHOT_VERSION 2 // Bumped from v1 (directory-of-blocks)

// ============================================================================
// Internal types
// ============================================================================

// Snapshot header written to checkpoint stream
typedef struct {
    uint32_t version;
    uint8_t has_data; // 1 = consolidated (all blocks), 0 = quick (bitmap only)
    uint8_t reserved[3];
    uint64_t block_count;
    uint32_t block_size;
} storage_snapshot_header_t;

// Context for checkpoint streaming callbacks
typedef struct {
    checkpoint_t *checkpoint;
} checkpoint_stream_ctx_t;

// The storage instance
struct storage_t {
    FILE *base_fp; // Original image, read-only, kept open
    FILE *delta_fp; // Delta file, read-write, kept open
    FILE *journal_fp; // Preimage journal, append+read, kept open

    uint8_t *bitmap; // Current modification bitmap (in memory)
    uint8_t *committed_bitmap; // Bitmap at last successful checkpoint

    uint64_t block_count;
    uint32_t block_size; // Always 512
    size_t bitmap_bytes; // ceil(block_count / 8)

    size_t base_data_offset; // Byte offset to data in base file
    size_t bitmap_offset; // Byte offset to bitmaps in delta (= DELTA_HEADER_SIZE)
    size_t data_offset; // Byte offset to block data in delta

    uint32_t *journal_lbas; // In-memory index of captured LBAs
    size_t journal_count;
    size_t journal_capacity;

    bool bitmap_dirty; // True if bitmap changed since last flush
};

// ============================================================================
// Bitmap helpers
// ============================================================================

static inline bool bitmap_test(const uint8_t *bm, uint32_t bit) {
    return (bm[bit >> 3] & (1u << (bit & 7))) != 0;
}

static inline void bitmap_set(uint8_t *bm, uint32_t bit) {
    bm[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

// ============================================================================
// Journal helpers
// ============================================================================

static bool journal_has_lba(const storage_t *s, uint32_t lba) {
    for (size_t i = 0; i < s->journal_count; i++) {
        if (s->journal_lbas[i] == lba)
            return true;
    }
    return false;
}

static int journal_index_add(storage_t *s, uint32_t lba) {
    if (s->journal_count >= s->journal_capacity) {
        size_t new_cap = s->journal_capacity ? s->journal_capacity * 2 : 64;
        uint32_t *tmp = realloc(s->journal_lbas, new_cap * sizeof(uint32_t));
        if (!tmp)
            return GS_ERROR;
        s->journal_lbas = tmp;
        s->journal_capacity = new_cap;
    }
    s->journal_lbas[s->journal_count++] = lba;
    return GS_SUCCESS;
}

// Append a preimage entry to the journal file and index.
static int journal_append(storage_t *s, uint32_t lba, const uint8_t *data) {
    // Write LBA (little-endian uint32_t)
    if (fwrite(&lba, sizeof(lba), 1, s->journal_fp) != 1)
        return GS_ERROR;
    // Write block data
    if (fwrite(data, STORAGE_BLOCK_SIZE, 1, s->journal_fp) != 1)
        return GS_ERROR;
    fflush(s->journal_fp);

    return journal_index_add(s, lba);
}

// Scan the journal file and rebuild the in-memory index.
static int journal_load_index(storage_t *s) {
    s->journal_count = 0;

    if (!s->journal_fp)
        return GS_SUCCESS;

    fseek(s->journal_fp, 0, SEEK_END);
    long size = ftell(s->journal_fp);
    if (size <= 0)
        return GS_SUCCESS;

    fseek(s->journal_fp, 0, SEEK_SET);
    size_t entries = (size_t)size / JOURNAL_ENTRY_SIZE;

    for (size_t i = 0; i < entries; i++) {
        uint32_t lba;
        if (fread(&lba, sizeof(lba), 1, s->journal_fp) != 1)
            break;
        // Skip block data
        if (fseek(s->journal_fp, STORAGE_BLOCK_SIZE, SEEK_CUR) != 0)
            break;
        journal_index_add(s, lba);
    }

    // Position at end for appending
    fseek(s->journal_fp, 0, SEEK_END);
    return GS_SUCCESS;
}

// ============================================================================
// Delta file I/O helpers
// ============================================================================

// Write the delta file header (called on creation).
static int delta_write_header(storage_t *s) {
    fseek(s->delta_fp, 0, SEEK_SET);

    // Magic
    if (fwrite(DELTA_MAGIC, DELTA_MAGIC_SIZE, 1, s->delta_fp) != 1)
        return GS_ERROR;
    // Version
    uint32_t version = DELTA_VERSION;
    if (fwrite(&version, sizeof(version), 1, s->delta_fp) != 1)
        return GS_ERROR;
    // Block count
    if (fwrite(&s->block_count, sizeof(s->block_count), 1, s->delta_fp) != 1)
        return GS_ERROR;
    // Block size
    if (fwrite(&s->block_size, sizeof(s->block_size), 1, s->delta_fp) != 1)
        return GS_ERROR;
    // Reserved
    uint32_t reserved = 0;
    if (fwrite(&reserved, sizeof(reserved), 1, s->delta_fp) != 1)
        return GS_ERROR;

    return GS_SUCCESS;
}

// Read and validate the delta file header.
static int delta_read_header(storage_t *s) {
    fseek(s->delta_fp, 0, SEEK_SET);

    char magic[DELTA_MAGIC_SIZE];
    if (fread(magic, DELTA_MAGIC_SIZE, 1, s->delta_fp) != 1)
        return GS_ERROR;
    if (memcmp(magic, DELTA_MAGIC, DELTA_MAGIC_SIZE) != 0)
        return GS_ERROR;

    uint32_t version;
    if (fread(&version, sizeof(version), 1, s->delta_fp) != 1)
        return GS_ERROR;
    if (version != DELTA_VERSION)
        return GS_ERROR;

    uint64_t block_count;
    if (fread(&block_count, sizeof(block_count), 1, s->delta_fp) != 1)
        return GS_ERROR;
    if (block_count != s->block_count)
        return GS_ERROR;

    uint32_t block_size;
    if (fread(&block_size, sizeof(block_size), 1, s->delta_fp) != 1)
        return GS_ERROR;
    if (block_size != s->block_size)
        return GS_ERROR;

    // Skip reserved
    fseek(s->delta_fp, 4, SEEK_CUR);

    return GS_SUCCESS;
}

// Flush both bitmaps to the delta file header.
static int delta_flush_bitmaps(storage_t *s) {
    fseek(s->delta_fp, (long)s->bitmap_offset, SEEK_SET);
    if (fwrite(s->bitmap, s->bitmap_bytes, 1, s->delta_fp) != 1)
        return GS_ERROR;
    if (fwrite(s->committed_bitmap, s->bitmap_bytes, 1, s->delta_fp) != 1)
        return GS_ERROR;
    fflush(s->delta_fp);
    return GS_SUCCESS;
}

// Read both bitmaps from the delta file header.
static int delta_read_bitmaps(storage_t *s) {
    fseek(s->delta_fp, (long)s->bitmap_offset, SEEK_SET);
    if (fread(s->bitmap, s->bitmap_bytes, 1, s->delta_fp) != 1)
        return GS_ERROR;
    if (fread(s->committed_bitmap, s->bitmap_bytes, 1, s->delta_fp) != 1)
        return GS_ERROR;
    return GS_SUCCESS;
}

// ============================================================================
// Checkpoint stream callbacks
// ============================================================================

static int checkpoint_storage_write_cb(void *ctx, const void *data, size_t size) {
    checkpoint_stream_ctx_t *c = (checkpoint_stream_ctx_t *)ctx;
    system_write_checkpoint_data(c->checkpoint, data, size);
    return checkpoint_has_error(c->checkpoint) ? -1 : 0;
}

static int checkpoint_storage_read_cb(void *ctx, void *data, size_t size) {
    checkpoint_stream_ctx_t *c = (checkpoint_stream_ctx_t *)ctx;
    system_read_checkpoint_data(c->checkpoint, data, size);
    return checkpoint_has_error(c->checkpoint) ? 0 : (int)size;
}

// ============================================================================
// Public API: Lifecycle
// ============================================================================

int storage_new(const storage_config_t *config, storage_t **out_storage) {
    if (!config || !out_storage || !config->delta_path || !config->journal_path)
        return GS_ERROR;
    if (config->block_size != STORAGE_BLOCK_SIZE)
        return GS_ERROR;
    if (config->block_count == 0)
        return GS_ERROR;

    storage_t *s = calloc(1, sizeof(storage_t));
    if (!s)
        return GS_ERROR;

    s->block_count = config->block_count;
    s->block_size = config->block_size;
    s->bitmap_bytes = (size_t)((config->block_count + 7) / 8);
    s->bitmap_offset = DELTA_HEADER_SIZE;
    s->data_offset = DELTA_HEADER_SIZE + 2 * s->bitmap_bytes;
    s->base_data_offset = config->base_data_offset;

    // Allocate bitmaps
    s->bitmap = calloc(1, s->bitmap_bytes);
    s->committed_bitmap = calloc(1, s->bitmap_bytes);
    if (!s->bitmap || !s->committed_bitmap)
        goto fail;

    // Open base file (read-only, optional — NULL for brand-new images)
    if (config->base_path) {
        s->base_fp = fopen(config->base_path, "rb");
        // base_fp can be NULL if base doesn't exist yet (new image)
    }

    // Open or create delta file
    bool delta_exists = (access(config->delta_path, F_OK) == 0);
    s->delta_fp = fopen(config->delta_path, delta_exists ? "r+b" : "w+b");
    if (!s->delta_fp)
        goto fail;

    if (delta_exists) {
        // Read and validate existing header + bitmaps
        if (delta_read_header(s) != GS_SUCCESS)
            goto fail;
        if (delta_read_bitmaps(s) != GS_SUCCESS)
            goto fail;
    } else {
        // Write fresh header + empty bitmaps
        if (delta_write_header(s) != GS_SUCCESS)
            goto fail;
        if (delta_flush_bitmaps(s) != GS_SUCCESS)
            goto fail;
    }

    // Open or create journal
    s->journal_fp = fopen(config->journal_path, "a+b");
    if (!s->journal_fp)
        goto fail;

    // Load journal index (does not replay — caller decides)
    journal_load_index(s);

    *out_storage = s;
    return GS_SUCCESS;

fail:
    storage_delete(s);
    *out_storage = NULL;
    return GS_ERROR;
}

int storage_delete(storage_t *storage) {
    if (!storage)
        return GS_SUCCESS;
    if (storage->base_fp)
        fclose(storage->base_fp);
    if (storage->delta_fp)
        fclose(storage->delta_fp);
    if (storage->journal_fp)
        fclose(storage->journal_fp);
    free(storage->bitmap);
    free(storage->committed_bitmap);
    free(storage->journal_lbas);
    free(storage);
    return GS_SUCCESS;
}

// ============================================================================
// Public API: Block I/O
// ============================================================================

int storage_read_block(storage_t *storage, size_t offset, void *buffer) {
    if (!storage || !buffer)
        return GS_ERROR;
    if (offset % storage->block_size != 0)
        return GS_ERROR;

    uint64_t lba64 = offset / storage->block_size;
    if (lba64 >= storage->block_count)
        return GS_ERROR;
    uint32_t lba = (uint32_t)lba64;

    if (bitmap_test(storage->bitmap, lba)) {
        // Modified block — read from delta
        fseek(storage->delta_fp, (long)(storage->data_offset + (size_t)lba * storage->block_size), SEEK_SET);
        if (fread(buffer, storage->block_size, 1, storage->delta_fp) != 1) {
            memset(buffer, 0, storage->block_size);
            return GS_ERROR;
        }
    } else if (storage->base_fp) {
        // Unmodified block — read from base image
        fseek(storage->base_fp, (long)(storage->base_data_offset + (size_t)lba * storage->block_size), SEEK_SET);
        if (fread(buffer, storage->block_size, 1, storage->base_fp) != 1) {
            memset(buffer, 0, storage->block_size);
        }
    } else {
        // No base file — unwritten block is zeros
        memset(buffer, 0, storage->block_size);
    }

    return GS_SUCCESS;
}

int storage_write_block(storage_t *storage, size_t offset, const void *buffer) {
    if (!storage || !buffer)
        return GS_ERROR;
    if (offset % storage->block_size != 0)
        return GS_ERROR;

    uint64_t lba64 = offset / storage->block_size;
    if (lba64 >= storage->block_count)
        return GS_ERROR;
    uint32_t lba = (uint32_t)lba64;

    // Capture preimage if this block was committed and not yet journaled
    if (bitmap_test(storage->committed_bitmap, lba) && !journal_has_lba(storage, lba)) {
        uint8_t old[STORAGE_BLOCK_SIZE];
        fseek(storage->delta_fp, (long)(storage->data_offset + (size_t)lba * storage->block_size), SEEK_SET);
        if (fread(old, storage->block_size, 1, storage->delta_fp) != 1) {
            return GS_ERROR;
        }
        if (journal_append(storage, lba, old) != GS_SUCCESS)
            return GS_ERROR;
    }

    // Write new data to delta
    fseek(storage->delta_fp, (long)(storage->data_offset + (size_t)lba * storage->block_size), SEEK_SET);
    if (fwrite(buffer, storage->block_size, 1, storage->delta_fp) != 1)
        return GS_ERROR;

    // Update bitmap in memory (flushed to disk at checkpoint time)
    bitmap_set(storage->bitmap, lba);
    storage->bitmap_dirty = true;

    return GS_SUCCESS;
}

// ============================================================================
// Public API: Rollback
// ============================================================================

int storage_apply_rollback(storage_t *storage) {
    if (!storage)
        return GS_ERROR;
    if (storage->journal_count == 0)
        return GS_SUCCESS;

    // Replay journal: restore preimages to delta
    fseek(storage->journal_fp, 0, SEEK_SET);
    for (size_t i = 0; i < storage->journal_count; i++) {
        uint32_t lba;
        uint8_t data[STORAGE_BLOCK_SIZE];

        if (fread(&lba, sizeof(lba), 1, storage->journal_fp) != 1)
            return GS_ERROR;
        if (fread(data, STORAGE_BLOCK_SIZE, 1, storage->journal_fp) != 1)
            return GS_ERROR;

        // Write preimage back to delta
        fseek(storage->delta_fp, (long)(storage->data_offset + (size_t)lba * storage->block_size), SEEK_SET);
        if (fwrite(data, storage->block_size, 1, storage->delta_fp) != 1)
            return GS_ERROR;
    }

    // Restore bitmap to committed state
    memcpy(storage->bitmap, storage->committed_bitmap, storage->bitmap_bytes);

    // Flush bitmaps and truncate journal
    if (delta_flush_bitmaps(storage) != GS_SUCCESS)
        return GS_ERROR;

    // Truncate journal
    if (storage->journal_fp) {
        fclose(storage->journal_fp);
        // Reopen with "w+b" to truncate, then reopen as "a+b"
        // (ftruncate may not be available on all platforms)
        storage->journal_fp = NULL;
    }
    storage->journal_count = 0;

    return GS_SUCCESS;
}

int storage_clear_rollback(storage_t *storage) {
    if (!storage)
        return GS_ERROR;

    // Update in-memory committed bitmap
    memcpy(storage->committed_bitmap, storage->bitmap, storage->bitmap_bytes);

    // Only do OPFS I/O if something changed since last commit.
    // This makes back-to-back checkpoints with no intervening writes free.
    if (storage->bitmap_dirty || storage->journal_count > 0) {
        delta_flush_bitmaps(storage);
        storage->bitmap_dirty = false;

        if (storage->journal_count > 0 && storage->journal_fp) {
            int fd = fileno(storage->journal_fp);
            if (fd >= 0)
                ftruncate(fd, 0);
            fseek(storage->journal_fp, 0, SEEK_SET);
            storage->journal_count = 0;
        }
    }

    return GS_SUCCESS;
}

// ============================================================================
// Public API: Checkpointing
// ============================================================================

int storage_checkpoint(storage_t *storage, checkpoint_t *checkpoint) {
    if (!storage)
        return GS_ERROR;

    // NULL checkpoint = just clear rollback
    if (!checkpoint)
        return storage_clear_rollback(storage);

    // Write snapshot header
    storage_snapshot_header_t header = {0};
    header.version = STORAGE_SNAPSHOT_VERSION;
    header.has_data = (checkpoint_get_kind(checkpoint) == CHECKPOINT_KIND_CONSOLIDATED) ? 1 : 0;
    header.block_count = storage->block_count;
    header.block_size = storage->block_size;
    system_write_checkpoint_data(checkpoint, &header, sizeof(header));
    if (checkpoint_has_error(checkpoint))
        return GS_ERROR;

    if (header.has_data) {
        // Consolidated: stream all blocks
        checkpoint_stream_ctx_t ctx = {checkpoint};
        int rc = storage_save_state(storage, &ctx, checkpoint_storage_write_cb);
        if (rc != GS_SUCCESS)
            return rc;
    } else {
        // Quick: write current bitmap
        system_write_checkpoint_data(checkpoint, storage->bitmap, storage->bitmap_bytes);
        if (checkpoint_has_error(checkpoint))
            return GS_ERROR;
    }

    return storage_clear_rollback(storage);
}

// Helper: skip/discard snapshot data from a checkpoint stream
static int storage_skip_snapshot(checkpoint_t *checkpoint, const storage_snapshot_header_t *header) {
    if (header->has_data) {
        // Skip all block data
        uint8_t discard[STORAGE_BLOCK_SIZE];
        for (uint64_t i = 0; i < header->block_count; i++) {
            system_read_checkpoint_data(checkpoint, discard, header->block_size);
            if (checkpoint_has_error(checkpoint))
                return GS_ERROR;
        }
    } else {
        // Skip bitmap
        size_t bm = (size_t)((header->block_count + 7) / 8);
        uint8_t *discard = malloc(bm);
        if (!discard)
            return GS_ERROR;
        system_read_checkpoint_data(checkpoint, discard, bm);
        free(discard);
        if (checkpoint_has_error(checkpoint))
            return GS_ERROR;
    }
    return GS_SUCCESS;
}

int storage_restore_from_checkpoint(storage_t *storage, checkpoint_t *checkpoint) {
    if (!checkpoint)
        return GS_ERROR;

    // Read snapshot header
    storage_snapshot_header_t header = {0};
    system_read_checkpoint_data(checkpoint, &header, sizeof(header));
    if (checkpoint_has_error(checkpoint))
        return GS_ERROR;
    if (header.version != STORAGE_SNAPSHOT_VERSION)
        return GS_ERROR;

    // NULL storage — consume and discard
    if (!storage)
        return storage_skip_snapshot(checkpoint, &header);

    // Validate geometry
    if (header.block_count != storage->block_count || header.block_size != storage->block_size)
        return GS_ERROR;

    if (header.has_data) {
        // Consolidated: load all blocks
        checkpoint_stream_ctx_t ctx = {checkpoint};
        return storage_load_state(storage, &ctx, checkpoint_storage_read_cb);
    }

    // Quick: read bitmap, set as current, clear journal
    system_read_checkpoint_data(checkpoint, storage->bitmap, storage->bitmap_bytes);
    if (checkpoint_has_error(checkpoint))
        return GS_ERROR;

    // Commit immediately (delta data is correct — OPFS auto-persisted writes)
    memcpy(storage->committed_bitmap, storage->bitmap, storage->bitmap_bytes);
    delta_flush_bitmaps(storage);

    // Truncate journal (no longer needed — we trust the delta data)
    if (storage->journal_fp) {
        int fd = fileno(storage->journal_fp);
        if (fd >= 0)
            ftruncate(fd, 0);
        fseek(storage->journal_fp, 0, SEEK_SET);
    }
    storage->journal_count = 0;

    return GS_SUCCESS;
}

// ============================================================================
// Public API: Streaming
// ============================================================================

int storage_save_state(storage_t *storage, void *context, storage_write_callback_t write_cb) {
    if (!storage || !context || !write_cb)
        return GS_ERROR;

    uint8_t buffer[STORAGE_BLOCK_SIZE];
    for (uint64_t block = 0; block < storage->block_count; block++) {
        size_t offset = (size_t)block * storage->block_size;
        int rc = storage_read_block(storage, offset, buffer);
        if (rc != GS_SUCCESS)
            return rc;
        if (write_cb(context, buffer, storage->block_size) != 0)
            return GS_ERROR;
    }
    return GS_SUCCESS;
}

static int read_exact(storage_read_callback_t read_cb, void *context, void *buf, size_t size) {
    int got = read_cb(context, buf, size);
    return (got == (int)size) ? GS_SUCCESS : GS_ERROR;
}

int storage_load_state(storage_t *storage, void *context, storage_read_callback_t read_cb) {
    if (!storage || !context || !read_cb)
        return GS_ERROR;

    uint8_t buffer[STORAGE_BLOCK_SIZE];
    for (uint64_t block = 0; block < storage->block_count; block++) {
        if (read_exact(read_cb, context, buffer, storage->block_size) != GS_SUCCESS)
            return GS_ERROR;

        // Write to delta
        size_t off = storage->data_offset + (size_t)block * storage->block_size;
        fseek(storage->delta_fp, (long)off, SEEK_SET);
        if (fwrite(buffer, storage->block_size, 1, storage->delta_fp) != 1)
            return GS_ERROR;

        bitmap_set(storage->bitmap, (uint32_t)block);
    }

    // Commit: bitmaps → delta, clear journal
    memcpy(storage->committed_bitmap, storage->bitmap, storage->bitmap_bytes);
    delta_flush_bitmaps(storage);

    if (storage->journal_fp) {
        int fd = fileno(storage->journal_fp);
        if (fd >= 0)
            ftruncate(fd, 0);
        fseek(storage->journal_fp, 0, SEEK_SET);
    }
    storage->journal_count = 0;

    return GS_SUCCESS;
}

// ============================================================================
// Public API: Maintenance
// ============================================================================

int storage_tick(storage_t *storage) {
    (void)storage;
    return GS_SUCCESS;
}
