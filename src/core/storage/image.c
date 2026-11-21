// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image.c
// Disk image handling: open, read/write, and format detection for floppy and hard disk images.
//
// Each image is backed by a delta-file storage engine.  The original image file
// is opened read-only as the "base"; all modifications go to a .delta file.
// A .journal file provides crash recovery.

#include "image.h"
#include "out.h"

#include "checkpoint.h"
#include "format_registry.h"
#include "gs_assert.h"
#include "image_internal.h"
#include "image_moof.h"
#include "image_scratch.h"
#include "image_udif.h"
#include "image_wrap.h"
#include "log.h"
#include "platform.h"
#include "source.h"
#include "status.h"
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
#include <time.h>
#include <unistd.h>

#if defined(_WIN32)
#include <direct.h>
#endif

LOG_USE_CATEGORY_NAME("image")

// ============================================================================
// Format detection
// ============================================================================

// ============================================================================
// Image lifecycle
// ============================================================================

uint64_t disk_size(image_t *disk) {
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
        if (im->source_key && source_key_within(key, im->source_key))
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
    uint8_t bytes[8];
    random_bytes(bytes, sizeof bytes);
    hex_encode(out, bytes, sizeof bytes, false);
}

// Give `image` ghost sidecars: a delta and a journal under a fresh id in the
// scratch root, removed on close, so they never land beside the media or in
// a user directory.  0, or -ENOMEM (either path may be left NULL).
static int ghost_paths(image_t *image) {
    mkdir_p(image_scratch_dir());
    char id[17];
    mint_random_hex_id(id);
    image->ghost_instance = true;
    image->delta_path = str_printf("%s/%s.delta", image_scratch_dir(), id);
    image->journal_path = str_printf("%s/%s.journal", image_scratch_dir(), id);
    return image->delta_path && image->journal_path ? 0 : -ENOMEM;
}

// The message and code of an export whose writer stopped with `rc` (a
// negative errno; a storage status is one): cancelled, an unreadable disk
// image (`unreadable`: the view's own read failed), or a failed write to
// `dest`.  Which of the last two is the caller's record, not the number:
// both are -EIO.  The words are user-visible.
static int export_fail(int rc, bool unreadable, const char *dest, char *err, size_t err_cap) {
    if (rc == -ECANCELED) {
        if (err)
            snprintf(err, err_cap, "cancelled");
        return -ECANCELED;
    }
    if (err)
        snprintf(err, err_cap,
                 unreadable ? "the disk image could not be read; '%s' not written" : "write to '%s' failed", dest);
    return -EIO;
}

// Keep a DiskCopy 4.2 header's disk name and format byte for a DiskCopy
// export, so an unchanged disk exports back to the same bytes; and say where
// its tag section is and how many bytes per sector it holds (0: none).
// `dc42` is the DiskCopy file itself.  Best-effort: a header that does not
// parse leaves the image unlabelled and tagless.
static uint32_t image_load_diskcopy_identity(image_t *image, source_t *dc42, uint64_t *tags_at) {
    uint8_t header[DISKCOPY_HEADER_SIZE];
    if (source_read_exact(dc42, 0, header, sizeof(header)) != 0)
        return 0;
    // Every value is re-derived from this header and checked against this
    // file.  DiskCopy 4.2 sectors are 512 data bytes.
    uint32_t data_size = 0, tag_size = 0;
    if (!dc42_parse_header(header, sizeof(header), source_size(dc42), &data_size, &tag_size))
        return 0;
    memcpy(image->dc42_name, header, DC42_NAME_FIELD);
    image->dc42_format_byte = header[DC42_OFF_FORMAT_BYTE];
    uint32_t count = data_size / STORAGE_BLOCK_SIZE;
    if (tag_size == 0 || count == 0 || (tag_size % count) != 0)
        return 0; // no tags (or an unexpected layout)
    *tags_at = (uint64_t)DISKCOPY_HEADER_SIZE + data_size;
    return tag_size / count;
}

// A base of 512-byte sectors seen as whole blocks, tag first: each block is
// its sector's `t` tag bytes -- from a tag section (a DiskCopy file's) when
// there is one, else zeros -- then its 512 data bytes.  What gives a
// 400K/800K GCR floppy from a raw, UDIF or DiskCopy file its in-band tags.
typedef struct {
    source_t *data; // the sectors
    source_t *tags; // the tag section's file, or NULL for zero tags
    uint64_t tags_at; // where the section starts in it
    uint32_t t; // tag bytes per sector
    uint64_t sectors;
    char *key;
} tagged_src_t;

static int64_t tagged_read(source_t *s, uint64_t off, void *buf, size_t len) {
    tagged_src_t *m = s->ctx;
    uint32_t unit = STORAGE_BLOCK_SIZE + m->t;
    uint64_t size = m->sectors * unit;
    if (off >= size)
        return 0;
    if (len > size - off)
        len = (size_t)(size - off);
    uint8_t block[STORAGE_MAX_BLOCK_SIZE];
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t sector = pos / unit;
        uint32_t within = (uint32_t)(pos % unit);
        // Assemble the sector's block: its tag, then its data.
        memset(block, 0, m->t);
        int rc = m->tags ? source_read_exact(m->tags, m->tags_at + sector * m->t, block, m->t) : 0;
        if (!rc)
            rc = source_read_exact(m->data, sector * STORAGE_BLOCK_SIZE, block + m->t, STORAGE_BLOCK_SIZE);
        if (rc)
            return done ? (int64_t)done : rc;
        size_t n = unit - within;
        if (n > len - done)
            n = len - done;
        memcpy((uint8_t *)buf + done, block + within, n);
        done += n;
    }
    return (int64_t)done;
}

static uint64_t tagged_size(source_t *s) {
    tagged_src_t *m = s->ctx;
    return m->sectors * (STORAGE_BLOCK_SIZE + m->t);
}

static const char *tagged_key(source_t *s) {
    return ((tagged_src_t *)s->ctx)->key;
}

static source_tier_t tagged_tier(source_t *s) {
    return source_tier(((tagged_src_t *)s->ctx)->data); // arithmetic over the sectors
}

static void tagged_close(source_t *s) {
    tagged_src_t *m = s->ctx;
    if (m) {
        source_release(m->tags);
        free(m->key);
        free(m);
    }
}

static const source_ops_t tagged_ops = {tagged_read, tagged_size, tagged_key, tagged_tier, tagged_close, NULL};

// The tagged view over `data` (`sectors` sectors), its tags from `tags` at
// `tags_at` or zeros.  NULL when out of memory.
static source_t *tagged_view(source_t *data, source_t *tags, uint64_t tags_at, uint32_t t, uint64_t sectors) {
    tagged_src_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->data = data;
    m->tags = tags ? source_retain(tags) : NULL;
    m->tags_at = tags_at;
    m->t = t;
    m->sectors = sectors;
    m->key = str_printf("%s#tags%u", source_key(data), (unsigned)t);
    if (!m->key) {
        source_release(m->tags);
        free(m);
        return NULL;
    }
    return source_new(&tagged_ops, m, data); // closes `m` itself when it fails
}

void image_set_diskcopy_identity(image_t *image, const char *name, uint8_t format_byte) {
    if (!image || image->from_diskcopy)
        return; // a DiskCopy source keeps the label it came with
    dc42_name_field(image->dc42_name, name);
    image->dc42_format_byte = format_byte;
}

// Note a read the backing image could not serve: the guest sees a read error
// (the short count each device turns into its own).  The first is logged.
static void note_read_error(image_t *disk, uint64_t offset, size_t size) {
    if (disk->read_errors++ == 0)
        LOG(0,
            "%s: unreadable at offset %llu (%zu bytes): the backing image failed the read; the guest sees a read error",
            disk->filename ? disk->filename : "image", (unsigned long long)offset, size);
    else
        LOG(1, "%s: unreadable at offset %llu (%zu bytes), read error %llu", disk->filename ? disk->filename : "image",
            (unsigned long long)offset, size, (unsigned long long)disk->read_errors);
}

// Sectors of a tagged image (tags in-band).
static uint64_t tagged_sectors(const image_t *disk) {
    return disk->raw_size / STORAGE_BLOCK_SIZE;
}

size_t disk_read_tag(image_t *disk, size_t sector, uint8_t *buf, size_t size) {
    if (!disk || !disk->tag_bytes || !buf || sector >= tagged_sectors(disk))
        return 0;
    uint8_t block[STORAGE_MAX_BLOCK_SIZE];
    if (storage_read_block(disk->storage, (uint64_t)sector * disk->block_size, block) != STATUS_OK) {
        note_read_error(disk, (uint64_t)sector * STORAGE_BLOCK_SIZE, disk->tag_bytes);
        return 0;
    }
    size_t n = size < disk->tag_bytes ? size : disk->tag_bytes;
    memcpy(buf, block, n); // the tag leads its block
    return n;
}

// Persist a sector's tag (pagelabel).  The Lisa Sony controller writes the
// 512-byte data sector *and* its tag together; modelling only the data drops
// the FS pagelabel updates the OS makes on every write.  The tag is part of
// its sector's block, so it lands in the delta with the data; the base file
// is untouched.
size_t disk_write_tag(image_t *disk, size_t sector, const uint8_t *buf, size_t size) {
    if (!disk || !disk->tag_bytes || !disk->writable || !buf || sector >= tagged_sectors(disk))
        return 0;
    uint8_t block[STORAGE_MAX_BLOCK_SIZE];
    uint64_t at = (uint64_t)sector * disk->block_size;
    if (storage_read_block(disk->storage, at, block) != STATUS_OK)
        return 0;
    size_t n = size < disk->tag_bytes ? size : disk->tag_bytes;
    memcpy(block, buf, n);
    if (storage_write_block(disk->storage, at, block) != STATUS_OK)
        return 0;
    disk->writes++;
    return n;
}

size_t disk_read_units(image_t *disk, uint64_t first, uint8_t *buf, size_t count) {
    if (!disk || !disk->storage || !buf || count == 0)
        return 0;
    disk->reads++;
    uint64_t blocks = disk->raw_size / STORAGE_BLOCK_SIZE;
    if (first >= blocks || count > blocks - first)
        return 0;
    if (storage_read_blocks(disk->storage, first * disk->block_size, buf, count) != STATUS_OK) {
        note_read_error(disk, first * STORAGE_BLOCK_SIZE, count * disk->block_size);
        return 0;
    }
    return count * disk->block_size;
}

size_t disk_write_units(image_t *disk, uint64_t first, const uint8_t *buf, size_t count) {
    if (!disk || !disk->storage || !buf || count == 0 || !disk->writable)
        return 0;
    disk->writes++;
    uint64_t blocks = disk->raw_size / STORAGE_BLOCK_SIZE;
    if (first >= blocks || count > blocks - first)
        return 0;
    for (size_t i = 0; i < count; i++)
        if (storage_write_block(disk->storage, (first + i) * disk->block_size, buf + i * disk->block_size) != STATUS_OK)
            return i * disk->block_size;
    return count * disk->block_size;
}

// The data plane of a tagged image: whole 512-byte sectors at `offset`,
// each the data half of its block.  A read past the end is blank media.
static size_t tagged_read_data(image_t *disk, uint64_t offset, uint8_t *buf, size_t size) {
    uint8_t block[STORAGE_MAX_BLOCK_SIZE];
    for (size_t done = 0; done < size; done += STORAGE_BLOCK_SIZE) {
        uint64_t sector = (offset + done) / STORAGE_BLOCK_SIZE;
        if (sector >= tagged_sectors(disk)) {
            memset(buf + done, 0, size - done);
            break;
        }
        if (storage_read_block(disk->storage, sector * disk->block_size, block) != STATUS_OK) {
            note_read_error(disk, offset + done, STORAGE_BLOCK_SIZE);
            return 0;
        }
        memcpy(buf + done, block + disk->tag_bytes, STORAGE_BLOCK_SIZE);
    }
    return size;
}

// ...and its writes: each sector's data replaces the data half of its block,
// whose tag stays as it was.  A write past the end is dropped.
static size_t tagged_write_data(image_t *disk, uint64_t offset, const uint8_t *buf, size_t size) {
    uint8_t block[STORAGE_MAX_BLOCK_SIZE];
    for (size_t done = 0; done < size; done += STORAGE_BLOCK_SIZE) {
        uint64_t sector = (offset + done) / STORAGE_BLOCK_SIZE;
        if (sector >= tagged_sectors(disk))
            break;
        uint64_t at = sector * disk->block_size;
        if (storage_read_block(disk->storage, at, block) != STATUS_OK)
            return done;
        memcpy(block + disk->tag_bytes, buf + done, STORAGE_BLOCK_SIZE);
        if (storage_write_block(disk->storage, at, block) != STATUS_OK)
            return done;
    }
    return size;
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
// through the format registry's wrapper loop (UDIF, NDIF, QCOW2, DiskCopy 4.2,
// MacBinary, BinHex, gzip -- any depth), and the innermost source is the
// storage's base.  The path openers first open the path's forks through the
// installed path opener, so a path may continue through an image or an
// archive (outer.img/partition1/inner.img, roms.zip/disk.img.gz).

// image_wrap_read_fn over a source.
static bool source_read_cb(void *ctx, uint64_t offset, uint8_t *buf, size_t size) {
    return source_read_exact((source_t *)ctx, offset, buf, size) == 0;
}

// How an image is opened.
typedef enum { OPEN_READONLY, OPEN_CREATE, OPEN_REOPEN } open_mode_t;

// Why this thread's last open of a container image failed (image_open_error).
static _Thread_local char t_open_error[256];

const char *image_open_error(void) {
    return t_open_error;
}

// Build an image over (`data`, `rsrc`), named `name`.  For OPEN_CREATE the
// delta goes in `dir` (a fresh instance); for OPEN_REOPEN `dir` is the
// instance stem.  NULL (with errno set) on failure.
static image_t *image_open_source(const char *name, source_t *data, source_t *rsrc, image_geometry_t geom,
                                  open_mode_t mode, const char *dir) {
    format_unwrapped_t u;
    t_open_error[0] = '\0';
    format_unwrap(data, rsrc, &u);
    // A UDIF, NDIF or QCOW2 image that would not open is no disk: read raw
    // it is its container, which no guest should see.  Nor is a MOOF that
    // does not decode -- its magic is unambiguous.  (DiskCopy 4.2 and
    // peeler's wrappers detect from content that a raw disk can also carry,
    // so their failure leaves the bytes as they are.)
    if (u.failed_format && strcmp(u.failed_format, "moof") == 0) {
        snprintf(t_open_error, sizeof t_open_error, "a MOOF image that cannot be decoded (%s)",
                 u.failed_rc == -ENOTSUP ? "only 400K and 800K GCR disks are read; this is a 1.44 MB MFM or Twiggy disk"
                 : u.failed_rc == -EINVAL ? "it holds no readable Macintosh GCR sector"
                                          : strerror(-u.failed_rc));
        out_printf("image: '%s' is %s\n", name, t_open_error);
        int frc = u.failed_rc;
        format_unwrapped_free(&u);
        errno = frc == -ENOTSUP ? ENOTSUP : EINVAL;
        return NULL;
    }
    if (u.failed_format && (strcmp(u.failed_format, "udif") == 0 || strcmp(u.failed_format, "ndif") == 0 ||
                            strcmp(u.failed_format, "qcow2") == 0)) {
        bool qcow2 = strcmp(u.failed_format, "qcow2") == 0;
        snprintf(t_open_error, sizeof t_open_error, "a %s image that cannot be read in place (%s)",
                 strcmp(u.failed_format, "udif") == 0   ? "UDIF"
                 : strcmp(u.failed_format, "ndif") == 0 ? "NDIF"
                                                        : "QCOW2",
                 qcow2 && u.failed_rc == -ENOTSUP
                     ? "it uses a backing file, encryption, an external data file or zstd compression"
                 : qcow2 && u.failed_rc == -EFBIG ? "its cluster map is too fragmented"
                 : u.failed_rc == -EFBIG          ? "a chunk is too large to decode on demand; import it to re-chunk it"
                 : u.failed_rc == -ENOTSUP        ? "it uses a compression this emulator does not decode"
                                                  : strerror(-u.failed_rc));
        out_printf("image: '%s' is %s\n", name, t_open_error);
        int frc = u.failed_rc;
        format_unwrapped_free(&u);
        errno = frc == -ENOTSUP ? ENOTSUP : EINVAL;
        return NULL;
    }
    uint64_t raw = source_size(u.data);
    // The tags its sectors carry: a kind's fixed ones, which its files hold
    // in-band (a ProFile's 532-byte blocks), or a GCR floppy's 12.
    uint32_t t = geom.tag_bytes;
    bool units_in_file = t != 0;
    if (geom.gcr_tags) {
        image_layout_t l = image_layout_of_size(raw);
        if (l == IMAGE_LAYOUT_GCR400 || l == IMAGE_LAYOUT_GCR800)
            t = IMAGE_GCR_TAG_BYTES;
    }
    uint32_t block_size = STORAGE_BLOCK_SIZE + t;
    uint32_t file_block = units_in_file ? block_size : STORAGE_BLOCK_SIZE;
    if (raw == 0 || (raw % file_block) != 0) {
        format_unwrapped_free(&u);
        errno = EINVAL;
        return NULL;
    }

    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image) {
        format_unwrapped_free(&u);
        errno = ENOMEM;
        return NULL;
    }
    // A volume trimmed short of its own header's claim is opened at that
    // claim (image_wrap.h): the storage's base becomes the file followed by
    // zeros, with a copy of the volume's header where its alternate belongs
    // (the second-last block; Disk First Aid rejects a volume without it).
    // Guest writes to the tail land in the delta like any others.
    source_t *padded = NULL;
    if (t == 0) {
        uint64_t start = 0;
        uint64_t blocks = image_wrap_extended_blocks(source_read_cb, u.data, raw / block_size, &start);
        uint8_t hdr[STORAGE_BLOCK_SIZE];
        if (blocks * block_size > raw && source_read_exact(u.data, (start + 2) * block_size, hdr, sizeof(hdr)) == 0) {
            uint64_t alt = (blocks - 2) * block_size;
            padded = source_pad(u.data, blocks * block_size, alt, hdr, alt >= raw ? sizeof(hdr) : 0);
        }
        if (padded) {
            LOG(1, "'%s': its volume claims %llu blocks but the file holds %llu; the missing tail reads as zeros", name,
                (unsigned long long)blocks, (unsigned long long)(raw / block_size));
            raw = blocks * block_size;
        }
    }

    image->filename = gs_strdup(name);
    image->checkpoint_index = IMAGE_REF_NONE;
    image->source_key = gs_strdup(source_key(data));
    image->format = gs_strdup(u.chain[0] ? u.chain : "raw");
    image->raw_size = raw / file_block * STORAGE_BLOCK_SIZE; // the data bytes
    image->block_size = block_size;
    image->tag_bytes = t;
    image->units_in_file = units_in_file;
    // The kind is the opening device's (image_open_kind); an image opened here
    // and nowhere else is held by no device (IMAGE_KIND_NONE, from calloc).
    image->writable = mode != OPEN_READONLY;
    image->from_diskcopy = u.dc42 != NULL;

    if (mode == OPEN_REOPEN) {
        image->instance_path = gs_strdup(dir);
    } else if (mode == OPEN_CREATE) {
        char id[17];
        mint_random_hex_id(id);
        image->instance_path = str_printf("%s/%s", dir, id);
    } else {
        // A read-only mount's delta+journal are ghosts in the scratch root,
        // so they never land beside the media (a NULL path is caught below),
        // and lazy: nothing writes to a read-only image (disk_write_data
        // refuses), so a probe or a mount never creates them.  Only a
        // consolidated checkpoint's restore over a missing base does.
        ghost_paths(image);
    }
    if (image->instance_path) {
        image->delta_path = str_printf("%s.delta", image->instance_path);
        image->journal_path = str_printf("%s.journal", image->instance_path);
    }
    if (!image->filename || !image->source_key || !image->format || !image->delta_path || !image->journal_path) {
        source_release(padded);
        format_unwrapped_free(&u);
        image_close(image);
        errno = ENOMEM;
        return NULL;
    }

    // A sector base that carries tags is seen through the tagged view: the
    // DiskCopy file's tag section when it has one per sector, else zeros.
    uint64_t sectors = image->raw_size / STORAGE_BLOCK_SIZE;
    uint64_t tags_at = 0;
    uint32_t dc42_t = u.dc42 ? image_load_diskcopy_identity(image, u.dc42, &tags_at) : 0;
    image->file_tag_bytes = units_in_file ? t : dc42_t;
    source_t *view = NULL;
    if (t && !units_in_file) {
        view = tagged_view(padded ? padded : u.data, dc42_t == t ? u.dc42 : NULL, tags_at, t, sectors);
        if (!view) {
            source_release(padded);
            format_unwrapped_free(&u);
            image_close(image);
            errno = ENOMEM;
            return NULL;
        }
    }
    storage_config_t config = {0};
    config.base = view ? view : padded ? padded : u.data;
    config.delta_path = image->delta_path;
    config.journal_path = image->journal_path;
    config.block_count = sectors;
    config.block_size = image->block_size;
    config.lazy = image->ghost_instance;
    status_t rc = storage_new(&config, &image->storage);
    source_release(view);
    source_release(padded);
    format_unwrapped_free(&u);
    if (rc != STATUS_OK) {
        out_printf("image: storage engine failed for %s (error %d)\n", name, rc);
        image_close(image);
        errno = EIO;
        return NULL;
    }
    if (image->writable)
        writable_register(image, name);
    LOG(3, "opened '%s' (%s, %llu bytes%s)", name, image->format, (unsigned long long)image->raw_size,
        image->writable ? ", writable" : "");
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
    source_t *data = source_open_path(path, GS_FORK_DATA, &err);
    if (!data) {
        errno = err ? -err : ENOENT;
        return NULL;
    }
    source_t *rsrc = source_open_path(path, GS_FORK_RSRC, NULL);
    image_t *img = image_open_source(path, data, rsrc, geom, mode, dir);
    int saved = errno;
    source_release(data);
    source_release(rsrc);
    errno = saved;
    return img;
}

image_t *image_open_readonly(const char *base_path) {
    return image_open_readonly_with_geometry(base_path, (image_geometry_t){0});
}

image_t *image_open_readonly_with_geometry(const char *base_path, image_geometry_t geom) {
    return image_open_path(base_path, geom, OPEN_READONLY, NULL);
}

image_t *image_open_readonly_source(const char *name, source_t *data, source_t *rsrc) {
    if (!data) {
        errno = EINVAL;
        return NULL;
    }
    return image_open_source(name, data, rsrc, (image_geometry_t){0}, OPEN_READONLY, NULL);
}

image_t *image_create(const char *base_path, const char *delta_dir) {
    return image_create_with_geometry(base_path, delta_dir, (image_geometry_t){0});
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
    if (mkdir_p(delta_dir) != 0) {
        out_printf("image_create: cannot create delta directory: %s\n", delta_dir);
        return NULL;
    }
    return image_open_path(base_path, geom, OPEN_CREATE, delta_dir);
}

image_t *image_open(const char *base_path, const char *instance_path) {
    return image_open_with_geometry(base_path, instance_path, (image_geometry_t){0});
}

image_t *image_open_with_geometry(const char *base_path, const char *instance_path, image_geometry_t geom) {
    if (!base_path || !*base_path || !instance_path || !*instance_path)
        return NULL;
    return image_open_path(base_path, geom, OPEN_REOPEN, instance_path);
}

image_t *image_create_blank(uint64_t block_count, image_geometry_t geom, const char *delta_dir, const char *instance) {
    uint32_t block_size = STORAGE_BLOCK_SIZE + geom.tag_bytes;
    if (block_count == 0)
        return NULL;
    if (!instance || !*instance) {
        if (!delta_dir || !*delta_dir)
            delta_dir = image_scratch_dir();
        if (mkdir_p(delta_dir) != 0) {
            out_printf("image_create_blank: cannot create delta directory: %s\n", delta_dir);
            return NULL;
        }
    }

    image_t *image = (image_t *)calloc(1, sizeof(image_t));
    if (!image)
        return NULL;
    image->filename = NULL; // no backing base file: unwritten blocks read as zeros
    image->checkpoint_index = IMAGE_REF_NONE;
    image->raw_size = block_count * STORAGE_BLOCK_SIZE; // the data bytes
    image->block_size = block_size;
    image->tag_bytes = geom.tag_bytes;
    image->units_in_file = geom.tag_bytes != 0;
    image->writable = true;
    image->from_diskcopy = false;
    // A writable instance like any other (its delta lives with the machine,
    // so a checkpoint can name it and a restore reopen it), with no base
    // behind it: the existing `instance`, or a fresh one in `delta_dir`.
    if (instance && *instance) {
        image->instance_path = gs_strdup(instance);
    } else {
        char id[17];
        mint_random_hex_id(id);
        image->instance_path = str_printf("%s/%s", delta_dir, id);
    }
    image->delta_path = image->instance_path ? str_printf("%s.delta", image->instance_path) : NULL;
    image->journal_path = image->instance_path ? str_printf("%s.journal", image->instance_path) : NULL;
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
    status_t err = storage_new(&config, &image->storage);
    if (err != STATUS_OK) {
        out_printf("image_create_blank: storage engine failed (%llu x %u, error %d)\n", (unsigned long long)block_count,
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
static uint64_t wrap_bytes(const image_t *disk) {
    return (uint64_t)disk->wrap_blocks * disk->block_size;
}

static size_t storage_read_range(image_t *disk, uint64_t offset, uint8_t *buf, size_t size);
static size_t storage_write_range(image_t *disk, uint64_t offset, uint8_t *buf, size_t size);

size_t disk_read_data(image_t *disk, uint64_t offset, uint8_t *buf, size_t size) {
    if (!disk || !disk->storage || !buf || size == 0)
        return 0;
    disk->reads++; // the activity light's only cost on this path
    if (disk->tag_bytes) {
        GS_ASSERT((offset % STORAGE_BLOCK_SIZE) == 0 && (size % STORAGE_BLOCK_SIZE) == 0);
        return tagged_read_data(disk, offset, buf, size);
    }
    GS_ASSERT((offset % disk->block_size) == 0);
    GS_ASSERT((size % disk->block_size) == 0);
    uint64_t pre = wrap_bytes(disk);
    if (offset < pre) {
        // The wrapper's prefix is served from memory; the rest of the
        // request continues on the volume's storage.
        size_t n = pre - offset < size ? (size_t)(pre - offset) : size;
        memcpy(buf, disk->wrap_prefix + offset, n);
        if (n == size)
            return size;
        return n + storage_read_range(disk, 0, buf + n, size - n);
    }
    return storage_read_range(disk, offset - pre, buf, size);
}

// Read whole blocks at `offset` of the image's volume (past any wrapper
// prefix; wrap_base bytes into the storage).  Returns `size`, or 0 on a backing failure.
static size_t storage_read_range(image_t *disk, uint64_t offset, uint8_t *buf, size_t size) {
    uint64_t vol_size = disk->raw_size - wrap_bytes(disk);
    // An undersized / truncated image (a host file shorter than the media it
    // backs) can be read past its end — e.g. the Finder reading high tracks
    // while ejecting a 400K/800K-geometry floppy backed by a too-small file.
    // Serve the unbacked tail as blank media (zeroes) rather than asserting,
    // so the guest sees readable-but-empty sectors and the operation can
    // finish.  `backed` is the in-bounds, whole-block byte count.
    uint64_t avail = (offset < vol_size) ? (vol_size - offset) : 0;
    size_t backed = avail < size ? (size_t)avail : size;
    backed -= backed % disk->block_size;
    if (backed < size) {
        memset(buf + backed, 0, size - backed);
        LOG(1,
            "disk_read_data: %zu-byte read at offset %llu runs past image end (raw_size=%llu); zero-filled %zu-byte "
            "tail",
            size, (unsigned long long)offset, (unsigned long long)disk->raw_size, size - backed);
    }
    if (backed) {
        status_t rc = storage_read_blocks(disk->storage, disk->wrap_base + offset, buf, backed / disk->block_size);
        // E_INVAL / E_RANGE would be this function's own arithmetic gone
        // wrong (the range is clamped to the volume above).  E_IO is the
        // medium: a base the image cannot read where it should hold data.
        // That is the guest's error to see, as an unreadable sector on a
        // real disk is -- never a halt, never silent zeros: the short count
        // makes each device model report its own read error.
        GS_ASSERTF(rc == STATUS_OK || rc == STATUS_E_IO, "storage_read_blocks failed (%d)", rc);
        if (rc != STATUS_OK) {
            note_read_error(disk, offset, backed);
            return 0;
        }
    }
    // Buffer fully populated: real data plus any zero-filled tail past EOF.
    return size;
}

size_t disk_write_data(image_t *disk, uint64_t offset, uint8_t *buf, size_t size) {
    if (!disk || !disk->storage || !buf || size == 0)
        return 0;
    // A read-only image is write-protected: the device tells the guest so,
    // and a write that reaches here anyway is refused, not kept somewhere.
    if (!disk->writable) {
        LOG(1, "disk_write_data: %zu-byte write at %llu to the write-protected %s refused", size,
            (unsigned long long)offset, disk->filename ? disk->filename : "image");
        return 0;
    }
    disk->writes++;
    if (disk->tag_bytes) {
        GS_ASSERT((offset % STORAGE_BLOCK_SIZE) == 0 && (size % STORAGE_BLOCK_SIZE) == 0);
        return tagged_write_data(disk, offset, buf, size);
    }
    GS_ASSERT((offset % disk->block_size) == 0);
    GS_ASSERT((size % disk->block_size) == 0);
    uint64_t pre = wrap_bytes(disk);
    if (offset < pre) {
        // A write into the wrapper's prefix (a partitioning tool rewriting
        // the map) lands in the in-memory prefix only: the volume file is
        // never given a partition map, and the prefix is rebuilt on reopen.
        size_t n = pre - offset < size ? (size_t)(pre - offset) : size;
        memcpy(disk->wrap_prefix + offset, buf, n);
        LOG(1, "disk_write_data: %zu-byte write into the wrapper prefix at %llu kept in memory only", n,
            (unsigned long long)offset);
        if (n == size)
            return size;
        return n + storage_write_range(disk, 0, buf + n, size - n);
    }
    return storage_write_range(disk, offset - pre, buf, size);
}

// Write whole blocks at `offset` of the image's volume (past any wrapper
// prefix; wrap_base bytes into the storage).  Returns `size`, or the bytes written before a backing failure.
static size_t storage_write_range(image_t *disk, uint64_t offset, uint8_t *buf, size_t size) {
    uint64_t vol_size = disk->raw_size - wrap_bytes(disk);
    // Symmetric with disk_read_data: a write past the end of an undersized
    // image targets sectors with no backing store.  Drop the unbacked tail
    // (it cannot be stored) rather than asserting, so the guest's volume
    // flush during eject completes; warn so the dropped write is visible.
    uint64_t avail = (offset < vol_size) ? (vol_size - offset) : 0;
    size_t backed = avail < size ? (size_t)avail : size;
    backed -= backed % disk->block_size;
    if (backed < size)
        LOG(1,
            "disk_write_data: %zu-byte write at offset %llu runs past image end (raw_size=%llu); dropped %zu-byte tail",
            size, (unsigned long long)offset, (unsigned long long)disk->raw_size, size - backed);
    size_t transferred = 0;
    while (transferred < backed) {
        status_t rc = storage_write_block(disk->storage, disk->wrap_base + offset + transferred, buf + transferred);
        GS_ASSERTF(rc == STATUS_OK, "storage_write_block failed (%d)", rc);
        if (rc != STATUS_OK)
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

// The format an export writes, picked from the destination's name.
typedef enum { EXPORT_RAW, EXPORT_UDIF, EXPORT_DC42, EXPORT_MOOF } export_format_t;

struct image_export {
    storage_export_view_t *view;
    char *dest;
    char *name; // the image's own name, recorded in a UDIF export
    uint32_t block_size;
    export_format_t format;
    udif_codec_t udif_codec; // what a UDIF export compresses with
    // A sector image's tags are in-band: each block streams as its tag then
    // its data, and the export splits them -- the data on to the format, the
    // tags (the view's moment, like the data) collected here for a DiskCopy
    // 4.2 or MOOF file.  A ProFile's raw file keeps whole blocks: no split.
    bool split; // strip the tags off each block
    uint8_t *tags; // tag_count * tag_bytes collected, or NULL for an untagged disk
    uint32_t tag_bytes;
    uint32_t tag_count;
    uint32_t tags_seen; // blocks split so far
    uint8_t dc42_name[DC42_NAME_FIELD]; // the header's name field
    uint8_t disk_format;
    uint8_t format_byte;
    bool unreadable; // the view could not read the disk (export_view_write)
};

// A split stream: one block per call (the view's record shape), its tag kept
// and its data passed on.
typedef struct {
    image_export_t *e;
    void *ctx;
    storage_write_callback_t write_cb;
} split_stream_t;

static int split_write_cb(void *ctx, const void *data, size_t size) {
    split_stream_t *sp = (split_stream_t *)ctx;
    image_export_t *e = sp->e;
    if (size != (size_t)e->block_size)
        return -1; // not a block: the view streams one per call
    const uint8_t *block = (const uint8_t *)data;
    if (e->tags && e->tags_seen < e->tag_count)
        memcpy(e->tags + (size_t)e->tags_seen * e->tag_bytes, block, e->tag_bytes);
    e->tags_seen++;
    return sp->write_cb(sp->ctx, block + e->tag_bytes, size - e->tag_bytes);
}

// Stream the view through `write_cb` -- the data alone when the export
// splits -- noting whether a failure was the disk's (STATUS_E_IO: the view's
// reads) rather than the writer's (STATUS_ERROR) or a cancel.  The status
// passes through.
static int export_view_write(image_export_t *e, void *ctx, storage_write_callback_t write_cb) {
    split_stream_t sp = {e, ctx, write_cb};
    e->tags_seen = 0;
    int rc = e->split ? storage_export_view_write(e->view, &sp, split_write_cb)
                      : storage_export_view_write(e->view, ctx, write_cb);
    e->unreadable = rc == STATUS_E_IO;
    return rc;
}

// DiskCopy 4.2's disk-format code for a floppy layout, or -1 for anything else.
static int dc42_disk_format(image_layout_t t) {
    switch (t) {
    case IMAGE_LAYOUT_GCR400:
        return 0; // 400K GCR
    case IMAGE_LAYOUT_GCR800:
        return 1; // 800K GCR
    case IMAGE_LAYOUT_MFM720:
        return 2; // 720K MFM
    case IMAGE_LAYOUT_MFM1440:
        return 3; // 1440K MFM
    default:
        return -1;
    }
}

// Fill in the DiskCopy half of an export: room for the tags the stream
// splits off and the header's label.  False on a medium DiskCopy 4.2 cannot
// hold, or out of memory.
static bool export_prepare_dc42(image_export_t *e, const image_t *image, char *err, size_t err_cap) {
    int fmt = dc42_disk_format(image_layout_of_size(image->raw_size));
    if (fmt < 0 || image->units_in_file || image->wrap_prefix) {
        if (err)
            snprintf(err, err_cap, "DiskCopy 4.2 holds 400K, 800K, 720K and 1440K floppies only");
        return false;
    }
    e->disk_format = (uint8_t)fmt;
    // The source's (or a machine's) format byte, else the Mac's: $12 for a
    // 400K disk, $22 for the rest.
    e->format_byte = image->dc42_format_byte ? image->dc42_format_byte : (fmt == 0 ? 0x12 : 0x22);
    if (image->dc42_name[0]) {
        memcpy(e->dc42_name, image->dc42_name, DC42_NAME_FIELD);
    } else {
        // No label of its own: the source file's name, without directory or
        // extension.
        const char *full = image->filename ? image->filename : "";
        const char *base = strrchr(full, '/') ? strrchr(full, '/') + 1 : full;
        const char *dot = strrchr(base, '.');
        char *stem = str_printf("%.*s", (int)(dot && dot != base ? dot - base : (ptrdiff_t)strlen(base)), base);
        if (!stem) {
            if (err)
                snprintf(err, err_cap, "out of memory");
            return false;
        }
        dc42_name_field(e->dc42_name, stem);
        free(stem);
    }
    if (image->tag_bytes) {
        e->tag_count = (uint32_t)(image->raw_size / STORAGE_BLOCK_SIZE);
        e->tags = (uint8_t *)calloc(e->tag_count, image->tag_bytes);
        if (!e->tags) {
            if (err)
                snprintf(err, err_cap, "out of memory");
            return false;
        }
    }
    return true;
}

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
    e->tag_bytes = image->tag_bytes;
    // A sector image streams its data alone; a ProFile's raw file is whole
    // blocks, tags in-band, as the file it came from.
    e->split = image->tag_bytes && !image->units_in_file;
    if (image->filename) {
        const char *slash = strrchr(image->filename, '/');
        e->name = strdup(slash ? slash + 1 : image->filename);
    }
    // The destination's name picks the format (image.h, image_export_to).
    if (!image->units_in_file && path_has_ext(dest_path, ".dmg"))
        e->format = EXPORT_UDIF;
    else if (path_has_ext(dest_path, ".dc42") || path_has_ext(dest_path, ".diskcopy") ||
             (path_has_ext(dest_path, ".image") && image_layout_of_size(image->raw_size) != IMAGE_LAYOUT_NONE))
        e->format = EXPORT_DC42;
    else if (path_has_ext(dest_path, ".moof")) {
        e->format = EXPORT_MOOF;
        image_layout_t layout = image_layout_of_size(image->raw_size);
        if (layout != IMAGE_LAYOUT_GCR400 && layout != IMAGE_LAYOUT_GCR800) {
            if (err)
                snprintf(err, err_cap, "MOOF holds 400K and 800K GCR floppies only");
            image_export_end(e);
            return NULL;
        }
    }
    if ((e->format == EXPORT_DC42 || e->format == EXPORT_MOOF) && !export_prepare_dc42(e, image, err, err_cap)) {
        image_export_end(e);
        return NULL;
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
// content's size.
static int udif_write_cb(void *ctx, const void *data, size_t size) {
    return udif_writer_append((udif_writer_t *)ctx, data, size) == 0 ? 0 : -1;
}

static int image_export_run_udif(image_export_t *e, char *err, size_t err_cap) {
    udif_writer_opts_t o = {.codec = e->udif_codec, .level = -1, .source_name = e->name};
    udif_writer_t *w = udif_writer_open(e->dest, &o, err, err_cap);
    if (!w)
        return -EIO;
    int rc = export_view_write(e, w, udif_write_cb);
    if (rc != STATUS_OK) {
        udif_writer_abort(w); // removes the partial file
        return export_fail(rc, e->unreadable, e->dest, err, err_cap);
    }
    rc = udif_writer_finish(w, NULL);
    if (rc != 0 && err)
        snprintf(err, err_cap, "write to '%s' failed: %s", e->dest, strerror(-rc));
    return rc;
}

// The data section streams through here on its way to the file, so the
// DiskCopy data checksum is taken in the same pass.
typedef struct {
    FILE *f;
    uint32_t sum; // running data checksum
    uint64_t bytes; // data bytes written
} dc42_stream_t;

static int dc42_write_cb(void *ctx, const void *data, size_t size) {
    dc42_stream_t *d = (dc42_stream_t *)ctx;
    // Blocks are 512 bytes, so every piece is whole words.
    d->sum = dc42_checksum(d->sum, (const uint8_t *)data, size);
    d->bytes += size;
    return fwrite(data, 1, size, d->f) == size ? 0 : -1;
}

// A DiskCopy 4.2 file: a header, the data section, then one tag per sector.
// The header goes last, once the data checksum is known; a space is held for
// it first.  The view's status passes through as it is, so an unreadable
// base block (STATUS_E_IO) fails the export as the raw one does.
static int image_export_run_dc42(image_export_t *e, FILE *f) {
    uint8_t hdr[DISKCOPY_HEADER_SIZE] = {0};
    if (fwrite(hdr, sizeof hdr, 1, f) != 1)
        return -EIO;
    dc42_stream_t d = {.f = f};
    int rc = export_view_write(e, &d, dc42_write_cb);
    if (rc != STATUS_OK)
        return rc; // STATUS_E_IO (the disk could not be read), -ECANCELED, or a write failure
    if (d.bytes > UINT32_MAX)
        return -EFBIG;
    uint32_t sectors = (uint32_t)(d.bytes / STORAGE_BLOCK_SIZE);
    uint32_t tag_bytes = e->tags ? e->tag_bytes : 0;
    // One tag per sector; a sector the tag area does not reach gets zeros.
    uint32_t tag_sum = 0;
    uint8_t zero[64] = {0};
    for (uint32_t i = 0; tag_bytes && i < sectors; i++) {
        const uint8_t *t = (i < e->tag_count && tag_bytes <= sizeof zero) ? e->tags + (size_t)i * tag_bytes : zero;
        if (i > 0) // the first sector's tag is not summed (as DiskCopy does)
            tag_sum = dc42_checksum(tag_sum, t, tag_bytes);
        if (fwrite(t, 1, tag_bytes, f) != tag_bytes)
            return -EIO;
    }
    dc42_build_header(hdr, e->dc42_name, (uint32_t)d.bytes, sectors * tag_bytes, d.sum, tag_sum, e->disk_format,
                      e->format_byte);
    if (fseek(f, 0, SEEK_SET) != 0 || fwrite(hdr, sizeof hdr, 1, f) != 1)
        return -EIO;
    return 0;
}

// The data section collects here, for a MOOF export to encode whole.
typedef struct {
    uint8_t *buf;
    size_t len, cap;
} mem_stream_t;

static int mem_write_cb(void *ctx, const void *data, size_t size) {
    mem_stream_t *m = (mem_stream_t *)ctx;
    if (m->len + size > m->cap)
        return -1; // more than a GCR floppy holds
    memcpy(m->buf + m->len, data, size);
    m->len += size;
    return 0;
}

// A MOOF file (image_moof.h): the disk's sectors and tags laid down as GCR
// bitstreams.  The view's status passes through as in the DiskCopy export.
static int image_export_run_moof(image_export_t *e, FILE *f) {
    mem_stream_t m = {.cap = 1600u * STORAGE_BLOCK_SIZE};
    m.buf = (uint8_t *)malloc(m.cap);
    if (!m.buf)
        return -ENOMEM;
    int rc = export_view_write(e, &m, mem_write_cb);
    uint32_t sectors = (uint32_t)(m.len / STORAGE_BLOCK_SIZE);
    // The tags, when the image has one per sector (else zeros).
    const uint8_t *tags = e->tags && e->tag_bytes == MOOF_TAG_BYTES && e->tag_count >= sectors ? e->tags : NULL;
    uint8_t *file = NULL;
    size_t len = 0;
    if (rc == STATUS_OK)
        rc = moof_encode(m.buf, sectors, tags, e->format_byte, &file, &len);
    free(m.buf);
    if (rc == 0 && fwrite(file, 1, len, f) != len)
        rc = -EIO;
    free(file);
    return rc;
}

int image_export_run(image_export_t *e, char *err, size_t err_cap) {
    if (!e || !e->view)
        return -EINVAL;
    mkdir_parents(e->dest);
    if (e->format == EXPORT_UDIF)
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
    int rc = e->format == EXPORT_DC42   ? image_export_run_dc42(e, f)
             : e->format == EXPORT_MOOF ? image_export_run_moof(e, f)
                                        : export_view_write(e, f, file_write_cb);
    bool closed = fclose(f) == 0;
    free(iobuf);
    if (rc != STATUS_OK || !closed) {
        remove(e->dest);
        return export_fail(rc, e->unreadable, e->dest, err, err_cap);
    }
    return 0;
}

void image_export_end(image_export_t *e) {
    if (!e)
        return;
    storage_export_view_end(e->view);
    free(e->dest);
    free(e->name);
    free(e->tags);
    free(e);
}

int image_export_set_codec(image_export_t *e, const char *codec, char *err, size_t err_cap) {
    udif_codec_t c;
    if (udif_codec_parse(codec, &c) != 0) {
        if (err)
            snprintf(err, err_cap, "codec '%s' is neither zlib nor xz", codec);
        return -1;
    }
    if (c != UDIF_CODEC_ZLIB && e->format != EXPORT_UDIF) {
        if (err)
            snprintf(err, err_cap, "codec %s applies to a .dmg (UDIF) export only", codec);
        return -1;
    }
    e->udif_codec = c;
    return 0;
}

// Export the full disk content (base + delta) to a new file at dest_path,
// here and now.
int image_export_to_codec(image_t *image, const char *dest_path, const char *codec) {
    image_export_t *e = image_export_begin(image, dest_path, NULL, 0);
    if (!e)
        return -1;
    int rc = image_export_set_codec(e, codec, NULL, 0) == 0 ? image_export_run(e, NULL, 0) : -1;
    image_export_end(e);
    return rc == 0 ? 0 : -1;
}

int image_export_to(image_t *image, const char *dest_path) {
    return image_export_to_codec(image, dest_path, NULL);
}

// ============================================================================
// Image creation
// ============================================================================

int image_create_empty(const char *filename, uint64_t size) {
    if (!filename || !*filename || size == 0)
        return -1;
    mkdir_parents(filename);
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
    mkdir_parents(filename);
    return udif_create_empty(filename, size) == 0 ? 0 : -1;
}

int image_create_zeros(const char *filename, uint64_t bytes) {
    if (!filename || !*filename || bytes == 0)
        return -1;
    mkdir_parents(filename);
    FILE *f = create_exclusive(filename);
    if (!f)
        return errno == EEXIST ? IMAGE_CREATE_EXISTS : -1;
    // ftruncate leaves the new bytes reading as zero, without writing them.
    int fd = fileno(f);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) != 0) {
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

// ============================================================================
// Checkpointing
// ============================================================================

void image_checkpoint(const image_t *image, checkpoint_t *checkpoint) {
    if (!image || !checkpoint)
        return;
    checkpoint_write_string(checkpoint, image->filename);

    // Bit 0: writable.  Bit 1: wrapped by the bare-volume wrapper — the
    // restore re-wraps rather than trusting the file to say so (the prefix
    // is never in the file).  raw_size is the storage's own size, which is
    // what the restore's geometry check and base materialisation expect.
    uint8_t flags =
        (uint8_t)((image->writable ? IMAGE_CKPT_WRITABLE : 0) | (image->wrap_prefix ? IMAGE_CKPT_WRAPPED : 0));
    system_write_checkpoint_data(checkpoint, &flags, sizeof(flags));
    // The kind the drive presents it as: the restore reopens it as the same
    // (a ProFile at its 532-byte block, a CD read-only).
    uint8_t kind = (uint8_t)image->kind;
    system_write_checkpoint_data(checkpoint, &kind, sizeof(kind));

    uint64_t raw_size = (uint64_t)(image->wrap_prefix ? image->wrap_storage_size : image->raw_size);
    system_write_checkpoint_data(checkpoint, &raw_size, sizeof(raw_size));

    // Persist the instance path so a future restore can reopen the same delta
    // directory without relying on adjacent-to-base sidecars.  None
    // (length 0) for read-only / ghost mounts.  Written here rather than
    // through checkpoint_write_string: the path names this run's own storage
    // cache, and the checkpoint decoders (scripts/lib/gscheckpoint.py,
    // is_storage_block) skip it as host bookkeeping by its writer's file.
    const char *instance = (image->writable && image->instance_path) ? image->instance_path : "";
    uint32_t instance_len = *instance ? (uint32_t)(strlen(instance) + 1) : 0;
    system_write_checkpoint_data(checkpoint, &instance_len, sizeof(instance_len));
    if (instance_len)
        system_write_checkpoint_data(checkpoint, instance, instance_len);

    // The key of the source the path opened (source.h): a restore that
    // reuses the base checks it is still the same bytes -- the same file,
    // size and time stamp, or the same member of the same archive.
    checkpoint_write_string(checkpoint, image->source_key);

    if (image->storage) {
        status_t rc = storage_checkpoint(image->storage, checkpoint);
        if (rc != STATUS_OK) {
            // A checkpoint missing this disk's blocks (or carrying a short
            // stream of them) must not replace a good one: failing the stream
            // is what keeps the consolidated .tmp from being renamed over it.
            LOG(0, "image_checkpoint: storage_checkpoint failed for %s (%d)",
                image->filename ? image->filename : "<unknown>", rc);
            checkpoint_set_error(checkpoint);
        }
    }
}
