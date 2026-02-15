// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image.c
// Disk image handling: open, read/write, and format detection for floppy and hard disk images.

#include "image.h"

#include "log.h"
#include "platform.h"
#include "system.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#endif

LOG_USE_CATEGORY_NAME("image")

#define STORAGE_ROOT_SUFFIX  ".blocks"
#define DISKCOPY_HEADER_SIZE 0x54

static char *dup_string(const char *src);
static char *str_printf(const char *fmt, ...);
static int mkdir_if_needed(const char *path);
static int ensure_storage_layout(image_t *image);
static enum image_type classify_image(size_t raw_size);
static int read_file_size(const char *path, size_t *out_size);
static uint32_t read_be32(const uint8_t *ptr);
static int detect_diskcopy(const char *path, size_t file_size, uint32_t *out_data_size);
void image_close(image_t *image);
static int file_write_cb(void *ctx, const void *data, size_t size);
static bool storage_dir_contains_ranges(const char *path);
static int image_seed_storage(image_t *image);
static int base_stream_read_cb(void *ctx, void *data, size_t size);

// Get the raw size of a disk image in bytes
size_t disk_size(image_t *disk) {
    if (!disk)
        return 0;
    return disk->raw_size;
}

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
    if (rc == 0 || errno == EEXIST) {
        return 0;
    }
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
    // Remove trailing slash if present
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

static int ensure_storage_layout(image_t *image) {
    if (!image)
        return -1;
    return mkdir_if_needed(image->storage_root);
}

static enum image_type classify_image(size_t raw_size) {
    if (raw_size == 400 * 1024)
        return image_fd_ss;
    if (raw_size == 800 * 1024)
        return image_fd_ds;
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

// Close and free an image, releasing all associated resources
void image_close(image_t *image) {
    if (!image)
        return;
    if (image->storage) {
        storage_delete(image->storage);
    }
    free(image->filename);
    free(image->storage_root);
    free(image);
}

image_t *image_open(const char *filename, bool writable) {
    if (!filename || !*filename)
        return NULL;
    bool final_writable = writable;
    if (final_writable) {
        FILE *test = fopen(filename, "r+b");
        if (!test) {
            final_writable = false;
        } else {
            fclose(test);
        }
    }
    size_t file_size = 0;
    if (read_file_size(filename, &file_size) != 0)
        return NULL;
    uint32_t diskcopy_size = 0;
    int dc_probe = detect_diskcopy(filename, file_size, &diskcopy_size);
    if (dc_probe < 0)
        return NULL;
    bool is_diskcopy = (dc_probe > 0);
    size_t raw_size = is_diskcopy ? (size_t)diskcopy_size : file_size;
    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image)
        return NULL;
    image->filename = dup_string(filename);
    if (!image->filename) {
        image_close(image);
        return NULL;
    }
    image->raw_size = raw_size;
    image->type = classify_image(raw_size);
    image->writable = final_writable;
    image->from_diskcopy = is_diskcopy;
    if ((image->raw_size % STORAGE_BLOCK_SIZE) != 0) {
        image_close(image);
        return NULL;
    }
    // Determine storage root: use GS_STORAGE_CACHE if set, else adjacent to image
    const char *cache_root = getenv("GS_STORAGE_CACHE");
    if (cache_root && *cache_root) {
        // Redirect storage to cache directory
        // Resolve to absolute path to handle relative disk paths correctly
        char *abs_filename = realpath(filename, NULL);
        if (!abs_filename) {
            LOG(1, "image_open: realpath failed for %s", filename);
            image_close(image);
            return NULL;
        }
        image->storage_root = str_printf("%s%s%s", cache_root, abs_filename, STORAGE_ROOT_SUFFIX);
        free(abs_filename);
    } else {
        // Legacy: create .blocks/ directory adjacent to image file
        image->storage_root = str_printf("%s%s", filename, STORAGE_ROOT_SUFFIX);
    }
    if (!image->storage_root) {
        image_close(image);
        return NULL;
    }
    // Create storage directory (with intermediate directories if using cache)
    int mkdir_rc = cache_root ? mkdir_recursive(image->storage_root) : mkdir_if_needed(image->storage_root);
    if (mkdir_rc != 0) {
        LOG(1, "image_open: failed to create storage directory %s", image->storage_root);
        image_close(image);
        return NULL;
    }
    bool need_seed = !storage_dir_contains_ranges(image->storage_root);
    storage_config_t config = {0};
    config.path_dir = image->storage_root;
    config.block_count = image->raw_size / STORAGE_BLOCK_SIZE;
    config.block_size = STORAGE_BLOCK_SIZE;
    config.consolidations_per_tick = 1;
    int err = storage_new(&config, &image->storage);
    if (err != GS_SUCCESS) {
        LOG(1, "image_open: storage_new failed for %s (%d)", filename, err);
        image_close(image);
        return NULL;
    }
    if (need_seed) {
        if (image_seed_storage(image) != 0) {
            LOG(1, "image_open: failed to seed storage for %s", filename);
            image_close(image);
            return NULL;
        }
    }
    return image;
}

// Ensure all parent directories of a file path exist (like mkdir -p for parents)
static int ensure_parent_dirs(const char *filepath) {
    if (!filepath || !*filepath)
        return -1;
    char *tmp = dup_string(filepath);
    if (!tmp)
        return -1;
    // Find last slash to isolate parent directory
    char *last_sep = strrchr(tmp, '/');
    if (!last_sep || last_sep == tmp) {
        free(tmp);
        return 0; // no parent or root-level — nothing to create
    }
    *last_sep = '\0';
    int rc = mkdir_recursive(tmp);
    free(tmp);
    return rc;
}

// Create an empty disk image file of the specified size
int image_create_empty(const char *filename, size_t size) {
    if (!filename || !*filename || size == 0)
        return -1;
    // Create parent directories if they don't exist (e.g. after page reload)
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

int image_create_blank_floppy(const char *filename, bool overwrite) {
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
    const size_t total = 819200;
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

// Read data from a disk image at the specified offset
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

// Write data to a disk image at the specified offset
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

// Save an image to its underlying file
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

static int file_write_cb(void *ctx, const void *data, size_t size) {
    FILE *f = (FILE *)ctx;
    size_t wrote = fwrite(data, 1, size, f);
    return (wrote == size) ? 0 : -1;
}

typedef struct {
    FILE *file;
    size_t remaining;
} base_stream_ctx_t;

static int base_stream_read_cb(void *ctx, void *data, size_t size) {
    base_stream_ctx_t *stream = (base_stream_ctx_t *)ctx;
    if (!stream || !stream->file || stream->remaining < size) {
        return 0;
    }
    size_t read = fread(data, 1, size, stream->file);
    if (read != size) {
        return 0;
    }
    stream->remaining -= size;
    return (int)read;
}

static bool storage_dir_contains_ranges(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        return false;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        size_t len = strlen(entry->d_name);
        if (len >= 4 && strcmp(entry->d_name + (len - 4), ".dat") == 0) {
            closedir(dir);
            return true;
        }
    }
    closedir(dir);
    return false;
}

static int image_seed_storage(image_t *image) {
    if (!image || !image->storage || !image->filename) {
        return -1;
    }
    FILE *f = fopen(image->filename, "rb");
    if (!f) {
        return -1;
    }
    size_t skip = image->from_diskcopy ? DISKCOPY_HEADER_SIZE : 0;
    if (fseek(f, (long)skip, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    base_stream_ctx_t ctx = {.file = f, .remaining = image->raw_size};
    int rc = storage_load_state(image->storage, &ctx, base_stream_read_cb);
    fclose(f);
    return (rc == GS_SUCCESS) ? 0 : -1;
}

// Add an image to the emulator's image list
void add_image(struct config *sim, image_t *image) {
    assert(sim->n_images < MAX_IMAGES);
    sim->images[sim->n_images] = image;
    sim->n_images++;
}

// Tick all active images to process pending operations
void image_tick_all(struct config *config) {
    if (!config)
        return;
    for (int i = 0; i < config->n_images; ++i) {
        image_t *img = config->images[i];
        if (!img || !img->storage)
            continue;
        int rc = storage_tick(img->storage);
        if (rc != GS_SUCCESS) {
            LOG(1, "storage_tick failed for %s (%d)", image_get_filename(img), rc);
        }
    }
}

static void cmd_images(struct config *sim, const char *op) {
    if (strcmp(op, "list") == 0) {
        for (int i = 0; i < sim->n_images; i++) {
            printf("%02d: %s\n", i, sim->images[i]->filename);
        }
    }
}

static void cmd_save(struct config *sim, uint64_t n) {
    image_save(sim->images[n]);
}

// Initialize image-related shell commands
void setup_images(struct config *config) {
    // Placeholder for future image-specific shell commands
}

// Image module init currently does not register cross-module commands.
void image_init(checkpoint_t *checkpoint) {
    (void)checkpoint;
}

// Clean up image module (currently no persistent state)
void image_delete(void) {
    // No persistent global state to clean up
}

const char *image_get_filename(const image_t *image) {
    return image ? image->filename : NULL;
}

// Save image reference to a checkpoint (filename, size, writable, storage data)
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
    if (len) {
        system_write_checkpoint_data(checkpoint, name, len);
    }
    char writable = (char)(image->writable ? 1 : 0);
    system_write_checkpoint_data(checkpoint, &writable, sizeof(writable));
    // Write raw image size so restore can recreate the file if missing
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
