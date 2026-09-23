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
#include "image_scratch.h"
#include "image_udif.h"
#include "log.h"
#include "platform.h"
#include "resource_fork.h"
#include "storage_util.h"
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

static int read_file_size(const char *path, size_t *out_size) {
    if (!path || !out_size)
        return -1;
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    *out_size = (size_t)st.st_size;
    return 0;
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
    uint32_t data_size = RD_BE32(header + 0x40);
    uint32_t tag_size = RD_BE32(header + 0x44);
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

uint32_t disk_block_size(image_t *disk) {
    return disk ? disk->block_size : 0;
}

// ============================================================================
// Open writable images
// ============================================================================

// Every image opened writable and not yet closed, keyed on the canonical
// path its caller named.  image_vfs asks image_path_is_open_writable() at the
// point of use rather than being told on attach and detach, so there is no
// notification for an attach or detach site to forget (09-storage F-39..F-41).
static image_t *g_open_writable;

static void image_canonicalise(const char *path, char *out, size_t cap);

// Record a writable image under `base_path`.  An image whose path cannot be
// recorded still works; the VFS just cannot see that it is open.
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

bool image_path_is_open_writable(const char *canonical_path) {
    if (!canonical_path)
        return false;
    for (const image_t *im = g_open_writable; im; im = im->next_writable)
        if (strcmp(im->source_canon, canonical_path) == 0)
            return true;
    return false;
}

void image_close(image_t *image) {
    if (!image)
        return;
    writable_unregister(image);
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
        return gs_strdup(".");
    const char *last = strrchr(path, '/');
    if (!last)
        return gs_strdup(".");
    if (last == path)
        return gs_strdup("/");
    size_t len = (size_t)(last - path);
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
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
    struct stat st;
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || fstat(fileno(f), &st) != 0) {
        fclose(f);
        return;
    }
    // Every value here is re-derived from this header and checked against
    // this file, rather than trusted because detect_diskcopy checked it on
    // an earlier open (09-storage F-50).  The sector count is the header's
    // own: DiskCopy 4.2 sectors are 512 data bytes whatever geometry the
    // image is opened with.  Bounding the tag section by the file bounds
    // the allocation by the file.
    uint32_t data_size = RD_BE32(header + 0x40);
    uint32_t tag_size = RD_BE32(header + 0x44);
    uint32_t count = data_size / STORAGE_BLOCK_SIZE;
    uint64_t end = (uint64_t)DISKCOPY_HEADER_SIZE + data_size + tag_size;
    if (tag_size == 0 || count == 0 || (tag_size % count) != 0 || end > (uint64_t)st.st_size) {
        fclose(f);
        return; // no tags (or unexpected layout)
    }
    uint8_t *tags = (uint8_t *)malloc(tag_size);
    if (!tags) {
        fclose(f);
        return;
    }
    if (fseeko(f, (off_t)DISKCOPY_HEADER_SIZE + (off_t)data_size, SEEK_SET) != 0 ||
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

// Scratch sidecars -- read-only deltas, blank-image deltas, decoded
// images -- live under image_scratch_dir() (image_scratch.h), which
// honours GS_STORAGE_CACHE; so does the default writable delta placement
// below.

// === AppleDouble fork acquisition + host-file NDIF materialisation =========
// A host `.img` file has only a data fork, but a Disk Copy 6 / NDIF image keeps
// its block map ('bcem') in the resource fork.  When such a file was copied out
// of an HFS volume as an AppleDouble pair, the resource fork lives in a sibling
// "._<name>" (or legacy "%<name>", or a raw "<name>.rsrc").  This reunites the
// forks and, when the data fork is NDIF-encoded, decodes it to a scratch raw
// image so the rest of image.c opens it as an ordinary base.  See
// proposal-appledouble-support.md §4.4.

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
        if (gs_read_file(path, RFORK_MAX_FORK_LEN, &raw, &raw_len) != 0)
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
    if (gs_read_file(path, RFORK_MAX_FORK_LEN, &raw, &raw_len) == 0 && raw_len > 0) {
        *out_len = raw_len;
        return raw;
    }
    free(raw);
    return NULL;
}

// Read exactly `len` bytes at absolute offset `off`.  0 / -errno.
static int read_at(FILE *f, uint64_t off, void *buf, size_t len) {
    if (fseeko(f, (off_t)off, SEEK_SET) != 0)
        return -EIO;
    return fread(buf, 1, len, f) == len ? 0 : -EIO;
}

// ndif_read_fn over a host file.
static int ndif_read_host(void *ctx, uint64_t off, void *buf, size_t n) {
    return read_at((FILE *)ctx, off, buf, n);
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
    int rc = ndif_materialize(map, ndif_read_host, df, out);
    fclose(df);
    if (fclose(out) != 0 && rc == 0)
        rc = -EIO;
    return rc;
}

// Resolve `path` through realpath() so every spelling of the same file — the
// relative one a script passes to storage.probe, the absolute one the VFS
// resolves, a symlink — reduces to one string.  Falls back to the input when
// realpath cannot resolve it (e.g. the file was just deleted), preserving the
// old keying.  image_vfs.c canonicalises its mount table the same way.
static void image_canonicalise(const char *path, char *out, size_t cap) {
    // Let realpath() allocate, so a host path limit above the build-time
    // PATH_MAX does not get silently truncated.
    char *resolved = realpath(path, NULL);
    snprintf(out, cap, "%s", resolved ? resolved : path);
    free(resolved);
}

// The identity a decoded image of `base_path` is cached under (see
// image_scratch.h): the decoder tag, the canonical path, and the file's size
// and mtime, so a changed source re-decodes.  The path is canonicalised
// first: keying on the caller's spelling decoded the same disc once per
// spelling, which for a CD-sized .dmg costs a second full decode and
// another copy of the whole image on disk.  Also fills the scratch path.
static bool decoded_identity(const char *base_path, const char *tag, char *identity, size_t id_cap, char *scratch,
                             size_t scratch_cap) {
    struct stat sb;
    char canon[PATH_MAX];
    image_canonicalise(base_path, canon, sizeof(canon));
    long long size = -1, mtime = -1;
    if (stat(base_path, &sb) == 0) {
        size = (long long)sb.st_size;
        mtime = (long long)sb.st_mtime;
    }
    int n = snprintf(identity, id_cap, "%s\x1f%s\x1f%lld:%lld", tag, canon, size, mtime);
    if (n < 0 || (size_t)n >= id_cap)
        return false;
    return image_scratch_path(tag, identity, scratch, scratch_cap);
}

// === UDIF (.dmg) materialisation ==========================================
// A UDIF image needs no resource fork: the 512-byte 'koly' trailer at EOF
// points at both the compressed payload and the XML block map, so everything
// is reachable from the one host path.  Decode it to a scratch raw image the
// same way NDIF is handled above.  See image_udif.h.

// Largest chunk we will buffer whole for decompression.  Real writers emit
// ~1 MB chunks; anything wildly larger means a corrupt map, not a big disk.
#define UDIF_MAX_CHUNK_BYTES (64u * 1024u * 1024u)

// Fold `len` zero bytes into a running CRC-32 without allocating them.
static uint32_t crc32_zeros(uint32_t crc, uint64_t len) {
    static const uint8_t zeros[4096] = {0};
    while (len) {
        size_t n = len < sizeof(zeros) ? (size_t)len : sizeof(zeros);
        crc = udif_crc32(crc, zeros, n);
        len -= n;
    }
    return crc;
}

// Copy a RAW chunk straight through in slices, folding it into `crc`.  Raw
// chunks can cover a whole disk, so this never buffers the run whole.
static int copy_raw_chunk(FILE *df, FILE *out, const udif_chunk_t *c, uint64_t base_sector, uint32_t *crc) {
    uint8_t buf[64 * 1024];
    uint64_t remaining = c->count * 512;
    if (c->length < remaining)
        return -EINVAL; // the map promises more sectors than the fork holds
    uint64_t src = c->offset;
    if (fseeko(out, (off_t)(base_sector + c->sector) * 512, SEEK_SET) != 0)
        return -EIO;
    while (remaining) {
        size_t n = remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
        if (read_at(df, src, buf, n) != 0 || fwrite(buf, 1, n, out) != n)
            return -EIO;
        *crc = udif_crc32(*crc, buf, n);
        src += n;
        remaining -= n;
    }
    return 0;
}

// Decode one compressed (or zero-fill) chunk to its place in `out`, folding
// the decoded bytes into `crc`.  0 / -errno.
static int write_udif_chunk(FILE *df, FILE *out, const udif_chunk_t *c, uint64_t base_sector, uint32_t *crc) {
    // Ignored chunks are unallocated space: they read as zeros and, unlike
    // zero-fill, are excluded from the table checksum entirely.
    if (c->type == UDIF_CHUNK_IGNORE)
        return 0;
    uint64_t need = c->count * 512;
    if (need == 0)
        return 0;
    // Zero-fill needs no bytes written — the file was pre-extended with zeros.
    if (c->type == UDIF_CHUNK_ZERO) {
        *crc = crc32_zeros(*crc, need);
        return 0;
    }
    if (c->type == UDIF_CHUNK_RAW)
        return copy_raw_chunk(df, out, c, base_sector, crc);

    if (need > UDIF_MAX_CHUNK_BYTES || c->length > UDIF_MAX_CHUNK_BYTES)
        return -EFBIG;
    uint8_t *cbuf = (uint8_t *)malloc((size_t)c->length ? (size_t)c->length : 1);
    uint8_t *dbuf = (uint8_t *)malloc((size_t)need);
    int rc = 0;
    if (!cbuf || !dbuf) {
        rc = -ENOMEM;
    } else if (read_at(df, c->offset, cbuf, (size_t)c->length) != 0) {
        rc = -EIO;
    } else if ((rc = udif_decode_chunk(c, cbuf, (size_t)c->length, dbuf, (size_t)need)) == 0) {
        if (fseeko(out, (off_t)(base_sector + c->sector) * 512, SEEK_SET) != 0 ||
            fwrite(dbuf, 1, (size_t)need, out) != need)
            rc = -EIO;
        else
            *crc = udif_crc32(*crc, dbuf, (size_t)need);
    }
    free(cbuf);
    free(dbuf);
    return rc;
}

// Decode a whole UDIF image (host file `base_path`, trailer `tr`, block map
// `map`) into a freshly created scratch raw file `scratch`.  Each block
// table's stored CRC-32 is verified as it is written, so a bad decode fails
// here rather than surfacing as a subtly corrupt disk. 0 / -errno.
static int materialize_udif_host(const char *base_path, const udif_trailer_t *tr, udif_map_t *map,
                                 const char *scratch) {
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
    if (ftruncate(fileno(out), (off_t)tr->sectors * 512) != 0) {
        rc = -EIO;
        goto done;
    }
    for (size_t i = 0; i < map->n_tables && rc == 0; i++) {
        udif_table_t *t = &map->tables[i];
        uint32_t crc = 0;
        for (size_t j = 0; j < t->n_chunks; j++) {
            udif_chunk_t *c = &t->chunks[j];
            // Absolute position is the table's base plus the chunk's own
            // sector, which restarts at 0 in every table.
            // Checked without an addition that could wrap (09-storage F-25).
            if (c->count > tr->sectors || t->base_sector > tr->sectors - c->count ||
                c->sector > tr->sectors - c->count - t->base_sector) {
                rc = -EINVAL;
                break;
            }
            rc = write_udif_chunk(df, out, c, t->base_sector, &crc);
            if (rc != 0) {
                LOG(1, "UDIF chunk %zu of '%s' (type %#x) failed: %d", j, t->name, c->type, rc);
                break;
            }
        }
        // A stored CRC of zero means the writer recorded none (every table of
        // purely unallocated space does), so only a real value is checked.
        if (rc == 0 && t->checksum_type == UDIF_CHECKSUM_CRC32 && t->checksum != 0 && crc != t->checksum) {
            LOG(1, "UDIF checksum mismatch in '%s': stored %08x, decoded %08x", t->name, t->checksum, crc);
            rc = -EINVAL;
        }
    }
done:
    fclose(df);
    if (fclose(out) != 0 && rc == 0)
        rc = -EIO;
    return rc;
}

// If base_path is a UDIF image, decode it to a cached scratch raw file and
// return that path (malloc'd, caller frees).  Returns NULL when the file is
// not UDIF at all, or when its decode failed — both fall through to the
// remaining formats.
static char *udif_decode(const char *base_path) {
    FILE *f = fopen(base_path, "rb");
    if (!f)
        return NULL;
    uint8_t trailer[UDIF_TRAILER_SIZE];
    int seek_rc = fseeko(f, -(off_t)sizeof(trailer), SEEK_END);
    bool have_trailer = seek_rc == 0 && fread(trailer, 1, sizeof(trailer), f) == sizeof(trailer);
    fclose(f);
    if (!have_trailer || !udif_detect(trailer, sizeof(trailer)))
        return NULL;

    udif_trailer_t tr;
    int rc = udif_parse_trailer(trailer, sizeof(trailer), &tr);
    if (rc != 0) {
        LOG(1, "unsupported UDIF trailer in '%s' (%d)", base_path, rc);
        return NULL;
    }

    char scratch[PATH_MAX], identity[PATH_MAX + 128];
    if (!decoded_identity(base_path, "udif", identity, sizeof(identity), scratch, sizeof(scratch)))
        return NULL;
    if (image_scratch_valid(scratch, identity, tr.sectors * 512))
        return gs_strdup(scratch); // already materialised

    // The block map lives in the XML plist the trailer points at.
    uint8_t *xml = (uint8_t *)malloc((size_t)tr.xml_length);
    if (!xml)
        return NULL;
    f = fopen(base_path, "rb");
    if (!f || read_at(f, tr.xml_offset, xml, (size_t)tr.xml_length) != 0) {
        if (f)
            fclose(f);
        free(xml);
        LOG(1, "UDIF '%s': cannot read block map", base_path);
        return NULL;
    }
    fclose(f);

    udif_map_t *map = NULL;
    rc = udif_parse_blkx(xml, (size_t)tr.xml_length, &map);
    free(xml);
    if (rc != 0) {
        LOG(1, "UDIF '%s': block map parse failed (%d)", base_path, rc);
        return NULL;
    }

    char *result = NULL;
    if (image_scratch_prepare(scratch) == 0 && materialize_udif_host(base_path, &tr, map, scratch) == 0 &&
        image_scratch_seal(scratch, identity) == 0) {
        LOG(3, "decoded UDIF '%s' -> '%s' (%llu sectors, %zu partitions)", base_path, scratch,
            (unsigned long long)tr.sectors, map->n_tables);
        result = gs_strdup(scratch);
    } else {
        remove(scratch);
        LOG(1, "UDIF decode failed for '%s'", base_path);
    }
    udif_map_free(map);
    return result;
}

// An NDIF (Disk Copy 6) image keeps its block map in the resource fork;
// decode it to a raw scratch file.  NULL if `base_path` is not NDIF or does
// not decode.
static char *ndif_decode(const char *base_path) {
    size_t rlen = 0;
    uint8_t *rfork = acquire_resource_fork(base_path, &rlen);
    if (!rfork)
        return NULL;

    char *result = NULL;
    if (ndif_detect(rfork, rlen)) {
        ndif_map_t *map = NULL;
        if (ndif_parse(rfork, rlen, &map) == 0) {
            char scratch[PATH_MAX], identity[PATH_MAX + 128];
            if (!decoded_identity(base_path, "ndif", identity, sizeof(identity), scratch, sizeof(scratch))) {
                LOG(1, "NDIF '%s': path too long for the decode cache", base_path);
            } else if (image_scratch_valid(scratch, identity, (uint64_t)map->sectors * 512)) {
                result = gs_strdup(scratch); // already materialised
            } else {
                if (image_scratch_prepare(scratch) == 0 && materialize_ndif_host(base_path, map, scratch) == 0 &&
                    image_scratch_seal(scratch, identity) == 0) {
                    LOG(3, "decoded NDIF '%s' -> '%s' (%u sectors)", base_path, scratch, map->sectors);
                    result = gs_strdup(scratch);
                } else {
                    remove(scratch);
                    LOG(1, "NDIF decode failed for '%s'", base_path);
                }
            }
            ndif_map_free(map);
        }
    }
    free(rfork);
    return result;
}

// ============================================================================
// Image formats
// ============================================================================
//
// Every format a disk image file can be in, in probe order (09-storage
// F-51).  Two kinds:
//   - a container is decoded to a raw scratch file first (UDIF, NDIF); the
//     first that decodes wins, else the file itself is used;
//   - a layout says where the disk data sits in that file (DiskCopy 4.2's
//     data after its 0x54-byte header; raw, the whole file); the first that
//     recognises the file wins, and raw recognises anything.
// A container that is detected but does not decode falls through to the
// next format, as the hard-coded chain this replaces did -- ultimately to
// raw.

typedef struct image_format {
    const char *name;
    // Container: the malloc'd path of the decoded raw image, or NULL (not
    // this format, or it did not decode).
    char *(*decode)(const char *base_path);
    // Layout: 1 if `path` (of `file_size` bytes) is this layout, setting
    // *data_size and *is_diskcopy; 0 if not; <0 if it cannot be read.
    int (*layout)(const char *path, size_t file_size, size_t *data_size, bool *is_diskcopy);
} image_format_t;

static int diskcopy42_layout(const char *path, size_t file_size, size_t *data_size, bool *is_diskcopy) {
    uint32_t n = 0;
    int rc = detect_diskcopy(path, file_size, &n);
    if (rc > 0) {
        *data_size = n;
        *is_diskcopy = true;
    }
    return rc;
}

static int raw_layout(const char *path, size_t file_size, size_t *data_size, bool *is_diskcopy) {
    (void)path;
    *data_size = file_size;
    *is_diskcopy = false;
    return 1;
}

static const image_format_t g_image_formats[] = {
    {"udif",       udif_decode, NULL             },
    {"ndif",       ndif_decode, NULL             },
    {"diskcopy42", NULL,        diskcopy42_layout},
    {"raw",        NULL,        raw_layout       },
};
#define N_IMAGE_FORMATS (sizeof(g_image_formats) / sizeof(g_image_formats[0]))

// Resolve `base_path` through the format table: the file storage should open
// (the source, or its decoded scratch copy; malloc'd into *out_path), how
// many bytes of disk data it holds, and whether they sit after a DiskCopy
// 4.2 header.  0, or -1 (unreadable, or not a whole number of blocks).
static int resolve_image(const char *base_path, uint32_t block_size, char **out_path, size_t *out_raw_size,
                         bool *out_is_diskcopy) {
    char *path = NULL;
    for (size_t i = 0; i < N_IMAGE_FORMATS && !path; i++)
        if (g_image_formats[i].decode)
            path = g_image_formats[i].decode(base_path);
    if (!path)
        path = gs_strdup(base_path);
    if (!path)
        return -1;

    size_t file_size = 0;
    if (read_file_size(path, &file_size) != 0) {
        printf("image: cannot read file size: %s\n", path);
        free(path);
        return -1;
    }
    size_t raw_size = 0;
    bool is_diskcopy = false;
    int rc = 0;
    for (size_t i = 0; i < N_IMAGE_FORMATS && rc == 0; i++)
        if (g_image_formats[i].layout)
            rc = g_image_formats[i].layout(path, file_size, &raw_size, &is_diskcopy);
    if (rc < 0 || (raw_size % block_size) != 0) {
        free(path);
        return -1;
    }
    *out_path = path;
    *out_raw_size = raw_size;
    *out_is_diskcopy = is_diskcopy;
    return 0;
}

image_t *image_open_readonly(const char *base_path) {
    return image_open_readonly_with_geometry(base_path, (image_geometry_t){.block_size = STORAGE_BLOCK_SIZE});
}

image_t *image_open_readonly_with_geometry(const char *base_path, image_geometry_t geom) {
    if (!base_path || !*base_path)
        return NULL;
    uint32_t block_size = geometry_block_size(geom);

    char *effective = NULL;
    size_t raw_size = 0;
    bool is_diskcopy = false;
    if (resolve_image(base_path, block_size, &effective, &raw_size, &is_diskcopy) != 0)
        return NULL;

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

    // Mint a scratch instance under the scratch root so the read-only
    // mount does not pollute the base image's directory with delta
    // sidecars.
    gs_mkdir_p(image_scratch_dir());
    char id[17];
    mint_random_hex_id(id, sizeof(id));
    image->instance_path = NULL; // never serialized for read-only mounts
    image->delta_path = gs_str_printf("%s/%s.delta", image_scratch_dir(), id);
    image->journal_path = gs_str_printf("%s/%s.journal", image_scratch_dir(), id);
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

    char *effective = NULL;
    size_t raw_size = 0;
    bool is_diskcopy = false;
    if (resolve_image(base_path, block_size, &effective, &raw_size, &is_diskcopy) != 0)
        return NULL;

    // Default delta_dir: GS_STORAGE_CACHE when set (sidecars routed away
    // from the media — see image_scratch_dir), else the directory
    // containing the (original) base image.  Headless callers may pass
    // NULL when they have no machine-id concept (§2.4).
    char *derived_dir = NULL;
    if (!delta_dir || !*delta_dir) {
        const char *cache = getenv("GS_STORAGE_CACHE");
        if (cache && *cache) {
            delta_dir = cache;
        } else {
            derived_dir = dirname_of(base_path);
            delta_dir = derived_dir;
        }
    }
    if (gs_mkdir_p(delta_dir) != 0) {
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
    image->instance_path = gs_str_printf("%s/%s", delta_dir, id);
    image->delta_path = gs_str_printf("%s.delta", image->instance_path);
    image->journal_path = gs_str_printf("%s.journal", image->instance_path);
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
    writable_register(image, base_path);
    return image;
}

image_t *image_open(const char *base_path, const char *instance_path) {
    return image_open_with_geometry(base_path, instance_path, (image_geometry_t){.block_size = STORAGE_BLOCK_SIZE});
}

image_t *image_open_with_geometry(const char *base_path, const char *instance_path, image_geometry_t geom) {
    if (!base_path || !*base_path || !instance_path || !*instance_path)
        return NULL;
    uint32_t block_size = geometry_block_size(geom);

    char *effective = NULL;
    size_t raw_size = 0;
    bool is_diskcopy = false;
    if (resolve_image(base_path, block_size, &effective, &raw_size, &is_diskcopy) != 0)
        return NULL;

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
    image->instance_path = gs_strdup(instance_path);
    image->delta_path = gs_str_printf("%s.delta", instance_path);
    image->journal_path = gs_str_printf("%s.journal", instance_path);
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
    writable_register(image, base_path);
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

    // Place the delta+journal in the scratch root so the blank disk's
    // sidecars don't clutter any user directory; ghost_instance unlinks them on
    // image_close.  The image is ephemeral unless exported via image_export_to.
    gs_mkdir_p(image_scratch_dir());
    char id[17];
    mint_random_hex_id(id, sizeof(id));
    image->instance_path = NULL; // never serialized
    image->delta_path = gs_str_printf("%s/%s.delta", image_scratch_dir(), id);
    image->journal_path = gs_str_printf("%s/%s.journal", image_scratch_dir(), id);
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
    gs_mkdir_parents(dest_path);
    FILE *f = fopen(dest_path, "wb");
    if (!f)
        return -1;
    char *iobuf = stream_set_large_buffer(f);
    int rc = storage_save_state(image->storage, f, file_write_cb);
    fclose(f);
    free(iobuf);
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
    gs_mkdir_parents(filename);
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
