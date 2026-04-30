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
#include <fcntl.h>
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
    // Ghost (read-only) instances were placed in a scratch dir; remove their
    // delta+journal so we don't leave clutter behind.
    if (image->ghost_instance) {
        if (image->delta_path)
            unlink(image->delta_path);
        if (image->journal_path)
            unlink(image->journal_path);
    }
    free(image->filename);
    free(image->instance_path);
    free(image->delta_path);
    free(image->journal_path);
    free(image);
}

// Mint a 16-hex-char opaque id (8 random bytes).  Used both for image
// instance ids and as scratch-name salt.
static void mint_random_hex_id(char *out, size_t out_len) {
    assert(out_len >= 17);
    uint8_t bytes[8] = {0};
    bool got = false;
#if !defined(_WIN32)
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, bytes, sizeof(bytes));
        close(fd);
        if (n == (ssize_t)sizeof(bytes))
            got = true;
    }
#endif
    if (!got) {
        // Fallback: combine PID + time + a counter for uniqueness within a
        // process even if /dev/urandom is unavailable.
        static uint32_t ctr = 0;
        uint32_t pid = (uint32_t)getpid();
        uint32_t now = (uint32_t)time(NULL);
        uint32_t c = ++ctr;
        bytes[0] = (uint8_t)(pid);
        bytes[1] = (uint8_t)(pid >> 8);
        bytes[2] = (uint8_t)(pid >> 16);
        bytes[3] = (uint8_t)(pid >> 24);
        bytes[4] = (uint8_t)(now ^ c);
        bytes[5] = (uint8_t)((now >> 8) ^ (c >> 8));
        bytes[6] = (uint8_t)((now >> 16) ^ (c >> 16));
        bytes[7] = (uint8_t)((now >> 24) ^ (c >> 24));
    }
    for (size_t i = 0; i < 8; i++) {
        static const char hex[] = "0123456789abcdef";
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0xf];
    }
    out[16] = '\0';
}

// Return the directory part of a path (caller frees).  Returns "." for paths
// without a slash.
static char *dirname_of(const char *path) {
    if (!path || !*path)
        return dup_string(".");
    const char *last = strrchr(path, '/');
    if (!last)
        return dup_string(".");
    if (last == path)
        return dup_string("/");
    size_t len = (size_t)(last - path);
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

// Probe a base image for size and DiskCopy framing; return 0 on success,
// negative on failure.  `out_raw_size` and `out_is_diskcopy` are populated
// even if the file is not a DiskCopy archive.
static int probe_base_image(const char *base_path, size_t *out_raw_size, bool *out_is_diskcopy) {
    size_t file_size = 0;
    if (read_file_size(base_path, &file_size) != 0) {
        printf("image: cannot read file size: %s\n", base_path);
        return -1;
    }
    uint32_t diskcopy_size = 0;
    int dc_probe = detect_diskcopy(base_path, file_size, &diskcopy_size);
    if (dc_probe < 0)
        return -1;
    bool is_diskcopy = (dc_probe > 0);
    size_t raw_size = is_diskcopy ? (size_t)diskcopy_size : file_size;
    if ((raw_size % STORAGE_BLOCK_SIZE) != 0)
        return -1;
    *out_raw_size = raw_size;
    *out_is_diskcopy = is_diskcopy;
    return 0;
}

// Common storage-engine wiring for all image_* entry points.  The caller has
// already populated image->filename, image->instance_path, image->delta_path,
// image->journal_path, image->raw_size, image->writable, image->from_diskcopy.
static int image_attach_storage(image_t *image, bool is_diskcopy) {
    storage_config_t config = {0};
    config.base_path = image->filename;
    config.delta_path = image->delta_path;
    config.journal_path = image->journal_path;
    config.block_count = image->raw_size / STORAGE_BLOCK_SIZE;
    config.block_size = STORAGE_BLOCK_SIZE;
    config.base_data_offset = is_diskcopy ? DISKCOPY_HEADER_SIZE : 0;
    return storage_new(&config, &image->storage);
}

// Scratch directory for read-only image deltas (kept volatile).
#define IMAGE_RO_SCRATCH_DIR "/tmp/gs-image-ro"

image_t *image_open_readonly(const char *base_path) {
    if (!base_path || !*base_path)
        return NULL;

    size_t raw_size = 0;
    bool is_diskcopy = false;
    if (probe_base_image(base_path, &raw_size, &is_diskcopy) != 0)
        return NULL;

    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image)
        return NULL;
    image->filename = dup_string(base_path);
    image->raw_size = raw_size;
    image->type = classify_image(raw_size);
    image->writable = false;
    image->from_diskcopy = is_diskcopy;
    image->ghost_instance = true;

    // Mint a scratch instance under /tmp so the read-only mount does not
    // pollute the base image's directory with delta sidecars.
    mkdir_recursive(IMAGE_RO_SCRATCH_DIR);
    char id[17];
    mint_random_hex_id(id, sizeof(id));
    image->instance_path = NULL; // never serialized for read-only mounts
    image->delta_path = str_printf("%s/%s.delta", IMAGE_RO_SCRATCH_DIR, id);
    image->journal_path = str_printf("%s/%s.journal", IMAGE_RO_SCRATCH_DIR, id);
    if (!image->filename || !image->delta_path || !image->journal_path) {
        image_close(image);
        return NULL;
    }

    int err = image_attach_storage(image, is_diskcopy);
    if (err != GS_SUCCESS) {
        printf("image_open_readonly: storage engine failed for %s (error %d)\n", base_path, err);
        image_close(image);
        return NULL;
    }
    return image;
}

image_t *image_create(const char *base_path, const char *delta_dir) {
    if (!base_path || !*base_path)
        return NULL;

    // Verify write access on base (used by floppy/hd insert; treat lack of
    // write access as a hard failure here so callers don't silently fall back).
    FILE *test = fopen(base_path, "r+b");
    if (!test) {
        // Some tests open via fd= and the base may live on a read-only FS;
        // create still proceeds — only the delta needs to be writable.
    } else {
        fclose(test);
    }

    size_t raw_size = 0;
    bool is_diskcopy = false;
    if (probe_base_image(base_path, &raw_size, &is_diskcopy) != 0)
        return NULL;

    // Default delta_dir to the directory containing the base image.  Headless
    // callers may pass NULL when they have no machine-id concept (§2.4).
    char *derived_dir = NULL;
    if (!delta_dir || !*delta_dir) {
        derived_dir = dirname_of(base_path);
        delta_dir = derived_dir;
    }
    if (mkdir_recursive(delta_dir) != 0) {
        printf("image_create: cannot create delta directory: %s\n", delta_dir);
        free(derived_dir);
        return NULL;
    }

    char id[17];
    mint_random_hex_id(id, sizeof(id));

    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image) {
        free(derived_dir);
        return NULL;
    }
    image->filename = dup_string(base_path);
    image->raw_size = raw_size;
    image->type = classify_image(raw_size);
    image->writable = true;
    image->from_diskcopy = is_diskcopy;
    image->instance_path = str_printf("%s/%s", delta_dir, id);
    image->delta_path = str_printf("%s.delta", image->instance_path);
    image->journal_path = str_printf("%s.journal", image->instance_path);
    free(derived_dir);
    if (!image->filename || !image->instance_path || !image->delta_path || !image->journal_path) {
        image_close(image);
        return NULL;
    }

    int err = image_attach_storage(image, is_diskcopy);
    if (err != GS_SUCCESS) {
        printf("image_create: storage engine failed for %s (error %d)\n", base_path, err);
        image_close(image);
        return NULL;
    }
    return image;
}

image_t *image_open(const char *base_path, const char *instance_path) {
    if (!base_path || !*base_path || !instance_path || !*instance_path)
        return NULL;

    size_t raw_size = 0;
    bool is_diskcopy = false;
    if (probe_base_image(base_path, &raw_size, &is_diskcopy) != 0)
        return NULL;

    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image)
        return NULL;
    image->filename = dup_string(base_path);
    image->raw_size = raw_size;
    image->type = classify_image(raw_size);
    image->writable = true;
    image->from_diskcopy = is_diskcopy;
    image->instance_path = dup_string(instance_path);
    image->delta_path = str_printf("%s.delta", instance_path);
    image->journal_path = str_printf("%s.journal", instance_path);
    if (!image->filename || !image->instance_path || !image->delta_path || !image->journal_path) {
        image_close(image);
        return NULL;
    }

    int err = image_attach_storage(image, is_diskcopy);
    if (err != GS_SUCCESS) {
        printf("image_open: storage engine failed for %s (instance %s, error %d)\n", base_path, instance_path, err);
        image_close(image);
        return NULL;
    }
    return image;
}

const char *image_path(const image_t *image) {
    if (!image || !image->writable)
        return NULL;
    return image->instance_path;
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

// Export the full disk content (base + delta) to a new file at dest_path.
int image_export_to(image_t *image, const char *dest_path) {
    if (!image || !image->storage || !dest_path || !*dest_path)
        return -1;
    // Refuse to overwrite existing files
    FILE *exist = fopen(dest_path, "rb");
    if (exist) {
        fclose(exist);
        return -1;
    }
    ensure_parent_dirs(dest_path);
    FILE *f = fopen(dest_path, "wb");
    if (!f)
        return -1;
    int rc = storage_save_state(image->storage, f, file_write_cb);
    fclose(f);
    if (rc != GS_SUCCESS) {
        remove(dest_path);
        return -1;
    }
    return 0;
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
    // Paths under /opfs/ are persistent (OPFS-backed). Everything else is volatile.
    return !(path && strncmp(path, "/opfs/", 6) == 0);
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
    char *dest_path = str_printf("/opfs/images/%08x.img", hash);
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
    mkdir_if_needed("/opfs/images");

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

    // Persist the instance path so a future restore can reopen the same delta
    // directory without relying on adjacent-to-base sidecars (§2.8).  Empty
    // string for read-only / ghost mounts.
    const char *instance = (image->writable && image->instance_path) ? image->instance_path : "";
    uint32_t instance_len = (uint32_t)(strlen(instance) + 1);
    system_write_checkpoint_data(checkpoint, &instance_len, sizeof(instance_len));
    system_write_checkpoint_data(checkpoint, instance, instance_len);

    if (image->storage) {
        int rc = storage_checkpoint(image->storage, checkpoint);
        if (rc != GS_SUCCESS) {
            LOG(1, "image_checkpoint: storage_checkpoint failed for %s (%d)",
                image->filename ? image->filename : "<unknown>", rc);
        }
    }
}
