// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_mfs.c
// Read-only MFS over a byte source.  See image_mfs.h.

#include "image_mfs.h"

#include "macroman.h"
#include "source.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Layout (Inside Macintosh II, "Data Organization on Volumes")
// ============================================================================

#define MFS_BLOCK      512u
#define MFS_MDB_OFF    1024u // the master directory block is block 2
#define MFS_MAP_OFF    64u // the allocation block map follows the MDB's fields
#define MFS_DIRENT_MIN 51u // a file directory entry without its name
#define MFS_MAX_FILES  4096 // far above any floppy's; bounds a corrupt count

// Allocation map values: 0 is a free block, 1 ends a file's chain, anything
// else is the next block of the chain.
#define MFS_MAP_FREE 0u
#define MFS_MAP_LAST 1u

struct mfs_volume {
    source_t *src; // retained
    uint64_t off, size;
    char name[256];
    uint16_t n_alloc; // drNmAlBlks
    uint32_t alloc_size; // drAlBlkSiz
    uint16_t alloc_start; // drAlBlSt: block (512 bytes) where allocation block 2 starts
    uint16_t *map; // next block of each allocation block, indexed by block number
    mfs_dirent_t *files;
    int n_files;
};

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// A Mac name (MacRoman, `len` bytes) as the VFS shows it: UTF-8, '/' as ':'.
static void mfs_name(const uint8_t *mac, size_t len, char *out, size_t cap) {
    macroman_to_utf8(mac, len, out, cap);
    for (char *p = out; *p; p++)
        if (*p == '/')
            *p = ':';
}

// ============================================================================
// Open
// ============================================================================

bool mfs_probe_source(struct peel_source *src, uint64_t off, uint64_t size) {
    uint8_t sig[2];
    if (!src || size < MFS_MDB_OFF + MFS_BLOCK || source_read_exact(src, off + MFS_MDB_OFF, sig, 2) != 0)
        return false;
    return be16(sig) == MFS_SIG;
}

// Read the allocation map: 12-bit entries for blocks 2 .. n_alloc + 1,
// packed two to three bytes, starting in the MDB.  0 or -1.
static int mfs_read_map(mfs_volume_t *v) {
    size_t bytes = ((size_t)v->n_alloc * 3 + 1) / 2;
    uint8_t *raw = malloc(bytes ? bytes : 1);
    v->map = calloc((size_t)v->n_alloc + 2, sizeof(*v->map));
    if (!raw || !v->map || (bytes && source_read_exact(v->src, v->off + MFS_MDB_OFF + MFS_MAP_OFF, raw, bytes) != 0)) {
        free(raw);
        return -1;
    }
    for (uint32_t i = 0; i < v->n_alloc; i++) {
        size_t o = (size_t)i * 3 / 2;
        uint16_t e =
            (i & 1) ? (uint16_t)((raw[o] & 0x0F) << 8 | raw[o + 1]) : (uint16_t)(raw[o] << 4 | raw[o + 1] >> 4);
        v->map[i + 2] = e;
    }
    free(raw);
    return 0;
}

// Read the file directory: `blocks` blocks from block `start`, each holding
// entries that never span a block; an unused entry ends a block's list.
// 0 or -1.
static int mfs_read_dir(mfs_volume_t *v, uint16_t start, uint16_t blocks, uint16_t expect) {
    int cap = expect > 0 && expect <= MFS_MAX_FILES ? expect : 16;
    v->files = calloc((size_t)cap, sizeof(*v->files));
    if (!v->files)
        return -1;
    uint8_t blk[MFS_BLOCK];
    for (uint32_t b = 0; b < blocks; b++) {
        uint64_t at = (uint64_t)start + b;
        if ((at + 1) * MFS_BLOCK > v->size || source_read_exact(v->src, v->off + at * MFS_BLOCK, blk, MFS_BLOCK) != 0)
            return -1;
        size_t p = 0;
        while (p + MFS_DIRENT_MIN <= MFS_BLOCK && (blk[p] & 0x80)) {
            const uint8_t *e = blk + p;
            size_t nlen = e[50];
            size_t len = MFS_DIRENT_MIN + nlen;
            if (p + len > MFS_BLOCK)
                return -1; // an entry spanning a block: corrupt
            if (v->n_files == MFS_MAX_FILES)
                return -1;
            if (v->n_files == cap) {
                cap *= 2;
                mfs_dirent_t *nf = realloc(v->files, (size_t)cap * sizeof(*nf));
                if (!nf)
                    return -1;
                v->files = nf;
            }
            mfs_dirent_t *f = &v->files[v->n_files++];
            memset(f, 0, sizeof(*f));
            memcpy(f->finder_info, e + 2, 16);
            f->fnum = be32(e + 18);
            f->data_start = be16(e + 22);
            f->data_len = be32(e + 24);
            f->rsrc_start = be16(e + 32);
            f->rsrc_len = be32(e + 34);
            f->created = be32(e + 42);
            f->modified = be32(e + 46);
            mfs_name(e + 51, nlen, f->name, sizeof(f->name));
            p += (len + 1) & ~(size_t)1; // entries are word aligned
        }
    }
    return 0;
}

mfs_volume_t *mfs_open_source(struct peel_source *src, uint64_t off, uint64_t size) {
    uint8_t mdb[MFS_MAP_OFF];
    if (!mfs_probe_source(src, off, size) || source_read_exact(src, off + MFS_MDB_OFF, mdb, sizeof(mdb)) != 0)
        return NULL;
    mfs_volume_t *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->src = source_retain(src);
    v->off = off;
    v->size = size;
    uint16_t n_files = be16(mdb + 12), dir_start = be16(mdb + 14), dir_blocks = be16(mdb + 16);
    v->n_alloc = be16(mdb + 18);
    v->alloc_size = be32(mdb + 20);
    v->alloc_start = be16(mdb + 28);
    mfs_name(mdb + 37, mdb[36] <= 27 ? mdb[36] : 27, v->name, sizeof(v->name));
    // The geometry must fit the volume: allocation blocks of whole sectors,
    // all of them inside it.
    bool ok = v->alloc_size >= MFS_BLOCK && v->alloc_size % MFS_BLOCK == 0 && v->n_alloc > 0 &&
              (uint64_t)v->alloc_start * MFS_BLOCK + (uint64_t)v->n_alloc * v->alloc_size <= size && dir_blocks > 0;
    if (!ok || mfs_read_map(v) != 0 || mfs_read_dir(v, dir_start, dir_blocks, n_files) != 0) {
        mfs_close(v);
        return NULL;
    }
    return v;
}

void mfs_close(mfs_volume_t *vol) {
    if (!vol)
        return;
    source_release(vol->src);
    free(vol->map);
    free(vol->files);
    free(vol);
}

// ============================================================================
// Access
// ============================================================================

const char *mfs_volume_name(const mfs_volume_t *vol) {
    return vol->name;
}

int mfs_count(const mfs_volume_t *vol) {
    return vol->n_files;
}

const mfs_dirent_t *mfs_entry(const mfs_volume_t *vol, int i) {
    return (i >= 0 && i < vol->n_files) ? &vol->files[i] : NULL;
}

// Names compare case-insensitively over ASCII, a Mac '/' (shown ':')
// matching either.
static bool mfs_name_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z')
            ca -= 32;
        if (cb >= 'a' && cb <= 'z')
            cb -= 32;
        if (ca == '/')
            ca = ':';
        if (cb == '/')
            cb = ':';
        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
}

int mfs_lookup(const mfs_volume_t *vol, const char *name, mfs_dirent_t *out) {
    for (int i = 0; i < vol->n_files; i++) {
        if (mfs_name_eq(vol->files[i].name, name)) {
            *out = vol->files[i];
            return 0;
        }
    }
    return -ENOENT;
}

int mfs_read_fork(mfs_volume_t *vol, const mfs_dirent_t *file, bool rsrc, uint64_t off, void *buf, size_t n,
                  size_t *nread) {
    *nread = 0;
    uint64_t len = rsrc ? file->rsrc_len : file->data_len;
    if (off >= len || n == 0)
        return 0;
    if (n > len - off)
        n = (size_t)(len - off);
    // Walk the chain, refusing a block met twice: a corrupt map with a cycle
    // in it ends in an error, never in the same bytes served again.  (Maps
    // hold at most 4095 entries, so the record of where the walk has been
    // costs at most that many bytes.)
    uint8_t *seen = calloc((size_t)vol->n_alloc + 2, 1);
    if (!seen)
        return -ENOMEM;
    int rc = 0;
    uint32_t blk = rsrc ? file->rsrc_start : file->data_start;
    uint64_t skip = off / vol->alloc_size, in = off % vol->alloc_size;
    uint8_t *out = buf;
    for (uint64_t i = 0;; i++) {
        if (blk < 2 || blk > (uint32_t)vol->n_alloc + 1 || seen[blk]) {
            rc = -EIO; // off the map, or round a cycle
            break;
        }
        seen[blk] = 1;
        if (i >= skip) {
            uint64_t at =
                vol->off + (uint64_t)vol->alloc_start * MFS_BLOCK + (uint64_t)(blk - 2) * vol->alloc_size + in;
            size_t k = n - *nread;
            if (k > vol->alloc_size - in)
                k = (size_t)(vol->alloc_size - in);
            rc = source_read_exact(vol->src, at, out + *nread, k);
            if (rc != 0)
                break;
            *nread += k;
            in = 0;
            if (*nread == n)
                break;
        }
        uint16_t next = vol->map[blk];
        if (next == MFS_MAP_LAST || next == MFS_MAP_FREE) {
            rc = -EIO; // the chain ends before the fork does
            break;
        }
        blk = next;
    }
    free(seen);
    return rc;
}
