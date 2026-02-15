// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage.c
// Directory-of-blocks storage engine implementation.

// ============================================================================
// Includes
// ============================================================================

#include "storage.h"

#include "log.h"
#include "system.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// ============================================================================
// Constants and Macros
// ============================================================================

#define MAX_PATH_LEN                            1024
#define DATA_EXTENSION                          ".dat"
#define TMP_EXTENSION                           ".tmp"
#define ROLLBACK_DIR_NAME                       "rollback"
#define ROLLBACK_EXTENSION                      ".pre"
#define META_FILENAME                           "meta.json"
#define STORAGE_MAX_LEVEL                       8
#define STORAGE_DEFAULT_CONSOLIDATIONS_PER_TICK 1
#define COPY_BUFFER_SIZE                        (64 * 1024)

LOG_USE_CATEGORY_NAME("storage")

#define STORAGE_SNAPSHOT_VERSION 1

// ============================================================================
// Type Definitions
// ============================================================================

typedef struct {
    checkpoint_t *checkpoint;
} checkpoint_stream_ctx_t;

typedef struct {
    uint32_t version;
    uint8_t has_data;
    uint8_t reserved[3];
    uint64_t block_count;
    uint32_t block_size;
} storage_snapshot_header_t;

/**
 * @brief Represents a single on-disk range file.
 */
typedef struct range_entry {
    uint32_t base_lba; // First block covered by the file
    uint8_t level; // Number of trailing hexadecimal nibbles replaced by X
} range_entry_t;

/**
 * @brief Per-level sorted index of range files.
 */
typedef struct level_index {
    range_entry_t *entries; // Sorted by base_lba
    size_t count; // Number of valid entries
    size_t capacity; // Allocated slots
} level_index_t;

/**
 * @brief Cursor used to continue consolidation scans across ticks.
 */
typedef struct consolidation_state {
    uint8_t level; // Next level to examine for consolidation
    size_t cursor; // Index within that level's vector
} consolidation_state_t;

/**
 * @brief Concrete storage handle.
 */
struct storage_t {
    char path_dir[MAX_PATH_LEN]; // Directory that holds all .dat files
    char meta_path[MAX_PATH_LEN]; // Path to meta.json
    char rollback_dir[MAX_PATH_LEN]; // Directory for rollback preimages
    uint64_t block_count; // Total logical blocks
    uint32_t block_size; // Always 512 today
    uint8_t max_level; // Highest usable consolidation level
    level_index_t levels[STORAGE_MAX_LEVEL + 1]; // Per-level indices
    consolidation_state_t consolidation; // Incremental consolidation cursor
    int consolidations_per_tick; // Budget per storage_tick()
    uint32_t *rollback_lbas; // Sorted list of LBAs with preimages
    size_t rollback_count; // Entries used in rollback_lbas
    size_t rollback_capacity; // Allocated entries in rollback_lbas
    bool rollback_capture_enabled; // Whether writes should record preimages
};

// ============================================================================
// Forward Declarations
// ============================================================================

static uint64_t pow16(uint8_t level);
static uint8_t largest_level_below_x(uint64_t x);
static int build_path(const storage_t *s, uint32_t base_lba, uint8_t level, char *out_path, size_t out_len);
static int build_rollback_path(const storage_t *s, uint32_t base_lba, char *out_path, size_t out_len);
static int parse_dat_filename(const char *name, uint32_t *base_lba, uint8_t *level);
static int parse_pre_filename(const char *name, uint32_t *lba);
static range_entry_t *level_index_insert(level_index_t *index, uint32_t base_lba, uint8_t level);
static range_entry_t *level_index_find(level_index_t *index, uint32_t base_lba);
static int level_index_find_pos(level_index_t *index, uint32_t base_lba, size_t *out_pos);
static void level_index_remove(level_index_t *index, size_t pos);
static void free_level_indices(storage_t *storage);
static void reset_level_indices(storage_t *storage);
static int rebuild_index(storage_t *storage);
static int read_block_from_entry(const storage_t *storage, const range_entry_t *entry, uint32_t lba, void *buffer);
static int remove_all_range_files(storage_t *storage);
static int read_exact(storage_read_callback_t cb, void *context, void *buffer, size_t size);
static int consolidate_group(storage_t *storage, uint8_t level, size_t start_index);
static int find_consolidation_candidate(storage_t *storage, uint8_t *out_level, size_t *out_index);
static int checkpoint_storage_write_cb(void *ctx, const void *data, size_t size);
static int checkpoint_storage_read_cb(void *ctx, void *data, size_t size);
static int storage_skip_snapshot(checkpoint_t *checkpoint, const storage_snapshot_header_t *header);
static int ensure_directory(const char *path);
static void rollback_reset_index(storage_t *storage);
static void rollback_free_index(storage_t *storage);
static bool rollback_find(const storage_t *storage, uint32_t lba, size_t *out_pos);
static int rollback_insert(storage_t *storage, uint32_t lba, size_t pos);
static int rollback_capture_preimage(storage_t *storage, uint32_t lba);
static int rollback_load_index(storage_t *storage);
static int rollback_clear_on_disk(storage_t *storage);
static int rollback_apply_internal(storage_t *storage);
static int storage_write_block_impl(storage_t *storage, size_t offset, const void *buffer, bool capture_preimage);
/* run_consolidation inlined into storage_tick; prototype removed. */

// ============================================================================
// Static Helpers
// ============================================================================

// Returns 16^level as a 64-bit integer.
static uint64_t pow16(uint8_t level) {
    uint64_t value = 1;
    for (uint8_t i = 0; i < level; ++i) {
        value *= 16ULL;
    }
    return value;
}

// Determines the largest level allowed by block_count.
static uint8_t largest_level_below_x(uint64_t x) {
    for (int level = (int)STORAGE_MAX_LEVEL; level >= 0; --level) {
        uint64_t span = pow16((uint8_t)level);
        if (span > x) {
            continue;
        }
        return (uint8_t)level;
    }
    return 0;
}

static int ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? GS_SUCCESS : GS_ERROR;
    }
    if (errno != ENOENT) {
        return GS_ERROR;
    }
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        return GS_ERROR;
    }
    return GS_SUCCESS;
}

// Builds an absolute path to the backing file.
static int build_path(const storage_t *s, uint32_t base_lba, uint8_t level, char *out_path, size_t out_len) {
    char pattern[9];
    snprintf(pattern, 9, "%08X", base_lba);
    for (uint8_t i = 0; i < level && i < 8; ++i) {
        pattern[7 - i] = 'X';
    }
    int written = snprintf(out_path, out_len, "%s/%s%s", s->path_dir, pattern, DATA_EXTENSION);
    if (written < 0 || (size_t)written >= out_len) {
        return GS_ERROR;
    }
    return GS_SUCCESS;
}

static int build_rollback_path(const storage_t *s, uint32_t base_lba, char *out_path, size_t out_len) {
    if (!s || !out_path) {
        return GS_ERROR;
    }
    int written = snprintf(out_path, out_len, "%s/%08X%s", s->rollback_dir, base_lba, ROLLBACK_EXTENSION);
    if (written < 0 || (size_t)written >= out_len) {
        return GS_ERROR;
    }
    return GS_SUCCESS;
}

// Attempts to parse an "XXXXXXXX.dat" filename into (base_lba, level).
static int parse_dat_filename(const char *name, uint32_t *base_lba, uint8_t *level) {
    size_t len = strlen(name);
    if (len != 12) {
        return GS_ERROR;
    }
    if (strcasecmp(name + 8, DATA_EXTENSION) != 0) {
        return GS_ERROR;
    }
    char pattern[9];
    memcpy(pattern, name, 8);
    pattern[8] = '\0';

    int trailing_x = 0;
    for (int i = 7; i >= 0; --i) {
        char c = pattern[i];
        if (c == 'X' || c == 'x') {
            trailing_x++;
            pattern[i] = '0';
            continue;
        }
        break;
    }
    for (int i = 0; i < 8 - trailing_x; ++i) {
        if (!isxdigit((unsigned char)pattern[i])) {
            return GS_ERROR;
        }
    }
    uint64_t value = 0;
    if (sscanf(pattern, "%" SCNx64, &value) != 1) {
        return GS_ERROR;
    }
    if (value > UINT32_MAX) {
        return GS_ERROR;
    }
    if (trailing_x > STORAGE_MAX_LEVEL) {
        return GS_ERROR;
    }
    *base_lba = (uint32_t)value;
    *level = (uint8_t)trailing_x;
    return GS_SUCCESS;
}

static int parse_pre_filename(const char *name, uint32_t *lba) {
    size_t len = strlen(name);
    size_t ext_len = strlen(ROLLBACK_EXTENSION);
    if (len != 8 + ext_len) {
        return GS_ERROR;
    }
    if (strcasecmp(name + len - ext_len, ROLLBACK_EXTENSION) != 0) {
        return GS_ERROR;
    }
    char pattern[9];
    memcpy(pattern, name, 8);
    pattern[8] = '\0';
    for (int i = 0; i < 8; ++i) {
        if (!isxdigit((unsigned char)pattern[i])) {
            return GS_ERROR;
        }
    }
    uint64_t value = 0;
    if (sscanf(pattern, "%" SCNx64, &value) != 1) {
        return GS_ERROR;
    }
    if (value > UINT32_MAX) {
        return GS_ERROR;
    }
    *lba = (uint32_t)value;
    return GS_SUCCESS;
}

// (inlined reserve logic removed; handled inside level_index_insert)

// Inserts or replaces a range entry.
static range_entry_t *level_index_insert(level_index_t *index, uint32_t base_lba, uint8_t level) {
    size_t pos = 0;
    if (level_index_find_pos(index, base_lba, &pos) == GS_SUCCESS) {
        index->entries[pos].base_lba = base_lba;
        index->entries[pos].level = level;
        return &index->entries[pos];
    }
    /* Ensure capacity for one more entry (inline of level_index_reserve). */
    size_t need = index->count + 1;
    if (index->capacity < need) {
        size_t new_capacity = index->capacity ? index->capacity : 8;
        while (new_capacity < need) {
            new_capacity *= 2;
        }
        range_entry_t *new_entries = (range_entry_t *)realloc(index->entries, new_capacity * sizeof(range_entry_t));
        if (!new_entries) {
            return NULL;
        }
        index->entries = new_entries;
        index->capacity = new_capacity;
    }
    memmove(&index->entries[pos + 1], &index->entries[pos], (index->count - pos) * sizeof(range_entry_t));
    index->entries[pos].base_lba = base_lba;
    index->entries[pos].level = level;
    index->count++;
    return &index->entries[pos];
}

// Finds an entry by base_lba.
static range_entry_t *level_index_find(level_index_t *index, uint32_t base_lba) {
    size_t pos = 0;
    if (level_index_find_pos(index, base_lba, &pos) != GS_SUCCESS) {
        return NULL;
    }
    return &index->entries[pos];
}

// Finds the position of base_lba using binary search.
static int level_index_find_pos(level_index_t *index, uint32_t base_lba, size_t *out_pos) {
    size_t lo = 0;
    size_t hi = index->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t value = index->entries[mid].base_lba;
        if (value == base_lba) {
            *out_pos = mid;
            return GS_SUCCESS;
        }
        if (value < base_lba) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *out_pos = lo;
    return GS_ERROR;
}

// Removes the entry at the provided position.
static void level_index_remove(level_index_t *index, size_t pos) {
    if (pos >= index->count) {
        return;
    }
    memmove(&index->entries[pos], &index->entries[pos + 1], (index->count - pos - 1) * sizeof(range_entry_t));
    index->count--;
}

// Releases all memory owned by the indices.
static void free_level_indices(storage_t *storage) {
    for (size_t i = 0; i <= STORAGE_MAX_LEVEL; ++i) {
        free(storage->levels[i].entries);
        storage->levels[i].entries = NULL;
        storage->levels[i].count = 0;
        storage->levels[i].capacity = 0;
    }
}

// Clears all entries without freeing the buffers.
static void reset_level_indices(storage_t *storage) {
    for (size_t i = 0; i <= STORAGE_MAX_LEVEL; ++i) {
        storage->levels[i].count = 0;
    }
}

static void rollback_reset_index(storage_t *storage) {
    if (!storage) {
        return;
    }
    storage->rollback_count = 0;
}

static void rollback_free_index(storage_t *storage) {
    if (!storage) {
        return;
    }
    free(storage->rollback_lbas);
    storage->rollback_lbas = NULL;
    storage->rollback_count = 0;
    storage->rollback_capacity = 0;
}

static bool rollback_find(const storage_t *storage, uint32_t lba, size_t *out_pos) {
    if (!storage) {
        if (out_pos) {
            *out_pos = 0;
        }
        return false;
    }
    size_t lo = 0;
    size_t hi = storage->rollback_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t value = storage->rollback_lbas[mid];
        if (value == lba) {
            if (out_pos) {
                *out_pos = mid;
            }
            return true;
        }
        if (value < lba) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (out_pos) {
        *out_pos = lo;
    }
    return false;
}

static int rollback_insert(storage_t *storage, uint32_t lba, size_t pos) {
    if (!storage) {
        return GS_ERROR;
    }
    size_t need = storage->rollback_count + 1;
    if (storage->rollback_capacity < need) {
        size_t new_capacity = storage->rollback_capacity ? storage->rollback_capacity : 32;
        while (new_capacity < need) {
            new_capacity *= 2;
        }
        uint32_t *resized = (uint32_t *)realloc(storage->rollback_lbas, new_capacity * sizeof(uint32_t));
        if (!resized) {
            return GS_ERROR;
        }
        storage->rollback_lbas = resized;
        storage->rollback_capacity = new_capacity;
    }
    memmove(&storage->rollback_lbas[pos + 1], &storage->rollback_lbas[pos],
            (storage->rollback_count - pos) * sizeof(uint32_t));
    storage->rollback_lbas[pos] = lba;
    storage->rollback_count++;
    return GS_SUCCESS;
}

/* read_meta and write_meta inlined into storage_new; helpers removed */

// Rebuilds the in-memory index by scanning the directory.
static int rebuild_index(storage_t *storage) {
    reset_level_indices(storage);
    DIR *dir = opendir(storage->path_dir);
    if (!dir) {
        return GS_ERROR;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        uint32_t base_lba = 0;
        uint8_t level = 0;
        if (parse_dat_filename(entry->d_name, &base_lba, &level) != GS_SUCCESS) {
            continue;
        }
        if (level > storage->max_level) {
            continue;
        }
        if (!level_index_insert(&storage->levels[level], base_lba, level)) {
            closedir(dir);
            return GS_ERROR;
        }
    }
    closedir(dir);
    return GS_SUCCESS;
}

// Reads a block from the file described by entry.
static int read_block_from_entry(const storage_t *storage, const range_entry_t *entry, uint32_t lba, void *buffer) {
    if (!storage || !entry || !buffer) {
        return GS_ERROR;
    }
    char path[MAX_PATH_LEN];
    if (build_path(storage, entry->base_lba, entry->level, path, sizeof(path)) != GS_SUCCESS) {
        return GS_ERROR;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return GS_ERROR;
    }
    uint64_t offset_blocks = (uint64_t)lba - (uint64_t)entry->base_lba;
    uint64_t byte_offset = offset_blocks * storage->block_size;
    if (fseeko(f, (off_t)byte_offset, SEEK_SET) != 0) {
        fclose(f);
        return GS_ERROR;
    }
    size_t read = fread(buffer, storage->block_size, 1, f);
    fclose(f);
    return (read == 1) ? GS_SUCCESS : GS_ERROR;
}

static int rollback_capture_preimage(storage_t *storage, uint32_t lba) {
    if (!storage) {
        return GS_ERROR;
    }
    size_t pos = 0;
    if (rollback_find(storage, lba, &pos)) {
        return GS_SUCCESS;
    }
    uint8_t buffer[STORAGE_BLOCK_SIZE];
    int rc = storage_read_block(storage, (size_t)lba * storage->block_size, buffer);
    if (rc != GS_SUCCESS) {
        return rc;
    }
    char path[MAX_PATH_LEN];
    char tmp[MAX_PATH_LEN];
    if (build_rollback_path(storage, lba, path, sizeof(path)) != GS_SUCCESS) {
        return GS_ERROR;
    }
    if (snprintf(tmp, sizeof(tmp), "%s%s", path, TMP_EXTENSION) >= (int)sizeof(tmp)) {
        return GS_ERROR;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return GS_ERROR;
    }
    size_t written = fwrite(buffer, storage->block_size, 1, f);
    if (written != 1 || fclose(f) != 0) {
        remove(tmp);
        return GS_ERROR;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return GS_ERROR;
    }
    return rollback_insert(storage, lba, pos);
}

static int rollback_load_index(storage_t *storage) {
    if (!storage) {
        return GS_ERROR;
    }
    if (ensure_directory(storage->rollback_dir) != GS_SUCCESS) {
        return GS_ERROR;
    }
    rollback_reset_index(storage);
    DIR *dir = opendir(storage->rollback_dir);
    if (!dir) {
        return GS_ERROR;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        uint32_t lba = 0;
        if (parse_pre_filename(entry->d_name, &lba) != GS_SUCCESS) {
            continue;
        }
        size_t pos = 0;
        if (rollback_find(storage, lba, &pos)) {
            continue;
        }
        if (rollback_insert(storage, lba, pos) != GS_SUCCESS) {
            closedir(dir);
            return GS_ERROR;
        }
    }
    closedir(dir);
    return GS_SUCCESS;
}

static int rollback_clear_on_disk(storage_t *storage) {
    if (!storage) {
        return GS_ERROR;
    }
    if (ensure_directory(storage->rollback_dir) != GS_SUCCESS) {
        return GS_ERROR;
    }
    DIR *dir = opendir(storage->rollback_dir);
    if (!dir) {
        return GS_ERROR;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[MAX_PATH_LEN];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", storage->rollback_dir, entry->d_name) >= sizeof(path)) {
            closedir(dir);
            return GS_ERROR;
        }
        remove(path);
    }
    closedir(dir);
    return GS_SUCCESS;
}

static int rollback_apply_internal(storage_t *storage) {
    if (!storage) {
        return GS_ERROR;
    }
    if (ensure_directory(storage->rollback_dir) != GS_SUCCESS) {
        return GS_ERROR;
    }
    DIR *dir = opendir(storage->rollback_dir);
    if (!dir) {
        return GS_SUCCESS;
    }
    uint8_t buffer[STORAGE_BLOCK_SIZE];
    int rc = GS_SUCCESS;
    storage->rollback_capture_enabled = false;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        uint32_t lba = 0;
        if (parse_pre_filename(entry->d_name, &lba) != GS_SUCCESS) {
            continue;
        }
        char path[MAX_PATH_LEN];
        if (build_rollback_path(storage, lba, path, sizeof(path)) != GS_SUCCESS) {
            rc = GS_ERROR;
            break;
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            rc = GS_ERROR;
            break;
        }
        size_t read = fread(buffer, storage->block_size, 1, f);
        fclose(f);
        if (read != 1) {
            rc = GS_ERROR;
            break;
        }
        rc = storage_write_block_impl(storage, (size_t)lba * storage->block_size, buffer, false);
        if (rc != GS_SUCCESS) {
            break;
        }
        if (remove(path) != 0) {
            rc = GS_ERROR;
            break;
        }
    }
    closedir(dir);
    storage->rollback_capture_enabled = true;
    if (rc == GS_SUCCESS) {
        rollback_reset_index(storage);
    }
    return rc;
}

// Removes every .dat and stray tmp file from the directory.
static int remove_all_range_files(storage_t *storage) {
    DIR *dir = opendir(storage->path_dir);
    if (!dir) {
        return GS_ERROR;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        size_t len = strlen(entry->d_name);
        bool is_dat = len >= 4 && strcasecmp(entry->d_name + len - 4, DATA_EXTENSION) == 0;
        bool is_tmp = len >= 4 && strcasecmp(entry->d_name + len - 4, TMP_EXTENSION) == 0;
        if (!is_dat && !is_tmp) {
            continue;
        }
        char path[MAX_PATH_LEN];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", storage->path_dir, entry->d_name) >= sizeof(path)) {
            closedir(dir);
            return GS_ERROR;
        }
        remove(path);
    }
    closedir(dir);
    reset_level_indices(storage);
    storage->consolidation.level = 0;
    storage->consolidation.cursor = 0;
    return GS_SUCCESS;
}

// Reads exactly size bytes from read_cb into buffer.
static int read_exact(storage_read_callback_t cb, void *context, void *buffer, size_t size) {
    uint8_t *dst = (uint8_t *)buffer;
    size_t total = 0;
    while (total < size) {
        int got = cb(context, dst + total, size - total);
        if (got <= 0) {
            return GS_ERROR;
        }
        total += (size_t)got;
    }
    return GS_SUCCESS;
}

// Copies 16 sibling files into a larger parent file and removes the siblings.
static int consolidate_group(storage_t *storage, uint8_t level, size_t start_index) {
    if (level >= STORAGE_MAX_LEVEL) {
        return GS_SUCCESS;
    }
    level_index_t *idx = &storage->levels[level];
    if (start_index + 16 > idx->count) {
        return GS_ERROR;
    }
    uint64_t child_span = pow16(level);
    uint32_t parent_base = idx->entries[start_index].base_lba;
    uint8_t parent_level = level + 1;
    char parent_path[MAX_PATH_LEN];
    char parent_tmp[MAX_PATH_LEN];
    if (build_path(storage, parent_base, parent_level, parent_path, sizeof(parent_path)) != GS_SUCCESS) {
        return GS_ERROR;
    }
    if (snprintf(parent_tmp, sizeof(parent_tmp), "%s%s", parent_path, TMP_EXTENSION) >= (int)sizeof(parent_tmp)) {
        return GS_ERROR;
    }
    FILE *dst = fopen(parent_tmp, "wb");
    if (!dst) {
        return GS_ERROR;
    }
    uint8_t *buffer = (uint8_t *)malloc(COPY_BUFFER_SIZE);
    if (!buffer) {
        fclose(dst);
        remove(parent_tmp);
        return GS_ERROR;
    }
    uint32_t child_bases[16];
    for (size_t i = 0; i < 16; ++i) {
        child_bases[i] = idx->entries[start_index + i].base_lba;
    }
    for (size_t child = 0; child < 16; ++child) {
        char child_path[MAX_PATH_LEN];
        if (build_path(storage, child_bases[child], level, child_path, sizeof(child_path)) != GS_SUCCESS) {
            free(buffer);
            fclose(dst);
            remove(parent_tmp);
            return GS_ERROR;
        }
        FILE *src = fopen(child_path, "rb");
        if (!src) {
            free(buffer);
            fclose(dst);
            remove(parent_tmp);
            return GS_ERROR;
        }
        uint64_t bytes_remaining = child_span * storage->block_size;
        while (bytes_remaining > 0) {
            size_t chunk = (bytes_remaining > COPY_BUFFER_SIZE) ? COPY_BUFFER_SIZE : (size_t)bytes_remaining;
            size_t read = fread(buffer, 1, chunk, src);
            if (read != chunk) {
                fclose(src);
                free(buffer);
                fclose(dst);
                remove(parent_tmp);
                return GS_ERROR;
            }
            if (fwrite(buffer, 1, chunk, dst) != chunk) {
                fclose(src);
                free(buffer);
                fclose(dst);
                remove(parent_tmp);
                return GS_ERROR;
            }
            bytes_remaining -= chunk;
        }
        fclose(src);
    }
    free(buffer);
    if (fclose(dst) != 0) {
        remove(parent_tmp);
        return GS_ERROR;
    }
    if (rename(parent_tmp, parent_path) != 0) {
        remove(parent_tmp);
        return GS_ERROR;
    }
    if (!level_index_insert(&storage->levels[parent_level], parent_base, parent_level)) {
        remove(parent_path);
        return GS_ERROR;
    }
    for (size_t child = 0; child < 16; ++child) {
        char child_path[MAX_PATH_LEN];
        if (build_path(storage, child_bases[child], level, child_path, sizeof(child_path)) != GS_SUCCESS) {
            continue;
        }
        remove(child_path);
        size_t pos = 0;
        if (level_index_find_pos(idx, child_bases[child], &pos) == GS_SUCCESS) {
            level_index_remove(idx, pos);
        }
    }
    return GS_SUCCESS;
}

// Finds the next consolidation candidate using the rolling cursor.
static int find_consolidation_candidate(storage_t *storage, uint8_t *out_level, size_t *out_index) {
    if (storage->max_level == 0) {
        return GS_ERROR;
    }
    uint8_t start_level = storage->consolidation.level;
    size_t start_cursor = storage->consolidation.cursor;
    for (uint8_t pass = 0; pass < storage->max_level; ++pass) {
        uint8_t level = (uint8_t)((start_level + pass) % storage->max_level);
        level_index_t *idx = &storage->levels[level];
        if (idx->count < 16) {
            if (pass + 1 == storage->max_level) {
                storage->consolidation.cursor = 0;
            }
            continue;
        }
        size_t cursor = (level == start_level) ? start_cursor : 0;
        uint64_t span = pow16(level);
        uint64_t parent_span = span * 16ULL;
        for (; cursor + 15 < idx->count; ++cursor) {
            uint32_t base = idx->entries[cursor].base_lba;
            if (((uint64_t)base % parent_span) != 0) {
                continue;
            }
            bool sequential = true;
            for (size_t i = 1; i < 16; ++i) {
                if (idx->entries[cursor + i].base_lba != base + (uint32_t)(span * i)) {
                    sequential = false;
                    break;
                }
            }
            if (!sequential) {
                continue;
            }
            *out_level = level;
            *out_index = cursor;
            storage->consolidation.level = level;
            storage->consolidation.cursor = cursor;
            return GS_SUCCESS;
        }
        storage->consolidation.cursor = 0;
    }
    return GS_ERROR;
}

// Executes up to consolidations_per_tick merges.
static int run_consolidation(storage_t *storage) {
    if (storage->consolidations_per_tick <= 0) {
        return GS_SUCCESS;
    }
    int remaining = storage->consolidations_per_tick;
    while (remaining > 0) {
        uint8_t level = 0;
        size_t index = 0;
        if (find_consolidation_candidate(storage, &level, &index) != GS_SUCCESS) {
            break;
        }
        int rc = consolidate_group(storage, level, index);
        if (rc != GS_SUCCESS) {
            return rc;
        }
        remaining--;
    }
    return GS_SUCCESS;
}

static int checkpoint_storage_write_cb(void *ctx, const void *data, size_t size) {
    checkpoint_stream_ctx_t *stream = (checkpoint_stream_ctx_t *)ctx;
    if (!stream || !stream->checkpoint) {
        return -1;
    }
    system_write_checkpoint_data(stream->checkpoint, data, size);
    return checkpoint_has_error(stream->checkpoint) ? -1 : 0;
}

static int checkpoint_storage_read_cb(void *ctx, void *data, size_t size) {
    checkpoint_stream_ctx_t *stream = (checkpoint_stream_ctx_t *)ctx;
    if (!stream || !stream->checkpoint) {
        return 0;
    }
    system_read_checkpoint_data(stream->checkpoint, data, size);
    if (checkpoint_has_error(stream->checkpoint)) {
        return 0;
    }
    return (int)size;
}

static int storage_skip_snapshot(checkpoint_t *checkpoint, const storage_snapshot_header_t *header) {
    if (!checkpoint || !header) {
        return GS_ERROR;
    }
    if (!header->has_data) {
        return GS_SUCCESS;
    }
    if (header->block_size == 0 || header->block_count == 0) {
        return GS_ERROR;
    }
    uint8_t *scratch = (uint8_t *)malloc(header->block_size);
    if (!scratch) {
        return GS_ERROR;
    }
    for (uint64_t block = 0; block < header->block_count; ++block) {
        system_read_checkpoint_data(checkpoint, scratch, header->block_size);
        if (checkpoint_has_error(checkpoint)) {
            free(scratch);
            return GS_ERROR;
        }
    }
    free(scratch);
    return GS_SUCCESS;
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

int storage_new(const storage_config_t *config, storage_t **out_storage) {
    if (!config || !out_storage || !config->path_dir) {
        return GS_ERROR;
    }
    if (config->block_size != 0 && config->block_size != STORAGE_BLOCK_SIZE) {
        return GS_ERROR;
    }
    if (config->block_count == 0 || config->block_count > (1ULL << 32)) {
        return GS_ERROR;
    }

    storage_t *storage = (storage_t *)calloc(1, sizeof(storage_t));
    if (!storage) {
        return GS_ERROR;
    }

    storage->block_count = config->block_count;
    storage->block_size = STORAGE_BLOCK_SIZE;
    storage->max_level = largest_level_below_x(storage->block_count);
    storage->rollback_lbas = NULL;
    storage->rollback_count = 0;
    storage->rollback_capacity = 0;
    storage->rollback_capture_enabled = true;

    if ((size_t)snprintf(storage->path_dir, sizeof(storage->path_dir), "%s", config->path_dir) >=
        sizeof(storage->path_dir)) {
        goto cleanup;
    }

    struct stat st;
    if (stat(storage->path_dir, &st) != 0) {
        if (mkdir(storage->path_dir, 0777) != 0 && errno != EEXIST) {
            goto cleanup;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        goto cleanup;
    }

    if ((size_t)snprintf(storage->meta_path, sizeof(storage->meta_path), "%s/%s", storage->path_dir, META_FILENAME) >=
        sizeof(storage->meta_path)) {
        goto cleanup;
    }
    if ((size_t)snprintf(storage->rollback_dir, sizeof(storage->rollback_dir), "%s/%s", storage->path_dir,
                         ROLLBACK_DIR_NAME) >= sizeof(storage->rollback_dir)) {
        goto cleanup;
    }
    if (ensure_directory(storage->rollback_dir) != GS_SUCCESS) {
        goto cleanup;
    }

    if (config->consolidations_per_tick < 0) {
        storage->consolidations_per_tick = 0;
    } else if (config->consolidations_per_tick == 0) {
        storage->consolidations_per_tick = STORAGE_DEFAULT_CONSOLIDATIONS_PER_TICK;
    } else {
        storage->consolidations_per_tick = config->consolidations_per_tick;
    }

    bool need_meta_write = false;
    FILE *meta_file = fopen(storage->meta_path, "rb");
    if (!meta_file) {
        if (errno == ENOENT) {
            need_meta_write = true;
        } else {
            goto cleanup;
        }
    } else {
        char buffer[256];
        size_t read = fread(buffer, 1, sizeof(buffer) - 1, meta_file);
        fclose(meta_file);
        buffer[read] = '\0';
        uint64_t bc = 0;
        uint32_t bs = 0;
        if (sscanf(buffer, "{\"block_count\":%" SCNu64 ",\"block_size\":%u", &bc, &bs) != 2) {
            goto cleanup;
        }
        if (bc != storage->block_count || bs != storage->block_size) {
            goto cleanup;
        }
    }

    if (need_meta_write) {
        FILE *meta_out = fopen(storage->meta_path, "wb");
        if (!meta_out) {
            goto cleanup;
        }
        if (fprintf(meta_out, "{\"block_count\":%" PRIu64 ",\"block_size\":%u}\n", storage->block_count,
                    storage->block_size) < 0) {
            fclose(meta_out);
            goto cleanup;
        }
        if (fclose(meta_out) != 0) {
            goto cleanup;
        }
    }

    if (rebuild_index(storage) != GS_SUCCESS) {
        goto cleanup;
    }
    if (rollback_load_index(storage) != GS_SUCCESS) {
        goto cleanup;
    }

    *out_storage = storage;
    return GS_SUCCESS;

cleanup:
    free_level_indices(storage);
    rollback_free_index(storage);
    free(storage);
    return GS_ERROR;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

int storage_delete(storage_t *storage) {
    if (!storage) {
        return GS_SUCCESS;
    }
    free_level_indices(storage);
    rollback_free_index(storage);
    free(storage);
    return GS_SUCCESS;
}

// ============================================================================
// Operations
// ============================================================================

int storage_read_block(storage_t *storage, size_t offset, void *buffer) {
    if (!storage || !buffer) {
        return GS_ERROR;
    }
    if (offset % storage->block_size != 0) {
        return GS_ERROR;
    }
    uint64_t lba64 = offset / storage->block_size;
    if (lba64 >= storage->block_count) {
        return GS_ERROR;
    }
    uint32_t lba = (uint32_t)lba64;
    for (uint8_t level = 0; level <= storage->max_level; ++level) {
        uint64_t span = pow16(level);
        uint64_t base64 = lba64 - (lba64 % span);
        range_entry_t *entry = level_index_find(&storage->levels[level], (uint32_t)base64);
        if (!entry) {
            continue;
        }
        return read_block_from_entry(storage, entry, lba, buffer);
    }
    memset(buffer, 0, storage->block_size);
    return GS_SUCCESS;
}

static int storage_write_block_impl(storage_t *storage, size_t offset, const void *buffer, bool capture_preimage) {
    if (!storage || !buffer) {
        return GS_ERROR;
    }
    if (offset % storage->block_size != 0) {
        return GS_ERROR;
    }
    uint64_t lba64 = offset / storage->block_size;
    if (lba64 >= storage->block_count) {
        return GS_ERROR;
    }
    uint32_t lba = (uint32_t)lba64;
    if (capture_preimage && storage->rollback_capture_enabled) {
        int rc = rollback_capture_preimage(storage, lba);
        if (rc != GS_SUCCESS) {
            return rc;
        }
    }
    char path[MAX_PATH_LEN];
    char tmp[MAX_PATH_LEN];
    int rc = build_path(storage, lba, 0, path, sizeof(path));
    if (rc != GS_SUCCESS) {
        return rc;
    }
    if (snprintf(tmp, sizeof(tmp), "%s%s", path, TMP_EXTENSION) >= (int)sizeof(tmp)) {
        return GS_ERROR;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return GS_ERROR;
    }
    size_t written = fwrite(buffer, storage->block_size, 1, f);
    if (written != 1 || fclose(f) != 0) {
        remove(tmp);
        return GS_ERROR;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return GS_ERROR;
    }
    if (!level_index_find(&storage->levels[0], lba)) {
        if (!level_index_insert(&storage->levels[0], lba, 0)) {
            return GS_ERROR;
        }
    }
    return GS_SUCCESS;
}

// Write a block to storage with deferred consolidation
int storage_write_block(storage_t *storage, size_t offset, const void *buffer) {
    return storage_write_block_impl(storage, offset, buffer, true);
}

// Periodic tick for background consolidation tasks
int storage_tick(storage_t *storage) {
    if (!storage) {
        return GS_ERROR;
    }
    return run_consolidation(storage);
}

// ============================================================================
// Lifecycle: Checkpointing
// ============================================================================

int storage_checkpoint(storage_t *storage, checkpoint_t *checkpoint) {
    if (!storage) {
        return GS_ERROR;
    }
    if (!checkpoint) {
        return GS_SUCCESS;
    }
    storage_snapshot_header_t header = {0};
    header.version = STORAGE_SNAPSHOT_VERSION;
    header.has_data = (checkpoint_get_kind(checkpoint) == CHECKPOINT_KIND_CONSOLIDATED) ? 1 : 0;
    header.block_count = storage->block_count;
    header.block_size = storage->block_size;
    system_write_checkpoint_data(checkpoint, &header, sizeof(header));
    if (checkpoint_has_error(checkpoint)) {
        return GS_ERROR;
    }
    if (!header.has_data) {
        return storage_clear_rollback(storage);
    }
    checkpoint_stream_ctx_t ctx = {checkpoint};
    int rc = storage_save_state(storage, &ctx, checkpoint_storage_write_cb);
    if (rc != GS_SUCCESS) {
        return rc;
    }
    return storage_clear_rollback(storage);
}

int storage_restore_from_checkpoint(storage_t *storage, checkpoint_t *checkpoint) {
    if (!checkpoint) {
        return GS_ERROR;
    }
    storage_snapshot_header_t header = {0};
    system_read_checkpoint_data(checkpoint, &header, sizeof(header));
    if (checkpoint_has_error(checkpoint)) {
        return GS_ERROR;
    }
    if (header.version != STORAGE_SNAPSHOT_VERSION) {
        return GS_ERROR;
    }
    if (!storage) {
        return storage_skip_snapshot(checkpoint, &header);
    }
    if (header.block_count != storage->block_count || header.block_size != storage->block_size) {
        return GS_ERROR;
    }
    if (!header.has_data) {
        return storage_apply_rollback(storage);
    }
    checkpoint_stream_ctx_t ctx = {checkpoint};
    return storage_load_state(storage, &ctx, checkpoint_storage_read_cb);
}

// Apply accumulated rollback changes to base storage
int storage_apply_rollback(storage_t *storage) {
    if (!storage) {
        return GS_ERROR;
    }
    int rc = rollback_apply_internal(storage);
    if (rc != GS_SUCCESS) {
        return rc;
    }
    return rollback_load_index(storage);
}

// Clear the rollback layer, discarding all pending changes
int storage_clear_rollback(storage_t *storage) {
    if (!storage) {
        return GS_ERROR;
    }
    int rc = rollback_clear_on_disk(storage);
    if (rc != GS_SUCCESS) {
        return rc;
    }
    rollback_reset_index(storage);
    return GS_SUCCESS;
}

// Save storage state by calling write callback for each block
int storage_save_state(storage_t *storage, void *context, storage_write_callback_t write_cb) {
    if (!storage || !context || !write_cb) {
        return GS_ERROR;
    }
    uint8_t buffer[STORAGE_BLOCK_SIZE];
    for (uint64_t block = 0; block < storage->block_count; ++block) {
        size_t offset = (size_t)block * storage->block_size;
        int rc = storage_read_block(storage, offset, buffer);
        if (rc != GS_SUCCESS) {
            return rc;
        }
        if (write_cb(context, buffer, storage->block_size) != 0) {
            return GS_ERROR;
        }
    }
    return GS_SUCCESS;
}

// Load storage state by calling read callback for each block
int storage_load_state(storage_t *storage, void *context, storage_read_callback_t read_cb) {
    if (!storage || !context || !read_cb) {
        return GS_ERROR;
    }
    int rc = storage_clear_rollback(storage);
    if (rc != GS_SUCCESS) {
        return rc;
    }
    rc = remove_all_range_files(storage);
    if (rc != GS_SUCCESS) {
        return rc;
    }
    uint64_t remaining = storage->block_count;
    uint64_t cursor = 0;
    while (remaining > 0) {
        uint8_t level = largest_level_below_x(remaining);
        uint64_t span = pow16(level);
        assert((cursor % span) == 0);
        if (cursor > UINT32_MAX) {
            remove_all_range_files(storage);
            return GS_ERROR;
        }
        {
            char path[MAX_PATH_LEN];
            char tmp[MAX_PATH_LEN];
            if (build_path(storage, (uint32_t)cursor, level, path, sizeof(path)) != GS_SUCCESS) {
                rc = GS_ERROR;
            } else if (snprintf(tmp, sizeof(tmp), "%s%s", path, TMP_EXTENSION) >= (int)sizeof(tmp)) {
                rc = GS_ERROR;
            } else {
                FILE *f = fopen(tmp, "wb");
                if (!f) {
                    rc = GS_ERROR;
                } else {
                    uint64_t blocks = pow16(level);
                    uint8_t buffer[STORAGE_BLOCK_SIZE];
                    bool io_error = false;
                    for (uint64_t i = 0; i < blocks; ++i) {
                        if (read_exact(read_cb, context, buffer, storage->block_size) != GS_SUCCESS) {
                            io_error = true;
                            break;
                        }
                        if (fwrite(buffer, storage->block_size, 1, f) != 1) {
                            io_error = true;
                            break;
                        }
                    }
                    if (io_error) {
                        fclose(f);
                        remove(tmp);
                        rc = GS_ERROR;
                    } else if (fclose(f) != 0) {
                        remove(tmp);
                        rc = GS_ERROR;
                    } else if (rename(tmp, path) != 0) {
                        remove(tmp);
                        rc = GS_ERROR;
                    } else if (!level_index_insert(&storage->levels[level], (uint32_t)cursor, level)) {
                        /* If we can't insert into the index, clean up the file we just created. */
                        remove(path);
                        rc = GS_ERROR;
                    } else {
                        rc = GS_SUCCESS;
                    }
                }
            }
        }
        if (rc != GS_SUCCESS) {
            remove_all_range_files(storage);
            return rc;
        }
        cursor += span;
        remaining -= span;
    }
    return GS_SUCCESS;
}
