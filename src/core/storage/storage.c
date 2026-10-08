// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage.c
// Delta-file storage engine implementation.
//
// Layout of a delta file (version 2, what every new delta is):
//   [0..63]                      Header (magic, version, block_count, block_size,
//                                cluster_blocks, cluster_count, slots_committed)
//   [64 .. +bm]                  Current bitmap (1 bit per block)
//   [.. +bm]                     Committed bitmap
//   [.. +4*C]                    Current cluster table (u32 per cluster: 0 = no slot, k = slot k-1)
//   [.. +4*C]                    Committed cluster table
//   [data_offset ..]             Slots of cluster_blocks blocks each, in allocation order
//
// A block's data lives in the slot its cluster was given the first time any
// block of that cluster was written, so the file grows with what the guest
// wrote, not with the highest block it wrote: the browser charges a file's
// logical length against the origin's quota, holes and all.  Rollback
// truncates away the slots allocated since the last commit.
//
// Version 1 (still opened, never created) placed block N at
// data_offset + N * block_size after a 24-byte header and the two bitmaps.
//
// Journal format (append-only):
//   Each entry: [uint32_t LBA, LE][block_size bytes data] = 4 + block_size bytes per
//   entry.  The header records block_size, so reopen self-describes the entry
//   stride — no fixed entry size.

#include "storage.h"

#include "checkpoint.h"
#include "source.h"
#include "status.h"
#include "io/io_worker.h"

#include "log.h"
#include "system.h"

LOG_USE_CATEGORY_NAME("storage");

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

#define STORAGE_DELTA_MAGIC      "GSDL"
#define STORAGE_DELTA_MAGIC_SIZE 4
#define STORAGE_DELTA_VERSION_V1 1 // LBA-positioned; opened, never created
#define STORAGE_DELTA_VERSION    2 // cluster-indexed
#define STORAGE_DELTA_HEADER_V1  24 // magic(4) + version(4) + block_count(8) + block_size(4) + reserved(4)
#define STORAGE_DELTA_HEADER_SIZE                                                                                      \
    64 // v1's fields + cluster_blocks(4) + cluster_count(8) + slots_committed(8) + reserved

// Blocks per cluster in a new delta: 64 x 512 B = 32 KB.  The header records
// it, so the choice is not frozen.
#define STORAGE_DELTA_CLUSTER_BLOCKS 64

// One journal entry = LBA(4) + one block of data.  Block size is per-instance
// (storage->block_size), so the stride is computed at runtime, not fixed.
#define STORAGE_JOURNAL_ENTRY_SIZE(s) (4 + (size_t)(s)->block_size)

#define STORAGE_SNAPSHOT_VERSION 3 // 3: a quick payload carries the cluster table

// Staging-buffer size for storage_save_state.  Streaming a disk one block at a
// time costs a seek plus a read per 512-byte block — over 130 000 filesystem
// round trips per 64 MB.  Under WASMFS/OPFS each of those is a synchronous
// access-handle call, which is what made exporting a real hard disk take
// minutes.  Batching contiguous same-source runs through a buffer this size
// cuts the round trips by three orders of magnitude.
#define STORAGE_STREAM_CHUNK_BYTES (4u * 1024 * 1024)

// ============================================================================
// Internal types
// ============================================================================

// Snapshot header in a checkpoint stream.  On the wire it is 24 bytes,
// little-endian, field by field:
//   [0..3] version  [4] has_data  [5..7] reserved  [8..15] block_count
//   [16..19] block_size  [20..23] reserved
// (the layout the struct had when it was written whole, padding included,
// so existing checkpoints still read).
typedef struct {
    uint32_t version;
    uint8_t has_data; // 1 = consolidated (all blocks), 0 = quick (bitmap only)
    uint64_t block_count;
    uint32_t block_size;
} storage_snapshot_header_t;

#define STORAGE_SNAPSHOT_HEADER_BYTES 24

// Context for checkpoint streaming callbacks
typedef struct {
    checkpoint_t *checkpoint;
} checkpoint_stream_ctx_t;

// The storage instance
struct storage_t {
    gs_source_t *base; // Original image, read-only (a locked wrapper: any thread may read)
    FILE *delta_fp; // Delta file, read-write, kept open
    FILE *journal_fp; // Preimage journal, append+read, kept open

    uint8_t *bitmap; // Current modification bitmap (in memory)
    uint8_t *committed_bitmap; // Bitmap at last successful checkpoint

    uint64_t block_count;
    uint32_t block_size; // Bytes per block (512 default, 532 ProFile); fixed for this instance
    size_t bitmap_bytes; // ceil(block_count / 8)

    uint32_t version; // STORAGE_DELTA_VERSION, or STORAGE_DELTA_VERSION_V1 for an old delta
    size_t bitmap_offset; // Byte offset to bitmaps in delta (the header size)
    size_t data_offset; // Byte offset to block data in delta

    // Version 2: the cluster tables (current and committed; NULL for v1).
    uint32_t cluster_blocks; // blocks per cluster
    uint64_t cluster_count; // ceil(block_count / cluster_blocks)
    uint32_t *table; // current: per cluster, 0 or slot + 1
    uint32_t *committed_table; // at the last commit
    size_t table_bytes; // 4 * cluster_count
    uint64_t slots_used; // slots allocated (the data area's high-water mark)
    uint64_t slots_committed; // slots_used at the last commit

    uint8_t *journaled; // In-memory index: 1 bit per block with a preimage in the journal
    size_t journal_count; // entries in the journal

    bool bitmap_dirty; // True if bitmap or table changed since last flush

    // The delta file, for an export view's own handle.
    char *delta_path;
    int export_locks; // exports in flight: guest writes are refused
    struct storage_t *live_next; // the registry of live storages (export_view_end)
};

// Every storage alive, so a view's end can tell whether the storage it
// locked still exists.
static storage_t *g_live_storages;

static void live_add(storage_t *s) {
    s->live_next = g_live_storages;
    g_live_storages = s;
}

static void live_remove(storage_t *s) {
    for (storage_t **pp = &g_live_storages; *pp; pp = &(*pp)->live_next) {
        if (*pp == s) {
            *pp = s->live_next;
            return;
        }
    }
}

static bool live_has(const storage_t *s) {
    for (storage_t *p = g_live_storages; p; p = p->live_next)
        if (p == s)
            return true;
    return false;
}

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
// Byte order
// ============================================================================
//
// Every multi-byte field this file puts on disk or in a checkpoint stream is
// little-endian: the delta header, the journal's LBAs and the snapshot
// header go through the helpers below.  The cluster tables are arrays of
// uint32_t written and read in place, which is little-endian only on a
// little-endian host -- every target is one (x86-64, arm64, wasm32), and the
// assertion below stops a big-endian build rather than let it write deltas
// no other build reads.  The bitmaps are plain byte streams with no byte
// order: block N is bit (N & 7) of byte N >> 3, least significant bit first.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the delta's cluster tables are stored host-order (LE)");
#endif

static void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void put_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get_le64(const uint8_t *p) {
    return (uint64_t)get_le32(p) | (uint64_t)get_le32(p + 4) << 32;
}

// ============================================================================
// Journal helpers
// ============================================================================

// Whether block `lba` already has a preimage in the journal: one bit per
// block, so the check on every write to a committed block is O(1) however
// long the journal has grown since the last commit.
static bool journal_has_lba(const storage_t *s, uint32_t lba) {
    return bitmap_test(s->journaled, lba);
}

static void journal_index_add(storage_t *s, uint32_t lba) {
    bitmap_set(s->journaled, lba);
    s->journal_count++;
}

// Append a preimage entry to the journal file and index.
static status_t journal_append(storage_t *s, uint32_t lba, const uint8_t *data) {
    uint8_t le[4];
    put_le32(le, lba);
    if (fwrite(le, sizeof(le), 1, s->journal_fp) != 1)
        return STATUS_E_IO;
    // Write block data
    if (fwrite(data, s->block_size, 1, s->journal_fp) != 1)
        return STATUS_E_IO;
    // Write-ahead: the preimage has to leave this FILE's buffer before the
    // overwrite of the block it saves can leave the delta's (the next seek
    // on the delta flushes that one).  Deferring this to the checkpoint
    // would let a crash land the new block with no preimage to undo it.
    if (fflush(s->journal_fp) != 0)
        return STATUS_E_IO;

    journal_index_add(s, lba);
    return STATUS_OK;
}

// Every seek in this file is fseeko with an off_t: a long is 32 bits on
// wasm32, where an fseek offset past 2 GiB wrapped and reads and writes
// landed at the wrong block with no error.
_Static_assert(sizeof(off_t) >= 8, "storage requires 64-bit off_t (build with _FILE_OFFSET_BITS=64)");

// Byte position of block `lba` in a file whose blocks start at `origin`,
// with the product formed in 64 bits before any narrowing.
static off_t block_pos(uint64_t origin, uint64_t lba, uint32_t block_size) {
    return (off_t)(origin + lba * block_size);
}

// Where a delta keeps its blocks: what a read needs, shared by the live
// storage and an export view's copy.
typedef struct {
    uint32_t block_size;
    uint64_t data_offset;
    uint32_t cluster_blocks; // 0 for a v1 (LBA-positioned) delta
    const uint32_t *table; // v2: the cluster table to resolve through
} delta_layout_t;

// Byte position of block `lba` in the delta, or -1 when its cluster has no
// slot yet (v2 only; a v1 delta has a place for every block).
static off_t delta_pos(const delta_layout_t *l, uint64_t lba) {
    if (!l->cluster_blocks)
        return block_pos(l->data_offset, lba, l->block_size);
    uint32_t slot = l->table[lba / l->cluster_blocks];
    if (!slot)
        return -1;
    uint64_t index = (uint64_t)(slot - 1) * l->cluster_blocks + lba % l->cluster_blocks;
    return block_pos(l->data_offset, index, l->block_size);
}

static delta_layout_t layout_of(const storage_t *s) {
    delta_layout_t l = {s->block_size, s->data_offset, s->version == STORAGE_DELTA_VERSION ? s->cluster_blocks : 0,
                        s->table};
    return l;
}

// Bytes of one slot.
static uint64_t slot_bytes(const storage_t *s) {
    return (uint64_t)s->cluster_blocks * s->block_size;
}

// Cut the data area back to the slots in use, dropping any slot allocated
// after them (v2).
static status_t delta_truncate_slots(storage_t *s) {
    if (s->version != STORAGE_DELTA_VERSION)
        return STATUS_OK;
    fflush(s->delta_fp);
    off_t want = (off_t)(s->data_offset + s->slots_used * slot_bytes(s));
    if (fseeko(s->delta_fp, 0, SEEK_END) != 0)
        return STATUS_E_IO;
    off_t have = ftello(s->delta_fp);
    if (have > want && ftruncate(fileno(s->delta_fp), want) != 0) {
        LOG(0, "storage: cannot truncate the delta to its committed slots (errno=%d)", errno);
        return STATUS_E_IO;
    }
    return STATUS_OK;
}

// Scan the journal file and rebuild the in-memory index.  The index is the
// longest valid prefix of the file: an entry that is cut short (a crash mid-
// append) or names a block outside the device ends it,
// and the file is truncated there so appends stay aligned and a replay
// never writes outside the delta's data area.
static status_t journal_load_index(storage_t *s) {
    s->journal_count = 0;
    memset(s->journaled, 0, s->bitmap_bytes);

    // storage_new opens the journal before it gets here: a missing handle
    // is a broken instance, not an empty journal.
    if (!s->journal_fp)
        return STATUS_ERROR;

    // ftello returns off_t (64-bit when _FILE_OFFSET_BITS=64) so a >2 GiB
    // journal doesn't silently truncate to int32 on wasm32.
    if (fseeko(s->journal_fp, 0, SEEK_END) != 0)
        return STATUS_E_IO;
    off_t size = ftello(s->journal_fp);
    if (size <= 0)
        return STATUS_OK;

    if (fseeko(s->journal_fp, 0, SEEK_SET) != 0)
        return STATUS_E_IO;
    uint64_t entries = (uint64_t)size / STORAGE_JOURNAL_ENTRY_SIZE(s);

    uint64_t valid = 0;
    for (; valid < entries; valid++) {
        uint8_t le[4];
        if (fread(le, sizeof(le), 1, s->journal_fp) != 1)
            break;
        uint32_t lba = get_le32(le);
        if (lba >= s->block_count) {
            LOG(0,
                "storage: journal entry %" PRIu64 " names block %" PRIu32 " of %" PRIu64 "; discarding it and the rest",
                valid, lba, s->block_count);
            break;
        }
        // Skip block data
        if (fseeko(s->journal_fp, (off_t)s->block_size, SEEK_CUR) != 0)
            break;
        journal_index_add(s, lba);
    }

    off_t keep = (off_t)valid * (off_t)STORAGE_JOURNAL_ENTRY_SIZE(s);
    if (keep != size) {
        if (valid == entries)
            LOG(0, "storage: journal ends in a partial entry; discarding it");
        if (ftruncate(fileno(s->journal_fp), keep) != 0)
            return STATUS_E_IO;
    }
    // Position at end for appending
    if (fseeko(s->journal_fp, 0, SEEK_END) != 0)
        return STATUS_E_IO;
    return STATUS_OK;
}

// ============================================================================
// Delta file I/O helpers
// ============================================================================

// Write the delta file header (v2: also on every commit, for slots_committed).
static status_t delta_write_header(storage_t *s) {
    uint8_t h[STORAGE_DELTA_HEADER_SIZE] = {0};
    memcpy(h, STORAGE_DELTA_MAGIC, STORAGE_DELTA_MAGIC_SIZE);
    put_le32(h + 4, s->version);
    put_le64(h + 8, s->block_count);
    put_le32(h + 16, s->block_size);
    size_t len = STORAGE_DELTA_HEADER_V1;
    if (s->version == STORAGE_DELTA_VERSION) {
        put_le32(h + 20, s->cluster_blocks);
        put_le64(h + 24, s->cluster_count);
        put_le64(h + 32, s->slots_committed);
        len = STORAGE_DELTA_HEADER_SIZE;
    }
    if (fseeko(s->delta_fp, 0, SEEK_SET) != 0 || fwrite(h, len, 1, s->delta_fp) != 1)
        return STATUS_E_IO;
    return STATUS_OK;
}

// Bytes of metadata (header, bitmaps, tables) before the data area,
// rounded to a whole sector.
static size_t delta_meta_bytes(const storage_t *s) {
    if (s->version != STORAGE_DELTA_VERSION)
        return STORAGE_DELTA_HEADER_V1 + 2 * s->bitmap_bytes;
    size_t n = STORAGE_DELTA_HEADER_SIZE + 2 * s->bitmap_bytes + 2 * s->table_bytes;
    return (n + 511) & ~(size_t)511;
}

// Size the per-version metadata from the geometry and allocate the tables.
static status_t delta_layout_init(storage_t *s) {
    s->bitmap_offset = s->version == STORAGE_DELTA_VERSION ? STORAGE_DELTA_HEADER_SIZE : STORAGE_DELTA_HEADER_V1;
    if (s->version == STORAGE_DELTA_VERSION) {
        s->cluster_count = (s->block_count + s->cluster_blocks - 1) / s->cluster_blocks;
        s->table_bytes = (size_t)s->cluster_count * sizeof(uint32_t);
        free(s->table);
        free(s->committed_table);
        s->table = calloc(1, s->table_bytes);
        s->committed_table = calloc(1, s->table_bytes);
        if (!s->table || !s->committed_table)
            return STATUS_E_NOMEM;
    }
    s->data_offset = delta_meta_bytes(s);
    return STATUS_OK;
}

// Read and validate the delta file header; sets the version and layout.
static status_t delta_read_header(storage_t *s) {
    uint8_t h[STORAGE_DELTA_HEADER_SIZE];
    if (fseeko(s->delta_fp, 0, SEEK_SET) != 0 || fread(h, STORAGE_DELTA_HEADER_V1, 1, s->delta_fp) != 1)
        return STATUS_E_IO;
    if (memcmp(h, STORAGE_DELTA_MAGIC, STORAGE_DELTA_MAGIC_SIZE) != 0)
        return STATUS_E_INVAL;
    uint32_t version = get_le32(h + 4);
    if (version != STORAGE_DELTA_VERSION && version != STORAGE_DELTA_VERSION_V1)
        return STATUS_E_INVAL;
    if (get_le64(h + 8) != s->block_count || get_le32(h + 16) != s->block_size)
        return STATUS_E_INVAL;
    s->version = version;
    if (version == STORAGE_DELTA_VERSION) {
        if (fread(h + STORAGE_DELTA_HEADER_V1, STORAGE_DELTA_HEADER_SIZE - STORAGE_DELTA_HEADER_V1, 1, s->delta_fp) !=
            1)
            return STATUS_E_IO;
        s->cluster_blocks = get_le32(h + 20);
        if (s->cluster_blocks == 0 || s->cluster_blocks > (1u << 20))
            return STATUS_E_INVAL;
        if (get_le64(h + 24) != (s->block_count + s->cluster_blocks - 1) / s->cluster_blocks)
            return STATUS_E_INVAL;
        s->slots_committed = get_le64(h + 32);
    }
    return delta_layout_init(s);
}

// Flush the metadata: both bitmaps, both cluster tables and the header's
// slot count.  The data the tables point at is flushed first, so a table
// never names a slot the file does not yet hold.
static status_t delta_flush_bitmaps(storage_t *s) {
    fflush(s->delta_fp);
    if (fseeko(s->delta_fp, (off_t)s->bitmap_offset, SEEK_SET) != 0)
        return STATUS_E_IO;
    if (fwrite(s->bitmap, s->bitmap_bytes, 1, s->delta_fp) != 1)
        return STATUS_E_IO;
    if (fwrite(s->committed_bitmap, s->bitmap_bytes, 1, s->delta_fp) != 1)
        return STATUS_E_IO;
    if (s->version == STORAGE_DELTA_VERSION) {
        if (fwrite(s->table, s->table_bytes, 1, s->delta_fp) != 1)
            return STATUS_E_IO;
        if (fwrite(s->committed_table, s->table_bytes, 1, s->delta_fp) != 1)
            return STATUS_E_IO;
        status_t rc = delta_write_header(s);
        if (rc != STATUS_OK)
            return rc;
    }
    fflush(s->delta_fp);
    return STATUS_OK;
}

// Read the bitmaps (and v2's tables) from the delta.  A v2 delta reopens at
// its last commit: slots past slots_committed are cut away, as a rollback
// would, and a current table naming one of them (a write-back torn by a
// crash) falls back to the committed state.
static status_t delta_read_bitmaps(storage_t *s) {
    if (fseeko(s->delta_fp, (off_t)s->bitmap_offset, SEEK_SET) != 0)
        return STATUS_E_IO;
    if (fread(s->bitmap, s->bitmap_bytes, 1, s->delta_fp) != 1)
        return STATUS_E_IO;
    if (fread(s->committed_bitmap, s->bitmap_bytes, 1, s->delta_fp) != 1)
        return STATUS_E_IO;
    if (s->version != STORAGE_DELTA_VERSION)
        return STATUS_OK;
    if (fread(s->table, s->table_bytes, 1, s->delta_fp) != 1)
        return STATUS_E_IO;
    if (fread(s->committed_table, s->table_bytes, 1, s->delta_fp) != 1)
        return STATUS_E_IO;
    bool torn = false;
    for (uint64_t c = 0; c < s->cluster_count; c++) {
        if (s->committed_table[c] > s->slots_committed)
            return STATUS_E_INVAL; // the committed state itself is inconsistent
        if (s->table[c] > s->slots_committed || (s->committed_table[c] && s->table[c] != s->committed_table[c]))
            torn = true;
    }
    if (torn) {
        LOG(0, "storage: delta's current tables name uncommitted slots; reopening at the last commit");
        memcpy(s->table, s->committed_table, s->table_bytes);
        memcpy(s->bitmap, s->committed_bitmap, s->bitmap_bytes);
    }
    s->slots_used = s->slots_committed;
    return delta_truncate_slots(s);
}

// Write a fresh delta's metadata: header, zero bitmaps and tables.
static status_t delta_create(storage_t *s) {
    status_t rc = delta_write_header(s);
    if (rc == STATUS_OK)
        rc = delta_flush_bitmaps(s);
    if (rc != STATUS_OK)
        return rc;
    // Pad the metadata to the data area, so slot 0 starts where it should.
    off_t end = (off_t)s->data_offset;
    if (ftruncate(fileno(s->delta_fp), end) != 0)
        return STATUS_E_IO;
    return STATUS_OK;
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
    // Negative on error so read_exact can distinguish "I/O error" from
    // "EOF / zero-byte read".
    return checkpoint_has_error(c->checkpoint) ? -1 : (int)size;
}

// ============================================================================
// Public API: Lifecycle
// ============================================================================

status_t storage_new(const storage_config_t *config, storage_t **out_storage) {
    if (!config || !out_storage || !config->delta_path || !config->journal_path)
        return STATUS_E_INVAL;
    // Block size must be word-aligned (the Lisa parallel bus is word-addressed)
    // and bounded so the journal/streaming stack buffers stay safe.  512 (flat
    // disks) and 532 (ProFile: 512 data + 20 inline tag) both pass.
    if (config->block_size < STORAGE_BLOCK_SIZE || config->block_size > STORAGE_MAX_BLOCK_SIZE ||
        (config->block_size % 4) != 0)
        return STATUS_E_INVAL;
    if (config->block_count == 0)
        return STATUS_E_INVAL;
    // LBAs are uint32_t internally — anything larger silently truncates
    // when an LBA is computed.
    if (config->block_count > UINT32_MAX)
        return STATUS_E_RANGE;

    storage_t *s = calloc(1, sizeof(storage_t));
    if (!s)
        return STATUS_E_NOMEM;

    s->block_count = config->block_count;
    s->block_size = config->block_size;
    s->delta_path = strdup(config->delta_path);
    s->bitmap_bytes = (size_t)((config->block_count + 7) / 8);
    status_t rc = STATUS_E_NOMEM; // what a `goto fail` reports

    // Allocate bitmaps
    s->bitmap = calloc(1, s->bitmap_bytes);
    s->committed_bitmap = calloc(1, s->bitmap_bytes);
    s->journaled = calloc(1, s->bitmap_bytes);
    if (!s->bitmap || !s->committed_bitmap || !s->journaled)
        goto fail;

    // The base (optional — NULL for a blank image).  Reads go through a
    // lock of its own: an export streams the base on the I/O worker while
    // the guest reads it here, and most sources are not thread-safe.
    if (config->base) {
        s->base = gs_source_locked(config->base);
        if (!s->base)
            goto fail;
    }

    // Open or create delta file. Try the "existing" path first so an
    // access()-then-fopen() race can't silently clobber the existing file
    // if a concurrent process deletes it between the two calls. ENOENT
    // means "go create one" — anything else is a real failure.
    bool delta_exists = false;
    s->delta_fp = fopen(config->delta_path, "r+b");
    if (s->delta_fp) {
        delta_exists = true;
    } else if (errno == ENOENT) {
        s->delta_fp = fopen(config->delta_path, "w+b");
    }
    if (!s->delta_fp) {
        rc = STATUS_E_IO;
        goto fail;
    }

    if (delta_exists) {
        // Read and validate existing header + bitmaps (either version)
        rc = delta_read_header(s);
        if (rc == STATUS_OK)
            rc = delta_read_bitmaps(s);
    } else {
        // A new delta is always version 2: header, empty bitmaps and tables
        s->version = STORAGE_DELTA_VERSION;
        s->cluster_blocks = STORAGE_DELTA_CLUSTER_BLOCKS;
        rc = delta_layout_init(s);
        if (rc == STATUS_OK)
            rc = delta_create(s);
    }
    if (rc != STATUS_OK)
        goto fail;

    // Open or create journal
    s->journal_fp = fopen(config->journal_path, "a+b");
    if (!s->journal_fp) {
        rc = STATUS_E_IO;
        goto fail;
    }

    // Load journal index (does not replay — caller decides).  It repairs a
    // damaged tail itself; failing here means it could not (the file could
    // not be read or truncated back into alignment).
    rc = journal_load_index(s);
    if (rc != STATUS_OK)
        goto fail;

    live_add(s);
    *out_storage = s;
    return STATUS_OK;

fail:
    storage_delete(s);
    *out_storage = NULL;
    return rc;
}

status_t storage_delete(storage_t *storage) {
    if (!storage)
        return STATUS_OK;
    live_remove(storage);
    free(storage->delta_path);
    gs_source_release(storage->base);
    if (storage->delta_fp)
        fclose(storage->delta_fp);
    if (storage->journal_fp)
        fclose(storage->journal_fp);
    free(storage->bitmap);
    free(storage->committed_bitmap);
    free(storage->table);
    free(storage->committed_table);
    free(storage->journaled);
    free(storage);
    return STATUS_OK;
}

// ============================================================================
// Public API: Block I/O
// ============================================================================

status_t storage_read_block(storage_t *storage, size_t offset, void *buffer) {
    if (!storage || !buffer)
        return STATUS_E_INVAL;
    if (offset % storage->block_size != 0)
        return STATUS_E_INVAL;

    uint64_t lba64 = offset / storage->block_size;
    if (lba64 >= storage->block_count)
        return STATUS_E_RANGE;
    uint32_t lba = (uint32_t)lba64;

    if (bitmap_test(storage->bitmap, lba)) {
        // Modified block — read from delta
        delta_layout_t l = layout_of(storage);
        off_t pos = delta_pos(&l, lba);
        if (pos < 0 || fseeko(storage->delta_fp, pos, SEEK_SET) != 0 ||
            fread(buffer, storage->block_size, 1, storage->delta_fp) != 1) {
            memset(buffer, 0, storage->block_size);
            return STATUS_E_IO;
        }
    } else if (storage->base) {
        // Unmodified block — read from the base.  A block the base does not
        // hold in full (a base shorter than the geometry) reads as zeros, as
        // the export stream serves it; a block it does hold and cannot read
        // is an error, as a failed delta read is, never silent zeros.
        uint64_t at = (uint64_t)block_pos(0, lba, storage->block_size);
        if (at + storage->block_size > gs_source_size(storage->base)) {
            memset(buffer, 0, storage->block_size);
        } else if (gs_source_read_exact(storage->base, at, buffer, storage->block_size) != 0) {
            memset(buffer, 0, storage->block_size);
            return STATUS_E_IO;
        }
    } else {
        // No base file — unwritten block is zeros
        memset(buffer, 0, storage->block_size);
    }

    return STATUS_OK;
}

status_t storage_write_block(storage_t *storage, size_t offset, const void *buffer) {
    if (!storage || !buffer)
        return STATUS_E_INVAL;
    if (offset % storage->block_size != 0)
        return STATUS_E_INVAL;

    uint64_t lba64 = offset / storage->block_size;
    if (lba64 >= storage->block_count)
        return STATUS_E_RANGE;
    uint32_t lba = (uint32_t)lba64;

    // An export in flight reads the delta through its own handle from a
    // bitmap it copied: a write now would tear its copy.  Refused, as a
    // drive being copied refuses.
    if (storage->export_locks > 0)
        return STATUS_ERROR;

    // Where the block goes.  The first write to any block of a cluster
    // gives the cluster the next slot at the end of the data area; a
    // committed block's cluster is committed too, so its slot is stable and
    // the preimage below is read from where the replay will write it.
    delta_layout_t l = layout_of(storage);
    off_t pos = delta_pos(&l, lba);
    if (pos < 0) {
        if (storage->slots_used >= UINT32_MAX)
            return STATUS_E_RANGE;
        storage->table[lba / storage->cluster_blocks] = (uint32_t)++storage->slots_used;
        storage->bitmap_dirty = true;
        pos = delta_pos(&l, lba);
    }

    // Capture preimage if this block was committed and not yet journaled
    if (bitmap_test(storage->committed_bitmap, lba) && !journal_has_lba(storage, lba)) {
        uint8_t old[STORAGE_MAX_BLOCK_SIZE];
        if (fseeko(storage->delta_fp, pos, SEEK_SET) != 0 ||
            fread(old, storage->block_size, 1, storage->delta_fp) != 1) {
            return STATUS_E_IO;
        }
        status_t rc = journal_append(storage, lba, old);
        if (rc != STATUS_OK)
            return rc;
    }

    // Write new data to delta
    if (fseeko(storage->delta_fp, pos, SEEK_SET) != 0 || fwrite(buffer, storage->block_size, 1, storage->delta_fp) != 1)
        return STATUS_E_IO;

    // Update the bitmap in memory only.  The delta's on-disk bitmap changes
    // at the next commit, and delta_flush_bitmaps flushes the block data
    // first, so the bitmap on disk never names a block whose write is still
    // sitting in this FILE's buffer.
    bitmap_set(storage->bitmap, lba);
    storage->bitmap_dirty = true;

    return STATUS_OK;
}

// ============================================================================
// Public API: Rollback
// ============================================================================

// Truncate the journal.  If ftruncate fails, journal_count is left alone so
// the in-memory index still matches whatever stayed on disk.
static status_t journal_clear(storage_t *storage) {
    if (storage->journal_fp) {
        int fd = fileno(storage->journal_fp);
        if (fd >= 0 && ftruncate(fd, 0) != 0) {
            LOG(0, "storage: ftruncate failed on journal (errno=%d); keeping in-memory index", errno);
            return STATUS_E_IO;
        }
        fseeko(storage->journal_fp, 0, SEEK_SET);
    }
    if (storage->journal_count)
        memset(storage->journaled, 0, storage->bitmap_bytes);
    storage->journal_count = 0;
    return STATUS_OK;
}

// True when anything was written since the last commit.
static bool uncommitted(const storage_t *s) {
    return s->journal_count > 0 || s->bitmap_dirty || s->slots_used != s->slots_committed;
}

status_t storage_apply_rollback(storage_t *storage) {
    if (!storage)
        return STATUS_E_INVAL;
    if (!uncommitted(storage))
        return STATUS_OK;

    // Replay journal: restore preimages to delta.  Every journaled block was
    // committed, so its cluster's slot is committed and unchanged.
    if (storage->journal_count > 0 && fseeko(storage->journal_fp, 0, SEEK_SET) != 0)
        return STATUS_E_IO;
    delta_layout_t l = layout_of(storage);
    for (size_t i = 0; i < storage->journal_count; i++) {
        uint8_t le[4];
        uint8_t data[STORAGE_MAX_BLOCK_SIZE];

        if (fread(le, sizeof(le), 1, storage->journal_fp) != 1)
            return STATUS_E_IO;
        uint32_t lba = get_le32(le);
        if (fread(data, storage->block_size, 1, storage->journal_fp) != 1)
            return STATUS_E_IO;

        // journal_load_index admitted only in-range blocks, but this reads
        // the file again: never write a block outside the device.
        if (lba >= storage->block_count)
            return STATUS_E_INVAL;
        // Write preimage back to delta
        off_t pos = delta_pos(&l, lba);
        if (pos < 0 || fseeko(storage->delta_fp, pos, SEEK_SET) != 0 ||
            fwrite(data, storage->block_size, 1, storage->delta_fp) != 1)
            return STATUS_E_IO;
    }

    // Restore bitmap and tables to the committed state; the slots allocated
    // since hold only blocks the committed bitmap does not name, so they go
    // wholesale.
    memcpy(storage->bitmap, storage->committed_bitmap, storage->bitmap_bytes);
    if (storage->version == STORAGE_DELTA_VERSION) {
        memcpy(storage->table, storage->committed_table, storage->table_bytes);
        storage->slots_used = storage->slots_committed;
        status_t rc = delta_truncate_slots(storage);
        if (rc != STATUS_OK)
            return rc;
    }

    // Flush bitmaps and truncate journal
    status_t rc = delta_flush_bitmaps(storage);
    if (rc != STATUS_OK)
        return rc;
    storage->bitmap_dirty = false;
    return journal_clear(storage);
}

// Make the current state the committed one, in memory and on disk.
static status_t commit_state(storage_t *storage) {
    memcpy(storage->committed_bitmap, storage->bitmap, storage->bitmap_bytes);
    if (storage->version == STORAGE_DELTA_VERSION) {
        memcpy(storage->committed_table, storage->table, storage->table_bytes);
        storage->slots_committed = storage->slots_used;
    }
    status_t rc = delta_flush_bitmaps(storage);
    if (rc != STATUS_OK)
        return rc;
    storage->bitmap_dirty = false;
    return journal_clear(storage);
}

status_t storage_clear_rollback(storage_t *storage) {
    if (!storage)
        return STATUS_E_INVAL;
    // Only do OPFS I/O if something changed since last commit.
    // This makes back-to-back checkpoints with no intervening writes free.
    if (!uncommitted(storage))
        return STATUS_OK;
    return commit_state(storage);
}

// ============================================================================
// Public API: Checkpointing
// ============================================================================

status_t storage_checkpoint(storage_t *storage, checkpoint_t *checkpoint) {
    if (!storage)
        return STATUS_E_INVAL;

    // A commit without a checkpoint stream is storage_clear_rollback.
    if (!checkpoint)
        return STATUS_E_INVAL;

    // Write snapshot header
    storage_snapshot_header_t header = {0};
    header.version = STORAGE_SNAPSHOT_VERSION;
    header.has_data = (checkpoint_get_kind(checkpoint) == CHECKPOINT_KIND_CONSOLIDATED) ? 1 : 0;
    header.block_count = storage->block_count;
    header.block_size = storage->block_size;
    uint8_t h[STORAGE_SNAPSHOT_HEADER_BYTES] = {0};
    put_le32(h, header.version);
    h[4] = header.has_data;
    put_le64(h + 8, header.block_count);
    put_le32(h + 16, header.block_size);
    system_write_checkpoint_data(checkpoint, h, sizeof(h));
    if (checkpoint_has_error(checkpoint))
        return STATUS_ERROR;

    if (header.has_data) {
        // Consolidated: stream all blocks
        checkpoint_stream_ctx_t ctx = {checkpoint};
        status_t rc = storage_save_state(storage, &ctx, checkpoint_storage_write_cb);
        if (rc != STATUS_OK)
            return rc;
    } else {
        // Quick: the current bitmap, then where the delta keeps those blocks
        // -- blocks per cluster (0 for a v1 delta), slots in use and the
        // cluster table -- which the commit below makes the delta's own.
        system_write_checkpoint_data(checkpoint, storage->bitmap, storage->bitmap_bytes);
        uint32_t cb = storage->version == STORAGE_DELTA_VERSION ? storage->cluster_blocks : 0;
        uint8_t le[8];
        put_le32(le, cb);
        system_write_checkpoint_data(checkpoint, le, 4);
        put_le64(le, storage->slots_used);
        system_write_checkpoint_data(checkpoint, le, 8);
        if (cb)
            system_write_checkpoint_data(checkpoint, storage->table, storage->table_bytes);
        if (checkpoint_has_error(checkpoint))
            return STATUS_ERROR;
    }

    return storage_clear_rollback(storage);
}

// Read a quick payload's layout fields after its bitmap.  The table goes to
// `table` (table_bytes of it) when non-NULL, else is skipped.
static status_t read_quick_layout(checkpoint_t *checkpoint, uint64_t block_count, uint32_t *cb, uint64_t *slots,
                                  uint32_t *table, size_t table_bytes) {
    uint8_t le[8];
    system_read_checkpoint_data(checkpoint, le, 4);
    *cb = get_le32(le);
    system_read_checkpoint_data(checkpoint, le, 8);
    *slots = get_le64(le);
    if (checkpoint_has_error(checkpoint))
        return STATUS_ERROR;
    if (!*cb)
        return STATUS_OK;
    if (*cb > (1u << 20))
        return STATUS_E_INVAL;
    size_t n = (size_t)((block_count + *cb - 1) / *cb) * sizeof(uint32_t);
    if (table && n != table_bytes)
        return STATUS_E_INVAL;
    uint32_t *buf = table ? table : malloc(n ? n : 1);
    if (!buf)
        return STATUS_E_NOMEM;
    system_read_checkpoint_data(checkpoint, buf, n);
    if (!table)
        free(buf);
    return checkpoint_has_error(checkpoint) ? STATUS_ERROR : STATUS_OK;
}

// Helper: skip/discard snapshot data from a checkpoint stream
static status_t storage_skip_snapshot(checkpoint_t *checkpoint, const storage_snapshot_header_t *header) {
    if (header->has_data) {
        // Skip all block data
        uint8_t discard[STORAGE_MAX_BLOCK_SIZE];
        for (uint64_t i = 0; i < header->block_count; i++) {
            system_read_checkpoint_data(checkpoint, discard, header->block_size);
            if (checkpoint_has_error(checkpoint))
                return STATUS_ERROR;
        }
    } else {
        // Skip bitmap
        size_t bm = (size_t)((header->block_count + 7) / 8);
        uint8_t *discard = malloc(bm);
        if (!discard)
            return STATUS_E_NOMEM;
        system_read_checkpoint_data(checkpoint, discard, bm);
        free(discard);
        if (checkpoint_has_error(checkpoint))
            return STATUS_ERROR;
        uint32_t cb;
        uint64_t slots;
        return read_quick_layout(checkpoint, header->block_count, &cb, &slots, NULL, 0);
    }
    return STATUS_OK;
}

status_t storage_restore_from_checkpoint(storage_t *storage, checkpoint_t *checkpoint) {
    if (!checkpoint)
        return STATUS_E_INVAL;

    // Read snapshot header
    uint8_t h[STORAGE_SNAPSHOT_HEADER_BYTES];
    system_read_checkpoint_data(checkpoint, h, sizeof(h));
    if (checkpoint_has_error(checkpoint))
        return STATUS_ERROR;
    storage_snapshot_header_t header = {
        .version = get_le32(h), .has_data = h[4], .block_count = get_le64(h + 8), .block_size = get_le32(h + 16)};
    if (header.version != STORAGE_SNAPSHOT_VERSION) {
        LOG(0, "storage: snapshot version mismatch (got %u, expected %u)", header.version, STORAGE_SNAPSHOT_VERSION);
        return STATUS_E_INVAL;
    }

    // NULL storage — consume and discard
    if (!storage)
        return storage_skip_snapshot(checkpoint, &header);

    // Validate geometry
    if (header.block_count != storage->block_count || header.block_size != storage->block_size) {
        LOG(0, "storage: geometry mismatch (snapshot %" PRIu64 "x%u, storage %" PRIu64 "x%u)", header.block_count,
            header.block_size, storage->block_count, storage->block_size);
        return STATUS_E_INVAL;
    }

    if (header.has_data) {
        // Consolidated: load all blocks
        checkpoint_stream_ctx_t ctx = {checkpoint};
        return storage_load_state(storage, &ctx, checkpoint_storage_read_cb);
    }

    // Quick checkpoint: the delta may have been modified AFTER the checkpoint
    // was saved (the emulator kept running).  The journal has preimages for
    // those post-checkpoint overwrites, and the slots allocated since are
    // past the committed high-water mark.  Roll back first to restore the
    // delta to its committed (= checkpoint-time) state before applying the
    // checkpoint's bitmap and table.
    status_t rc = storage_apply_rollback(storage);
    if (rc != STATUS_OK)
        return rc;

    // Now read the checkpoint bitmap and layout and set them as current
    system_read_checkpoint_data(checkpoint, storage->bitmap, storage->bitmap_bytes);
    if (checkpoint_has_error(checkpoint))
        return STATUS_ERROR;
    uint32_t cb = 0;
    uint64_t slots = 0;
    bool v2 = storage->version == STORAGE_DELTA_VERSION;
    rc = read_quick_layout(checkpoint, header.block_count, &cb, &slots, v2 ? storage->table : NULL,
                           storage->table_bytes);
    if (rc != STATUS_OK)
        return rc;
    if (cb != (v2 ? storage->cluster_blocks : 0) || (v2 && slots > storage->slots_committed)) {
        LOG(0,
            "storage: checkpoint's delta layout does not match the delta (clusters %u/%u, slots %" PRIu64 "/%" PRIu64
            ")",
            cb, v2 ? storage->cluster_blocks : 0, slots, storage->slots_committed);
        memcpy(storage->bitmap, storage->committed_bitmap, storage->bitmap_bytes);
        if (v2)
            memcpy(storage->table, storage->committed_table, storage->table_bytes);
        return STATUS_E_INVAL;
    }
    if (v2) {
        for (uint64_t c = 0; c < storage->cluster_count; c++)
            if (storage->table[c] > slots) {
                memcpy(storage->table, storage->committed_table, storage->table_bytes);
                memcpy(storage->bitmap, storage->committed_bitmap, storage->bitmap_bytes);
                return STATUS_E_INVAL;
            }
        storage->slots_used = slots;
        rc = delta_truncate_slots(storage);
        if (rc != STATUS_OK)
            return rc;
    }

    // Commit: the delta data now matches this bitmap
    return commit_state(storage);
}

// ============================================================================
// Public API: Streaming
// ============================================================================

// Where a block's data comes from.  This is the axis runs are coalesced
// along: consecutive blocks with the same source sit contiguously in the same
// file, so they can be read in one call.
typedef enum {
    BLOCK_SRC_DELTA, // modified — read from the delta
    BLOCK_SRC_BASE, // unmodified — read from the base image
    BLOCK_SRC_ZERO, // unmodified with no base file — reads as zeros
} block_src_t;

// The read side an export streams from: the live storage's own handles
// (the checkpoint, on the emulator thread) or a view's copies (an export on
// the I/O worker).
typedef struct {
    gs_source_t *base;
    FILE *delta_fp;
    const uint8_t *bitmap;
    uint64_t block_count;
    uint32_t block_size;
    delta_layout_t layout; // where the delta keeps each modified block
} block_src_view_t;

static block_src_t block_source(const block_src_view_t *s, uint64_t block) {
    if (bitmap_test(s->bitmap, (uint32_t)block))
        return BLOCK_SRC_DELTA;
    return s->base ? BLOCK_SRC_BASE : BLOCK_SRC_ZERO;
}

// Stream every block through write_cb.  A status_t, or -ECANCELED when
// `cancellable` and the I/O job was cancelled.
static int stream_blocks(const block_src_view_t *storage, void *context, storage_write_callback_t write_cb,
                         bool cancellable) {
    // Chunk sized in whole blocks.  If the staging allocation fails, fall back
    // to a single block so a memory-starved host still exports, just slowly.
    uint64_t chunk_blocks = STORAGE_STREAM_CHUNK_BYTES / storage->block_size;
    if (chunk_blocks == 0)
        chunk_blocks = 1;
    uint8_t *buffer = malloc((size_t)chunk_blocks * storage->block_size);
    if (!buffer) {
        chunk_blocks = 1;
        buffer = malloc(storage->block_size);
        if (!buffer)
            return STATUS_E_NOMEM;
    }

    int rc = STATUS_OK;
    uint64_t block = 0;
    while (block < storage->block_count) {
        if (cancellable && io_check_cancelled()) {
            rc = -ECANCELED;
            break;
        }
        // Extend the run while the source stays the same, capped by the chunk.
        block_src_t src = block_source(storage, block);
        uint64_t max_run = storage->block_count - block;
        if (max_run > chunk_blocks)
            max_run = chunk_blocks;
        // A delta run also has to stay contiguous in the file: in a v2
        // delta, consecutive clusters sit wherever their slots were given.
        off_t pos = src == BLOCK_SRC_DELTA ? delta_pos(&storage->layout, block) : 0;
        uint64_t run = 1;
        while (run < max_run && block_source(storage, block + run) == src &&
               (src != BLOCK_SRC_DELTA ||
                delta_pos(&storage->layout, block + run) == pos + (off_t)(run * storage->block_size)))
            run++;

        size_t run_bytes = (size_t)run * storage->block_size;

        if (src == BLOCK_SRC_ZERO) {
            memset(buffer, 0, run_bytes);
        } else {
            size_t got = 0;
            if (src == BLOCK_SRC_DELTA) {
                if (pos >= 0 && fseeko(storage->delta_fp, pos, SEEK_SET) == 0)
                    got = fread(buffer, 1, run_bytes, storage->delta_fp);
            } else {
                // The base: as much of the run as it holds.
                uint64_t at = block_pos(0, block, storage->block_size);
                while (got < run_bytes) {
                    int64_t n = gs_source_read(storage->base, at + got, buffer + got, run_bytes - got);
                    if (n <= 0)
                        break;
                    got += (size_t)n;
                }
            }
            if (got < run_bytes) {
                // A short read on the base means the base file is shorter than
                // the declared geometry; storage_read_block zero-fills a block
                // past the base's end and carries on, so match that.  The delta is written a block at a
                // time and every bit-set block therefore lies within EOF, so a
                // short read there is real corruption and stays an error.
                if (src == BLOCK_SRC_DELTA) {
                    rc = STATUS_E_IO;
                    break;
                }
                // storage_read_block zeroes a block it could not read *in
                // full*, so a base whose length is not a whole multiple of
                // block_size must not leak its trailing partial block.
                got -= got % storage->block_size;
                memset(buffer + got, 0, run_bytes - got);
            }
        }

        // One callback per block, not per run: the checkpoint stream is a
        // record format whose reader asserts each record's size, and
        // storage_load_state pulls it back a block at a time.
        for (uint64_t i = 0; i < run; i++) {
            if (write_cb(context, buffer + (size_t)i * storage->block_size, storage->block_size) != 0) {
                rc = STATUS_ERROR;
                break;
            }
        }
        if (rc != STATUS_OK)
            break;
        block += run;
        if (cancellable)
            io_report_progress(block * storage->block_size, storage->block_count * storage->block_size);
    }

    free(buffer);
    return rc;
}

status_t storage_save_state(storage_t *storage, void *context, storage_write_callback_t write_cb) {
    if (!storage || !context || !write_cb)
        return STATUS_E_INVAL;
    block_src_view_t v = {
        .base = storage->base,
        .delta_fp = storage->delta_fp,
        .bitmap = storage->bitmap,
        .block_count = storage->block_count,
        .block_size = storage->block_size,
        .layout = layout_of(storage),
    };
    // Not cancellable, so the result is always a status_t
    return (status_t)stream_blocks(&v, context, write_cb, false);
}

// === Export views ============================================================

struct storage_export_view {
    block_src_view_t src;
    uint8_t *bitmap_copy;
    uint32_t *table_copy; // v2: the cluster table, as the bitmap
    storage_t *storage; // locked; checked against the live registry at end
};

bool storage_export_locked(const storage_t *storage) {
    return storage && storage->export_locks > 0;
}

storage_export_view_t *storage_export_view_begin(storage_t *storage) {
    if (!storage)
        return NULL;
    storage_export_view_t *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    // What the guest wrote so far is in the delta file once flushed; the
    // bitmap copy names those blocks.
    if (storage->delta_fp)
        fflush(storage->delta_fp);
    v->bitmap_copy = malloc(storage->bitmap_bytes);
    if (!v->bitmap_copy) {
        free(v);
        return NULL;
    }
    memcpy(v->bitmap_copy, storage->bitmap, storage->bitmap_bytes);
    v->src.bitmap = v->bitmap_copy;
    v->src.block_count = storage->block_count;
    v->src.block_size = storage->block_size;
    v->src.layout = layout_of(storage);
    if (v->src.layout.cluster_blocks) {
        v->table_copy = malloc(storage->table_bytes);
        if (!v->table_copy) {
            free(v->bitmap_copy);
            free(v);
            return NULL;
        }
        memcpy(v->table_copy, storage->table, storage->table_bytes);
        v->src.layout.table = v->table_copy;
    }
    // The base source is shared (its reads are locked); the delta gets a
    // handle of its own.
    v->src.base = gs_source_retain(storage->base);
    v->src.delta_fp = storage->delta_path ? fopen(storage->delta_path, "rb") : NULL;
    if (!v->src.delta_fp) {
        gs_source_release(v->src.base);
        free(v->bitmap_copy);
        free(v->table_copy);
        free(v);
        return NULL;
    }
    v->storage = storage;
    storage->export_locks++;
    return v;
}

int storage_export_view_write(storage_export_view_t *v, void *context, storage_write_callback_t write_cb) {
    if (!v || !context || !write_cb)
        return STATUS_E_INVAL;
    return stream_blocks(&v->src, context, write_cb, true);
}

void storage_export_view_end(storage_export_view_t *v) {
    if (!v)
        return;
    gs_source_release(v->src.base);
    if (v->src.delta_fp)
        fclose(v->src.delta_fp);
    free(v->bitmap_copy);
    free(v->table_copy);
    if (v->storage && live_has(v->storage) && v->storage->export_locks > 0)
        v->storage->export_locks--;
    free(v);
}

static status_t read_exact(storage_read_callback_t read_cb, void *context, void *buf, size_t size) {
    int got = read_cb(context, buf, size);
    return (got == (int)size) ? STATUS_OK : STATUS_ERROR;
}

status_t storage_load_state(storage_t *storage, void *context, storage_read_callback_t read_cb) {
    if (!storage || !context || !read_cb)
        return STATUS_E_INVAL;

    // The stream yields a block at a time, but the delta is written a run at
    // a time: a seek and a write per block was a filesystem call per 512
    // bytes, which under WasmFS/OPFS made opening a Save State of a machine
    // with a hard disk take minutes.  A run is blocks whose delta positions
    // are contiguous, capped at the streaming chunk; if the staging
    // allocation fails, runs fall back to a single block.
    uint64_t chunk_blocks = STORAGE_STREAM_CHUNK_BYTES / storage->block_size;
    if (chunk_blocks == 0)
        chunk_blocks = 1;
    uint8_t one_block[STORAGE_MAX_BLOCK_SIZE];
    uint8_t *buffer = malloc((size_t)chunk_blocks * storage->block_size);
    if (!buffer) {
        buffer = one_block;
        chunk_blocks = 1;
    }

    status_t rc = STATUS_OK;
    uint64_t run = 0; // blocks staged in buffer
    off_t run_pos = 0; // delta position of the run's first block
    for (uint64_t block = 0; block < storage->block_count; block++) {
        // Where this block goes (a v2 delta gives each cluster a slot as it goes)
        delta_layout_t l = layout_of(storage);
        off_t pos = delta_pos(&l, block);
        if (pos < 0) {
            storage->table[block / storage->cluster_blocks] = (uint32_t)++storage->slots_used;
            pos = delta_pos(&l, block);
        }

        // Write out the staged run when this block does not extend it
        if (run && (run == chunk_blocks || pos != run_pos + (off_t)(run * storage->block_size))) {
            if (fseeko(storage->delta_fp, run_pos, SEEK_SET) != 0 ||
                fwrite(buffer, storage->block_size, run, storage->delta_fp) != run) {
                rc = STATUS_E_IO;
                break;
            }
            run = 0;
        }
        if (!run)
            run_pos = pos;

        if (read_exact(read_cb, context, buffer + (size_t)run * storage->block_size, storage->block_size) !=
            STATUS_OK) {
            rc = STATUS_ERROR;
            break;
        }
        run++;
        bitmap_set(storage->bitmap, (uint32_t)block);
    }
    if (rc == STATUS_OK && run &&
        (fseeko(storage->delta_fp, run_pos, SEEK_SET) != 0 ||
         fwrite(buffer, storage->block_size, run, storage->delta_fp) != run))
        rc = STATUS_E_IO;

    if (buffer != one_block)
        free(buffer);
    if (rc != STATUS_OK)
        return rc;

    // Commit: bitmaps → delta, clear journal
    return commit_state(storage);
}

// ============================================================================
// Public API: Maintenance
// ============================================================================

status_t storage_tick(storage_t *storage) {
    (void)storage;
    return STATUS_OK;
}
