// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sit.c — StuffIt (.sit) classic and SIT5 archive peeler.
//
// Format spec: docs/sit.md
//
// StuffIt archives come in two structurally incompatible layouts that share
// the .sit extension:
//   - Classic (versions 1.x–4.x): sequential 112-byte entry headers.
//   - SIT5 (version 5.x): linked-list entry headers with 80-byte ASCII magic.
//
// Both layouts are parsed by this file and exposed through a single
// peel_sit() entry point that returns a peel_file_list_t.
//
// Supported compression methods (sit.md § 6 "Compression Methods"), each a
// codec of the codec library (src/codec, codec.h):
//   0  = raw copy                    CODEC_COPY        (sit.md § 7)
//   1  = RLE90 escape-based RLE      CODEC_RLE90       (sit.md § 8)
//   2  = LZW (14-bit max, LE bits)   CODEC_SIT_LZW     (sit.md § 9)
//   3  = static Huffman              CODEC_SIT_HUFFMAN (sit3.md)
//   13 = LZSS+Huffman                CODEC_SIT13       (sit.md § 10, sit13.md)
//   15 = Arsenic/BWT                 CODEC_SIT_ARSENIC (sit.md § 11, sit15.md)

// Ensure strnlen is declared under strict C99 mode.
#define _POSIX_C_SOURCE 200809L

#include "internal.h"

// ============================================================================
// Constants and Macros
// ============================================================================

// sit.md § 4.2 "Main Archive Header" — the classic archive header is 22 bytes.
#define SIT_CLASSIC_HDR_SIZE 22

// sit.md § 4.3 "File / Folder Header" — each entry header is 112 bytes.
#define SIT_ENTRY_HDR_SIZE 112

// sit.md § 5.2 "Top Header" — minimum SIT5 archive size is 100 bytes.
#define SIT5_MIN_SIZE 100

// sit.md § 4.4 "Method Byte Encoding" — folder start/end markers.
#define SIT_FOLDER_START 0x20
#define SIT_FOLDER_END   0x21

// sit.md § 4.7 "Classic Iteration Rules" — max folder nesting depth.
#define SIT_MAX_DEPTH 10

// sit.md § 5.3 "Entry Header" — SIT5 entry magic value.
#define SIT5_ENTRY_MAGIC 0xA5A5A5A5

// Number of known classic SIT signatures.
#define SIT_NUM_SIGS 9

// Maximum number of files from a single archive (safety limit).
#define SIT_MAX_FILES 65536

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Per-fork metadata unpacked from an entry header.
// sit.md § 4.3 "File / Folder Header"
typedef struct {
    uint32_t raw_len; // Uncompressed length
    uint32_t packed_len; // Compressed length
    uint16_t crc; // CRC-16 from header
    uint8_t method; // Compression method ID (low nibble)
    uint64_t off; // Offset of the compressed bytes in the source
    const uint8_t *data; // Those bytes in memory, or NULL: read from the source
} sit_fork_info_t;

// A single parsed file entry (metadata + fork info + path).
typedef struct {
    char name[512]; // Full path (folders prepended)
    uint32_t mac_type; // Mac file type
    uint32_t mac_creator; // Mac creator code
    uint16_t finder_flags; // Finder flags
    sit_fork_info_t data_fork; // Data fork info
    sit_fork_info_t rsrc_fork; // Resource fork info
    bool has_rsrc; // Resource fork present
    bool is_dir; // A folder (no forks)
} sit_entry_t;

// Growable list of parsed file entries.
typedef struct {
    sit_entry_t *items;
    int count;
    int cap;
} sit_entry_list_t;

// SIT5 directory map entry for path construction.
// sit.md § 5.7 "Iteration Rules"
typedef struct {
    uint32_t offset; // Byte offset of the folder header
    char path[512]; // Reconstructed full path
} sit5_dir_entry_t;

// Growable SIT5 directory map: every folder seen so far, so a child's parent
// offset always resolves (a fixed table dropped folders past its size, and
// their files surfaced at the top level).
typedef struct {
    sit5_dir_entry_t *items;
    int count;
    int cap;
} sit5_dir_map_t;

// ============================================================================
// Static Helpers — Entry List
// ============================================================================

// Initialize an empty entry list.
static void entry_list_init(sit_entry_list_t *list) {
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

// Append one entry, growing the backing array if needed.
static sit_entry_t *entry_list_push(sit_entry_list_t *list, peel_err_t **err) {
    if (list->count >= SIT_MAX_FILES) {
        *err = make_err("SIT: too many files in archive (limit %d)", SIT_MAX_FILES);
        return NULL;
    }
    if (list->count == list->cap) {
        // Double the capacity (start at 16)
        int new_cap = list->cap ? list->cap * 2 : 16;
        sit_entry_t *tmp = realloc(list->items, (size_t)new_cap * sizeof(sit_entry_t));
        if (!tmp) {
            *err = make_err("SIT: out of memory growing entry list");
            return NULL;
        }
        list->items = tmp;
        list->cap = new_cap;
    }
    sit_entry_t *e = &list->items[list->count++];
    memset(e, 0, sizeof(*e));
    return e;
}

// Release the entry list backing storage.
static void entry_list_free(sit_entry_list_t *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

// ============================================================================
// Static Helpers — Path Construction
// ============================================================================

// Both StuffIt layouts store a resource fork and then a data fork end to end,
// starting at archive offset `off` (sit.md § 4.5, § 5.5).  Check both extents
// as offsets, wrap-safe, and return where the data fork starts.
//
// This used to form `data_ptr = rsrc_ptr + rclen` from an unvalidated 32-bit
// length and then test `(data_ptr - blob) + dlen > blob_len`.  On a 64-bit
// host a huge resource length pushes that far past the end and the test
// rejects it -- by accident.  On wasm32 the pointer wraps back inside the
// buffer, the test passes, and the resource fork is decoded with a ~4 GiB
// length: a dropped .sit trapped the shipping build with "memory access out
// of bounds".  The resource fork's own extent was never
// checked at all; only its end, as the data fork's start.
static bool sit_forks_fit(uint64_t off, uint32_t rsrc_len, uint32_t data_len, uint64_t total, uint64_t *data_off) {
    if (off > total || rsrc_len > total - off)
        return false;
    uint64_t d = off + rsrc_len; // inside the source, so no wrap
    if (data_len > total - d)
        return false;
    *data_off = d;
    return true;
}

// Build "dir/name" into dst: `dir` is a path this parser already built (so
// already safe), `name` one raw Mac name, appended as a sanitised component
// (peel_append_segment).  Either part may be empty.
// sit.md § 5.7 "Iteration Rules" — paths are built by resolving parent_offset.
static void build_path(char *dst, size_t cap, const char *dir, const char *name) {
    if (!cap)
        return;
    size_t p = 0;
    dst[0] = '\0';
    if (dir && dir[0]) {
        p = strnlen(dir, cap - 1);
        memcpy(dst, dir, p);
        dst[p] = '\0';
    }
    peel_append_segment(dst, cap, &p, (const uint8_t *)(name ? name : ""), name ? strlen(name) : 0);
}

// ============================================================================
// Static Helpers — Fork Decompression (a producer per fork)
// ============================================================================

// A fork's decode as a stream: the caller pulls its bytes a buffer at a
// time and the method's decoder stops wherever the buffer fills, so a
// decode-through source fills its sink only as far as reads reach.  The
// packed bytes are read on the first run(), not when the fork is opened.
// The fork CRC is checked over what was produced, once all of it has been
// (method 15 checks its own stream, and has no fork CRC).
typedef struct {
    peel_producer_t base;
    sit_fork_info_t fi;
    peel_source_t *src; // the archive, retained; NULL when fi.data is given
    uint8_t *packed; // owned, once read
    const uint8_t *data; // the packed bytes
    peel_producer_t *inner; // the method's decoder (peel_codec_producer)
    uint32_t produced;
    uint16_t crc;
    bool started, failed;
} sit_prod_t;

// Fail the stream with a message.
static int sit_prod_fail(sit_prod_t *p, const char *msg) {
    snprintf(p->base.err, sizeof(p->base.err), "%s", msg);
    p->failed = true;
    return -5;
}

// The codec for a method, or -1 (sit.md § 12 "Unsupported Methods").
static int sit_method_codec(int method) {
    switch (method) {
    case 0:
        return CODEC_COPY;
    case 1:
        return CODEC_RLE90; // sit.md § 8.2 "State" -- last_byte starts at 0
    case 2:
        return CODEC_SIT_LZW;
    case 3:
        return CODEC_SIT_HUFFMAN;
    case 13:
        return CODEC_SIT13;
    case 15:
        return CODEC_SIT_ARSENIC;
    default:
        return -1;
    }
}

// First run: read the packed bytes and set the method's decoder up.
static int sit_prod_start(sit_prod_t *p) {
    p->started = true;
    int id = sit_method_codec(p->fi.method);
    if (id < 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "SIT: unsupported compression method %d", p->fi.method);
        return sit_prod_fail(p, msg);
    }
    size_t len = p->fi.packed_len;
    if (id == CODEC_COPY) {
        // sit.md § 7 "Method 0: None" -- a copy (a view, when it is whole).
        if (p->fi.packed_len < p->fi.raw_len) {
            char msg[96];
            snprintf(msg, sizeof(msg), "SIT: method 0 packed (%u) < raw (%u)", p->fi.packed_len, p->fi.raw_len);
            return sit_prod_fail(p, msg);
        }
        len = p->fi.raw_len;
    }
    if (p->src) {
        if (p->fi.packed_len > PEEL_MAX_INPUT)
            return sit_prod_fail(p, "SIT: fork data over the input limit");
        p->packed = malloc(p->fi.packed_len ? p->fi.packed_len : 1);
        if (!p->packed)
            return sit_prod_fail(p, "SIT: out of memory reading fork data");
        if (p->fi.packed_len && peel_source_read_exact(p->src, p->fi.off, p->packed, p->fi.packed_len) != 0)
            return sit_prod_fail(p, "SIT: cannot read fork data");
        p->data = p->packed;
    }
    peel_err_t *err = NULL;
    codec_id_t stage = (codec_id_t)id;
    p->inner = peel_codec_producer(&stage, 1, 0, p->data, len, p->fi.raw_len, true, "SIT", &err);
    if (!p->inner) {
        int rc = sit_prod_fail(p, peel_err_msg(err));
        peel_err_free(err);
        return rc;
    }
    return 0;
}

static int sit_prod_run(peel_producer_t *pp, uint8_t *out, size_t cap, size_t *n) {
    sit_prod_t *p = (sit_prod_t *)pp;
    *n = 0;
    if (p->failed)
        return -5;
    if (!p->started && sit_prod_start(p) != 0)
        return -5;
    size_t want = p->fi.raw_len - p->produced < cap ? p->fi.raw_len - p->produced : cap;
    size_t got = 0;
    int rc = want ? p->inner->run(p->inner, out, want, &got) : 1;
    if (rc < 0)
        return sit_prod_fail(p, p->inner->err);
    if (rc == 1 && p->produced + got < p->fi.raw_len)
        return sit_prod_fail(p, "SIT: fork data ends before its declared length");
    p->crc = codec_crc16_arc(p->crc, out, got);
    p->produced += (uint32_t)got;
    *n = got;
    if (p->produced < p->fi.raw_len)
        return 0;
    // sit.md § 6.3 "CRC Verification Rule" -- over the decompressed fork;
    // method 15 checks its own stream instead.
    if (p->fi.method != 15 && p->crc != p->fi.crc) {
        char msg[96];
        snprintf(msg, sizeof(msg), "SIT: fork CRC mismatch (expected 0x%04X, got 0x%04X)", p->fi.crc, p->crc);
        return sit_prod_fail(p, msg);
    }
    return 1;
}

static void sit_prod_free(peel_producer_t *pp) {
    sit_prod_t *p = (sit_prod_t *)pp;
    if (p->inner)
        p->inner->free(p->inner);
    free(p->packed);
    peel_source_release(p->src);
    free(p);
}

// A producer for a fork: of `src` at fi->off (retained), or of fi->data in
// memory when `src` is NULL.  NULL + *err when the fork's size is refused.
static peel_producer_t *sit_fork_producer(const sit_fork_info_t *fi, peel_source_t *src, peel_err_t **err) {
    // Bound what the header declares before anything is read or allocated.
    if (fi->raw_len > PEEL_MAX_FORK) {
        *err = make_err("SIT: fork declares %u bytes, over the %u MiB limit", fi->raw_len,
                        (unsigned)(PEEL_MAX_FORK >> 20));
        return NULL;
    }
    sit_prod_t *p = calloc(1, sizeof(*p));
    if (!p) {
        *err = make_err("SIT: out of memory");
        return NULL;
    }
    p->base.run = sit_prod_run;
    p->base.free = sit_prod_free;
    p->fi = *fi;
    p->src = peel_source_retain(src);
    p->data = fi->data;
    return &p->base;
}

// ============================================================================
// Static Helpers — Classic Archive Parsing
// ============================================================================

// The 9 recognized 4-byte signatures for classic StuffIt.
// sit.md § 4.1 "Identification"
static const char *classic_sigs[SIT_NUM_SIGS] = {"SIT!", "ST46", "ST50", "ST60", "ST65",
                                                 "STin", "STi2", "STi3", "STi4"};

// Scan the input for the classic SIT magic (any of 9 signatures + "rLau").
// Returns the byte offset of the archive start, or -1 if not found.
// sit.md § 4.1 "Identification" and § 14.2 "Embedded Archive Detection"
static int64_t find_classic_magic(const uint8_t *src, size_t len) {
    if (len < SIT_CLASSIC_HDR_SIZE)
        return -1;
    size_t limit = len - 14;
    for (size_t off = 0; off <= limit; ++off) {
        // Check for "rLau" at offset 10–13 first (fast rejection)
        if (memcmp(src + off + 10, "rLau", 4) != 0)
            continue;
        // Check for any of the 9 known signatures at offset 0–3
        for (int s = 0; s < SIT_NUM_SIGS; ++s) {
            if (memcmp(src + off, classic_sigs[s], 4) == 0)
                return (int64_t)off;
        }
    }
    return -1;
}

// Parse all file entries from a classic StuffIt archive, reading only the
// headers through `rd`: each header states its forks' packed lengths, so
// the next header's offset is computable without touching fork data.
// sit.md § 4.7 "Classic Iteration Rules" and Appendix B
static bool parse_classic(peel_reader_t *rd, uint64_t archive_off, sit_entry_list_t *entries, peel_err_t **err) {
    uint64_t blob_len = rd->size;
    const uint8_t *top = peel_reader_at(rd, archive_off, SIT_CLASSIC_HDR_SIZE);
    if (!top) {
        *err = make_err("SIT classic: archive too small");
        return false;
    }

    // sit.md § 4.2 "Main Archive Header" — file_count at offset 4
    uint16_t file_count = rd16be(top + 4);
    uint64_t cursor = SIT_CLASSIC_HDR_SIZE;
    uint32_t done = 0;

    // sit.md § 4.7 — folder stack of up to 10 nesting levels
    char dirs[SIT_MAX_DEPTH][64];
    int depth = 0;

    while (done < file_count) {
        const uint8_t *hdr = peel_reader_at(rd, archive_off + cursor, SIT_ENTRY_HDR_SIZE);
        // The header's entry count is not trusted to the end (other readers
        // walk to the archive's total size instead): an archive that ends
        // before it lists the entries it holds, not nothing.
        if (!hdr)
            break;

        uint8_t rm = hdr[0];
        uint8_t dm = hdr[1];

        // sit.md § 4.4 — folder start marker (0x20)
        // sit.md § 4.7 — file_count counts top-level entries only; a folder
        // (open through close) is one top-level entry regardless of contents.
        // The structural markers themselves do not increment `done`; the
        // outermost folder-end does (when depth pops back to 0).
        if (rm == SIT_FOLDER_START || dm == SIT_FOLDER_START) {
            // Clamp, never skip: skipping the copy for a name of 64 bytes or
            // more left dirs[depth] as uninitialised stack, which the path
            // builder then read with strlen.
            uint8_t nlen = hdr[2];
            if (nlen > 63)
                nlen = 63;
            if (depth < SIT_MAX_DEPTH) {
                memcpy(dirs[depth], hdr + 3, nlen);
                dirs[depth][nlen] = '\0';
                // The folder itself is an entry of the listing.
                char path[512] = "";
                size_t p = 0;
                for (int d = 0; d <= depth; d++)
                    peel_append_segment(path, sizeof(path), &p, (const uint8_t *)dirs[d], strlen(dirs[d]));
                sit_entry_t *dent = entry_list_push(entries, err);
                if (!dent)
                    return false;
                memset(dent, 0, sizeof(*dent));
                snprintf(dent->name, sizeof(dent->name), "%s", path);
                dent->is_dir = true;
            }
            if (depth < SIT_MAX_DEPTH)
                depth++;
            cursor += SIT_ENTRY_HDR_SIZE;
            continue;
        }

        // sit.md § 4.4 — folder end marker (0x21)
        if (rm == SIT_FOLDER_END || dm == SIT_FOLDER_END) {
            if (depth > 0)
                depth--;
            cursor += SIT_ENTRY_HDR_SIZE;
            if (depth == 0)
                done++;
            continue;
        }

        // sit.md § 4.4 — skip entries with unknown high bits
        if ((rm & 0xE0) || (dm & 0xE0)) {
            cursor += SIT_ENTRY_HDR_SIZE;
            if (depth == 0)
                done++;
            continue;
        }

        // ---- Regular file entry ----
        // sit.md § 4.3 "File / Folder Header (Fixed 112 Bytes)"
        uint8_t nlen = hdr[2];
        char fname[64];
        if (nlen >= sizeof(fname))
            nlen = (uint8_t)(sizeof(fname) - 1);
        memcpy(fname, hdr + 3, nlen);
        fname[nlen] = '\0';

        // Build full path from folder stack, each name a sanitised component
        char path[512] = "";
        size_t p = 0;
        for (int d = 0; d < depth && d < SIT_MAX_DEPTH; d++)
            peel_append_segment(path, sizeof(path), &p, (const uint8_t *)dirs[d], strlen(dirs[d]));
        peel_append_segment(path, sizeof(path), &p, (const uint8_t *)fname, strlen(fname));

        // sit.md § 4.3 — type at 66, creator at 70, finder flags at 74
        uint32_t ftype = rd32be(hdr + 66);
        uint32_t fcreator = rd32be(hdr + 70);
        uint16_t fflags = rd16be(hdr + 74);

        // sit.md § 4.3 — fork lengths and CRCs
        uint32_t rulen = rd32be(hdr + 84);
        uint32_t dulen = rd32be(hdr + 88);
        uint32_t rclen = rd32be(hdr + 92);
        uint32_t dclen = rd32be(hdr + 96);
        uint16_t rcrc = rd16be(hdr + 100);
        uint16_t dcrc = rd16be(hdr + 102);

        // sit.md § 4.5 "Fork Data Layout" — rsrc first, then data
        uint64_t rsrc_off = archive_off + cursor + SIT_ENTRY_HDR_SIZE;
        uint64_t data_off;
        if (!sit_forks_fit(rsrc_off, rclen, dclen, blob_len, &data_off)) {
            *err = make_err("SIT classic: fork data extends past archive end");
            return false;
        }

        // Add entry to the list
        sit_entry_t *ent = entry_list_push(entries, err);
        if (!ent)
            return false;

        snprintf(ent->name, sizeof(ent->name), "%s", path);
        ent->mac_type = ftype;
        ent->mac_creator = fcreator;
        ent->finder_flags = fflags;
        ent->is_dir = false;
        ent->data_fork = (sit_fork_info_t){
            .raw_len = dulen, .packed_len = dclen, .crc = dcrc, .method = (uint8_t)(dm & 0x0F), .off = data_off};
        ent->rsrc_fork = (sit_fork_info_t){
            .raw_len = rulen, .packed_len = rclen, .crc = rcrc, .method = (uint8_t)(rm & 0x0F), .off = rsrc_off};
        ent->has_rsrc = (rulen > 0);

        // Advance past both fork data regions
        cursor = data_off - archive_off + dclen;
        if (depth == 0)
            done++;
    }

    return true;
}

// ============================================================================
// Static Helpers — SIT5 Archive Parsing
// ============================================================================

// Scan the input for the SIT5 magic string.
// Returns the byte offset of the archive start, or -1 if not found.
// sit.md § 5.1 "Identification"
static int64_t find_sit5_magic(const uint8_t *src, size_t len) {
    if (len < 80)
        return -1;
    size_t limit = len - 80;
    for (size_t off = 0; off <= limit; ++off) {
        // sit.md § 5.1 — check two validated substrings; bytes 16–19 (year)
        // and bytes 78–79 (CR LF) are NOT validated.
        if (memcmp(src + off, "StuffIt (c)1997-", 16) == 0 && memcmp(src + off + 20,
                                                                     " Aladdin Systems, Inc., "
                                                                     "http://www.aladdinsys.com/StuffIt/",
                                                                     58) == 0)
            return (int64_t)off;
    }
    return -1;
}

// Record one SIT5 folder in the directory map, growing it as needed.  Folders
// are listing entries too, so SIT_MAX_FILES bounds the map as it does them.
static bool dir_map_push(sit5_dir_map_t *m, uint32_t offset, const char *path, peel_err_t **err) {
    if (m->count == m->cap) {
        int new_cap = m->cap ? m->cap * 2 : 16;
        sit5_dir_entry_t *tmp = realloc(m->items, (size_t)new_cap * sizeof(sit5_dir_entry_t));
        if (!tmp) {
            *err = make_err("SIT5: out of memory growing the folder map");
            return false;
        }
        m->items = tmp;
        m->cap = new_cap;
    }
    m->items[m->count].offset = offset;
    snprintf(m->items[m->count].path, sizeof(m->items[m->count].path), "%s", path);
    m->count++;
    return true;
}

// Parse all file entries from a SIT5 archive through `rd`: a linked list of
// headers with explicit offsets, each read on its own.  `dmap` collects the
// folders for path resolution; the caller frees it.
// sit.md § 5.7 "Iteration Rules" and Appendix C
static bool parse_sit5_walk(peel_reader_t *rd, uint64_t archive_off, sit_entry_list_t *entries, sit5_dir_map_t *dmap,
                            peel_err_t **err) {
    uint64_t blob_len = rd->size;
    uint64_t avail = blob_len - archive_off;

    const uint8_t *top = peel_reader_at(rd, archive_off, SIT5_MIN_SIZE);
    if (!top) {
        *err = make_err("SIT5: archive too small (%llu bytes)", (unsigned long long)avail);
        return false;
    }

    // sit.md § 5.2 "Top Header" — entry count at offset 92, cursor at 94
    uint16_t entry_count = rd16be(top + 92);
    uint32_t cursor = rd32be(top + 94);
    uint32_t remaining = entry_count;

    while (remaining > 0 && cursor != 0 && (uint64_t)cursor + 48 <= avail) {
        const uint8_t *h1 = peel_reader_at(rd, archive_off + cursor, 48);
        if (!h1) {
            *err = make_err("SIT5: cannot read entry header at offset %u", cursor);
            return false;
        }

        // sit.md § 5.3 "Entry Header" — validate entry magic
        if (rd32be(h1) != SIT5_ENTRY_MAGIC) {
            *err = make_err("SIT5: invalid entry magic at offset %u", cursor);
            return false;
        }

        // sit.md § 5.3 — only version 1 is supported
        if (h1[4] != 1) {
            *err = make_err("SIT5: unsupported entry version %d", h1[4]);
            return false;
        }

        uint16_t h1_len = rd16be(h1 + 6);
        uint16_t namelen = rd16be(h1 + 30);
        if ((uint64_t)cursor + h1_len > avail) {
            *err = make_err("SIT5: header1 extends past archive end");
            return false;
        }
        // sit.md § 5.3 — header 1 is 48 fixed bytes, then the name, then an
        // optional comment, and h1_len is its whole extent.  Anything shorter
        // is not a header.  Unchecked, the CRC step below zeroed bytes 32–33
        // of a malloc(h1_len) -- a heap write past the end for h1_len < 34 --
        // and a skip marker with h1_len == 0 left the cursor where it was,
        // forever.  With this bound in place the name read below cannot leave
        // the header either.
        if ((size_t)h1_len < 48 + (size_t)namelen) {
            *err = make_err("SIT5: header1 length %u is shorter than its fixed fields and name", (unsigned)h1_len);
            return false;
        }
        // The whole of header 1 (name and comment included).
        h1 = peel_reader_at(rd, archive_off + cursor, h1_len);
        if (!h1) {
            *err = make_err("SIT5: header1 extends past archive end");
            return false;
        }

        // sit.md § 3.5 "Where CRCs Are Used" — verify header 1 CRC
        // (bytes 32–33 zeroed before computation)
        {
            uint8_t *tmp = malloc(h1_len);
            if (!tmp) {
                *err = make_err("SIT5: out of memory for header CRC");
                return false;
            }
            memcpy(tmp, h1, h1_len);
            tmp[32] = tmp[33] = 0;
            uint16_t computed = codec_crc16_arc(0, tmp, h1_len);
            uint16_t stored = rd16be(h1 + 32);
            free(tmp);
            if (computed != stored) {
                *err = make_err("SIT5: header CRC mismatch at offset %u", cursor);
                return false;
            }
        }

        uint32_t h2_off = cursor + h1_len;
        uint8_t flags = h1[9];
        uint32_t parent_off = rd32be(h1 + 26);
        uint32_t d_raw_len = rd32be(h1 + 34);
        uint32_t d_packed_len = rd32be(h1 + 38);
        uint16_t d_crc = rd16be(h1 + 42);
        uint16_t child_count = rd16be(h1 + 46);
        uint8_t d_algo = h1[46];
        uint8_t d_passlen = h1[47];
        uint8_t h1_version = h1[4];

        // Read entry name (starts at byte 48 of header 1)
        char namebuf[256];
        {
            size_t cl = namelen; // within header 1: h1_len >= 48 + namelen, above
            if (cl > sizeof(namebuf) - 1)
                cl = sizeof(namebuf) - 1;
            memcpy(namebuf, h1 + 48, cl);
            namebuf[cl] = '\0';
        }

        // sit.md § 5.6 "Special Markers" — 0xFFFFFFFF entries are skipped
        // before header 2 parsing, since header 2 may not exist.
        if (d_raw_len == 0xFFFFFFFF) {
            // sit.md § 5.6 — folder markers: increment remaining by 1
            if (flags & 0x40)
                remaining++;
            cursor = h2_off;
            continue;
        }

        // Parse header 2
        // sit.md § 5.4 "Secondary Header (Header 2)"
        const uint8_t *h2 = (uint64_t)h2_off + 32 <= avail ? peel_reader_at(rd, archive_off + h2_off, 32) : NULL;
        if (!h2) {
            *err = make_err("SIT5: header2 extends past archive end");
            return false;
        }
        uint16_t flags2 = rd16be(h2 + 0);
        uint32_t ftype = rd32be(h2 + 4);
        uint32_t fcreator = rd32be(h2 + 8);
        uint16_t fflags = rd16be(h2 + 12);

        // sit.md § 5.4 — version-dependent skip past header 2 prefix
        uint32_t skip_extra = (h1_version == 1) ? 22 : 18;
        bool rsrc_present = (flags2 & 0x01) != 0;
        // Offsets into the source: everything past header 2 is located by
        // lengths the archive supplies (see sit_forks_fit).
        uint64_t after_off = archive_off + h2_off + 14 + skip_extra;
        uint64_t payload_off = after_off;

        // sit.md § 5.4 — resource fork fields (conditional)
        uint32_t r_raw_len = 0, r_packed_len = 0;
        uint16_t r_crc = 0;
        uint8_t r_algo = 0;
        if (rsrc_present) {
            const uint8_t *ri = after_off + 14 <= blob_len ? peel_reader_at(rd, after_off, 14) : NULL;
            if (!ri) {
                *err = make_err("SIT5: resource info past archive end");
                return false;
            }
            r_raw_len = rd32be(ri + 0);
            r_packed_len = rd32be(ri + 4);
            r_crc = rd16be(ri + 8);
            r_algo = ri[12];
            payload_off = after_off + 14 + ri[13]; // + the password blob
        }

        // Build the parent path from the directory map
        char ppath[512] = "";
        if (parent_off != 0) {
            for (int i = 0; i < dmap->count; ++i) {
                if (dmap->items[i].offset == parent_off) {
                    memcpy(ppath, dmap->items[i].path, sizeof(ppath)); // same size, NUL-terminated
                    break;
                }
            }
        }

        // sit.md § 5.3 — folder entries (flags bit 6)
        if (flags & 0x40) {
            // Record folder in directory map
            char folder_full[512];
            build_path(folder_full, sizeof(folder_full), ppath, namebuf);
            if (!dir_map_push(dmap, cursor, folder_full, err))
                return false;
            // The folder itself is an entry of the listing.
            sit_entry_t *dent = entry_list_push(entries, err);
            if (!dent)
                return false;
            memset(dent, 0, sizeof(*dent));
            snprintf(dent->name, sizeof(dent->name), "%s", folder_full);
            dent->is_dir = true;

            // sit.md § 5.7 — add child count, advance into children
            remaining += child_count;
            cursor = (uint32_t)(payload_off - archive_off);
            continue;
        }

        // ---- Regular file entry ----
        // sit.md § 5.3 — data method at byte 46, password at byte 47

        // sit.md § 13.2 "Decompression Errors" — reject encrypted entries
        if ((flags & 0x20) && d_raw_len && d_passlen) {
            *err = make_err("SIT5: encrypted entries are not supported");
            return false;
        }

        char full_name[512];
        build_path(full_name, sizeof(full_name), ppath, namebuf);

        // sit.md § 5.5 "Fork Data Layout" — resource fork first, then data
        uint64_t d_off;
        if (!sit_forks_fit(payload_off, rsrc_present ? r_packed_len : 0, d_packed_len, blob_len, &d_off)) {
            *err = make_err("SIT5: fork data extends past archive end");
            return false;
        }

        // Add entry to the list
        sit_entry_t *ent = entry_list_push(entries, err);
        if (!ent)
            return false;

        snprintf(ent->name, sizeof(ent->name), "%s", full_name);
        ent->mac_type = ftype;
        ent->mac_creator = fcreator;
        ent->finder_flags = fflags;
        ent->is_dir = false;

        ent->data_fork = (sit_fork_info_t){.raw_len = d_raw_len,
                                           .packed_len = d_packed_len,
                                           .crc = d_crc,
                                           .method = (uint8_t)(d_algo & 0x0F),
                                           .off = d_off};
        ent->has_rsrc = rsrc_present && r_raw_len > 0;
        if (ent->has_rsrc) {
            ent->rsrc_fork = (sit_fork_info_t){.raw_len = r_raw_len,
                                               .packed_len = r_packed_len,
                                               .crc = r_crc,
                                               .method = (uint8_t)(r_algo & 0x0F),
                                               .off = payload_off};
        } else {
            memset(&ent->rsrc_fork, 0, sizeof(ent->rsrc_fork));
        }

        // Advance cursor past the fork data
        cursor = (uint32_t)(d_off - archive_off + d_packed_len);
        remaining--;
    }

    return true;
}

// Parse a SIT5 archive: parse_sit5_walk with a directory map it owns.
static bool parse_sit5(peel_reader_t *rd, uint64_t archive_off, sit_entry_list_t *entries, peel_err_t **err) {
    sit5_dir_map_t dmap = {0};
    bool ok = parse_sit5_walk(rd, archive_off, entries, &dmap, err);
    free(dmap.items);
    return ok;
}

// ============================================================================
// Operations (Public API) — Detection
// ============================================================================

// sit.md § 2.3 "Detection Strategy" — check for classic or SIT5 magic.
bool sit_detect(const uint8_t *src, size_t len) {
    if (find_classic_magic(src, len) >= 0)
        return true;
    if (find_sit5_magic(src, len) >= 0)
        return true;
    return false;
}

// ============================================================================
// Structure-first access (peeler.h: peel_open)
// ============================================================================
//
// The archive is found in the probe's head (an embedded archive further in
// than the detection budget is not found: partial access cannot scan a
// whole file for it), then walked header by header.  Method-0 forks are
// views of the source; everything else decodes through, a buffer at a time.

typedef struct {
    sit_entry_list_t entries; // one per peel entry, same order
} sit_priv_t;

static bool sit_detect_probe(const peel_probe_t *p) {
    return sit_detect(p->head, p->head_len);
}

// The tier and the packed range a fork reports.
static void sit_fill_fork(const sit_fork_info_t *fi, uint64_t *len, uint64_t *packed, uint64_t *off, uint8_t *method,
                          peel_tier_t *tier) {
    *len = fi->raw_len;
    *packed = fi->packed_len;
    *off = fi->off;
    *method = fi->method;
    // Method 0 is a copy: a view, as long as the packed range holds it.
    // Everything else decodes through: the sink keeps what has been decoded.
    *tier = (fi->method == 0 && fi->packed_len >= fi->raw_len) ? PEEL_TIER_RANDOM : PEEL_TIER_EARNED;
}

static int sit_open(peel_archive_t *a, const peel_probe_t *p, peel_err_t **err) {
    // sit.md § 2.3 "Detection Strategy" — prefer the earliest match.
    int64_t classic_off = find_classic_magic(p->head, p->head_len);
    int64_t sit5_off = find_sit5_magic(p->head, p->head_len);

    sit_priv_t *priv = calloc(1, sizeof(*priv));
    if (!priv) {
        *err = make_err("SIT: out of memory");
        return -1;
    }
    entry_list_init(&priv->entries);
    a->priv = priv;

    peel_reader_t rd;
    if (peel_reader_init(&rd, a->src) != 0) {
        *err = make_err("SIT: out of memory");
        return -1;
    }
    bool ok;
    if (classic_off >= 0 && (sit5_off < 0 || classic_off <= sit5_off)) {
        ok = parse_classic(&rd, (uint64_t)classic_off, &priv->entries, err);
    } else if (sit5_off >= 0) {
        ok = parse_sit5(&rd, (uint64_t)sit5_off, &priv->entries, err);
    } else {
        *err = make_err("SIT: no valid StuffIt signature found");
        ok = false;
    }
    peel_reader_free(&rd);
    if (!ok)
        return -1;

    for (int i = 0; i < priv->entries.count; i++) {
        const sit_entry_t *se = &priv->entries.items[i];
        peel_entry_t *e = peel_archive_add(a);
        if (!e) {
            *err = make_err("SIT: out of memory");
            return -1;
        }
        snprintf(e->path, sizeof(e->path), "%s", se->name);
        e->is_dir = se->is_dir;
        if (se->is_dir)
            continue;
        e->mac_type = se->mac_type;
        e->mac_creator = se->mac_creator;
        e->finder_flags = se->finder_flags;
        sit_fill_fork(&se->data_fork, &e->data_len, &e->data_packed, &e->data_off, &e->data_method, &e->data_tier);
        if (se->has_rsrc)
            sit_fill_fork(&se->rsrc_fork, &e->rsrc_len, &e->rsrc_packed, &e->rsrc_off, &e->rsrc_method, &e->rsrc_tier);
    }
    return 0;
}

// Read a fork's packed bytes and run its method over them.
static peel_buf_t sit_decode_fork(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    sit_priv_t *priv = a->priv;
    const sit_entry_t *se = &priv->entries.items[i];
    const sit_fork_info_t *fi = fork == PEEL_FORK_RSRC ? &se->rsrc_fork : &se->data_fork;
    peel_producer_t *p = sit_fork_producer(fi, a->src, err);
    return p ? peel_producer_drain(p, fi->raw_len, NULL, err) : (peel_buf_t){0};
}

static peel_producer_t *sit_producer(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    sit_priv_t *priv = a->priv;
    const sit_entry_t *se = &priv->entries.items[i];
    return sit_fork_producer(fork == PEEL_FORK_RSRC ? &se->rsrc_fork : &se->data_fork, a->src, err);
}

static void sit_close(peel_archive_t *a) {
    sit_priv_t *priv = a->priv;
    if (!priv)
        return;
    entry_list_free(&priv->entries);
    free(priv);
}

const peel_fmt_t peel_fmt_sit = {
    .desc = {"sit", false, sit_detect_probe},
    .open = sit_open,
    .decode = sit_decode_fork,
    .producer = sit_producer,
    .close = sit_close,
};
