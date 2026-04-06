// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image.c
// Disk image handling: open, read/write, and format detection for floppy and hard disk images.
//
// Each image is backed by a delta-file storage engine.  The original image file
// is opened read-only as the "base"; all modifications go to a .delta file.
// A .journal file provides crash recovery.

#include "image.h"

#include "log.h"
#include "platform.h"
#include "system.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(_WIN32)
#include <direct.h>
#endif

LOG_USE_CATEGORY_NAME("image")

#define DISKCOPY_HEADER_SIZE 0x54

// ============================================================================
// String / path helpers
// ============================================================================

static char *dup_string(const char *src) {
    if (!src)
        return NULL;
    size_t len = strlen(src) + 1;
    char *copy = (char *)malloc(len);
    if (!copy)
        return NULL;
    memcpy(copy, src, len);
    return copy;
}

static char *str_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (needed < 0) {
        va_end(ap);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)needed + 1);
    if (!buf) {
        va_end(ap);
        return NULL;
    }
    vsnprintf(buf, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    return buf;
}

static int mkdir_if_needed(const char *path) {
    if (!path || !*path)
        return -1;
#if defined(_WIN32)
    int rc = _mkdir(path);
#else
    int rc = mkdir(path, 0777);
#endif
    if (rc == 0 || errno == EEXIST)
        return 0;
    return -1;
}

// Recursively create directories (like mkdir -p)
static int mkdir_recursive(const char *path) {
    if (!path || !*path)
        return -1;
    char *tmp = dup_string(path);
    if (!tmp)
        return -1;
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir_if_needed(tmp) != 0) {
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }
    int rc = mkdir_if_needed(tmp);
    free(tmp);
    return rc;
}

// Ensure all parent directories of a file path exist
static int ensure_parent_dirs(const char *filepath) {
    if (!filepath || !*filepath)
        return -1;
    char *tmp = dup_string(filepath);
    if (!tmp)
        return -1;
    char *last_sep = strrchr(tmp, '/');
    if (!last_sep || last_sep == tmp) {
        free(tmp);
        return 0;
    }
    *last_sep = '\0';
    int rc = mkdir_recursive(tmp);
    free(tmp);
    return rc;
}

// ============================================================================
// Format detection
// ============================================================================

static enum image_type classify_image(size_t raw_size) {
    if (raw_size == 400 * 1024)
        return image_fd_ss;
    if (raw_size == 800 * 1024)
        return image_fd_ds;
    if (raw_size == 1440 * 1024)
        return image_fd_hd;
    return image_hd;
}

static int read_file_size(const char *path, size_t *out_size) {
    if (!path || !out_size)
        return -1;
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    *out_size = (size_t)st.st_size;
    return 0;
}

static uint32_t read_be32(const uint8_t *ptr) {
    uint32_t temp = 0;
    memcpy(&temp, ptr, sizeof(temp));
    return BE32(temp);
}

static int detect_diskcopy(const char *path, size_t file_size, uint32_t *out_data_size) {
    if (file_size < DISKCOPY_HEADER_SIZE)
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    uint8_t header[DISKCOPY_HEADER_SIZE];
    size_t r = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (r != sizeof(header))
        return -1;
    uint16_t magic = ((uint16_t)header[0x52] << 8) | header[0x53];
    if (magic != 0x0100)
        return 0;
    uint32_t data_size = read_be32(header + 0x40);
    uint32_t tag_size = read_be32(header + 0x44);
    if (data_size == 0 || (data_size % STORAGE_BLOCK_SIZE) != 0)
        return 0;
    uint64_t total = (uint64_t)DISKCOPY_HEADER_SIZE + (uint64_t)data_size + (uint64_t)tag_size;
    if (total > file_size)
        return 0;
    if (out_data_size)
        *out_data_size = data_size;
    return 1;
}

// ============================================================================
// Image lifecycle
// ============================================================================

size_t disk_size(image_t *disk) {
    if (!disk)
        return 0;
    return disk->raw_size;
}

void image_close(image_t *image) {
    if (!image)
        return;
    if (image->storage)
        storage_delete(image->storage);
    free(image->filename);
    free(image->delta_path);
    free(image->journal_path);
    free(image);
}

image_t *image_open(const char *filename, bool writable) {
    if (!filename || !*filename)
        return NULL;

    // Check write access
    bool final_writable = writable;
    if (final_writable) {
        FILE *test = fopen(filename, "r+b");
        if (!test)
            final_writable = false;
        else
            fclose(test);
    }

    // Get file size and detect format
    size_t file_size = 0;
    if (read_file_size(filename, &file_size) != 0)
        return NULL;

    uint32_t diskcopy_size = 0;
    int dc_probe = detect_diskcopy(filename, file_size, &diskcopy_size);
    if (dc_probe < 0)
        return NULL;
    bool is_diskcopy = (dc_probe > 0);
    size_t raw_size = is_diskcopy ? (size_t)diskcopy_size : file_size;

    if ((raw_size % STORAGE_BLOCK_SIZE) != 0)
        return NULL;

    // Allocate image
    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image)
        return NULL;

    image->filename = dup_string(filename);
    image->raw_size = raw_size;
    image->type = classify_image(raw_size);
    image->writable = final_writable;
    image->from_diskcopy = is_diskcopy;

    // Build delta and journal paths adjacent to the image file
    image->delta_path = str_printf("%s.delta", filename);
    image->journal_path = str_printf("%s.journal", filename);
    if (!image->filename || !image->delta_path || !image->journal_path) {
        image_close(image);
        return NULL;
    }

    // Create storage engine with delta model
    storage_config_t config = {0};
    config.base_path = filename;
    config.delta_path = image->delta_path;
    config.journal_path = image->journal_path;
    config.block_count = raw_size / STORAGE_BLOCK_SIZE;
    config.block_size = STORAGE_BLOCK_SIZE;
    config.base_data_offset = is_diskcopy ? DISKCOPY_HEADER_SIZE : 0;

    int err = storage_new(&config, &image->storage);
    if (err != GS_SUCCESS) {
        LOG(1, "image_open: storage_new failed for %s (%d)", filename, err);
        image_close(image);
        return NULL;
    }

    return image;
}

// ============================================================================
// Image I/O
// ============================================================================

size_t disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    if (!disk || !disk->storage || !buf || size == 0)
        return 0;
    GS_ASSERT((offset % STORAGE_BLOCK_SIZE) == 0);
    GS_ASSERT((size % STORAGE_BLOCK_SIZE) == 0);
    GS_ASSERT(offset + size <= disk->raw_size);
    size_t transferred = 0;
    while (transferred < size) {
        int rc = storage_read_block(disk->storage, offset + transferred, buf + transferred);
        GS_ASSERTF(rc == GS_SUCCESS, "storage_read_block failed (%d)", rc);
        if (rc != GS_SUCCESS)
            break;
        transferred += STORAGE_BLOCK_SIZE;
    }
    return transferred;
}

size_t disk_write_data(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    if (!disk || !disk->storage || !buf || size == 0)
        return 0;
    GS_ASSERT((offset % STORAGE_BLOCK_SIZE) == 0);
    GS_ASSERT((size % STORAGE_BLOCK_SIZE) == 0);
    GS_ASSERT(offset + size <= disk->raw_size);
    size_t transferred = 0;
    while (transferred < size) {
        int rc = storage_write_block(disk->storage, offset + transferred, buf + transferred);
        GS_ASSERTF(rc == GS_SUCCESS, "storage_write_block failed (%d)", rc);
        if (rc != GS_SUCCESS)
            break;
        transferred += STORAGE_BLOCK_SIZE;
    }
    return transferred;
}

// ============================================================================
// Image save / export
// ============================================================================

static int file_write_cb(void *ctx, const void *data, size_t size) {
    FILE *f = (FILE *)ctx;
    size_t wrote = fwrite(data, 1, size, f);
    return (wrote == size) ? 0 : -1;
}

size_t image_save(image_t *image) {
    if (!image || !image->storage || !image->filename)
        return (size_t)-1;
    if (image->from_diskcopy) {
        LOG(1, "image_save: exporting DiskCopy images is not supported (%s)", image->filename);
        return (size_t)-1;
    }
    FILE *f = fopen(image->filename, "wb");
    if (!f)
        return (size_t)-1;
    storage_checkpoint(image->storage, NULL);
    int rc = storage_save_state(image->storage, f, file_write_cb);
    fclose(f);
    return (rc == GS_SUCCESS) ? 0 : (size_t)-1;
}

// ============================================================================
// Image creation
// ============================================================================

int image_create_empty(const char *filename, size_t size) {
    if (!filename || !*filename || size == 0)
        return -1;
    ensure_parent_dirs(filename);
    FILE *f = fopen(filename, "wb");
    if (!f)
        return -1;
    uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    size_t remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(zeros) ? sizeof(zeros) : remaining;
        size_t w = fwrite(zeros, 1, chunk, f);
        if (w != chunk) {
            fclose(f);
            remove(filename);
            return -1;
        }
        remaining -= w;
    }
    fclose(f);
    return 0;
}

int image_create_blank_floppy(const char *filename, bool overwrite, bool high_density) {
    if (!filename || !*filename)
        return -1;
    if (!overwrite) {
        FILE *exist = fopen(filename, "rb");
        if (exist) {
            fclose(exist);
            return -2;
        }
    }
    FILE *f = fopen(filename, "wb");
    if (!f)
        return -1;
    const size_t total = high_density ? 1440 * 1024 : 800 * 1024;
    uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    size_t remaining = total;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(zeros) ? sizeof(zeros) : remaining;
        size_t w = fwrite(zeros, 1, chunk, f);
        if (w != chunk) {
            fclose(f);
            remove(filename);
            return -1;
        }
        remaining -= w;
    }
    fclose(f);
    return 0;
}

// ============================================================================
// Volatile → persistent image copy
// ============================================================================

static bool path_is_volatile(const char *path) {
    return (strncmp(path, "/tmp/", 5) == 0 || strncmp(path, "/fd/", 4) == 0);
}

// FNV-1a hash over the first 64 KB of data plus total file size → 8-char hex.
static uint32_t fnv1a_image_hash(FILE *f, size_t file_size) {
    uint32_t h = 0x811c9dc5u;
    uint8_t buf[4096];
    size_t remaining = file_size < 65536 ? file_size : 65536;

    fseek(f, 0, SEEK_SET);
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        size_t got = fread(buf, 1, chunk, f);
        if (got == 0)
            break;
        for (size_t i = 0; i < got; i++) {
            h ^= buf[i];
            h *= 0x01000193u;
        }
        remaining -= got;
    }
    // Mix in file size
    h ^= (uint32_t)file_size;
    h *= 0x01000193u;
    return h;
}

char *image_persist_volatile(const char *path) {
    if (!path || !*path)
        return NULL;

    // Already persistent — return a copy
    if (!path_is_volatile(path))
        return dup_string(path);

    FILE *src = fopen(path, "rb");
    if (!src)
        return NULL;

    // Get file size
    fseek(src, 0, SEEK_END);
    size_t file_size = (size_t)ftell(src);
    if (file_size == 0) {
        fclose(src);
        return NULL;
    }

    // Hash the file for content-addressed naming
    uint32_t hash = fnv1a_image_hash(src, file_size);
    char *dest_path = str_printf("/images/%08x.img", hash);
    if (!dest_path) {
        fclose(src);
        return NULL;
    }

    // Skip copy if destination already exists with the same size
    struct stat st;
    if (stat(dest_path, &st) == 0 && (size_t)st.st_size == file_size) {
        fclose(src);
        return dest_path;
    }

    // Ensure /images/ directory exists
    mkdir_if_needed("/images");

    // Copy file
    FILE *dst = fopen(dest_path, "wb");
    if (!dst) {
        fclose(src);
        free(dest_path);
        return NULL;
    }

    fseek(src, 0, SEEK_SET);
    uint8_t buf[4096];
    size_t remaining = file_size;
    bool ok = true;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        size_t got = fread(buf, 1, chunk, src);
        if (got != chunk) {
            ok = false;
            break;
        }
        if (fwrite(buf, 1, got, dst) != got) {
            ok = false;
            break;
        }
        remaining -= got;
    }

    fclose(src);
    fclose(dst);

    if (!ok) {
        remove(dest_path);
        free(dest_path);
        return NULL;
    }

    printf("[persist] copied %s -> %s (%zu bytes)\n", path, dest_path, file_size);
    return dest_path;
}

// ============================================================================
// Tracking / module lifecycle
// ============================================================================

void add_image(config_t *sim, image_t *image) {
    config_add_image(sim, image);
}

void image_tick_all(config_t *config) {
    if (!config)
        return;
    int n = config_get_n_images(config);
    for (int i = 0; i < n; ++i) {
        image_t *img = config_get_image(config, i);
        if (!img || !img->storage)
            continue;
        storage_tick(img->storage);
    }
}

const char *image_get_filename(const image_t *image) {
    return image ? image->filename : NULL;
}

void image_init(checkpoint_t *checkpoint) {
    (void)checkpoint;
}

void image_delete(void) {}

void setup_images(struct config *config) {
    (void)config;
}

// ============================================================================
// Checkpointing
// ============================================================================

void image_checkpoint(const image_t *image, checkpoint_t *checkpoint) {
    if (!image || !checkpoint)
        return;
    uint32_t len = 0;
    const char *name = image->filename;
    if (name && *name) {
        size_t n = strlen(name) + 1;
        len = (uint32_t)n;
    }
    system_write_checkpoint_data(checkpoint, &len, sizeof(len));
    if (len)
        system_write_checkpoint_data(checkpoint, name, len);

    char writable_flag = (char)(image->writable ? 1 : 0);
    system_write_checkpoint_data(checkpoint, &writable_flag, sizeof(writable_flag));

    uint64_t raw_size = (uint64_t)image->raw_size;
    system_write_checkpoint_data(checkpoint, &raw_size, sizeof(raw_size));

    if (image->storage) {
        int rc = storage_checkpoint(image->storage, checkpoint);
        if (rc != GS_SUCCESS) {
            LOG(1, "image_checkpoint: storage_checkpoint failed for %s (%d)",
                image->filename ? image->filename : "<unknown>", rc);
        }
    }
}
