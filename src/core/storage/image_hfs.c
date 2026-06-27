// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_hfs.c
// Read-only HFS / HFS+ catalog walker.  Loads the catalog file into memory
// on open, scans every leaf node to collect catalog records in a flat
// array, and answers directory enumeration / path lookup / fork reads
// against that snapshot.  This trades a one-time O(n) scan for O(log n)
// lookups against a full walk — realistic volumes (System 6/7 floppies,
// 800 KB–400 MB A/UX-era HFS partitions, HFS+ CD-ROMs) have at most a few
// thousand catalog entries, so the snapshot fits comfortably in RAM and
// lookups are instant.
//
// HFS and HFS+ share the snapshot machinery (the flat cat_rec_t / xt_rec
// arrays, lookup, readdir, and the extent-walking fork reader); only the
// on-disk parse differs.  hfs_open sniffs the signature at volume+1024 and
// dispatches to open_classic (MDB-based, MacRoman names, 16-bit allocation
// blocks) or open_plus (Volume-Header-based, UTF-16 names, 32-bit
// allocation blocks, 8 inline fork extents).  See Apple TN1150 for the
// HFS Plus on-disk format.

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

static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
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

// Classic-HFS "embedded HFS+" wrapper fields (TN1150 "HFS Wrapper").  When a
// classic MDB carries drEmbedSigWord == "H+", the real volume is an HFS Plus
// volume embedded inside the wrapper's allocation space at drEmbedExtent.
#define MDB_OFF_EMBED_SIG   0x7C // uint16 drEmbedSigWord
#define MDB_OFF_EMBED_START 0x7E // uint16 drEmbedExtent.startBlock (wrapper ablocks)
#define MDB_OFF_EMBED_COUNT 0x80 // uint16 drEmbedExtent.blockCount (wrapper ablocks)

// ---- HFS Plus on-disk offsets (TN1150) ------------------------------------

#define HFSP_VH_OFF_IN_VOL 1024 // Volume Header sits 1024 bytes into the volume

// HFS Plus Volume Header field offsets (bytes, from header start).
#define VH_OFF_SIG        0x00 // uint16 "H+" / "HX"
#define VH_OFF_BLOCK_SIZE 0x28 // uint32 allocation block size
#define VH_OFF_TOTAL_BLKS 0x2C // uint32 total allocation blocks
#define VH_OFF_CAT_FORK   0x110 // HFSPlusForkData catalogFile
#define VH_OFF_EXT_FORK   0xC0 // HFSPlusForkData extentsFile

// HFSPlusForkData (80 bytes): logicalSize(8) clumpSize(4) totalBlocks(4)
// then 8 x HFSPlusExtentDescriptor (startBlock:4, blockCount:4).
#define FORKDATA_OFF_LGLEN  0x00 // uint64
#define FORKDATA_OFF_EXTENT 0x10 // 8 x (startBlock:4, blockCount:4)

// HFS Plus catalog key: keyLength(2) parentID(4) nodeName{length:2, UTF16[]}.
#define HFSP_KEY_OFF_PARENT  0x02
#define HFSP_KEY_OFF_NAMELEN 0x06
#define HFSP_KEY_OFF_NAME    0x08

// HFS Plus catalog record types (int16 at record-data offset 0).
#define HFSP_REC_FOLDER        0x0001
#define HFSP_REC_FILE          0x0002
#define HFSP_REC_FOLDER_THREAD 0x0003
#define HFSP_REC_FILE_THREAD   0x0004

// HFSPlusCatalogFolder field offsets (record data, after recordType).
#define HFSP_FOLDER_OFF_VALENCE 0x04
#define HFSP_FOLDER_OFF_ID      0x08
// HFSPlusCatalogFile field offsets.
#define HFSP_FILE_OFF_ID       0x08
#define HFSP_FILE_OFF_USERINFO 0x30 // FInfo(16) + FXInfo(16) = 32 contiguous bytes
#define HFSP_FILE_OFF_DATAFORK 0x58 // HFSPlusForkData (80)
#define HFSP_FILE_OFF_RSRCFORK 0xA8 // HFSPlusForkData (80)
#define HFSP_FILE_REC_MIN      0xF8 // 248 bytes: a complete file record
// HFSPlusCatalogThread: recordType(2) reserved(2) parentID(4) nodeName{...}.
#define HFSP_THREAD_OFF_NAMELEN 0x08

// Well-known HFS Plus CNIDs (TN1150).
#define HFSP_ROOT_PARENT_ID  1 // parent of the root folder
#define HFSP_ROOT_FOLDER_ID  2 // the root folder itself (kHFSRootFolderID)
#define HFSP_EXTENTS_FILE_ID 3 // kHFSExtentsFileID
#define HFSP_CATALOG_FILE_ID 4 // kHFSCatalogFileID

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
// dates and attribute flags are dropped.  The name is stored already
// transcoded to UTF-8 (from MacRoman for HFS, UTF-16 for HFS+) so the
// shared lookup/enumerate code is encoding-agnostic.
typedef struct cat_rec {
    uint32_t parent_cnid;
    char name[256]; // UTF-8, NUL-terminated (long HFS+ names truncated)
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
    uint32_t start_block; // logical allocation-block index within the fork
    struct {
        uint32_t start_ablock; // allocation block on volume
        uint32_t num_ablocks;
    } extents[HFS_FORK_EXTENTS]; // 3 used by HFS, up to 8 by HFS+
} hfs_xt_rec_t;

struct hfs_volume {
    image_t *img;
    uint64_t partition_off; // byte offset of the partition inside the image
    uint64_t partition_size; // partition length in bytes
    uint32_t alloc_block_size;
    uint64_t alloc_block0_byte_off; // partition_off + drAlBlSt * 512
    char volume_name[256]; // UTF-8; sized to hold a full HFS+ name
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
        // Transcode the MacRoman on-disk name to UTF-8 once, at parse time.
        macroman_to_utf8(rec + 7, name_len, r->name, sizeof(r->name));
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

// Compare a record's stored UTF-8 name against a supplied UTF-8 component.
// Matching is case-insensitive over ASCII (good enough for v1; full Unicode
// case folding is future work), with the HFS↔Unix '/' ↔ ':' equivalence
// applied symmetrically so a name containing a slash on disk can be matched
// via a colon-separated path component on the way in.
static bool name_matches(const char *stored_utf8, const char *input_utf8) {
    size_t a = strlen(stored_utf8);
    size_t b = strlen(input_utf8);
    if (a != b)
        return false;
    return ci_compare_name((const uint8_t *)stored_utf8, a, (const uint8_t *)input_utf8, b) == 0;
}

// Populate a VFS-facing dirent from an internal record.
static void fill_dirent(const cat_rec_t *r, hfs_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", r->name);
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

// ---- Classic HFS open -----------------------------------------------------

// Open a classic HFS volume.  `mdb` is the already-read 512-byte Master
// Directory Block (signature "BD" verified by the caller); re-using it
// avoids a second read.
static hfs_volume_t *open_classic(image_t *img, uint64_t partition_byte_offset, uint64_t partition_byte_size,
                                  const uint8_t *mdb) {
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

// ---- HFS Plus open --------------------------------------------------------
//
// HFS+ shares the snapshot consumers (lookup / readdir / hfs_read_fork) but
// has its own on-disk parse: a Volume Header instead of an MDB, UTF-16
// names, 32-bit allocation blocks based at the volume start, and 8 inline
// fork extents.  We assemble the same cat_rec_t / hfs_xt_rec_t arrays so
// everything downstream is identical.  See Apple TN1150.

// Decode `units` UTF-16 big-endian code units to UTF-8 in `dst` (always
// NUL-terminated when dstcap > 0).  Handles surrogate pairs; on a lone
// surrogate emits the replacement character.  Output is truncated rather
// than overflowed.
static void utf16be_to_utf8(const uint8_t *src, size_t units, char *dst, size_t dstcap) {
    size_t o = 0;
    if (dstcap == 0)
        return;
    for (size_t i = 0; i < units; i++) {
        uint32_t c = be16(src + i * 2);
        if (c >= 0xD800 && c <= 0xDBFF) {
            // High surrogate: combine with the following low surrogate.
            uint32_t lo = (i + 1 < units) ? be16(src + (i + 1) * 2) : 0;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            } else {
                c = 0xFFFD; // unpaired surrogate
            }
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            c = 0xFFFD; // stray low surrogate
        }
        if (c < 0x80) {
            if (o + 1 >= dstcap)
                break;
            dst[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= dstcap)
                break;
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            if (o + 3 >= dstcap)
                break;
            dst[o++] = (char)(0xE0 | (c >> 12));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 4 >= dstcap)
                break;
            dst[o++] = (char)(0xF0 | (c >> 18));
            dst[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = '\0';
}

// Populate a fork descriptor from an 80-byte HFSPlusForkData (8 extents).
static void parse_hfsplus_fork(const uint8_t *fd, uint32_t file_id, uint8_t fork_type, hfs_fork_t *out) {
    out->logical_size = be64(fd + FORKDATA_OFF_LGLEN);
    out->file_id = file_id;
    out->fork_type = fork_type;
    for (int i = 0; i < HFS_FORK_EXTENTS; i++) {
        out->extents[i].start_ablock = be32(fd + FORKDATA_OFF_EXTENT + i * 8);
        out->extents[i].num_ablocks = be32(fd + FORKDATA_OFF_EXTENT + i * 8 + 4);
    }
}

// Read a whole special-file fork (catalog / extents) into a freshly
// allocated buffer via the shared extent reader.  `cap` bounds the
// allocation against a corrupt logicalSize.  Returns 0 on success; caller
// frees *out_buf.
static int read_fork_to_buffer(hfs_volume_t *vol, const hfs_fork_t *fork, uint64_t cap, uint8_t **out_buf,
                               size_t *out_size) {
    uint64_t sz = fork->logical_size;
    if (sz == 0 || sz > cap)
        return -EINVAL;
    uint8_t *buf = malloc((size_t)sz);
    if (!buf)
        return -ENOMEM;
    size_t got = 0;
    int rc = hfs_read_fork(vol, fork, 0, buf, (size_t)sz, &got);
    if (rc < 0) {
        free(buf);
        return rc;
    }
    *out_buf = buf;
    *out_size = (size_t)sz;
    return 0;
}

// Parse one HFS+ extents-overflow leaf node.  Key is a fixed 10-byte
// HFSPlusExtentKey; data is 8 extent descriptors (64 bytes).
static int parse_hfsplus_xt_leaf(const uint8_t *node, size_t node_size, hfs_xt_rec_t **dst, size_t *dst_n,
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
        if (rec_size < 2)
            continue;
        uint16_t key_len = be16(rec); // big (uint16) keys
        if (key_len != 10)
            continue;
        size_t key_area = 2 + 10;
        if (key_area + 64 > rec_size)
            continue; // need 8 extents of data
        uint8_t fork_type = rec[2]; // forkType (rec[3] is pad)
        uint32_t file_id = be32(rec + 4);
        uint32_t start_block = be32(rec + 8);
        const uint8_t *recdata = rec + key_area;

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
        for (int e = 0; e < HFS_FORK_EXTENTS; e++) {
            r->extents[e].start_ablock = be32(recdata + e * 8);
            r->extents[e].num_ablocks = be32(recdata + e * 8 + 4);
        }
    }
    return 0;
}

// Walk the HFS+ extents-overflow leaf chain into vol->xt_records.
static int collect_hfsplus_xt(hfs_volume_t *vol, const uint8_t *xt_buf, size_t xt_size, size_t node_size,
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
        if (node[NODE_OFF_KIND] != NODE_KIND_LEAF)
            break;
        int rc = parse_hfsplus_xt_leaf(node, node_size, &dst, &n, &cap);
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

// Parse one HFS+ catalog leaf node, appending folder/file records to *dst.
// Thread records are skipped, except the root folder's thread (and the
// root folder record itself), from which we recover the volume name.
static int parse_hfsplus_catalog_leaf(const uint8_t *node, size_t node_size, cat_rec_t **dst, size_t *dst_n,
                                      size_t *dst_cap, char *vol_name, size_t vol_name_cap) {
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
        if (rec_size < 2)
            continue;
        // HFSPlusCatalogKey: keyLength(2) parentID(4) nodeName{len:2, UTF16}.
        uint16_t key_len = be16(rec);
        size_t key_area = 2 + (size_t)key_len;
        if (key_len < 6 || key_area > rec_size)
            continue;
        uint32_t parent_cnid = be32(rec + HFSP_KEY_OFF_PARENT);
        uint16_t name_units = be16(rec + HFSP_KEY_OFF_NAMELEN);
        if (name_units > 255 || HFSP_KEY_OFF_NAME + (size_t)name_units * 2 > key_area)
            continue;
        const uint8_t *namep = rec + HFSP_KEY_OFF_NAME;
        const uint8_t *recdata = rec + key_area;
        size_t data_size = rec_size - key_area;
        if (data_size < 2)
            continue;
        int16_t type = (int16_t)be16(recdata);

        // The root folder's thread record holds the volume name.
        if (type == HFSP_REC_FOLDER_THREAD && parent_cnid == HFSP_ROOT_FOLDER_ID && vol_name) {
            uint16_t vn_units =
                (data_size >= HFSP_THREAD_OFF_NAMELEN + 2) ? be16(recdata + HFSP_THREAD_OFF_NAMELEN) : 0;
            if (vn_units > 0 && (size_t)HFSP_THREAD_OFF_NAMELEN + 2 + (size_t)vn_units * 2 <= data_size)
                utf16be_to_utf8(recdata + HFSP_THREAD_OFF_NAMELEN + 2, vn_units, vol_name, vol_name_cap);
            continue;
        }
        if (type != HFSP_REC_FOLDER && type != HFSP_REC_FILE)
            continue; // skip threads / unknown record types
        if (type == HFSP_REC_FOLDER && data_size < HFSP_FOLDER_OFF_ID + 4)
            continue;
        if (type == HFSP_REC_FILE && data_size < HFSP_FILE_REC_MIN)
            continue;

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
        utf16be_to_utf8(namep, name_units, r->name, sizeof(r->name));

        if (type == HFSP_REC_FOLDER) {
            r->record_type = CAT_REC_FOLDER;
            r->valence = be32(recdata + HFSP_FOLDER_OFF_VALENCE);
            r->cnid = be32(recdata + HFSP_FOLDER_OFF_ID);
            // The root folder record (folderID==2) names the volume; use it
            // as a fallback if the thread record didn't already set the name.
            if (r->cnid == HFSP_ROOT_FOLDER_ID && vol_name && vol_name[0] == '\0')
                snprintf(vol_name, vol_name_cap, "%s", r->name);
        } else { // HFSP_REC_FILE
            r->record_type = CAT_REC_FILE;
            r->cnid = be32(recdata + HFSP_FILE_OFF_ID);
            parse_hfsplus_fork(recdata + HFSP_FILE_OFF_DATAFORK, r->cnid, 0x00, &r->data_fork);
            parse_hfsplus_fork(recdata + HFSP_FILE_OFF_RSRCFORK, r->cnid, 0xFF, &r->rsrc_fork);
            // FInfo (16) + FXInfo (16) are contiguous in the record.
            memcpy(r->finder_info, recdata + HFSP_FILE_OFF_USERINFO, 32);
        }
    }
    return 0;
}

// Walk the HFS+ catalog leaf chain into vol->records (and vol->volume_name).
static int collect_hfsplus_catalog(hfs_volume_t *vol, const uint8_t *cat_buf, size_t cat_size, size_t node_size,
                                   uint32_t first_leaf) {
    cat_rec_t *dst = NULL;
    size_t n = 0, cap = 0;
    uint32_t node_idx = first_leaf;
    size_t max_nodes = node_size ? (cat_size / node_size) + 1 : 0;
    size_t visited = 0;
    while (node_idx != 0 && visited++ < max_nodes) {
        uint64_t off = (uint64_t)node_idx * node_size;
        if (off + node_size > cat_size)
            break;
        const uint8_t *node = cat_buf + off;
        if (node[NODE_OFF_KIND] != NODE_KIND_LEAF)
            break;
        int rc =
            parse_hfsplus_catalog_leaf(node, node_size, &dst, &n, &cap, vol->volume_name, sizeof(vol->volume_name));
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

// Validate a B-tree node size read from a header record: power of two in
// [512, 32768] and no larger than the file we loaded.
static bool node_size_ok(size_t node_size, size_t file_size) {
    return node_size >= 512 && node_size <= 32768 && (node_size & (node_size - 1)) == 0 && node_size <= file_size;
}

// Open an HFS+ / HFSX volume whose Volume Header sits at offset 1024 from
// `partition_byte_offset`.  Returns NULL on any error.
static hfs_volume_t *open_plus(image_t *img, uint64_t partition_byte_offset, uint64_t partition_byte_size) {
    if (partition_byte_size < HFSP_VH_OFF_IN_VOL + 512)
        return NULL;

    uint8_t vh[512];
    if (disk_read_data(img, partition_byte_offset + HFSP_VH_OFF_IN_VOL, vh, sizeof(vh)) != sizeof(vh))
        return NULL;
    uint16_t sig = be16(vh + VH_OFF_SIG);
    if (sig != HFS_SIG_HP && sig != HFS_SIG_HX)
        return NULL;
    uint32_t block_size = be32(vh + VH_OFF_BLOCK_SIZE);
    if (block_size == 0 || block_size % 512 != 0 || block_size > (1u << 20))
        return NULL;

    hfs_volume_t *vol = calloc(1, sizeof(*vol));
    if (!vol)
        return NULL;
    vol->img = img;
    vol->partition_off = partition_byte_offset;
    vol->partition_size = partition_byte_size;
    vol->alloc_block_size = block_size;
    vol->alloc_block0_byte_off = 0; // HFS+ allocation block 0 is the volume start

    // 1) Extents-overflow file first, so the catalog read can follow
    // overflow extents.  The extents file is never fragmented past its own
    // 8 inline extents, so reading it with no overflow table (the table we
    // are about to build) is safe and well-defined.
    hfs_fork_t ext_fork;
    parse_hfsplus_fork(vh + VH_OFF_EXT_FORK, HFSP_EXTENTS_FILE_ID, 0x00, &ext_fork);
    if (ext_fork.logical_size > 0) {
        uint8_t *xt = NULL;
        size_t xt_size = 0;
        if (read_fork_to_buffer(vol, &ext_fork, 32u * 1024 * 1024, &xt, &xt_size) == 0) {
            if (xt_size >= 14 + HDR_OFF_NODE_SIZE + 2) {
                size_t xt_node = be16(xt + 14 + HDR_OFF_NODE_SIZE);
                uint32_t xt_first = be32(xt + 14 + HDR_OFF_FIRST_LEAF);
                // An empty extents tree (firstLeafNode == 0) is normal.
                if (xt_first != 0 && node_size_ok(xt_node, xt_size))
                    (void)collect_hfsplus_xt(vol, xt, xt_size, xt_node, xt_first); // non-fatal
            }
            free(xt);
        }
    }

    // 2) Catalog file (now able to follow overflow extents).
    hfs_fork_t cat_fork;
    parse_hfsplus_fork(vh + VH_OFF_CAT_FORK, HFSP_CATALOG_FILE_ID, 0x00, &cat_fork);
    uint8_t *cat = NULL;
    size_t cat_size = 0;
    if (read_fork_to_buffer(vol, &cat_fork, 128u * 1024 * 1024, &cat, &cat_size) < 0) {
        free(vol->xt_records);
        free(vol);
        return NULL;
    }
    if (cat_size < 14 + HDR_OFF_NODE_SIZE + 2) {
        free(cat);
        free(vol->xt_records);
        free(vol);
        return NULL;
    }
    size_t node_size = be16(cat + 14 + HDR_OFF_NODE_SIZE);
    uint32_t first_leaf = be32(cat + 14 + HDR_OFF_FIRST_LEAF);
    if (!node_size_ok(node_size, cat_size)) {
        free(cat);
        free(vol->xt_records);
        free(vol);
        return NULL;
    }
    int rc = collect_hfsplus_catalog(vol, cat, cat_size, node_size, first_leaf);
    free(cat);
    if (rc < 0) {
        free(vol->records);
        free(vol->xt_records);
        free(vol);
        return NULL;
    }

    if (vol->volume_name[0] == '\0')
        snprintf(vol->volume_name, sizeof(vol->volume_name), "HFS+");
    return vol;
}

// ---- Public API -----------------------------------------------------------

hfs_volume_t *hfs_open(image_t *img, uint64_t partition_byte_offset, uint64_t partition_byte_size) {
    if (!img || partition_byte_size < HFS_MDB_OFF_IN_VOL + 512)
        return NULL;

    // Read the 512-byte block at volume+1024: a classic HFS MDB or an HFS+
    // Volume Header.  The signature word decides which parser to run.
    uint8_t hdr[512];
    if (disk_read_data(img, partition_byte_offset + HFS_MDB_OFF_IN_VOL, hdr, sizeof(hdr)) != sizeof(hdr))
        return NULL;
    uint16_t sig = be16(hdr + MDB_OFF_SIG);

    if (sig == HFS_SIG_HP || sig == HFS_SIG_HX)
        return open_plus(img, partition_byte_offset, partition_byte_size);

    if (sig == HFS_SIG_BD) {
        // Classic HFS — unless it's a thin wrapper around an embedded HFS+
        // volume (drEmbedSigWord == "H+"), in which case redirect there.
        if (be16(hdr + MDB_OFF_EMBED_SIG) == HFS_SIG_HP) {
            uint32_t al_blk_siz = be32(hdr + MDB_OFF_AL_BLK_SIZ);
            uint32_t al_bl_st = be16(hdr + MDB_OFF_AL_BL_ST);
            uint32_t emb_start = be16(hdr + MDB_OFF_EMBED_START);
            uint32_t emb_count = be16(hdr + MDB_OFF_EMBED_COUNT);
            if (al_blk_siz == 0 || al_blk_siz % 512 != 0)
                return NULL;
            // Embedded-volume location, per TN1150 "HFS Wrapper".
            uint64_t emb_off = (uint64_t)al_bl_st * 512 + (uint64_t)emb_start * al_blk_siz;
            uint64_t emb_size = (uint64_t)emb_count * al_blk_siz;
            if (emb_off + HFSP_VH_OFF_IN_VOL + 512 > partition_byte_size)
                return NULL;
            if (emb_size == 0 || emb_off + emb_size > partition_byte_size)
                emb_size = partition_byte_size - emb_off; // tolerate a bad blockCount
            return open_plus(img, partition_byte_offset + emb_off, emb_size);
        }
        return open_classic(img, partition_byte_offset, partition_byte_size, hdr);
    }

    return NULL;
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
        if (name_matches(r->name, name))
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

    // 1) Inline extents from the catalog record (3 for HFS, up to 8 for
    // HFS+; unused trailing slots have num_ablocks==0 and are skipped).
    for (int i = 0; i < HFS_FORK_EXTENTS && done < n; i++) {
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
        for (int i = 0; i < HFS_FORK_EXTENTS && done < n; i++) {
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
