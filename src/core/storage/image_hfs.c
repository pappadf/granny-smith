// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_hfs.c
// Read-only HFS catalog walker.  Loads the catalog file into memory on
// open, scans every leaf node to collect catalog records in a flat array,
// and answers directory enumeration / path lookup / fork reads against
// that snapshot.  This trades a one-time O(n) scan for O(log n) lookups
// against a full walk — realistic HFS volumes (System 6/7 floppies,
// 800 KB–400 MB A/UX-era HFS partitions) have at most a few thousand
// catalog entries, so the snapshot fits comfortably in RAM and lookups
// are instant.

#include "image_hfs.h"
#include "image.h"
#include "macroman.h"
#include "storage.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// disk_read_data requires 512-aligned offset + length.  HFS and the VFS
// layer work in byte-granular chunks; this helper does the bounce-buffer
// work so callers don't repeat it.  Returns 0 on success, -EIO on short
// read.
static int disk_read_bytes(image_t *img, uint64_t off, void *buf, size_t n) {
    uint8_t *dst = buf;
    size_t done = 0;
    uint8_t blk[STORAGE_BLOCK_SIZE];
    while (done < n) {
        uint64_t abs = off + done;
        uint64_t block_off = abs & ~(uint64_t)(STORAGE_BLOCK_SIZE - 1);
        size_t in_block = (size_t)(abs - block_off);
        size_t take = STORAGE_BLOCK_SIZE - in_block;
        if (take > n - done)
            take = n - done;
        if (in_block == 0 && take == STORAGE_BLOCK_SIZE) {
            // Fast path: block-aligned whole-block read straight into dst.
            if (disk_read_data(img, (size_t)block_off, dst + done, STORAGE_BLOCK_SIZE) != STORAGE_BLOCK_SIZE)
                return -EIO;
        } else {
            if (disk_read_data(img, (size_t)block_off, blk, STORAGE_BLOCK_SIZE) != STORAGE_BLOCK_SIZE)
                return -EIO;
            memcpy(dst + done, blk + in_block, take);
        }
        done += take;
    }
    return 0;
}

// ---- Big-endian helpers ----------------------------------------------------

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// MacRoman -> UTF-8 transcoder lives in macroman.{c,h} so future
// consumers can share the table without depending on image_hfs.c.

// ---- HFS on-disk offsets --------------------------------------------------

#define HFS_MDB_OFF_IN_VOL 1024

// Master Directory Block field offsets (bytes, from MDB start).
#define MDB_OFF_SIG        0
#define MDB_OFF_NM_AL_BLKS 18
#define MDB_OFF_AL_BLK_SIZ 20
#define MDB_OFF_AL_BL_ST   28
#define MDB_OFF_VN         36 // pstring up to 28 bytes
#define MDB_OFF_XT_FL_SIZE 130 // extents overflow file size (u32)
#define MDB_OFF_XT_EXT_REC 134 // 3 x (startABlk:2, numABlks:2) for the EO file
#define MDB_OFF_CT_FL_SIZE 146
#define MDB_OFF_CT_EXT_REC 150 // 3 x (startABlk:2, numABlks:2)

// B-tree node descriptor (14 bytes).
#define NODE_OFF_F_LINK  0
#define NODE_OFF_B_LINK  4
#define NODE_OFF_KIND    8
#define NODE_OFF_HEIGHT  9
#define NODE_OFF_NRECS   10
#define NODE_KIND_LEAF   0xFF // stored as signed int8 = -1
#define NODE_KIND_INDEX  0
#define NODE_KIND_HEADER 1
#define NODE_KIND_MAP    2

// Header record offsets (within node 0's first record).
#define HDR_OFF_DEPTH      0
#define HDR_OFF_ROOT       2
#define HDR_OFF_NRECS      6
#define HDR_OFF_FIRST_LEAF 10
#define HDR_OFF_LAST_LEAF  14
#define HDR_OFF_NODE_SIZE  18

// Catalog record types.
#define CAT_REC_FOLDER        1
#define CAT_REC_FILE          2
#define CAT_REC_FOLDER_THREAD 3
#define CAT_REC_FILE_THREAD   4

// Catalog folder/file record field offsets (record-data, after cdrType byte).
// Folder (kHFSFolderRecord):
#define FOLDER_OFF_VALENCE 4 // after cdrType(1)+reserved(1)+flags(2)
#define FOLDER_OFF_DIRID   6
// File (kHFSFileRecord):
#define FILE_OFF_FINFO      4
#define FILE_OFF_FILEID     20
#define FILE_OFF_DATA_LGLEN 26
#define FILE_OFF_RSRC_LGLEN 36
#define FILE_OFF_FXINFO     56
#define FILE_OFF_DATA_EXT   74
#define FILE_OFF_RSRC_EXT   86

// ---- Internal types -------------------------------------------------------

// One flattened catalog record.  We keep only what callers need; original
// dates and attribute flags are dropped.
typedef struct cat_rec {
    uint32_t parent_cnid;
    uint8_t name_len;
    uint8_t name_raw[31];
    uint8_t record_type; // CAT_REC_FOLDER / CAT_REC_FILE (threads filtered out)
    uint32_t cnid;
    uint32_t valence; // folder only
    hfs_fork_t data_fork; // file only
    hfs_fork_t rsrc_fork; // file only
    uint8_t finder_info[32];
} cat_rec_t;

// One leaf record from the Extents Overflow file.  Each one supplies an
// additional 3 extents for some (fork, file) past its inline extents.
// HFS specifies that the EO file is keyed by (forkType, fileNumber,
// startBlock) where startBlock is the *logical* block number within the
// fork where this set of extents begins.  Multiple EO records per fork
// chain together: their startBlock values cover [startBlock, startBlock
// + sum(num_ablocks across the 3 extents)).
typedef struct hfs_xt_rec {
    uint8_t fork_type; // 0x00 = data fork, 0xFF = resource fork
    uint32_t file_id; // CNID of the owning file
    uint16_t start_block; // logical allocation-block index within the fork
    struct {
        uint32_t start_ablock; // allocation block on volume
        uint32_t num_ablocks;
    } extents[HFS_INLINE_EXTENTS];
} hfs_xt_rec_t;

struct hfs_volume {
    image_t *img;
    uint64_t partition_off; // byte offset of the partition inside the image
    uint64_t partition_size; // partition length in bytes
    uint32_t alloc_block_size;
    uint64_t alloc_block0_byte_off; // partition_off + drAlBlSt * 512
    char volume_name[128];
    cat_rec_t *records;
    size_t n_records;
    // Extents Overflow snapshot.  Loaded eagerly at hfs_open time using
    // the EO file's own inline extents (drXTExtRec in the MDB); the EO
    // file is small enough in practice (usually < 8 KB) that walking it
    // linearly per lookup is fine.  When the EO file *itself* is
    // fragmented beyond 3 extents we fall back to whatever we managed to
    // load — same compromise as the catalog file.
    hfs_xt_rec_t *xt_records;
    size_t n_xt_records;
};

struct hfs_dir_iter {
    const hfs_volume_t *vol;
    uint32_t parent_cnid;
    size_t cursor;
};

// ---- Helpers --------------------------------------------------------------

// Populate a fork descriptor from a file record's length + extents field.
// `file_id` and `fork_type` are the inputs to the Extents Overflow file
// lookup that hfs_read_fork performs when a request spills past the
// inline extents.
static void parse_fork(const uint8_t *rec_data, size_t logical_off, size_t ext_off, uint32_t file_id, uint8_t fork_type,
                       hfs_fork_t *out) {
    out->logical_size = be32(rec_data + logical_off);
    out->file_id = file_id;
    out->fork_type = fork_type;
    for (int i = 0; i < HFS_INLINE_EXTENTS; i++) {
        out->extents[i].start_ablock = be16(rec_data + ext_off + i * 4);
        out->extents[i].num_ablocks = be16(rec_data + ext_off + i * 4 + 2);
    }
}

// Read raw bytes from the image relative to the partition start.
static int read_partition(hfs_volume_t *vol, uint64_t off, void *buf, size_t n) {
    if (off + n > vol->partition_size)
        return -EIO;
    return disk_read_bytes(vol->img, vol->partition_off + off, buf, n);
}

// ---- Catalog file assembly ------------------------------------------------

// Read up to 3 extents of the catalog file into a freshly-allocated buffer.
// Returns 0 on success, negated errno on failure; caller frees *out_buf.
static int load_catalog_file(hfs_volume_t *vol, const uint8_t *mdb, uint8_t **out_buf, size_t *out_size) {
    uint32_t cat_size = be32(mdb + MDB_OFF_CT_FL_SIZE);
    if (cat_size == 0 || cat_size > 64 * 1024 * 1024) {
        // 64 MiB cap guards against corruption; realistic catalogs are < 8 MiB.
        return -EINVAL;
    }
    uint8_t *buf = malloc(cat_size);
    if (!buf)
        return -ENOMEM;

    uint64_t filled = 0;
    for (int i = 0; i < HFS_INLINE_EXTENTS && filled < cat_size; i++) {
        uint32_t start = be16(mdb + MDB_OFF_CT_EXT_REC + i * 4);
        uint32_t count = be16(mdb + MDB_OFF_CT_EXT_REC + i * 4 + 2);
        if (count == 0)
            continue;
        uint64_t ext_bytes = (uint64_t)count * vol->alloc_block_size;
        uint64_t byte_off = vol->alloc_block0_byte_off + (uint64_t)start * vol->alloc_block_size;
        uint64_t take = cat_size - filled;
        if (take > ext_bytes)
            take = ext_bytes;
        if (byte_off + take > vol->partition_size) {
            // Don't read past the end of the partition; catalog is malformed
            // or the extent overflow file is needed.  Bail out; remaining
            // bytes stay zeroed.
            break;
        }
        int rc = disk_read_bytes(vol->img, vol->partition_off + byte_off, buf + filled, (size_t)take);
        if (rc < 0) {
            free(buf);
            return rc;
        }
        filled += take;
    }
    if (filled == 0) {
        free(buf);
        return -EIO;
    }

    *out_buf = buf;
    *out_size = cat_size;
    return 0;
}

// ---- B-tree scan ----------------------------------------------------------

// Parse one leaf node and append its records to *dst (realloc as needed).
// Returns 0 on success, negated errno on failure.
static int parse_leaf_node(const uint8_t *node, size_t node_size, cat_rec_t **dst, size_t *dst_n, size_t *dst_cap) {
    uint16_t nrecs = be16(node + NODE_OFF_NRECS);
    // Record offset table lives at the end of the node: 2 bytes per pointer,
    // nrecs+1 entries (last one is the free-space sentinel).
    if (node_size < 14 + (size_t)(nrecs + 1) * 2)
        return -EINVAL;

    for (uint16_t i = 0; i < nrecs; i++) {
        uint16_t off = be16(node + node_size - (i + 1) * 2);
        uint16_t next = be16(node + node_size - (i + 2) * 2);
        if (off < 14 || next > node_size || next <= off)
            continue; // skip malformed record quietly
        size_t rec_size = next - off;
        const uint8_t *rec = node + off;
        // Key: keyLen(1) + body
        uint8_t key_len = rec[0];
        if (key_len < 1 + 4 || key_len > 37)
            continue;
        // Catalog key body: reserved(1) + parID(4) + nameLen(1) + name[]
        uint32_t parent_cnid = be32(rec + 2);
        uint8_t name_len = rec[6];
        if (name_len > 31)
            continue;
        // Key storage (including keyLen byte) padded up to even length.
        size_t key_bytes = 1 + key_len;
        if (key_bytes & 1)
            key_bytes++;
        if (key_bytes >= rec_size)
            continue;
        const uint8_t *rec_data = rec + key_bytes;
        size_t data_size = rec_size - key_bytes;
        if (data_size < 2)
            continue;

        uint8_t type = rec_data[0];
        if (type != CAT_REC_FOLDER && type != CAT_REC_FILE)
            continue; // filter out thread records

        // Ensure capacity.
        if (*dst_n == *dst_cap) {
            size_t ncap = (*dst_cap == 0) ? 64 : *dst_cap * 2;
            cat_rec_t *nb = realloc(*dst, ncap * sizeof(cat_rec_t));
            if (!nb)
                return -ENOMEM;
            *dst = nb;
            *dst_cap = ncap;
        }
        cat_rec_t *r = &(*dst)[(*dst_n)++];
        memset(r, 0, sizeof(*r));
        r->parent_cnid = parent_cnid;
        r->name_len = name_len;
        memcpy(r->name_raw, rec + 7, name_len);
        r->record_type = type;

        if (type == CAT_REC_FOLDER && data_size >= 10) {
            r->valence = be16(rec_data + FOLDER_OFF_VALENCE);
            r->cnid = be32(rec_data + FOLDER_OFF_DIRID);
        } else if (type == CAT_REC_FILE && data_size >= 98) {
            r->cnid = be32(rec_data + FILE_OFF_FILEID);
            // 0x00 = data fork, 0xFF = resource fork (HFS convention,
            // matches the EO-file key's forkType byte).
            parse_fork(rec_data, FILE_OFF_DATA_LGLEN, FILE_OFF_DATA_EXT, r->cnid, 0x00, &r->data_fork);
            parse_fork(rec_data, FILE_OFF_RSRC_LGLEN, FILE_OFF_RSRC_EXT, r->cnid, 0xFF, &r->rsrc_fork);
            // FInfo (16 bytes) + FXInfo (16 bytes) form a contiguous 32-byte
            // Finder info block in the on-disk record.
            memcpy(r->finder_info, rec_data + FILE_OFF_FINFO, 16);
            memcpy(r->finder_info + 16, rec_data + FILE_OFF_FXINFO, 16);
        } else {
            (*dst_n)--; // malformed; drop it
        }
    }
    return 0;
}

// ---- Extents Overflow file --------------------------------------------------
//
// Loads the EO file's three inline extents into memory at hfs_open time
// (the same compromise as the catalog file — the EO file's own
// fragmentation past 3 extents falls through unreached, but in practice
// the EO file is tiny).  Walks its leaf chain and builds a flat sorted
// list of (forkType, fileID, startBlock, extents[3]) records that
// hfs_read_fork can binary-search when a request spills past a fork's
// inline extents.

// Read up to 3 extents of the extents-overflow file into a freshly
// allocated buffer.  Behaves like load_catalog_file but reads
// drXTFlSize / drXTExtRec instead.  Returns 0 on success, negated
// errno on failure; caller frees *out_buf.  If the file is empty
// (drXTFlSize == 0) this returns 0 with *out_size == 0.
static int load_xt_file(hfs_volume_t *vol, const uint8_t *mdb, uint8_t **out_buf, size_t *out_size) {
    uint32_t xt_size = be32(mdb + MDB_OFF_XT_FL_SIZE);
    *out_buf = NULL;
    *out_size = 0;
    if (xt_size == 0)
        return 0;
    if (xt_size > 8 * 1024 * 1024)
        return -EINVAL; // sanity cap
    uint8_t *buf = malloc(xt_size);
    if (!buf)
        return -ENOMEM;
    uint64_t filled = 0;
    for (int i = 0; i < HFS_INLINE_EXTENTS && filled < xt_size; i++) {
        uint32_t start = be16(mdb + MDB_OFF_XT_EXT_REC + i * 4);
        uint32_t count = be16(mdb + MDB_OFF_XT_EXT_REC + i * 4 + 2);
        if (count == 0)
            continue;
        uint64_t ext_bytes = (uint64_t)count * vol->alloc_block_size;
        uint64_t byte_off = vol->alloc_block0_byte_off + (uint64_t)start * vol->alloc_block_size;
        uint64_t take = xt_size - filled;
        if (take > ext_bytes)
            take = ext_bytes;
        if (byte_off + take > vol->partition_size)
            break;
        int rc = disk_read_bytes(vol->img, vol->partition_off + byte_off, buf + filled, (size_t)take);
        if (rc < 0) {
            free(buf);
            return rc;
        }
        filled += take;
    }
    if (filled == 0) {
        free(buf);
        return 0; // empty after all (corrupt header)
    }
    *out_buf = buf;
    *out_size = (size_t)filled;
    return 0;
}

// Parse one EO leaf node into the volume's xt_records list.  Each record
// in the EO B-tree has:
//   key: keyLen(1) + forkType(1) + reserved(1?) + fileNumber(4) + startBlock(2)
//   data: 3 × (startBlock(2) + numBlocks(2))
// Inside Macintosh: Files §2-72 documents keyLen = 7 (the byte after it
// is the forkType; reserved byte may or may not be present — in HFS the
// "reserved" byte is the high byte of the keyLen field, so keyLen of 7
// implies a 7-byte key body of forkType(1) + fileNumber(4) + startBlock(2)).
static int parse_xt_leaf_node(const uint8_t *node, size_t node_size, hfs_xt_rec_t **dst, size_t *dst_n,
                              size_t *dst_cap) {
    uint16_t nrecs = be16(node + NODE_OFF_NRECS);
    if (node_size < 14 + (size_t)(nrecs + 1) * 2)
        return -EINVAL;
    for (uint16_t i = 0; i < nrecs; i++) {
        uint16_t off = be16(node + node_size - (i + 1) * 2);
        uint16_t next = be16(node + node_size - (i + 2) * 2);
        if (off < 14 || next > node_size || next <= off)
            continue;
        size_t rec_size = next - off;
        const uint8_t *rec = node + off;
        uint8_t key_len = rec[0];
        // EO key is exactly 7 bytes: forkType + fileNumber + startBlock.
        if (key_len != 7)
            continue;
        // Key body starts at rec+1.
        uint8_t fork_type = rec[1];
        uint32_t file_id = be32(rec + 2);
        uint16_t start_block = be16(rec + 6);
        // Key storage rounds up to even.
        size_t key_bytes = 1 + key_len; // == 8, already even
        if (key_bytes & 1)
            key_bytes++;
        if (key_bytes + 12 > rec_size)
            continue; // need 12 bytes of extent data
        const uint8_t *rec_data = rec + key_bytes;

        if (*dst_n == *dst_cap) {
            size_t ncap = (*dst_cap == 0) ? 32 : *dst_cap * 2;
            hfs_xt_rec_t *nb = realloc(*dst, ncap * sizeof(hfs_xt_rec_t));
            if (!nb)
                return -ENOMEM;
            *dst = nb;
            *dst_cap = ncap;
        }
        hfs_xt_rec_t *r = &(*dst)[(*dst_n)++];
        memset(r, 0, sizeof(*r));
        r->fork_type = fork_type;
        r->file_id = file_id;
        r->start_block = start_block;
        for (int e = 0; e < HFS_INLINE_EXTENTS; e++) {
            r->extents[e].start_ablock = be16(rec_data + e * 4);
            r->extents[e].num_ablocks = be16(rec_data + e * 4 + 2);
        }
    }
    return 0;
}

// Walk the EO B-tree's leaf chain and populate vol->xt_records.
static int collect_xt_records(hfs_volume_t *vol, const uint8_t *xt_buf, size_t xt_size, size_t node_size,
                              uint32_t first_leaf) {
    hfs_xt_rec_t *dst = NULL;
    size_t n = 0, cap = 0;
    uint32_t node_idx = first_leaf;
    size_t max_nodes = node_size ? (xt_size / node_size) + 1 : 0;
    size_t visited = 0;
    while (node_idx != 0 && visited++ < max_nodes) {
        uint64_t off = (uint64_t)node_idx * node_size;
        if (off + node_size > xt_size)
            break;
        const uint8_t *node = xt_buf + off;
        uint8_t kind = node[NODE_OFF_KIND];
        if (kind != NODE_KIND_LEAF)
            break;
        int rc = parse_xt_leaf_node(node, node_size, &dst, &n, &cap);
        if (rc < 0) {
            free(dst);
            return rc;
        }
        node_idx = be32(node + NODE_OFF_F_LINK);
    }
    vol->xt_records = dst;
    vol->n_xt_records = n;
    return 0;
}

// Find the EO record that covers the requested logical-block offset for
// a (forkType, fileID).  Multiple EO records can chain together; the
// matching one is the highest-start_block record with start_block <=
// `need_block`.  Returns NULL when no overflow extent covers the
// request (the caller falls through to zero-padding).
static const hfs_xt_rec_t *find_xt_record(const hfs_volume_t *vol, uint8_t fork_type, uint32_t file_id,
                                          uint32_t need_block) {
    const hfs_xt_rec_t *best = NULL;
    for (size_t i = 0; i < vol->n_xt_records; i++) {
        const hfs_xt_rec_t *r = &vol->xt_records[i];
        if (r->fork_type != fork_type || r->file_id != file_id)
            continue;
        if (r->start_block > need_block)
            continue;
        if (!best || r->start_block > best->start_block)
            best = r;
    }
    return best;
}

// Walk the catalog B-tree's leaf chain starting from firstLeaf.
static int collect_catalog_records(hfs_volume_t *vol, const uint8_t *cat_buf, size_t cat_size, size_t node_size,
                                   uint32_t first_leaf) {
    cat_rec_t *dst = NULL;
    size_t n = 0, cap = 0;
    uint32_t node_idx = first_leaf;
    // Hard upper bound on chain length: at most one visit per node in the
    // catalog. Anything past that means a forward-link cycle.
    size_t max_nodes = node_size ? (cat_size / node_size) + 1 : 0;
    size_t visited = 0;

    while (node_idx != 0 && visited++ < max_nodes) {
        uint64_t off = (uint64_t)node_idx * node_size;
        if (off + node_size > cat_size)
            break;
        const uint8_t *node = cat_buf + off;
        uint8_t kind = node[NODE_OFF_KIND];
        if (kind != NODE_KIND_LEAF)
            break;
        int rc = parse_leaf_node(node, node_size, &dst, &n, &cap);
        if (rc < 0) {
            free(dst);
            return rc;
        }
        node_idx = be32(node + NODE_OFF_F_LINK);
    }

    vol->records = dst;
    vol->n_records = n;
    return 0;
}

// ---- Comparison / lookup --------------------------------------------------

// HFS filenames compare case-insensitively (the native comparison folds
// via the Macintosh Roman case table; ASCII-only volumes see plain toupper).
// Fold a single byte for case-insensitive HFS comparison.  Also maps
// '/' (HFS-legal in filenames) and ':' (Unix-side stand-in we expose
// via the VFS) to the same canonical value so a file named
// "MacTest cx/ci" on disk can be addressed as ".../MacTest cx:ci"
// through Unix-style "/"-separated paths.  See `fill_dirent` for the
// matching outbound swap.
static uint8_t hfs_fold_byte(uint8_t c) {
    if (c >= 'a' && c <= 'z')
        c -= 32;
    if (c == '/' || c == ':')
        c = ':';
    return c;
}

static int ci_compare_name(const uint8_t *a, size_t la, const uint8_t *b, size_t lb) {
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) {
        uint8_t ca = hfs_fold_byte(a[i]);
        uint8_t cb = hfs_fold_byte(b[i]);
        if (ca != cb)
            return (int)ca - (int)cb;
    }
    if (la != lb)
        return (la < lb) ? -1 : 1;
    return 0;
}

// Compare a supplied UTF-8 component string (with our internal restriction
// that inputs are ASCII for lookup — MacRoman hi-bytes in filenames still
// list correctly on ls but cannot be looked up via plain-ASCII shell paths
// in v1; a proper UTF-8 decode is future work).
static bool name_matches_utf8(const uint8_t *raw, size_t raw_len, const char *utf8) {
    // First try: treat both as case-insensitive bytes if UTF-8 input is ASCII.
    size_t ul = strlen(utf8);
    bool ascii = true;
    for (size_t i = 0; i < ul; i++) {
        if ((unsigned char)utf8[i] >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii && raw_len == ul) {
        return ci_compare_name(raw, raw_len, (const uint8_t *)utf8, ul) == 0;
    }
    // Fallback: transcode the raw name and compare UTF-8 strings.
    char tmp[256];
    macroman_to_utf8(raw, raw_len, tmp, sizeof(tmp));
    if (strlen(tmp) != ul)
        return false;
    // Case-insensitive ASCII fold on both sides (good enough for v1),
    // with the HFS↔Unix '/' ↔ ':' equivalence applied symmetrically so
    // a name containing a slash on disk can be matched via a colon-
    // separated path component on the way in.
    for (size_t i = 0; i < ul; i++) {
        uint8_t a = hfs_fold_byte((uint8_t)tmp[i]);
        uint8_t b = hfs_fold_byte((uint8_t)utf8[i]);
        if (a != b)
            return false;
    }
    return true;
}

// Populate a VFS-facing dirent from an internal record.
static void fill_dirent(const cat_rec_t *r, hfs_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    macroman_to_utf8(r->name_raw, r->name_len, out->name, sizeof(out->name));
    // HFS↔Unix path-separator swap: HFS allows '/' in filenames and
    // uses ':' as the on-disk separator.  Our VFS uses '/' as the
    // path separator, so we expose any '/' bytes in HFS names as ':'
    // instead — symmetric with the comparison fold in hfs_fold_byte().
    // Callers that need the literal on-disk name can pull it from the
    // raw record; this is the VFS-facing view.
    for (char *p = out->name; *p; p++) {
        if (*p == '/')
            *p = ':';
    }
    out->is_dir = (r->record_type == CAT_REC_FOLDER);
    out->cnid = r->cnid;
    out->valence = r->valence;
    out->data_fork = r->data_fork;
    out->rsrc_fork = r->rsrc_fork;
    memcpy(out->finder_info, r->finder_info, 32);
}

// ---- Public API -----------------------------------------------------------

hfs_volume_t *hfs_open(image_t *img, uint64_t partition_byte_offset, uint64_t partition_byte_size) {
    if (!img || partition_byte_size < HFS_MDB_OFF_IN_VOL + 512)
        return NULL;

    // Read the MDB (512 bytes at partition offset + 1024).
    uint8_t mdb[512];
    if (disk_read_data(img, partition_byte_offset + HFS_MDB_OFF_IN_VOL, mdb, sizeof(mdb)) != sizeof(mdb))
        return NULL;
    if (be16(mdb + MDB_OFF_SIG) != HFS_SIG_BD)
        return NULL;

    hfs_volume_t *vol = calloc(1, sizeof(*vol));
    if (!vol)
        return NULL;
    vol->img = img;
    vol->partition_off = partition_byte_offset;
    vol->partition_size = partition_byte_size;
    vol->alloc_block_size = be32(mdb + MDB_OFF_AL_BLK_SIZ);
    uint32_t al_bl_st = be16(mdb + MDB_OFF_AL_BL_ST);
    vol->alloc_block0_byte_off = (uint64_t)al_bl_st * 512;
    if (vol->alloc_block_size == 0 || vol->alloc_block_size % 512 != 0) {
        free(vol);
        return NULL;
    }

    // Volume name pstring at MDB + 36.
    uint8_t vn_len = mdb[MDB_OFF_VN];
    if (vn_len > 27)
        vn_len = 27;
    macroman_to_utf8(mdb + MDB_OFF_VN + 1, vn_len, vol->volume_name, sizeof(vol->volume_name));

    // Load catalog file.
    uint8_t *cat = NULL;
    size_t cat_size = 0;
    int rc = load_catalog_file(vol, mdb, &cat, &cat_size);
    if (rc < 0) {
        free(vol);
        return NULL;
    }

    // Parse B-tree header node (node 0).  Standard HFS node size is 512.
    if (cat_size < 14 + 106) {
        free(cat);
        free(vol);
        return NULL;
    }
    // The header record starts right after the 14-byte node descriptor.
    size_t node_size = be16(cat + 14 + HDR_OFF_NODE_SIZE);
    if (node_size == 0)
        node_size = 512;
    if (node_size < 512 || node_size > 8192 || (node_size & (node_size - 1)) != 0) {
        // node_size must be a power of two in [512, 8192].
        free(cat);
        free(vol);
        return NULL;
    }
    // Reject volumes whose catalog file is shorter than one B-tree node —
    // otherwise the leaf-chain walker below reads garbage past EOF.
    if (cat_size < node_size) {
        free(cat);
        free(vol);
        return NULL;
    }
    uint32_t first_leaf = be32(cat + 14 + HDR_OFF_FIRST_LEAF);

    rc = collect_catalog_records(vol, cat, cat_size, node_size, first_leaf);
    free(cat);
    if (rc < 0) {
        free(vol->records);
        free(vol);
        return NULL;
    }

    // Extents Overflow file — populates vol->xt_records for any fork
    // that spills past its 3 inline extents.  Empty / missing EO file is
    // not an error; many small volumes have nothing in it.  We tolerate
    // partial reads of the EO file itself for the same reason we
    // tolerate partial reads of the catalog: realistic volumes keep
    // these special files small enough to fit in their inline extents.
    uint8_t *xt = NULL;
    size_t xt_size = 0;
    if (load_xt_file(vol, mdb, &xt, &xt_size) == 0 && xt != NULL && xt_size >= 14 + HDR_OFF_NODE_SIZE + 2) {
        size_t xt_node_size = be16(xt + 14 + HDR_OFF_NODE_SIZE);
        if (xt_node_size > 0 && xt_node_size <= xt_size) {
            uint32_t xt_first_leaf = be32(xt + 14 + HDR_OFF_FIRST_LEAF);
            if (collect_xt_records(vol, xt, xt_size, xt_node_size, xt_first_leaf) < 0) {
                // Non-fatal — fall through with whatever (if anything)
                // we did manage to collect.
            }
        }
    }
    free(xt);
    return vol;
}

void hfs_close(hfs_volume_t *vol) {
    if (!vol)
        return;
    free(vol->records);
    free(vol->xt_records);
    free(vol);
}

const char *hfs_volume_name(const hfs_volume_t *vol) {
    return vol ? vol->volume_name : "";
}

// Find a record with the given parent CNID and UTF-8 name.  O(n) scan; fine
// for realistic catalog sizes.
static const cat_rec_t *find_child(const hfs_volume_t *vol, uint32_t parent_cnid, const char *name) {
    for (size_t i = 0; i < vol->n_records; i++) {
        const cat_rec_t *r = &vol->records[i];
        if (r->parent_cnid != parent_cnid)
            continue;
        if (name_matches_utf8(r->name_raw, r->name_len, name))
            return r;
    }
    return NULL;
}

int hfs_lookup(hfs_volume_t *vol, const char *const *components, size_t nc, hfs_dirent_t *out) {
    if (!vol || !out)
        return -EINVAL;
    if (nc == 0) {
        // Synthetic root folder entry — no real catalog record exists for
        // CNID 2 as a child, but callers may still ask for its metadata.
        memset(out, 0, sizeof(*out));
        snprintf(out->name, sizeof(out->name), "%s", vol->volume_name);
        out->is_dir = true;
        out->cnid = HFS_ROOT_CNID;
        // Count root-level children to populate valence.
        for (size_t i = 0; i < vol->n_records; i++)
            if (vol->records[i].parent_cnid == HFS_ROOT_CNID)
                out->valence++;
        return 0;
    }
    uint32_t parent = HFS_ROOT_CNID;
    const cat_rec_t *r = NULL;
    for (size_t i = 0; i < nc; i++) {
        r = find_child(vol, parent, components[i]);
        if (!r)
            return -ENOENT;
        // Descend only if this isn't the last component.
        if (i + 1 < nc) {
            if (r->record_type != CAT_REC_FOLDER)
                return -ENOTDIR;
            parent = r->cnid;
        }
    }
    fill_dirent(r, out);
    return 0;
}

hfs_dir_iter_t *hfs_opendir_cnid(hfs_volume_t *vol, uint32_t parent_cnid) {
    if (!vol)
        return NULL;
    hfs_dir_iter_t *iter = calloc(1, sizeof(*iter));
    if (!iter)
        return NULL;
    iter->vol = vol;
    iter->parent_cnid = parent_cnid;
    iter->cursor = 0;
    return iter;
}

int hfs_readdir_next(hfs_dir_iter_t *iter, hfs_dirent_t *out) {
    if (!iter || !out)
        return -EINVAL;
    const hfs_volume_t *vol = iter->vol;
    while (iter->cursor < vol->n_records) {
        const cat_rec_t *r = &vol->records[iter->cursor++];
        if (r->parent_cnid != iter->parent_cnid)
            continue;
        fill_dirent(r, out);
        return 1;
    }
    return 0;
}

void hfs_closedir_iter(hfs_dir_iter_t *iter) {
    free(iter);
}

int hfs_read_fork(hfs_volume_t *vol, const hfs_fork_t *fork, uint64_t off, void *buf, size_t n, size_t *nread) {
    if (!vol || !fork || !buf)
        return -EINVAL;
    if (nread)
        *nread = 0;
    if (off >= fork->logical_size) {
        // Reads past EOF return zero bytes, not an error.
        return 0;
    }
    // Clamp to logical size.
    uint64_t remaining = fork->logical_size - off;
    if (n > remaining)
        n = (size_t)remaining;

    uint8_t *dst = buf;
    size_t done = 0;
    uint64_t cursor = 0; // logical byte offset at start of extent
    uint32_t blocks_consumed = 0; // logical allocation blocks consumed so far

    // Helper closure: read from one extent if it overlaps the request,
    // updating `done` and `cursor`/`blocks_consumed`.  Returns 0 on
    // success, negated errno on read failure.
#define APPLY_EXTENT(START, COUNT)                                                                                     \
    do {                                                                                                               \
        uint64_t ext_bytes = (uint64_t)(COUNT) * vol->alloc_block_size;                                                \
        if (ext_bytes != 0) {                                                                                          \
            uint64_t ext_end = cursor + ext_bytes;                                                                     \
            if (off + done < ext_end) {                                                                                \
                uint64_t rel = (off + done > cursor) ? (off + done - cursor) : 0;                                      \
                uint64_t take = ext_bytes - rel;                                                                       \
                if (take > n - done)                                                                                   \
                    take = n - done;                                                                                   \
                uint64_t ext_start_byte = vol->alloc_block0_byte_off + (uint64_t)(START) * vol->alloc_block_size;      \
                int _rc = read_partition(vol, ext_start_byte + rel, dst + done, (size_t)take);                         \
                if (_rc < 0)                                                                                           \
                    return _rc;                                                                                        \
                done += (size_t)take;                                                                                  \
            }                                                                                                          \
            cursor = ext_end;                                                                                          \
            blocks_consumed += (uint32_t)(COUNT);                                                                      \
        }                                                                                                              \
    } while (0)

    // 1) Inline extents from the catalog record.
    for (int i = 0; i < HFS_INLINE_EXTENTS && done < n; i++) {
        APPLY_EXTENT(fork->extents[i].start_ablock, fork->extents[i].num_ablocks);
    }

    // 2) Extents Overflow file — repeatedly look up the next EO record
    // for (fork_type, file_id) whose start_block matches our current
    // logical position.  Each EO record supplies 3 more extents; chain
    // them together until either the request is satisfied or no more
    // records cover us (in which case the tail zero-fills, same as the
    // pre-EO behaviour).
    while (done < n && fork->file_id != 0) {
        const hfs_xt_rec_t *xt = find_xt_record(vol, fork->fork_type, fork->file_id, blocks_consumed);
        if (!xt)
            break;
        // Skip past any blocks within this EO record that lie before
        // our logical cursor (defensive — find_xt_record returns the
        // record whose start_block <= blocks_consumed, so the first
        // extent in the record covers blocks [start_block, start_block
        // + extents[0].num_ablocks), and we may need to skip some of
        // those if the previous record's extents stopped mid-coverage).
        uint32_t rec_block = xt->start_block;
        if (rec_block > blocks_consumed)
            break; // gap — should not happen on a well-formed volume
        size_t inflight_done = done;
        for (int i = 0; i < HFS_INLINE_EXTENTS && done < n; i++) {
            uint32_t ext_count = xt->extents[i].num_ablocks;
            if (ext_count == 0)
                continue;
            uint32_t ext_start_block = xt->extents[i].start_ablock;
            if (rec_block + ext_count <= blocks_consumed) {
                // Already past this extent (shouldn't really happen
                // since find_xt_record picks the latest applicable
                // record, but be defensive).
                rec_block += ext_count;
                continue;
            }
            uint32_t skip_blocks = (blocks_consumed > rec_block) ? (blocks_consumed - rec_block) : 0;
            APPLY_EXTENT(ext_start_block + skip_blocks, ext_count - skip_blocks);
            rec_block += ext_count;
        }
        if (done == inflight_done) {
            // Didn't make any forward progress; bail to avoid infinite loop
            // on a malformed EO record.
            break;
        }
    }

#undef APPLY_EXTENT

    // Whatever isn't covered by inline + overflow extents (typically:
    // the EO file itself is fragmented past 3 extents, or the volume
    // is corrupt) zero-pads to satisfy the logical_size contract.
    if (done < n)
        memset(dst + done, 0, n - done);
    if (nread)
        *nread = n;
    return 0;
}
