// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image.c
// Disk image handling: open, read/write, and format detection for floppy and hard disk images.
//
// Each image is backed by a delta-file storage engine.  The original image file
// is opened read-only as the "base"; all modifications go to a .delta file.
// A .journal file provides crash recovery.

#include "image.h"
#include "gs_out.h"

#include "format_registry.h"
#include "gs_assert.h"
#include "image_iso9660.h"
#include "image_scratch.h"
#include "image_udif.h"
#include "image_wrap.h"
#include "log.h"
#include "platform.h"
#include "source.h"
#include "storage_util.h"
#include "system.h"
#include "udif_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#if defined(_WIN32)
#include <direct.h>
#endif

LOG_USE_CATEGORY_NAME("image")

// ============================================================================
// Format detection
// ============================================================================

static enum image_type classify_image(size_t raw_size) {
    if (raw_size == 400 * 1024)
        return image_fd_ss;
    if (raw_size == 800 * 1024)
        return image_fd_ds;
    if (raw_size == 720 * 1024)
        return image_fd_dd_mfm;
    if (raw_size == 1440 * 1024)
        return image_fd_hd;
    return image_hd;
}

// ============================================================================
// Image lifecycle
// ============================================================================

size_t disk_size(image_t *disk) {
    if (!disk)
        return 0;
    return disk->raw_size;
}

uint32_t disk_block_size(image_t *disk) {
    return disk ? disk->block_size : 0;
}

// ============================================================================
// Open writable images
// ============================================================================

// Every image opened writable and not yet closed, keyed on the canonical
// path its caller named and on its source's key.  image_vfs asks at the
// point of use rather than being told on attach and detach, so there is no
// notification for an attach or detach site to forget.
static image_t *g_open_writable;

// Resolve `path` through realpath() so every spelling of the same file — the
// relative one a script passes to files.probe, the absolute one the VFS
// resolves, a symlink — reduces to one string.  Falls back to the input
// when realpath cannot resolve it (a path through an image, or a file just
// deleted).
static void image_canonicalise(const char *path, char *out, size_t cap) {
    // Let realpath() allocate, so a host path limit above the build-time
    // PATH_MAX does not get silently truncated.
    char *resolved = realpath(path, NULL);
    snprintf(out, cap, "%s", resolved ? resolved : path);
    free(resolved);
}

// Record a writable image under the path its caller named and the key of
// the source it opened.  An image that cannot be recorded still works; the
// VFS just cannot see that it is open.
static void writable_register(image_t *image, const char *base_path) {
    char canon[PATH_MAX];
    image_canonicalise(base_path, canon, sizeof(canon));
    image->source_canon = gs_strdup(canon);
    if (!image->source_canon)
        return;
    image->next_writable = g_open_writable;
    g_open_writable = image;
}

static void writable_unregister(image_t *image) {
    for (image_t **pp = &g_open_writable; *pp; pp = &(*pp)->next_writable) {
        if (*pp == image) {
            *pp = image->next_writable;
            break;
        }
    }
    free(image->source_canon);
}

bool image_path_is_open_writable(const char *path) {
    if (!path)
        return false;
    char canon[PATH_MAX];
    image_canonicalise(path, canon, sizeof(canon));
    for (const image_t *im = g_open_writable; im; im = im->next_writable)
        if (strcmp(im->source_canon, canon) == 0 || strcmp(im->source_canon, path) == 0)
            return true;
    return false;
}

bool image_key_is_open_writable(const char *key) {
    if (!key || !*key)
        return false;
    for (const image_t *im = g_open_writable; im; im = im->next_writable)
        if (im->source_key && gs_key_within(key, im->source_key))
            return true;
    return false;
}

void image_close(image_t *image) {
    if (!image)
        return;
    if (image->writable)
        writable_unregister(image);
    if (image->storage)
        storage_delete(image->storage);
    free(image->tags);
    free(image->wrap_prefix);
    // Ghost (read-only) instances were placed in a scratch dir; remove their
    // delta+journal so we don't leave clutter behind.
    if (image->ghost_instance) {
        if (image->delta_path)
            unlink(image->delta_path);
        if (image->journal_path)
            unlink(image->journal_path);
    }
    free(image->filename);
    free(image->source_key);
    free(image->format);
    free(image->instance_path);
    free(image->delta_path);
    free(image->journal_path);
    free(image);
}

// Mint a 16-hex-char opaque id (8 random bytes).  Used both for image
// instance ids and as scratch-name salt.  `out` holds 16 chars + NUL; the
// [static 17] lets the compiler reject a smaller buffer at the call site.
static void mint_random_hex_id(char out[static 17]) {
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
        // process even if /dev/urandom is unavailable.  Unique only while
        // ids are minted on one thread (the emulator's; the counter is a
        // plain static) -- two threads minting in the same second could
        // collide.
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

// Normalise a geometry's block size, treating 0 as the default (512).
static uint32_t geometry_block_size(image_geometry_t geom) {
    return geom.block_size ? geom.block_size : STORAGE_BLOCK_SIZE;
}

// Load the DiskCopy 4.2 tag section (after the data) into image->tags.  These
// are read-only per-sector tags the Lisa boot ROM/OS read (e.g. the boot
// block's FILEID = $AAAA).  Best-effort: on any failure the image simply has
// no tags (disk_read_tag returns 0).  `dc42` is the DiskCopy file itself.
static void image_load_diskcopy_tags(image_t *image, gs_source_t *dc42) {
    uint8_t header[DISKCOPY_HEADER_SIZE];
    if (gs_source_read_exact(dc42, 0, header, sizeof(header)) != 0)
        return;
    // Every value is re-derived from this header and checked against this
    // file.  The sector count is the header's own: DiskCopy 4.2 sectors are
    // 512 data bytes whatever geometry the image is opened with.  Bounding
    // the tag section by the file bounds the allocation by the file.
    uint32_t data_size = 0, tag_size = 0;
    if (!dc42_parse_header(header, sizeof(header), gs_source_size(dc42), &data_size, &tag_size))
        return;
    uint32_t count = data_size / STORAGE_BLOCK_SIZE;
    if (tag_size == 0 || count == 0 || (tag_size % count) != 0)
        return; // no tags (or unexpected layout)
    uint8_t *tags = (uint8_t *)malloc(tag_size);
    if (!tags)
        return;
    if (gs_source_read_exact(dc42, (uint64_t)DISKCOPY_HEADER_SIZE + data_size, tags, tag_size) != 0) {
        free(tags);
        return;
    }
    image->tags = tags;
    image->tag_bytes = tag_size / count;
    image->tag_count = count;
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

// Scratch sidecars -- read-only deltas and blank-image deltas -- live under
// image_scratch_dir() (image_scratch.h), which honours GS_STORAGE_CACHE;
// so does a writable mount whose caller names no delta directory.  Decoded images are
// never written anywhere: an NDIF, UDIF or archived image is a source the
// storage reads through (source.h, format_registry.h).

// ============================================================================
// Opening
// ============================================================================
//
// Every opener comes down to image_open_source: the caller's forks go
// through the format registry's wrapper loop (UDIF, NDIF, DiskCopy 4.2,
// MacBinary, BinHex, gzip -- any depth), and the innermost source is the
// storage's base.  The path openers first open the path's forks through the
// installed path opener, so a path may continue through an image or an
// archive (outer.img/partition1/inner.img, roms.zip/disk.img.gz).

// image_wrap_read_fn over a source.
static bool source_read_cb(void *ctx, uint64_t offset, uint8_t *buf, size_t size) {
    return gs_source_read_exact((gs_source_t *)ctx, offset, buf, size) == 0;
}

// How an image is opened.
typedef enum { OPEN_READONLY, OPEN_CREATE, OPEN_REOPEN } open_mode_t;

// Build an image over (`data`, `rsrc`), named `name`.  For OPEN_CREATE the
// delta goes in `dir` (a fresh instance); for OPEN_REOPEN `dir` is the
// instance stem.  NULL (with errno set) on failure.
static image_t *image_open_source(const char *name, gs_source_t *data, gs_source_t *rsrc, image_geometry_t geom,
                                  open_mode_t mode, const char *dir) {
    uint32_t block_size = geometry_block_size(geom);
    gs_unwrapped_t u;
    gs_format_unwrap(data, rsrc, &u);
    // A UDIF or NDIF image that would not open is no disk: read raw it is
    // its compressed payload, which no guest should see.  (DiskCopy 4.2 and
    // peeler's wrappers detect from content that a raw disk can also carry,
    // so their failure leaves the bytes as they are.)
    if (u.failed_format && (strcmp(u.failed_format, "udif") == 0 || strcmp(u.failed_format, "ndif") == 0)) {
        gs_outf("image: '%s' is a %s image that cannot be read in place (%s)\n", name,
                strcmp(u.failed_format, "udif") == 0 ? "UDIF" : "NDIF",
                u.failed_rc == -EFBIG     ? "a chunk is too large to decode on demand; import it to re-chunk it"
                : u.failed_rc == -ENOTSUP ? "it uses a compression this emulator does not decode"
                                          : strerror(-u.failed_rc));
        int frc = u.failed_rc;
        gs_unwrapped_free(&u);
        errno = frc == -ENOTSUP ? ENOTSUP : EINVAL;
        return NULL;
    }
    uint64_t raw = gs_source_size(u.data);
    if (raw == 0 || (raw % block_size) != 0 || raw > SIZE_MAX) {
        gs_unwrapped_free(&u);
        errno = EINVAL;
        return NULL;
    }

    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image) {
        gs_unwrapped_free(&u);
        errno = ENOMEM;
        return NULL;
    }
    // A volume trimmed short of its own header's claim is opened at that
    // claim (image_wrap.h): the storage's base becomes the file followed by
    // zeros, with a copy of the volume's header where its alternate belongs
    // (the second-last block; Disk First Aid rejects a volume without it).
    // Guest writes to the tail land in the delta like any others.
    gs_source_t *padded = NULL;
    if (block_size == STORAGE_BLOCK_SIZE) {
        uint64_t start = 0;
        uint64_t blocks = image_wrap_extended_blocks(source_read_cb, u.data, raw / block_size, &start);
        uint8_t hdr[STORAGE_BLOCK_SIZE];
        if (blocks * block_size > raw && blocks * block_size <= SIZE_MAX &&
            gs_source_read_exact(u.data, (start + 2) * block_size, hdr, sizeof(hdr)) == 0) {
            uint64_t alt = (blocks - 2) * block_size;
            padded = gs_source_pad(u.data, blocks * block_size, alt, hdr, alt >= raw ? sizeof(hdr) : 0);
        }
        if (padded) {
            LOG(1, "'%s': its volume claims %llu blocks but the file holds %llu; the missing tail reads as zeros", name,
                (unsigned long long)blocks, (unsigned long long)(raw / block_size));
            raw = blocks * block_size;
        }
    }

    image->filename = gs_strdup(name);
    image->source_key = gs_strdup(gs_source_key(data));
    image->format = gs_strdup(u.chain[0] ? u.chain : "raw");
    image->raw_size = (size_t)raw;
    image->block_size = block_size;
    image->type = classify_image((size_t)raw);
    // A disc is told apart by its content, not its size: an ISO 9660 primary
    // volume descriptor at 32 KB (an HFS-only CD stays a hard disk -- nothing
    // in its bytes says CD).
    if (image->type == image_hd && block_size == STORAGE_BLOCK_SIZE &&
        iso_probe_source(u.data, 0, gs_source_size(u.data)))
        image->type = image_cdrom;
    image->writable = mode != OPEN_READONLY;
    image->from_diskcopy = u.dc42 != NULL;

    if (mode == OPEN_REOPEN) {
        image->instance_path = gs_strdup(dir);
    } else if (mode == OPEN_CREATE) {
        char id[17];
        mint_random_hex_id(id);
        image->instance_path = gs_str_printf("%s/%s", dir, id);
    } else {
        // A read-only mount's delta+journal are ghosts in the scratch root,
        // so they never land beside the media.
        gs_mkdir_p(image_scratch_dir());
        char id[17];
        mint_random_hex_id(id);
        image->ghost_instance = true;
        image->delta_path = gs_str_printf("%s/%s.delta", image_scratch_dir(), id);
        image->journal_path = gs_str_printf("%s/%s.journal", image_scratch_dir(), id);
    }
    if (image->instance_path) {
        image->delta_path = gs_str_printf("%s.delta", image->instance_path);
        image->journal_path = gs_str_printf("%s.journal", image->instance_path);
    }
    if (!image->filename || !image->source_key || !image->format || !image->delta_path || !image->journal_path) {
        gs_source_release(padded);
        gs_unwrapped_free(&u);
        image_close(image);
        errno = ENOMEM;
        return NULL;
    }

    storage_config_t config = {0};
    config.base = padded ? padded : u.data;
    config.delta_path = image->delta_path;
    config.journal_path = image->journal_path;
    config.block_count = image->raw_size / image->block_size;
    config.block_size = image->block_size;
    int rc = storage_new(&config, &image->storage);
    gs_source_release(padded);
    if (rc == GS_SUCCESS && u.dc42)
        image_load_diskcopy_tags(image, u.dc42);
    gs_unwrapped_free(&u);
    if (rc != GS_SUCCESS) {
        gs_outf("image: storage engine failed for %s (error %d)\n", name, rc);
        image_close(image);
        errno = EIO;
        return NULL;
    }
    if (image->writable)
        writable_register(image, name);
    LOG(3, "opened '%s' (%s, %zu bytes%s)", name, image->format, image->raw_size, image->writable ? ", writable" : "");
    return image;
}

// Open the forks of `path` and build the image.  The resource fork is
// optional (an NDIF image needs it; nothing else does).
static image_t *image_open_path(const char *path, image_geometry_t geom, open_mode_t mode, const char *dir) {
    if (!path || !*path) {
        errno = EINVAL;
        return NULL;
    }
    int err = 0;
    gs_source_t *data = gs_source_open_path(path, GS_FORK_DATA, &err);
    if (!data) {
        errno = err ? -err : ENOENT;
        return NULL;
    }
    gs_source_t *rsrc = gs_source_open_path(path, GS_FORK_RSRC, NULL);
    image_t *img = image_open_source(path, data, rsrc, geom, mode, dir);
    int saved = errno;
    gs_source_release(data);
    gs_source_release(rsrc);
    errno = saved;
    return img;
}

image_t *image_open_readonly(const char *base_path) {
    return image_open_readonly_with_geometry(base_path, (image_geometry_t){.block_size = STORAGE_BLOCK_SIZE});
}

image_t *image_open_readonly_with_geometry(const char *base_path, image_geometry_t geom) {
    return image_open_path(base_path, geom, OPEN_READONLY, NULL);
}

image_t *image_open_readonly_source(const char *name, gs_source_t *data, gs_source_t *rsrc) {
    if (!data) {
        errno = EINVAL;
        return NULL;
    }
    return image_open_source(name, data, rsrc, (image_geometry_t){.block_size = STORAGE_BLOCK_SIZE}, OPEN_READONLY,
                             NULL);
}

image_t *image_create(const char *base_path, const char *delta_dir) {
    return image_create_with_geometry(base_path, delta_dir, (image_geometry_t){.block_size = STORAGE_BLOCK_SIZE});
}

image_t *image_create_with_geometry(const char *base_path, const char *delta_dir, image_geometry_t geom) {
    if (!base_path || !*base_path)
        return NULL;
    // No write-access probe: only the delta needs to be writable, and the
    // base can legitimately live on a read-only FS (some tests, distribution
    // mounts) -- or inside an image or an archive.
    //
    // Default delta_dir: the scratch root (image_scratch_dir: GS_STORAGE_CACHE
    // when set).  Never the base's own directory: a delta is per-instance, so
    // one left beside the base is an orphan the moment the process exits, and
    // shared media (tests/data) would collect them.  Headless callers with no
    // machine directory pass NULL.
    if (!delta_dir || !*delta_dir)
        delta_dir = image_scratch_dir();
    if (gs_mkdir_p(delta_dir) != 0) {
        gs_outf("image_create: cannot create delta directory: %s\n", delta_dir);
        return NULL;
    }
    return image_open_path(base_path, geom, OPEN_CREATE, delta_dir);
}

image_t *image_open(const char *base_path, const char *instance_path) {
    return image_open_with_geometry(base_path, instance_path, (image_geometry_t){.block_size = STORAGE_BLOCK_SIZE});
}

image_t *image_open_with_geometry(const char *base_path, const char *instance_path, image_geometry_t geom) {
    if (!base_path || !*base_path || !instance_path || !*instance_path)
        return NULL;
    return image_open_path(base_path, geom, OPEN_REOPEN, instance_path);
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

    // Place the delta+journal in the scratch root so the blank disk's
    // sidecars don't clutter any user directory; ghost_instance unlinks them on
    // image_close.  The image is ephemeral unless exported via image_export_to.
    gs_mkdir_p(image_scratch_dir());
    char id[17];
    mint_random_hex_id(id);
    image->instance_path = NULL; // never serialized
    image->delta_path = gs_str_printf("%s/%s.delta", image_scratch_dir(), id);
    image->journal_path = gs_str_printf("%s/%s.journal", image_scratch_dir(), id);
    if (!image->delta_path || !image->journal_path) {
        image->writable = false; // never registered
        image_close(image);
        return NULL;
    }

    storage_config_t config = {0};
    config.delta_path = image->delta_path;
    config.journal_path = image->journal_path;
    config.block_count = block_count;
    config.block_size = block_size;
    int err = storage_new(&config, &image->storage);
    if (err != GS_SUCCESS) {
        gs_outf("image_create_blank: storage engine failed (%llu x %u, error %d)\n", (unsigned long long)block_count,
                block_size, err);
        image->writable = false;
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

// Bytes of synthesised wrapper prefix in front of the storage (0 unless
// the image was wrapped — image_wrap.h).
static size_t wrap_bytes(const image_t *disk) {
    return (size_t)disk->wrap_blocks * disk->block_size;
}

static size_t storage_read_range(image_t *disk, size_t offset, uint8_t *buf, size_t size);
static size_t storage_write_range(image_t *disk, size_t offset, uint8_t *buf, size_t size);

size_t disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    if (!disk || !disk->storage || !buf || size == 0)
        return 0;
    disk->reads++; // the activity light's only cost on this path
    GS_ASSERT((offset % disk->block_size) == 0);
    GS_ASSERT((size % disk->block_size) == 0);
    size_t pre = wrap_bytes(disk);
    if (offset < pre) {
        // The wrapper's prefix is served from memory; the rest of the
        // request continues on the volume's storage.
        size_t n = pre - offset < size ? pre - offset : size;
        memcpy(buf, disk->wrap_prefix + offset, n);
        if (n == size)
            return size;
        return n + storage_read_range(disk, 0, buf + n, size - n);
    }
    return storage_read_range(disk, offset - pre, buf, size);
}

// Read whole blocks at `offset` of the image's volume (past any wrapper
// prefix; wrap_base bytes into the storage).  Returns `size`, or the bytes read before a backing failure.
static size_t storage_read_range(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    size_t vol_size = disk->raw_size - wrap_bytes(disk);
    // An undersized / truncated image (a host file shorter than the media it
    // backs) can be read past its end — e.g. the Finder reading high tracks
    // while ejecting a 400K/800K-geometry floppy backed by a too-small file.
    // Serve the unbacked tail as blank media (zeroes) rather than asserting,
    // so the guest sees readable-but-empty sectors and the operation can
    // finish.  `backed` is the in-bounds, whole-block byte count.
    size_t backed = (offset < vol_size) ? (vol_size - offset) : 0;
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
        int rc = storage_read_block(disk->storage, disk->wrap_base + offset + transferred, buf + transferred);
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
    disk->writes++;
    GS_ASSERT((offset % disk->block_size) == 0);
    GS_ASSERT((size % disk->block_size) == 0);
    size_t pre = wrap_bytes(disk);
    if (offset < pre) {
        // A write into the wrapper's prefix (a partitioning tool rewriting
        // the map) lands in the in-memory prefix only: the volume file is
        // never given a partition map, and the prefix is rebuilt on reopen.
        size_t n = pre - offset < size ? pre - offset : size;
        memcpy(disk->wrap_prefix + offset, buf, n);
        LOG(1, "disk_write_data: %zu-byte write into the wrapper prefix at %zu kept in memory only", n, offset);
        if (n == size)
            return size;
        return n + storage_write_range(disk, 0, buf + n, size - n);
    }
    return storage_write_range(disk, offset - pre, buf, size);
}

// Write whole blocks at `offset` of the image's volume (past any wrapper
// prefix; wrap_base bytes into the storage).  Returns `size`, or the bytes written before a backing failure.
static size_t storage_write_range(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    size_t vol_size = disk->raw_size - wrap_bytes(disk);
    // Symmetric with disk_read_data: a write past the end of an undersized
    // image targets sectors with no backing store.  Drop the unbacked tail
    // (it cannot be stored) rather than asserting, so the guest's volume
    // flush during eject completes; warn so the dropped write is visible.
    size_t backed = (offset < vol_size) ? (vol_size - offset) : 0;
    if (backed > size)
        backed = size;
    backed -= backed % disk->block_size;
    if (backed < size)
        LOG(1,
            "disk_write_data: %zu-byte write at offset %zu runs past image end (raw_size=%zu); dropped %zu-byte tail",
            size, offset, disk->raw_size, size - backed);
    size_t transferred = 0;
    while (transferred < backed) {
        int rc = storage_write_block(disk->storage, disk->wrap_base + offset + transferred, buf + transferred);
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

// storage_save_state emits a block at a time, so the destination stream sees
// one small fwrite per block.  With stdio's default ~1 KB buffer that is still
// a filesystem call every two blocks; a large buffer turns the whole export
// into a handful of big writes.  Returns the buffer, which the caller must
// keep alive until fclose and then free (NULL is harmless — the stream just
// keeps its default buffering).
#define IMAGE_EXPORT_BUFFER_BYTES (4u * 1024 * 1024)

static char *stream_set_large_buffer(FILE *f) {
    char *buf = malloc(IMAGE_EXPORT_BUFFER_BYTES);
    if (buf && setvbuf(f, buf, _IOFBF, IMAGE_EXPORT_BUFFER_BYTES) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

// Create `path` for writing, refusing a file that exists: the check and the
// create are one open(O_CREAT | O_EXCL), so a file that appears between a
// caller's check and the write is never truncated.  NULL with errno set
// (EEXIST for an existing file).
static FILE *create_exclusive(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        int e = errno;
        close(fd);
        remove(path);
        errno = e;
    }
    return f;
}

struct image_export {
    storage_export_view_t *view;
    char *dest;
    char *name; // the image's own name, recorded in a UDIF export
    uint32_t block_size;
};

image_export_t *image_export_begin(image_t *image, const char *dest_path, char *err, size_t err_cap) {
    if (err && err_cap)
        err[0] = '\0';
    if (!image || !image->storage || !dest_path || !*dest_path) {
        if (err)
            snprintf(err, err_cap, "no medium, or no destination");
        return NULL;
    }
    // Refuse to overwrite existing files
    FILE *exist = fopen(dest_path, "rb");
    if (exist) {
        fclose(exist);
        if (err)
            snprintf(err, err_cap, "'%s' exists (refuses to overwrite)", dest_path);
        return NULL;
    }
    image_export_t *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->dest = strdup(dest_path);
    e->block_size = image->block_size;
    if (image->filename) {
        const char *slash = strrchr(image->filename, '/');
        e->name = strdup(slash ? slash + 1 : image->filename);
    }
    e->view = storage_export_view_begin(image->storage);
    if (!e->dest || !e->view) {
        if (err)
            snprintf(err, err_cap, "cannot snapshot the disk for export");
        image_export_end(e);
        return NULL;
    }
    return e;
}

// A destination named .dmg gets a UDIF (udif_writer.h): zero runs cost
// nothing and the rest is deflated, so a modified 2 GB disk exports at its
// content's size.  Any other name gets the flat raw image.
static bool export_is_udif(const image_export_t *e) {
    size_t n = strlen(e->dest);
    return e->block_size == UDIF_SECTOR_SIZE && n >= 4 && strcasecmp(e->dest + n - 4, ".dmg") == 0;
}

static int udif_write_cb(void *ctx, const void *data, size_t size) {
    return udif_writer_append((udif_writer_t *)ctx, data, size) == 0 ? 0 : -1;
}

static int image_export_run_udif(image_export_t *e, char *err, size_t err_cap) {
    udif_writer_opts_t o = {.level = 1, .source_name = e->name};
    udif_writer_t *w = udif_writer_open(e->dest, &o, err, err_cap);
    if (!w)
        return -EIO;
    int rc = storage_export_view_write(e->view, w, udif_write_cb);
    if (rc != GS_SUCCESS) {
        udif_writer_abort(w);
        if (rc == -ECANCELED) {
            if (err)
                snprintf(err, err_cap, "cancelled");
            return -ECANCELED;
        }
        if (err)
            snprintf(err, err_cap, "write to '%s' failed", e->dest);
        return -EIO;
    }
    rc = udif_writer_finish(w, NULL);
    if (rc != 0 && err)
        snprintf(err, err_cap, "write to '%s' failed: %s", e->dest, strerror(-rc));
    return rc;
}

int image_export_run(image_export_t *e, char *err, size_t err_cap) {
    if (!e || !e->view)
        return -EINVAL;
    gs_mkdir_parents(e->dest);
    if (export_is_udif(e))
        return image_export_run_udif(e, err, err_cap);
    // Exclusively: begin refused an existing file, and one that has appeared
    // since is refused here rather than truncated.
    FILE *f = create_exclusive(e->dest);
    if (!f) {
        int rc = errno ? errno : EIO;
        if (err)
            snprintf(err, err_cap, "cannot create '%s': %s", e->dest, strerror(rc));
        return -rc;
    }
    char *iobuf = stream_set_large_buffer(f);
    int rc = storage_export_view_write(e->view, f, file_write_cb);
    bool closed = fclose(f) == 0;
    free(iobuf);
    if (rc != GS_SUCCESS || !closed) {
        remove(e->dest);
        if (rc == -ECANCELED) {
            if (err)
                snprintf(err, err_cap, "cancelled");
            return -ECANCELED;
        }
        if (err)
            snprintf(err, err_cap, "write to '%s' failed", e->dest);
        return -EIO;
    }
    return 0;
}

void image_export_end(image_export_t *e) {
    if (!e)
        return;
    storage_export_view_end(e->view);
    free(e->dest);
    free(e->name);
    free(e);
}

// Export the full disk content (base + delta) to a new file at dest_path,
// here and now.
int image_export_to(image_t *image, const char *dest_path) {
    image_export_t *e = image_export_begin(image, dest_path, NULL, 0);
    if (!e)
        return -1;
    int rc = image_export_run(e, NULL, 0);
    image_export_end(e);
    return rc == 0 ? 0 : -1;
}

// ============================================================================
// Image creation
// ============================================================================

int image_create_empty(const char *filename, size_t size) {
    if (!filename || !*filename || size == 0)
        return -1;
    gs_mkdir_parents(filename);
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

int image_create_empty_udif(const char *filename, uint64_t size) {
    if (!filename || !*filename || size == 0)
        return -1;
    gs_mkdir_parents(filename);
    return udif_create_empty(filename, size) == 0 ? 0 : -1;
}

int image_create_blank_floppy(const char *filename, bool overwrite, bool high_density) {
    if (!filename || !*filename)
        return -1;
    FILE *f = overwrite ? fopen(filename, "wb") : create_exclusive(filename);
    if (!f)
        return (!overwrite && errno == EEXIST) ? IMAGE_CREATE_EXISTS : -1;
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
    gs_mkdir_parents(filename);
    FILE *f = create_exclusive(filename);
    if (!f)
        return errno == EEXIST ? IMAGE_CREATE_EXISTS : -1;
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
// Tracking / module lifecycle
// ============================================================================

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

image_t *images_find(const image_list_t *images, const char *name) {
    if (!images || !name)
        return NULL;
    for (int i = 0; i < images->n; i++) {
        const char *n = image_get_filename(images->items[i]);
        if (n && strcmp(n, name) == 0)
            return images->items[i];
    }
    return NULL;
}

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

    // Bit 0: writable.  Bit 1: wrapped by the bare-volume wrapper — the
    // restore re-wraps rather than trusting the file to say so (the prefix
    // is never in the file).  raw_size is the storage's own size, which is
    // what the restore's geometry check and base materialisation expect.
    uint8_t flags =
        (uint8_t)((image->writable ? IMAGE_CKPT_WRITABLE : 0) | (image->wrap_prefix ? IMAGE_CKPT_WRAPPED : 0));
    system_write_checkpoint_data(checkpoint, &flags, sizeof(flags));

    uint64_t raw_size = (uint64_t)(image->wrap_prefix ? image->wrap_storage_size : image->raw_size);
    system_write_checkpoint_data(checkpoint, &raw_size, sizeof(raw_size));

    // Persist the instance path so a future restore can reopen the same delta
    // directory without relying on adjacent-to-base sidecars.  Empty
    // string for read-only / ghost mounts.
    const char *instance = (image->writable && image->instance_path) ? image->instance_path : "";
    uint32_t instance_len = (uint32_t)(strlen(instance) + 1);
    system_write_checkpoint_data(checkpoint, &instance_len, sizeof(instance_len));
    system_write_checkpoint_data(checkpoint, instance, instance_len);

    // The key of the source the path opened (source.h): a restore that
    // reuses the base checks it is still the same bytes -- the same file,
    // size and time stamp, or the same member of the same archive.
    const char *key = image->source_key ? image->source_key : "";
    uint32_t key_len = (uint32_t)(strlen(key) + 1);
    system_write_checkpoint_data(checkpoint, &key_len, sizeof(key_len));
    system_write_checkpoint_data(checkpoint, key, key_len);

    if (image->storage) {
        int rc = storage_checkpoint(image->storage, checkpoint);
        if (rc != GS_SUCCESS) {
            LOG(1, "image_checkpoint: storage_checkpoint failed for %s (%d)",
                image->filename ? image->filename : "<unknown>", rc);
        }
    }
}
