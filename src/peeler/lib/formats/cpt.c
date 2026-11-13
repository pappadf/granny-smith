// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpt.c — Compact Pro (.cpt) archive peeler.
//
// Format spec: docs/cpt.md

// Ensure strnlen is declared under strict C99 mode.
#define _POSIX_C_SOURCE 200809L

#include "internal.h"

// ============================================================================
// Constants and Macros
//
// cpt.md § 3.1 "Initial Archive Header" — CP_MAGIC / CP_VOLUME_SINGLE.
// cpt.md § 3.2.3 "Directory Entry — File" — CP_FLAG_RSRC_LZH / CP_FLAG_DATA_LZH / CP_DIR_MARKER.
// A fork is RLE-coded (cpt.md § 5), and LZH-coded under that when its flag
// says so (§ 6): the codec library's CODEC_CPT_RLE over CODEC_CPT_LZH.
// ============================================================================

#define CP_MAGIC         0x01
#define CP_VOLUME_SINGLE 0x01
#define CP_FLAG_ENCRYPT  0x0001
#define CP_FLAG_RSRC_LZH 0x0002
#define CP_FLAG_DATA_LZH 0x0004
#define CP_DIR_MARKER    0x80

// The directory's total entry count is 16 bits (cpt.md § 3.2.1), so no
// well-formed archive lists more; folder counts that disagree with it can.
#define CP_MAX_ENTRIES 65535

// ============================================================================
// CPT directory entry (file)
//
// cpt.md § 3.2.3 "Directory Entry — File" — 45-byte metadata block
// per file after the name.  Fields are big-endian.
// cpt.md § 4.3 "Per-File Data CRC-32" — data_crc is stored but not validated.
// ============================================================================

// Parsed metadata for a single file entry in the directory.
typedef struct {
    char name[256];
    uint8_t volume;
    uint32_t file_offset;
    uint32_t type;
    uint32_t creator;
    uint32_t create_date;
    uint32_t mod_date;
    uint16_t finder_flags;
    uint32_t data_crc;
    uint16_t flags;
    uint32_t rsrc_uncomp;
    uint32_t data_uncomp;
    uint32_t rsrc_comp;
    uint32_t data_comp;
    bool is_dir;
} cp_entry_t;

// ============================================================================
// Archive Context — holds parsed directory entries
// ============================================================================

// Growable list of parsed file entries from the directory.
typedef struct {
    cp_entry_t *entries;
    size_t count;
    size_t cap;
} cp_archive_t;

// ============================================================================
// Static Helpers — Directory Parsing
// ============================================================================

// Append a file entry to the archive's entry list, growing as needed.
static int cp_push_entry(cp_archive_t *ar, const cp_entry_t *e) {
    // Folder counts that overrun their parent's let three-byte entries
    // multiply into millions, and ncap * sizeof toward a 32-bit size_t's limit.
    if (ar->count >= CP_MAX_ENTRIES)
        return -1;
    if (ar->count >= ar->cap) {
        size_t ncap = ar->cap ? ar->cap * 2 : 16;
        void *tmp = realloc(ar->entries, ncap * sizeof(cp_entry_t));
        if (!tmp)
            return -1;
        ar->entries = (cp_entry_t *)tmp;
        ar->cap = ncap;
    }
    ar->entries[ar->count++] = *e;
    return 0;
}

// Concatenate parent and name into dst (max 256).
// cpt.md § 3.3 "Directory Hierarchy" — paths are reconstructed by
// walking the recursive depth-first entry tree and concatenating segments.
static void cp_join_path(char dst[256], const char *parent, const char *seg, size_t seg_len) {
    size_t dp = 0;
    dst[0] = '\0';
    if (parent && parent[0]) { // built by this walker, so already safe
        dp = strnlen(parent, 255);
        memcpy(dst, parent, dp);
        dst[dp] = '\0';
    }
    peel_append_segment(dst, 256, &dp, (const uint8_t *)seg, seg_len);
}

// Recursively parse directory entries from in-memory archive data.
// cpt.md § 3.3 "Directory Hierarchy" — a directory entry with subtree
// count C is followed by C depth-first entries.  Consumes C+1 entries
// from the parent's remaining total.
static int cp_walk_entries(cp_archive_t *ar, const uint8_t *data, size_t size, size_t *cursor, int remaining,
                           const char *parent, int depth) {
    // Each level is one recursive call carrying two 256-byte path buffers,
    // bought by three bytes of archive; unbounded, a 180 KB archive of empty
    // folders overflowed the stack.  Paths are joined into
    // 256 bytes, so no real archive nests anywhere near the cap.
    if (depth > PEEL_MAX_DIR_DEPTH)
        return -1;
    while (remaining > 0) {
        if (*cursor >= size)
            return -1;

        // cpt.md § 3.2.2 "Directory Entry — Directory" — high bit of
        // name-length byte marks a directory.
        uint8_t nl_flag = data[*cursor];
        int nlen = nl_flag & 0x7F;
        int is_dir = (nl_flag & CP_DIR_MARKER) != 0;

        if (*cursor + 1 + (size_t)nlen > size)
            return -1;

        // Extract entry name
        const char *raw_name = (const char *)(data + *cursor + 1);
        char seg[256];
        size_t cplen = (size_t)nlen < 255 ? (size_t)nlen : 255;
        memcpy(seg, raw_name, cplen);
        seg[cplen] = '\0';
        *cursor += 1 + (size_t)nlen;

        // Build full path by joining parent and segment
        char full[256];
        cp_join_path(full, parent, seg, cplen);

        if (is_dir) {
            // cpt.md § 3.2.2 — directory entry has 2-byte subtree count
            if (*cursor + 2 > size)
                return -1;
            uint16_t child_cnt = rd16be(data + *cursor);
            *cursor += 2;
            // The folder itself is an entry of the listing.
            cp_entry_t de;
            memset(&de, 0, sizeof(de));
            snprintf(de.name, sizeof(de.name), "%s", full);
            de.is_dir = true;
            if (cp_push_entry(ar, &de) < 0)
                return -1;
            int rc = cp_walk_entries(ar, data, size, cursor, (int)child_cnt, full, depth + 1);
            if (rc < 0)
                return rc;
            remaining -= (int)child_cnt + 1;
            continue;
        }

        // cpt.md § 3.2.3 "Directory Entry — File" — 45 bytes of metadata
        // after the name: volume(1), file_offset(4), type(4), creator(4),
        // create_date(4), mod_date(4), finder_flags(2), data_crc(4),
        // flags(2), rsrc_uncomp(4), data_uncomp(4), rsrc_comp(4),
        // data_comp(4).
        if (*cursor + 45 > size)
            return -1;

        const uint8_t *m = data + *cursor;
        cp_entry_t fe;
        memset(&fe, 0, sizeof(fe));
        snprintf(fe.name, sizeof(fe.name), "%s", full);

        // Parse all 45 bytes of file metadata in field order
        size_t off = 0;
        fe.volume = m[off];
        off += 1;
        fe.file_offset = rd32be(m + off);
        off += 4;
        fe.type = rd32be(m + off);
        off += 4;
        fe.creator = rd32be(m + off);
        off += 4;
        fe.create_date = rd32be(m + off);
        off += 4;
        fe.mod_date = rd32be(m + off);
        off += 4;
        fe.finder_flags = rd16be(m + off);
        off += 2;
        fe.data_crc = rd32be(m + off);
        off += 4;
        fe.flags = rd16be(m + off);
        off += 2;
        fe.rsrc_uncomp = rd32be(m + off);
        off += 4;
        fe.data_uncomp = rd32be(m + off);
        off += 4;
        fe.rsrc_comp = rd32be(m + off);
        off += 4;
        fe.data_comp = rd32be(m + off);
        off += 4;
        (void)off;

        // Add this file entry to the archive's entry list
        if (cp_push_entry(ar, &fe) < 0)
            return -1;

        *cursor += 45;
        remaining -= 1;
    }
    return 0;
}

// Parse the directory at the given offset.
// cpt.md § 3.2.1 "Second Header" — 4-byte CRC, 2-byte total entry count,
// 1-byte comment length, then the recursive entry tree.
static int cp_parse_directory(cp_archive_t *ar, const uint8_t *data, size_t size, uint32_t dir_off) {
    if ((size_t)dir_off + 7 > size)
        return -1;

    // Skip the 4-byte directory CRC (not validated)
    uint16_t total = rd16be(data + dir_off + 4);
    uint8_t comment_len = data[dir_off + 6];
    size_t cursor = (size_t)dir_off + 7 + comment_len;
    if (cursor > size)
        return -1;

    return cp_walk_entries(ar, data, size, &cursor, (int)total, "", 0);
}

// ============================================================================
// Static Helpers — Fork Decompression
// ============================================================================

// ============================================================================
// Operations (Public API) — Detection
// ============================================================================

// cpt.md § 3.1 "Initial Archive Header" — byte 0 is CP_MAGIC (0x01),
// byte 1 is CP_VOLUME_SINGLE (0x01), and bytes 4..7 hold the directory
// offset which must be at least 8 and no more than 256 MiB.
bool cpt_detect(const uint8_t *src, size_t len) {
    if (!src || len < 8)
        return false;

    // Check magic and volume bytes
    if (src[0] != CP_MAGIC || src[1] != CP_VOLUME_SINGLE)
        return false;

    // Validate directory offset is within sane bounds
    uint32_t dir_off = rd32be(src + 4);
    if (dir_off < 8 || dir_off > 0x10000000)
        return false;
    return true;
}

// ============================================================================
// Structure-first access (peeler.h: peel_open)
// ============================================================================
//
// Two reads: the 8-byte header gives the directory's absolute offset, and
// the directory (at the end of the archive) lists everything.  Every fork is
// at least RLE-coded, so each decodes through, as far as reads reach.

typedef struct {
    cp_archive_t ar; // one cp_entry_t per peel entry, same order
} cpt_priv_t;

static bool cpt_detect_probe(const peel_probe_t *p) {
    return cpt_detect(p->head, p->head_len);
}

static int cpt_open(peel_archive_t *a, const peel_probe_t *p, peel_err_t **err) {
    const uint8_t *src = p->head;
    if (p->head_len < 8) {
        *err = make_err("CPT: input too short (%llu bytes)", (unsigned long long)p->size);
        return -1;
    }
    if (src[0] != CP_MAGIC || src[1] != CP_VOLUME_SINGLE) {
        *err = make_err("CPT: bad magic (0x%02X 0x%02X)", src[0], src[1]);
        return -1;
    }
    // cpt.md § 3.1 "Initial Archive Header" — directory offset at bytes 4..7
    uint32_t dir_off = rd32be(src + 4);
    if (dir_off < 8 || dir_off > 0x10000000 || (uint64_t)dir_off >= p->size) {
        *err = make_err("CPT: directory offset out of range (%u)", dir_off);
        return -1;
    }
    cpt_priv_t *priv = calloc(1, sizeof(*priv));
    if (!priv) {
        *err = make_err("CPT: out of memory");
        return -1;
    }
    a->priv = priv;
    // The directory runs to the end of the archive.
    uint64_t dir_len = p->size - dir_off;
    uint8_t *dir = peel_archive_read(a, dir_off, dir_len, "CPT directory", err);
    if (!dir)
        return -1;
    int rc = cp_parse_directory(&priv->ar, dir, (size_t)dir_len, 0);
    free(dir);
    if (rc < 0) {
        *err = make_err("CPT: failed to parse directory");
        return -1;
    }
    for (size_t i = 0; i < priv->ar.count; i++) {
        const cp_entry_t *ce = &priv->ar.entries[i];
        peel_entry_t *e = peel_archive_add(a);
        if (!e) {
            *err = make_err("CPT: out of memory");
            return -1;
        }
        snprintf(e->path, sizeof(e->path), "%s", ce->name);
        e->is_dir = ce->is_dir;
        if (ce->is_dir)
            continue;
        e->mac_type = ce->type;
        e->mac_creator = ce->creator;
        e->finder_flags = ce->finder_flags;
        e->rsrc_len = ce->rsrc_uncomp;
        e->data_len = ce->data_uncomp;
        e->rsrc_packed = ce->rsrc_comp;
        e->data_packed = ce->data_comp;
        e->rsrc_off = ce->file_offset;
        e->data_off = (uint64_t)ce->file_offset + ce->rsrc_comp;
        e->rsrc_method = (ce->flags & CP_FLAG_RSRC_LZH) ? 1 : 0;
        e->data_method = (ce->flags & CP_FLAG_DATA_LZH) ? 1 : 0;
        // Every fork is at least RLE-coded: it decodes through, the sink
        // keeping what has been decoded.
        e->rsrc_tier = e->data_tier = PEEL_TIER_EARNED;
    }
    return 0;
}

// The checks peel_cpt always made, in its words, then a decoder for the
// fork.  NULL + *err.
static codec_dec_t *cpt_decoder(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    cpt_priv_t *priv = a->priv;
    const cp_entry_t *e = &priv->ar.entries[i];
    uint64_t len = peel_source_size(a->src);
    // Check for encrypted files (cpt.md § 3.2.3 — flag bit 0)
    if (e->flags & CP_FLAG_ENCRYPT) {
        *err = make_err("CPT: file '%s' is encrypted (unsupported)", e->name);
        return NULL;
    }
    // cpt.md § 3.4 "Fork Data Layout" — resource fork at file_offset,
    // data fork at file_offset + rsrc_comp, each checked wrap-safe.
    uint64_t rsrc_offset = e->file_offset;
    if (rsrc_offset > len || e->rsrc_comp > len - rsrc_offset) {
        *err = make_err("CPT: resource fork of '%s' extends past archive", e->name);
        return NULL;
    }
    uint64_t data_offset = rsrc_offset + e->rsrc_comp;
    if (e->data_comp > len - data_offset) {
        *err = make_err("CPT: data fork of '%s' extends past archive", e->name);
        return NULL;
    }
    bool rsrc = fork == PEEL_FORK_RSRC;
    uint32_t uncomp = rsrc ? e->rsrc_uncomp : e->data_uncomp;
    if (uncomp > PEEL_MAX_FORK) {
        *err = make_err("CPT: fork declares %u bytes, over the %u MiB limit", uncomp, (unsigned)(PEEL_MAX_FORK >> 20));
        return NULL;
    }
    // RLE over the packed range, or RLE over LZH over it.  The fork must
    // decode to exactly its declared length (its CRC is never verified):
    // one that runs dry early is refused.
    uint64_t off = rsrc ? rsrc_offset : data_offset, comp = rsrc ? e->rsrc_comp : e->data_comp;
    bool lzh = (e->flags & (rsrc ? CP_FLAG_RSRC_LZH : CP_FLAG_DATA_LZH)) != 0;
    codec_dec_opts_t rle = {.out_len = uncomp};
    if (!lzh)
        return peel_fork_decoder(a->src, off, comp, CODEC_CPT_RLE, &rle, "CPT", err);
    codec_dec_opts_t lz = {.out_len = CODEC_LEN_UNKNOWN};
    codec_dec_t *inner = peel_fork_decoder(a->src, off, comp, CODEC_CPT_LZH, &lz, "CPT", err);
    if (!inner)
        return NULL;
    int rc = 0;
    codec_dec_t *d = codec_dec_stack(inner, CODEC_CPT_RLE, &rle, &rc);
    if (!d)
        *err = make_err("CPT: %s", codec_strerror(rc));
    return d;
}

// Decode one fork whole: the same decoder, drained.
static peel_buf_t cpt_decode_fork(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    cpt_priv_t *priv = a->priv;
    const cp_entry_t *e = &priv->ar.entries[i];
    codec_dec_t *d = cpt_decoder(a, i, fork, err);
    if (!d)
        return (peel_buf_t){0};
    return peel_decoder_drain(d, fork == PEEL_FORK_RSRC ? e->rsrc_uncomp : e->data_uncomp, "CPT", err);
}

static void cpt_close(peel_archive_t *a) {
    cpt_priv_t *priv = a->priv;
    if (!priv)
        return;
    free(priv->ar.entries);
    free(priv);
}

const peel_fmt_t peel_fmt_cpt = {
    .desc = {"cpt", false, cpt_detect_probe},
    .open = cpt_open,
    .decode = cpt_decode_fork,
    .decoder = cpt_decoder,
    .close = cpt_close,
};
