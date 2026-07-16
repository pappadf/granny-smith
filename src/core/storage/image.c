// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image.c
// Disk image handling: open, read/write, and format detection for floppy and hard disk images.
//
// Each image is backed by a delta-file storage engine.  The original image file
// is opened read-only as the "base"; all modifications go to a .delta file.
// A .journal file provides crash recovery.

#include "image.h"

#include "appledouble.h"
#include "image_ndif.h"
#include "log.h"
#include "platform.h"
#include "system.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
    free(image->tags);
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
// even if the file is not a DiskCopy archive.  The raw size must be a whole
// number of `block_size`-byte blocks.
static int probe_base_image(const char *base_path, uint32_t block_size, size_t *out_raw_size, bool *out_is_diskcopy) {
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
    if ((raw_size % block_size) != 0)
        return -1;
    *out_raw_size = raw_size;
    *out_is_diskcopy = is_diskcopy;
    return 0;
}

// Normalise a geometry's block size, treating 0 as the default (512).
static uint32_t geometry_block_size(image_geometry_t geom) {
    return geom.block_size ? geom.block_size : STORAGE_BLOCK_SIZE;
}

// Common storage-engine wiring for all image_* entry points.  The caller has
// already populated image->filename, image->instance_path, image->delta_path,
// image->journal_path, image->raw_size, image->writable, image->from_diskcopy.
// Load the DiskCopy 4.2 tag section (after the data) into image->tags.  These
// are read-only per-sector tags the Lisa boot ROM/OS read (e.g. the boot
// block's FILEID = $AAAA).  Best-effort: on any failure the image simply has no
// tags (disk_read_tag returns 0).
static void image_load_diskcopy_tags(image_t *image) {
    FILE *f = fopen(image->filename, "rb");
    if (!f)
        return;
    uint8_t header[DISKCOPY_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return;
    }
    uint32_t data_size = read_be32(header + 0x40);
    uint32_t tag_size = read_be32(header + 0x44);
    uint32_t count = (uint32_t)(image->raw_size / STORAGE_BLOCK_SIZE);
    if (tag_size == 0 || count == 0 || (tag_size % count) != 0) {
        fclose(f);
        return; // no tags (or unexpected layout)
    }
    uint8_t *tags = (uint8_t *)malloc(tag_size);
    if (!tags) {
        fclose(f);
        return;
    }
    if (fseek(f, (long)DISKCOPY_HEADER_SIZE + (long)data_size, SEEK_SET) != 0 ||
        fread(tags, 1, tag_size, f) != tag_size) {
        free(tags);
        fclose(f);
        return;
    }
    fclose(f);
    image->tags = tags;
    image->tag_bytes = tag_size / count;
    image->tag_count = count;
}

static int image_attach_storage(image_t *image, bool is_diskcopy) {
    storage_config_t config = {0};
    config.base_path = image->filename; // NULL for a blank no-base image
    config.delta_path = image->delta_path;
    config.journal_path = image->journal_path;
    config.block_count = image->raw_size / image->block_size;
    config.block_size = image->block_size;
    config.base_data_offset = is_diskcopy ? DISKCOPY_HEADER_SIZE : 0;
    int rc = storage_new(&config, &image->storage);
    if (rc == GS_SUCCESS && is_diskcopy)
        image_load_diskcopy_tags(image);
    return rc;
}

size_t disk_read_tag(image_t *disk, size_t sector, uint8_t *buf, size_t size) {
    if (!disk || !disk->tags || !buf || sector >= disk->tag_count)
        return 0;
    size_t n = size < disk->tag_bytes ? size : disk->tag_bytes;
    memcpy(buf, disk->tags + sector * disk->tag_bytes, n);
    return n;
}

// Persist a sector's tag (pagelabel).  The Lisa Sony controller writes the
// 512-byte data sector *and* its tag together; modelling only the data drops
// the FS pagelabel updates the OS makes on every write.  Tags live in the
// in-memory image->tags buffer (per-run; the read-only base file is untouched).
size_t disk_write_tag(image_t *disk, size_t sector, const uint8_t *buf, size_t size) {
    if (!disk || !disk->tags || !buf || sector >= disk->tag_count)
        return 0;
    size_t n = size < disk->tag_bytes ? size : disk->tag_bytes;
    memcpy(disk->tags + sector * disk->tag_bytes, buf, n);
    return n;
}

// Scratch directory for read-only image deltas (kept volatile).
#define IMAGE_RO_SCRATCH_DIR "/tmp/gs-image-ro"

// === AppleDouble fork acquisition + host-file NDIF materialisation =========
// A host `.img` file has only a data fork, but a Disk Copy 6 / NDIF image keeps
// its block map ('bcem') in the resource fork.  When such a file was copied out
// of an HFS volume as an AppleDouble pair, the resource fork lives in a sibling
// "._<name>" (or legacy "%<name>", or a raw "<name>.rsrc").  This reunites the
// forks and, when the data fork is NDIF-encoded, decodes it to a scratch raw
// image so the rest of image.c opens it as an ordinary base.  See
// proposal-appledouble-support.md §4.4.
#define IMAGE_FORK_READ_CAP (16u * 1024u * 1024u)

// Read up to `cap` bytes of a host file into a malloc'd buffer. 0 / -errno.
static int read_whole_host_file(const char *path, size_t cap, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -errno;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -EIO;
    }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > cap) {
        fclose(f);
        return sz < 0 ? -EIO : -EFBIG;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        return -ENOMEM;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return -EIO;
    }
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

// Split base_path into "<dir>/" prefix (with trailing slash, or empty) and
// basename, writing the "._<name>"-style sidecar into `out`.
static void fork_sidecar_path(const char *base_path, const char *prefix, char *out, size_t cap) {
    const char *slash = strrchr(base_path, '/');
    if (slash)
        snprintf(out, cap, "%.*s%s%s", (int)(slash - base_path + 1), base_path, prefix, slash + 1);
    else
        snprintf(out, cap, "%s%s", prefix, base_path);
}

// Obtain a resource fork for base_path from a companion sidecar, tried in
// order: AppleDouble/AppleSingle header "._<name>" then legacy "%<name>", then
// a raw sibling "<name>.rsrc".  Returns a malloc'd buffer (caller frees) and
// sets *out_len, or NULL when no resource fork is found.
static uint8_t *acquire_resource_fork(const char *base_path, size_t *out_len) {
    char path[PATH_MAX];
    // (1)+(2): AppleDouble/AppleSingle header sidecars.
    const char *prefixes[] = {"._", "%"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        fork_sidecar_path(base_path, prefixes[i], path, sizeof(path));
        uint8_t *raw = NULL;
        size_t raw_len = 0;
        if (read_whole_host_file(path, IMAGE_FORK_READ_CAP, &raw, &raw_len) != 0)
            continue;
        ad_file_t ad;
        if (ad_detect(raw, raw_len) && ad_parse(raw, raw_len, &ad) == 0 && ad.rsrc && ad.rsrc_len) {
            uint8_t *rf = (uint8_t *)malloc(ad.rsrc_len);
            if (rf) {
                memcpy(rf, ad.rsrc, ad.rsrc_len);
                *out_len = ad.rsrc_len;
                free(raw);
                return rf;
            }
        }
        free(raw);
    }
    // (3): raw sibling "<name>.rsrc".
    snprintf(path, sizeof(path), "%s.rsrc", base_path);
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    if (read_whole_host_file(path, IMAGE_FORK_READ_CAP, &raw, &raw_len) == 0 && raw_len > 0) {
        *out_len = raw_len;
        return raw;
    }
    free(raw);
    return NULL;
}

// Decode an NDIF data fork (host file `base_path`, block map `map`) into a
// freshly created scratch raw file `scratch`. 0 / -errno.
static int materialize_ndif_host(const char *base_path, ndif_map_t *map, const char *scratch) {
    FILE *df = fopen(base_path, "rb");
    if (!df)
        return -errno;
    FILE *out = fopen(scratch, "wb");
    if (!out) {
        int e = errno;
        fclose(df);
        return e ? -e : -EIO;
    }
    int rc = 0;
    if (ftruncate(fileno(out), (off_t)map->sectors * 512) != 0) {
        rc = -EIO;
        goto done;
    }
    for (size_t i = 0; i < map->n_chunks; i++) {
        ndif_chunk_t *c = &map->chunks[i];
        if (c->type == NDIF_CHUNK_ZERO || c->count == 0)
            continue; // ftruncate already zero-filled the gap
        size_t need = (size_t)c->count * 512;
        uint8_t *dbuf = (uint8_t *)malloc(need);
        uint8_t *cbuf = c->length ? (uint8_t *)malloc(c->length) : NULL;
        if (!dbuf || (c->length && !cbuf)) {
            free(dbuf);
            free(cbuf);
            rc = -ENOMEM;
            break;
        }
        size_t clen = 0;
        if (c->length) {
            if (fseek(df, (long)c->offset, SEEK_SET) != 0) {
                free(dbuf);
                free(cbuf);
                rc = -EIO;
                break;
            }
            clen = fread(cbuf, 1, c->length, df);
        }
        if (ndif_decode_chunk(c, cbuf, clen, dbuf, need) == 0) {
            if (fseek(out, (long)c->sector * 512, SEEK_SET) != 0 || fwrite(dbuf, 1, need, out) != need)
                rc = -EIO;
        } else {
            rc = -EINVAL;
        }
        free(dbuf);
        free(cbuf);
        if (rc != 0)
            break;
    }
done:
    fclose(df);
    if (fclose(out) != 0 && rc == 0)
        rc = -EIO;
    return rc;
}

// FNV-1a over a NUL-terminated string, seeded, for scratch-name derivation.
static uint32_t fork_hash_str(const char *s, uint32_t h) {
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 0x01000193u;
    return h;
}

// If base_path is an NDIF image whose block map can be recovered from a fork
// sidecar, decode it to a cached scratch raw file and return that path
// (malloc'd, caller frees / image takes ownership).  Otherwise return a copy of
// base_path.  NULL only on allocation failure.
static char *resolve_base_image(const char *base_path) {
    size_t rlen = 0;
    uint8_t *rfork = acquire_resource_fork(base_path, &rlen);
    if (!rfork)
        return dup_string(base_path);

    char *result = NULL;
    if (ndif_detect(rfork, rlen)) {
        ndif_map_t *map = NULL;
        if (ndif_parse(rfork, rlen, &map) == 0) {
            // Deterministic scratch name from path + size + mtime, so repeated
            // inserts reuse the decode and a changed source re-decodes.
            struct stat sb;
            uint32_t h = fork_hash_str(base_path, 0x811c9dc5u);
            if (stat(base_path, &sb) == 0) {
                h = fork_hash_str("\x1f", h);
                char meta[64];
                snprintf(meta, sizeof(meta), "%lld:%lld", (long long)sb.st_size, (long long)sb.st_mtime);
                h = fork_hash_str(meta, h);
            }
            char scratch[PATH_MAX];
            snprintf(scratch, sizeof(scratch), "%s/ndif-%08x.img", IMAGE_RO_SCRATCH_DIR, h);

            struct stat cached;
            if (stat(scratch, &cached) == 0 && cached.st_size == (off_t)map->sectors * 512) {
                result = dup_string(scratch); // already materialised
            } else {
                mkdir_recursive(IMAGE_RO_SCRATCH_DIR);
                if (materialize_ndif_host(base_path, map, scratch) == 0) {
                    LOG(3, "decoded NDIF '%s' -> '%s' (%u sectors)", base_path, scratch, map->sectors);
                    result = dup_string(scratch);
                } else {
                    remove(scratch);
                    LOG(1, "NDIF decode failed for '%s'", base_path);
                }
            }
            ndif_map_free(map);
        }
    }
    free(rfork);
    return result ? result : dup_string(base_path);
}

image_t *image_open_readonly(const char *base_path) {
    return image_open_readonly_with_geometry(base_path, (image_geometry_t){.block_size = STORAGE_BLOCK_SIZE});
}

image_t *image_open_readonly_with_geometry(const char *base_path, image_geometry_t geom) {
    if (!base_path || !*base_path)
        return NULL;
    uint32_t block_size = geometry_block_size(geom);

    char *effective = resolve_base_image(base_path);
    if (!effective)
        return NULL;

    size_t raw_size = 0;
    bool is_diskcopy = false;
    if (probe_base_image(effective, block_size, &raw_size, &is_diskcopy) != 0) {
        free(effective);
        return NULL;
    }

    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image) {
        free(effective);
        return NULL;
    }
    image->filename = effective; // owns the (possibly NDIF-materialised) base path
    image->raw_size = raw_size;
    image->block_size = block_size;
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
    return image_create_with_geometry(base_path, delta_dir, (image_geometry_t){.block_size = STORAGE_BLOCK_SIZE});
}

image_t *image_create_with_geometry(const char *base_path, const char *delta_dir, image_geometry_t geom) {
    if (!base_path || !*base_path)
        return NULL;
    uint32_t block_size = geometry_block_size(geom);

    // No write-access probe: only the delta needs to be writable, and the
    // base can legitimately live on a read-only FS (some tests, distribution
    // mounts). The probe that used to live here had no effect on subsequent
    // behaviour.

    char *effective = resolve_base_image(base_path);
    if (!effective)
        return NULL;

    size_t raw_size = 0;
    bool is_diskcopy = false;
    if (probe_base_image(effective, block_size, &raw_size, &is_diskcopy) != 0) {
        free(effective);
        return NULL;
    }

    // Default delta_dir to the directory containing the (original) base image.
    // Headless callers may pass NULL when they have no machine-id concept (§2.4).
    char *derived_dir = NULL;
    if (!delta_dir || !*delta_dir) {
        derived_dir = dirname_of(base_path);
        delta_dir = derived_dir;
    }
    if (mkdir_recursive(delta_dir) != 0) {
        printf("image_create: cannot create delta directory: %s\n", delta_dir);
        free(derived_dir);
        free(effective);
        return NULL;
    }

    char id[17];
    mint_random_hex_id(id, sizeof(id));

    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image) {
        free(derived_dir);
        free(effective);
        return NULL;
    }
    image->filename = effective; // owns the (possibly NDIF-materialised) base path
    image->raw_size = raw_size;
    image->block_size = block_size;
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
    return image_open_with_geometry(base_path, instance_path, (image_geometry_t){.block_size = STORAGE_BLOCK_SIZE});
}

image_t *image_open_with_geometry(const char *base_path, const char *instance_path, image_geometry_t geom) {
    if (!base_path || !*base_path || !instance_path || !*instance_path)
        return NULL;
    uint32_t block_size = geometry_block_size(geom);

    char *effective = resolve_base_image(base_path);
    if (!effective)
        return NULL;

    size_t raw_size = 0;
    bool is_diskcopy = false;
    if (probe_base_image(effective, block_size, &raw_size, &is_diskcopy) != 0) {
        free(effective);
        return NULL;
    }

    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image) {
        free(effective);
        return NULL;
    }
    image->filename = effective; // owns the (possibly NDIF-materialised) base path
    image->raw_size = raw_size;
    image->block_size = block_size;
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

image_t *image_create_blank(uint64_t block_count, image_geometry_t geom) {
    uint32_t block_size = geometry_block_size(geom);
    if (block_count == 0)
        return NULL;

    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image)
        return NULL;
    image->filename = NULL; // no backing base file: unwritten blocks read as zeros
    image->raw_size = (size_t)block_count * block_size;
    image->block_size = block_size;
    image->type = image_hd;
    image->writable = true;
    image->from_diskcopy = false;
    image->ghost_instance = true; // delta+journal are scratch, removed on close

    // Place the delta+journal in the read-only scratch dir so the blank disk's
    // sidecars don't clutter any user directory; ghost_instance unlinks them on
    // image_close.  The image is ephemeral unless exported via image_export_to.
    mkdir_recursive(IMAGE_RO_SCRATCH_DIR);
    char id[17];
    mint_random_hex_id(id, sizeof(id));
    image->instance_path = NULL; // never serialized
    image->delta_path = str_printf("%s/%s.delta", IMAGE_RO_SCRATCH_DIR, id);
    image->journal_path = str_printf("%s/%s.journal", IMAGE_RO_SCRATCH_DIR, id);
    if (!image->delta_path || !image->journal_path) {
        image_close(image);
        return NULL;
    }

    int err = image_attach_storage(image, false);
    if (err != GS_SUCCESS) {
        printf("image_create_blank: storage engine failed (%llu x %u, error %d)\n", (unsigned long long)block_count,
               block_size, err);
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
    GS_ASSERT((offset % disk->block_size) == 0);
    GS_ASSERT((size % disk->block_size) == 0);
    // An undersized / truncated image (a host file shorter than the media it
    // backs) can be read past its end — e.g. the Finder reading high tracks
    // while ejecting a 400K/800K-geometry floppy backed by a too-small file.
    // Serve the unbacked tail as blank media (zeroes) rather than asserting,
    // so the guest sees readable-but-empty sectors and the operation can
    // finish.  `backed` is the in-bounds, whole-block byte count.
    size_t backed = (offset < disk->raw_size) ? (disk->raw_size - offset) : 0;
    if (backed > size)
        backed = size;
    backed -= backed % disk->block_size;
    if (backed < size) {
        memset(buf + backed, 0, size - backed);
        LOG(1,
            "disk_read_data: %zu-byte read at offset %zu runs past image end (raw_size=%zu); zero-filled %zu-byte tail",
            size, offset, disk->raw_size, size - backed);
    }
    size_t transferred = 0;
    while (transferred < backed) {
        int rc = storage_read_block(disk->storage, offset + transferred, buf + transferred);
        GS_ASSERTF(rc == GS_SUCCESS, "storage_read_block failed (%d)", rc);
        if (rc != GS_SUCCESS)
            return transferred; // genuine in-bounds backing-store failure
        transferred += disk->block_size;
    }
    // Buffer fully populated: real data plus any zero-filled tail past EOF.
    return size;
}

size_t disk_write_data(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    if (!disk || !disk->storage || !buf || size == 0)
        return 0;
    GS_ASSERT((offset % disk->block_size) == 0);
    GS_ASSERT((size % disk->block_size) == 0);
    // Symmetric with disk_read_data: a write past the end of an undersized
    // image targets sectors with no backing store.  Drop the unbacked tail
    // (it cannot be stored) rather than asserting, so the guest's volume
    // flush during eject completes; warn so the dropped write is visible.
    size_t backed = (offset < disk->raw_size) ? (disk->raw_size - offset) : 0;
    if (backed > size)
        backed = size;
    backed -= backed % disk->block_size;
    if (backed < size)
        LOG(1,
            "disk_write_data: %zu-byte write at offset %zu runs past image end (raw_size=%zu); dropped %zu-byte tail",
            size, offset, disk->raw_size, size - backed);
    size_t transferred = 0;
    while (transferred < backed) {
        int rc = storage_write_block(disk->storage, offset + transferred, buf + transferred);
        GS_ASSERTF(rc == GS_SUCCESS, "storage_write_block failed (%d)", rc);
        if (rc != GS_SUCCESS)
            return transferred; // genuine in-bounds backing-store failure
        transferred += disk->block_size;
    }
    // Accepted the write; any portion past EOF was intentionally dropped.
    return size;
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
    // Use ftruncate to extend the file — POSIX guarantees the new bytes read
    // as zero. Skips the 40 000+ iteration chunked-fwrite loop a 160 MB HD
    // would otherwise take.
    int fd = fileno(f);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
        fclose(f);
        remove(filename);
        return -1;
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
    int fd = fileno(f);
    if (fd < 0 || ftruncate(fd, (off_t)total) != 0) {
        fclose(f);
        remove(filename);
        return -1;
    }
    fclose(f);
    return 0;
}

int image_create_blank_profile(const char *filename, uint32_t block_count) {
    if (!filename || !*filename || block_count == 0)
        return -1;
    FILE *exist = fopen(filename, "rb");
    if (exist) {
        fclose(exist);
        return -2;
    }
    ensure_parent_dirs(filename);
    FILE *f = fopen(filename, "wb");
    if (!f)
        return -1;
    // A blank ProFile is just zeros — block_count × 532.  ftruncate leaves the
    // new bytes reading as zero, so the controller serves an all-zero disk the
    // OS then formats; the device-info block reports block_count as capacity.
    const off_t total = (off_t)block_count * (off_t)PROFILE_BLOCK_BYTES;
    int fd = fileno(f);
    if (fd < 0 || ftruncate(fd, total) != 0) {
        fclose(f);
        remove(filename);
        return -1;
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
