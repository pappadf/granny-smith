// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// In-memory HFS+ volumes and resource forks for storage tests.  See the header.

#include "hfsplus_builder.h"

#include <string.h>

static void w8(uint8_t *p, uint8_t v) {
    p[0] = v;
}
static void w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void w32(uint8_t *p, uint32_t v) {
    w16(p, (uint16_t)(v >> 16));
    w16(p + 2, (uint16_t)v);
}
static void w64(uint8_t *p, uint64_t v) {
    w32(p, (uint32_t)(v >> 32));
    w32(p + 4, (uint32_t)v);
}

// An ASCII name as HFSUniStr255: length, then UTF-16BE.  Returns bytes written.
static size_t wname(uint8_t *p, const char *s) {
    size_t n = strlen(s);
    w16(p, (uint16_t)n);
    for (size_t i = 0; i < n; i++)
        w16(p + 2 + 2 * i, (uint8_t)s[i]);
    return 2 + 2 * n;
}

// HFSPlusForkData: logical size and one extent.
static void wfork(uint8_t *p, uint64_t logical, uint32_t start, uint32_t count) {
    w64(p + 0x00, logical);
    w32(p + 0x0C, count); // totalBlocks
    w32(p + 0x10, start); // extents[0]
    w32(p + 0x14, count);
}

static uint32_t blocks_for(size_t len) {
    return (uint32_t)((len + HFSB_BLOCK - 1) / HFSB_BLOCK);
}

size_t hfsb_build(uint8_t *img, size_t cap, const char *volname, const hfsb_file_t *files, int n_files) {
    if (n_files < 0 || n_files > HFSB_MAX_FILES)
        return 0;

    // Blocks: 0 holds the volume header (at byte 1024), 1-2 the catalog,
    // then each file's data fork and resource fork in turn.
    uint32_t next = 3;
    uint32_t data_start[HFSB_MAX_FILES], rsrc_start[HFSB_MAX_FILES];
    for (int i = 0; i < n_files; i++) {
        data_start[i] = next;
        next += blocks_for(files[i].data_len);
        rsrc_start[i] = next;
        next += blocks_for(files[i].rsrc ? files[i].rsrc_len : 0);
    }
    size_t size = (size_t)next * HFSB_BLOCK;
    if (size > cap)
        return 0;
    memset(img, 0, size);

    uint8_t *vh = img + 1024;
    w16(vh + 0x00, 0x482B); // "H+"
    w16(vh + 0x02, 4);
    w32(vh + 0x28, HFSB_BLOCK);
    w32(vh + 0x2C, next);
    wfork(vh + 0x110, 2u * HFSB_BLOCK, 1, 2); // catalog: blocks 1-2

    // Catalog header node (block 1).
    uint8_t *n0 = img + HFSB_BLOCK;
    w8(n0 + 8, 1); // kind: header
    w16(n0 + 10, 3);
    uint8_t *hdr = n0 + 14;
    w16(hdr + 0, 1); // treeDepth
    w32(hdr + 2, 1); // rootNode
    w32(hdr + 6, (uint32_t)(2 + n_files)); // leafRecords
    w32(hdr + 10, 1); // firstLeafNode
    w32(hdr + 14, 1); // lastLeafNode
    w16(hdr + 18, (uint16_t)HFSB_BLOCK); // nodeSize

    // Catalog leaf node (block 2).
    uint8_t *n1 = img + 2u * HFSB_BLOCK;
    w8(n1 + 8, 0xFF); // kind: leaf
    w8(n1 + 9, 1); // height
    w16(n1 + 10, (uint16_t)(2 + n_files));
    uint16_t off[2 + HFSB_MAX_FILES + 1];
    uint8_t *p = n1 + 14;
    int r = 0;

    // Root folder thread: key (parent 2, no name) -> the volume name.
    off[r++] = (uint16_t)(p - n1);
    w16(p, 6);
    w32(p + 2, 2);
    w16(p + 6, 0);
    p += 8;
    w16(p, 3); // folder thread
    w32(p + 4, 1);
    p += 8 + wname(p + 8, volname);

    // Root folder: key (parent 1, volname), CNID 2.
    off[r++] = (uint16_t)(p - n1);
    size_t klen = wname(p + 6, volname);
    w16(p, (uint16_t)(4 + klen));
    w32(p + 2, 1);
    p += 6 + klen;
    w16(p + 0x00, 1); // folder
    w32(p + 0x04, (uint32_t)n_files);
    w32(p + 0x08, 2);
    p += 88;

    for (int i = 0; i < n_files; i++) {
        off[r++] = (uint16_t)(p - n1);
        klen = wname(p + 6, files[i].name);
        w16(p, (uint16_t)(4 + klen));
        w32(p + 2, 2); // parent: root
        p += 6 + klen;
        w16(p + 0x00, 2); // file
        w32(p + 0x08, (uint32_t)(16 + i));
        wfork(p + 0x58, files[i].data_len, data_start[i], blocks_for(files[i].data_len));
        if (files[i].rsrc) {
            uint64_t logical = files[i].rsrc_logical_override ? files[i].rsrc_logical_override : files[i].rsrc_len;
            wfork(p + 0xA8, logical, rsrc_start[i], blocks_for(files[i].rsrc_len));
        }
        p += 248;
        if ((size_t)(p - n1) > HFSB_BLOCK - 2u * (size_t)(r + 2))
            return 0; // leaf node full
    }
    off[r] = (uint16_t)(p - n1); // free space
    for (int i = 0; i <= r; i++)
        w16(n1 + HFSB_BLOCK - 2u * (size_t)(i + 1), off[i]);

    for (int i = 0; i < n_files; i++) {
        if (files[i].data_len)
            memcpy(img + (size_t)data_start[i] * HFSB_BLOCK, files[i].data, files[i].data_len);
        if (files[i].rsrc && files[i].rsrc_len)
            memcpy(img + (size_t)rsrc_start[i] * HFSB_BLOCK, files[i].rsrc, files[i].rsrc_len);
    }
    return size;
}

size_t rforkb_build(uint8_t *out, size_t cap, uint32_t type, int16_t id, const uint8_t *data, size_t len) {
    // Header at 0; data area at 256 (one entry: length + bytes); map after it.
    const size_t data_off = 256;
    size_t data_len = 4 + len;
    size_t map_off = data_off + data_len;
    const size_t map_len = 28 + 2 + 8 + 12; // header .. type list .. one ref
    size_t total = map_off + map_len;
    if (total > cap)
        return 0;
    memset(out, 0, total);
    w32(out + 0, (uint32_t)data_off);
    w32(out + 4, (uint32_t)map_off);
    w32(out + 8, (uint32_t)data_len);
    w32(out + 12, (uint32_t)map_len);
    w32(out + data_off, (uint32_t)len);
    memcpy(out + data_off + 4, data, len);

    uint8_t *map = out + map_off;
    w16(map + 24, 28); // type list, from the map
    w16(map + 26, (uint16_t)map_len); // name list: empty, at the end
    uint8_t *tl = map + 28;
    w16(tl, 0); // one type
    w32(tl + 2, type);
    w16(tl + 6, 0); // one resource of it
    w16(tl + 8, 10); // its ref list, from the type list
    uint8_t *ref = tl + 10;
    w16(ref + 0, (uint16_t)id);
    w16(ref + 2, 0xFFFF); // no name
    // attrs 0; data offset 0 (the first entry of the data area); handle 0
    return total;
}
