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

#include "image.h"
#include "image_apm.h"
#include "log.h"
#include "object.h"
#include "shell.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "vfs.h"

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
        int fd = fileno(storage->journal_fp);
        if (fd >= 0)
            ftruncate(fd, 0);
        fseek(storage->journal_fp, 0, SEEK_SET);
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

    // Quick checkpoint: the delta may have been modified AFTER the checkpoint
    // was saved (the emulator kept running).  The journal has preimages for
    // those post-checkpoint overwrites.  Replay the journal first to restore
    // the delta to its committed (= checkpoint-time) state before applying
    // the checkpoint's bitmap.
    if (storage->journal_count > 0)
        storage_apply_rollback(storage);

    // Now read the checkpoint bitmap and set it as current
    system_read_checkpoint_data(checkpoint, storage->bitmap, storage->bitmap_bytes);
    if (checkpoint_has_error(checkpoint))
        return GS_ERROR;

    // Commit: the delta data now matches this bitmap
    memcpy(storage->committed_bitmap, storage->bitmap, storage->bitmap_bytes);
    delta_flush_bitmaps(storage);

    // Truncate journal
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

// === Object-model class descriptors =========================================
//
// Replaces the M2 `storage` stub with a real class. `storage.images`
// enumerates the cfg->images[] entries. Slot index in the indexed
// child matches the slot in cfg->images[]; n_images is dense from
// 0..n_images-1, so the collection's count() returns cfg->n_images
// and next(prev) advances to prev+1 until n_images.

typedef struct {
    config_t *cfg;
    int slot;
} storage_image_data_t;

static storage_image_data_t g_storage_image_data[MAX_IMAGES];
static struct object *g_storage_image_objs[MAX_IMAGES];

static image_t *storage_image_at(struct object *self) {
    storage_image_data_t *d = (storage_image_data_t *)object_data(self);
    if (!d || !d->cfg)
        return NULL;
    if (d->slot < 0 || d->slot >= d->cfg->n_images)
        return NULL;
    return d->cfg->images[d->slot];
}

static value_t storage_image_attr_index(struct object *self, const member_t *m) {
    (void)m;
    storage_image_data_t *d = (storage_image_data_t *)object_data(self);
    return val_int(d ? d->slot : -1);
}
static value_t storage_image_attr_filename(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    const char *s = img ? image_get_filename(img) : NULL;
    return val_str(s ? s : "");
}
static value_t storage_image_attr_path(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    const char *s = img ? image_path(img) : NULL;
    return val_str(s ? s : "");
}
static value_t storage_image_attr_raw_size(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    return val_uint(8, img ? (uint64_t)img->raw_size : 0);
}
static value_t storage_image_attr_writable(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    return val_bool(img ? img->writable : false);
}

static const char *const STORAGE_IMAGE_TYPE_NAMES[] = {
    "other", "fd_ss", "fd_ds", "fd_hd", "hd", "cdrom",
};

static value_t storage_image_attr_type(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    int t = img ? (int)img->type : 0;
    int max = (int)(sizeof(STORAGE_IMAGE_TYPE_NAMES) / sizeof(STORAGE_IMAGE_TYPE_NAMES[0]));
    if (t < 0 || t >= max)
        t = 0;
    return val_enum(t, STORAGE_IMAGE_TYPE_NAMES, (size_t)max);
}

static const member_t storage_image_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = storage_image_attr_index, .set = NULL}      },
    {.kind = M_ATTR,
     .name = "filename",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = storage_image_attr_filename, .set = NULL}},
    {.kind = M_ATTR,
     .name = "path",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = storage_image_attr_path, .set = NULL}    },
    {.kind = M_ATTR,
     .name = "raw_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = storage_image_attr_raw_size, .set = NULL}  },
    {.kind = M_ATTR,
     .name = "writable",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = storage_image_attr_writable, .set = NULL}  },
    {.kind = M_ATTR,
     .name = "type",
     .flags = VAL_RO,
     .attr = {.type = V_ENUM, .get = storage_image_attr_type, .set = NULL}      },
};

const class_desc_t storage_image_class = {
    .name = "image",
    .members = storage_image_members,
    .n_members = sizeof(storage_image_members) / sizeof(storage_image_members[0]),
};

static struct object *storage_images_get(struct object *self, int index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || index < 0 || index >= MAX_IMAGES)
        return NULL;
    if (index >= cfg->n_images || !cfg->images[index])
        return NULL;
    return g_storage_image_objs[index];
}
static int storage_images_count(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg)
        return 0;
    int n = 0;
    for (int i = 0; i < cfg->n_images; i++)
        if (cfg->images[i])
            n++;
    return n;
}
static int storage_images_next(struct object *self, int prev_index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg)
        return -1;
    for (int i = prev_index + 1; i < cfg->n_images; i++)
        if (cfg->images[i])
            return i;
    return -1;
}

// `storage.import(host_path, dst_path?)` — copy `host_path` into the
// emulator's persistent storage. When `dst_path` is empty (or absent
// — second arg is optional), falls back to the content-hash path
// produced by image_persist_volatile (/opfs/images/<hash>.img). When
// `dst_path` is non-empty, the source is copied verbatim through the
// VFS so paths like "/opfs/images/foo.img" can be picked explicitly.
//
// Returns the resolved destination path as a V_STRING.
static value_t storage_method_import(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("storage.import: expected (host_path, [dst_path])");
    const char *host_path = argv[0].s;
    const char *dst_path = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) ? argv[1].s : NULL;

    if (!dst_path) {
        // Hash-named persistence — handles the drag-drop / volatile-
        // path case and is idempotent on repeat imports.
        char *resolved = image_persist_volatile(host_path);
        if (!resolved)
            return val_err("storage.import: failed to persist '%s'", host_path);
        value_t v = val_str(resolved);
        free(resolved);
        return v;
    }

    // Explicit destination — call shell_cp directly so VFS handling
    // stays in one place (no shell_dispatch).
    char err[256] = {0};
    if (shell_cp(host_path, dst_path, false, err, sizeof(err)) < 0)
        return val_err("storage.import: cp '%s' -> '%s' failed: %s", host_path, dst_path, err[0] ? err : "unknown");
    return val_str(dst_path);
}

static const arg_decl_t storage_import_args[] = {
    {.name = "host_path", .kind = V_STRING, .doc = "Host path to read"},
    {.name = "dst_path",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "Destination path; empty → /opfs/images/<hash>.img"},
};

static const member_t storage_images_collection_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &storage_image_class,
               .indexed = true,
               .get = storage_images_get,
               .count = storage_images_count,
               .next = storage_images_next,
               .lookup = NULL}},
};

const class_desc_t storage_images_collection_class = {
    .name = "storage_images",
    .members = storage_images_collection_members,
    .n_members = sizeof(storage_images_collection_members) / sizeof(storage_images_collection_members[0]),
};

// `storage.list_dir(path)` — list directory entries via the VFS as a
// V_LIST<V_STRING>. Powers the M10b/c migration of url-media.js's
// legacy `ls $ROMS_DIR` (whose stdout-only output had no typed
// successor until now).
static value_t storage_method_list_dir(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("storage.list_dir: expected (path)");
    vfs_dir_t *d = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_opendir(argv[0].s, &d, &be);
    if (rc < 0 || !d || !be)
        return val_list(NULL, 0); // empty list (treat unreadable dirs as no entries)
    size_t cap = 16, n = 0;
    value_t *items = (value_t *)calloc(cap, sizeof(value_t));
    if (!items) {
        be->closedir(d);
        return val_err("storage.list_dir: out of memory");
    }
    vfs_dirent_t ent;
    while (be->readdir(d, &ent) > 0) {
        if (ent.name[0] == '.' && (ent.name[1] == '\0' || (ent.name[1] == '.' && ent.name[2] == '\0')))
            continue;
        if (n >= cap) {
            size_t new_cap = cap * 2;
            value_t *nb = (value_t *)realloc(items, new_cap * sizeof(value_t));
            if (!nb) {
                for (size_t i = 0; i < n; i++)
                    value_free(&items[i]);
                free(items);
                be->closedir(d);
                return val_err("storage.list_dir: out of memory");
            }
            items = nb;
            cap = new_cap;
        }
        items[n++] = val_str(ent.name);
    }
    be->closedir(d);
    return val_list(items, n);
}

static const arg_decl_t storage_list_dir_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Directory path"},
};

static const member_t storage_members[] = {
    {.kind = M_METHOD,
     .name = "import",
     .doc = "Persist a host file under /images/ (deferred — see proposal §5.7)",
     .method = {.args = storage_import_args, .nargs = 2, .result = V_STRING, .fn = storage_method_import}  },
    {.kind = M_METHOD,
     .name = "list_dir",
     .doc = "List directory entries (V_LIST of V_STRING names)",
     .method = {.args = storage_list_dir_args, .nargs = 1, .result = V_LIST, .fn = storage_method_list_dir}},
};

const class_desc_t storage_class_real = {
    .name = "storage",
    .members = storage_members,
    .n_members = sizeof(storage_members) / sizeof(storage_members[0]),
};

// Per-slot image-entry object setup/teardown for storage.images
// indexed children. Called from gs_classes_install / gs_classes_uninstall.
void storage_object_classes_init(struct config *cfg) {
    for (int i = 0; i < MAX_IMAGES; i++) {
        g_storage_image_data[i].cfg = cfg;
        g_storage_image_data[i].slot = i;
        g_storage_image_objs[i] = object_new(&storage_image_class, &g_storage_image_data[i], NULL);
    }
}

void storage_object_classes_teardown(void) {
    for (int i = 0; i < MAX_IMAGES; i++) {
        if (g_storage_image_objs[i]) {
            object_delete(g_storage_image_objs[i]);
            g_storage_image_objs[i] = NULL;
        }
        g_storage_image_data[i].cfg = NULL;
        g_storage_image_data[i].slot = 0;
    }
}
