// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// declrom.c
// Declaration-ROM builder + loader.  The builder generates a complete
// declaration-ROM image at runtime from declarative inputs — board
// identity, functional video sResources, spliced 68K code fragments —
// and stamps the Format Block CRC in C (proposal-nubus-runtime-vrom
// §3.3).  Serialisation is single-pass bottom-up: leaf records first,
// then the lists that reference them, then the directory, then the
// Format Block, so every stored offset is a backward self-relative
// reference and no patching pass exists to get wrong.

#include "declrom.h"
#include "card.h"
#include "log.h"
#include "machine_config.h" // resolved-pick reporting into the built-from record
#include "vrom.h" // content identification + the platform-fed offer registry

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("nubus");

// sResource / list-entry ids used by the serialiser (the byte contract
// in docs/core/peripherals/nubus_vrom.md; clean-room values, mirroring
// tools/vrom/gsvrom_equ.i).
enum {
    ID_SRSRC_TYPE = 1,
    ID_SRSRC_NAME = 2,
    ID_SRSRC_DRVR_DIR = 4,
    ID_SRSRC_FLAGS = 7,
    ID_SRSRC_HWDEVID = 8,
    ID_MINOR_BASE = 10,
    ID_MINOR_LENGTH = 11,
    ID_MAJOR_BASE = 12,
    ID_MAJOR_LENGTH = 13,
    ID_BOARD_ID = 32,
    ID_PRAM_INIT = 33,
    ID_PRIMARY_INIT = 34,
    ID_VENDOR_INFO = 36,
    ID_SECONDARY_INIT = 38,
    ID_VENDOR_ID = 1,
    ID_VENDOR_REVLEVEL = 3,
    ID_VENDOR_PARTNUM = 4,
    ID_MVIDPARAMS = 1,
    ID_MPAGECNT = 3,
    ID_MDEVTYPE = 4,
    ID_FIRST_VIDMODE = 128,
    ID_SMACOS68020 = 2,
    ID_END_OF_LIST = 0xFF,
};

#define DECLROM_TESTPATTERN 0x5A932BC7u
#define DECLROM_FB_SIZE     20 // trailing Format Block bytes
#define DECLROM_MAX_VIDS    8 // functional video sResources per image
#define DECLROM_MAX_MODES   8 // depth entries per video sResource

// One staged functional video sResource (deep copy of the caller's
// descriptor; serialised in declrom_finalise).
typedef struct staged_vid {
    uint8_t spid;
    char *name;
    uint16_t drhw;
    uint16_t flags;
    bool use_major;
    uint32_t base;
    uint32_t length;
    declrom_vidmode_t modes[DECLROM_MAX_MODES];
    size_t mode_count;
} staged_vid_t;

struct declrom_builder {
    uint8_t *buf;
    size_t size; // buffer capacity
    size_t used; // bytes appended; the finished image size after finalise
    bool finalised;
    bool overflow; // an append ran past `size` — finalise will fail

    // Staged board identity.
    char *board_name;
    uint16_t board_id;
    bool board_set;
    char *vendor_id, *vendor_rev, *vendor_part;
    uint8_t pram_init[6];
    bool pram_set;

    // Staged spliced fragments (deep copies).
    uint8_t *exec_frag[2]; // indexed by declrom_exec_kind_t
    size_t exec_size[2];
    uint8_t *drvr_frag;
    size_t drvr_size;

    // Staged functional video sResources.
    staged_vid_t vids[DECLROM_MAX_VIDS];
    size_t vid_count;
};

declrom_builder_t *declrom_builder_new(size_t size) {
    if (size == 0)
        return NULL;
    declrom_builder_t *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->buf = calloc(1, size);
    if (!b->buf) {
        free(b);
        return NULL;
    }
    b->size = size;
    return b;
}

void declrom_builder_free(declrom_builder_t *b) {
    if (!b)
        return;
    free(b->board_name);
    free(b->vendor_id);
    free(b->vendor_rev);
    free(b->vendor_part);
    free(b->exec_frag[0]);
    free(b->exec_frag[1]);
    free(b->drvr_frag);
    for (size_t i = 0; i < b->vid_count; i++)
        free(b->vids[i].name);
    free(b->buf);
    free(b);
}

const uint8_t *declrom_builder_bytes(const declrom_builder_t *b, size_t *out_size) {
    if (!b)
        return NULL;
    if (out_size)
        *out_size = b->used;
    return b->buf;
}

// === Serialiser primitives ==================================================
// Appends track `used`; an overflow latches and fails the finalise —
// callers never partially fill past the buffer.

static void put8(declrom_builder_t *b, uint8_t v) {
    if (b->used + 1 > b->size) {
        b->overflow = true;
        return;
    }
    b->buf[b->used++] = v;
}

static void put16(declrom_builder_t *b, uint16_t v) {
    put8(b, (uint8_t)(v >> 8));
    put8(b, (uint8_t)v);
}

static void put32(declrom_builder_t *b, uint32_t v) {
    put16(b, (uint16_t)(v >> 16));
    put16(b, (uint16_t)v);
}

static void put_bytes(declrom_builder_t *b, const uint8_t *p, size_t n) {
    if (b->used + n > b->size) {
        b->overflow = true;
        return;
    }
    memcpy(b->buf + b->used, p, n);
    b->used += n;
}

// NUL-terminated cString, word-aligned after (the assembler's `even`).
static size_t put_cstring(declrom_builder_t *b, const char *s) {
    size_t at = b->used;
    put_bytes(b, (const uint8_t *)s, strlen(s) + 1);
    if (b->used & 1)
        put8(b, 0);
    return at;
}

// Offset-form list entry: id byte + signed self-relative 24-bit offset
// from the entry itself to `target` (always backward here — the
// serialiser emits every record before the list that references it).
static void put_oslst(declrom_builder_t *b, uint8_t id, size_t target) {
    uint32_t rel = (uint32_t)((int32_t)((int64_t)target - (int64_t)b->used)) & 0x00FFFFFFu;
    put32(b, ((uint32_t)id << 24) | rel);
}

// Data-form list entry: id byte + 16-bit immediate.
static void put_datlst(declrom_builder_t *b, uint8_t id, uint16_t v) {
    put32(b, ((uint32_t)id << 24) | v);
}

static void put_end_of_list(declrom_builder_t *b) {
    put32(b, 0xFF000000u);
}

// The rotate-left-1-add checksum over the whole image with the 4 CRC
// bytes (at image end - 12) read as zero (nubus_vrom.md §2.6) — the
// same computation crc.py performed at build time.
static uint32_t declrom_crc(const uint8_t *img, size_t size) {
    size_t crc_at = size - 12;
    uint32_t acc = 0;
    for (size_t i = 0; i < size; i++) {
        acc = (acc << 1) | (acc >> 31); // rol.l #1
        uint8_t byte = (i >= crc_at && i < crc_at + 4) ? 0 : img[i];
        acc += byte;
    }
    return acc;
}

// === Structured inputs ======================================================

bool declrom_set_board(declrom_builder_t *b, const char *name, uint16_t board_id) {
    if (!b || !name || b->finalised)
        return false;
    free(b->board_name);
    b->board_name = strdup(name);
    b->board_id = board_id;
    b->board_set = b->board_name != NULL;
    return b->board_set;
}

void declrom_set_vendor(declrom_builder_t *b, const char *vendor_id, const char *rev_level, const char *part_num) {
    if (!b || b->finalised)
        return;
    free(b->vendor_id);
    free(b->vendor_rev);
    free(b->vendor_part);
    b->vendor_id = vendor_id ? strdup(vendor_id) : NULL;
    b->vendor_rev = rev_level ? strdup(rev_level) : NULL;
    b->vendor_part = part_num ? strdup(part_num) : NULL;
}

void declrom_set_pram_init(declrom_builder_t *b, const uint8_t bytes[6]) {
    if (!b || !bytes || b->finalised)
        return;
    memcpy(b->pram_init, bytes, 6);
    b->pram_set = true;
}

bool declrom_add_video_srsrc(declrom_builder_t *b, uint8_t spid, const declrom_vidsrsrc_t *v) {
    if (!b || !v || !v->name || b->finalised)
        return false;
    // The documented designer range is 128..254, but Apple's own cards
    // use lower ids too (the 24AC's functional sResources are $6B..$6D)
    // — only the board id (1) and the terminator id are off limits.
    if (spid <= 1 || spid == 0xFF) {
        LOG(0, "declrom: video sResource spid $%02X collides with the board id or terminator", spid);
        return false;
    }
    if (b->vid_count >= DECLROM_MAX_VIDS || v->mode_count == 0 || v->mode_count > DECLROM_MAX_MODES) {
        LOG(0, "declrom: video sResource $%02X rejected (table full or bad mode count %zu)", spid, v->mode_count);
        return false;
    }
    for (size_t i = 0; i < b->vid_count; i++) {
        if (b->vids[i].spid == spid) {
            LOG(0, "declrom: duplicate video sResource spid $%02X", spid);
            return false;
        }
    }
    staged_vid_t *s = &b->vids[b->vid_count];
    s->spid = spid;
    s->name = strdup(v->name);
    if (!s->name)
        return false;
    s->drhw = v->drhw;
    s->flags = v->flags;
    s->use_major = v->use_major;
    s->base = v->base;
    s->length = v->length;
    memcpy(s->modes, v->modes, v->mode_count * sizeof(declrom_vidmode_t));
    s->mode_count = v->mode_count;
    b->vid_count++;
    return true;
}

// Deep-copy one spliced fragment after a minimal shape check.
static uint8_t *copy_frag(const uint8_t *frag, size_t size, const char *what) {
    if (!frag || size < 8) {
        LOG(0, "declrom: %s fragment missing or too small (%zu bytes)", what, size);
        return NULL;
    }
    uint32_t claimed = ((uint32_t)frag[0] << 24) | ((uint32_t)frag[1] << 16) | ((uint32_t)frag[2] << 8) | frag[3];
    if (claimed > size) {
        LOG(0, "declrom: %s fragment size field %u exceeds the %zu supplied bytes", what, claimed, size);
        return NULL;
    }
    uint8_t *copy = malloc(size);
    if (copy)
        memcpy(copy, frag, size);
    return copy;
}

bool declrom_add_exec(declrom_builder_t *b, declrom_exec_kind_t kind, const uint8_t *frag, size_t size) {
    if (!b || b->finalised || (kind != DECLROM_PRIMARY_INIT && kind != DECLROM_SECONDARY_INIT))
        return false;
    uint8_t *copy = copy_frag(frag, size, "sExec");
    if (!copy)
        return false;
    // sExec prologue: revision byte $02 right after the size long.
    if (copy[4] != 0x02) {
        LOG(0, "declrom: sExec fragment revision byte is $%02X, expected $02", copy[4]);
        free(copy);
        return false;
    }
    free(b->exec_frag[kind]);
    b->exec_frag[kind] = copy;
    b->exec_size[kind] = size;
    return true;
}

bool declrom_add_drvr(declrom_builder_t *b, const uint8_t *frag, size_t size) {
    if (!b || b->finalised)
        return false;
    uint8_t *copy = copy_frag(frag, size, "DRVR");
    if (!copy)
        return false;
    free(b->drvr_frag);
    b->drvr_frag = copy;
    b->drvr_size = size;
    return true;
}

// === Serialisation ==========================================================

// Emit an 8-byte sRsrcType record; returns its offset.
static size_t put_type_record(declrom_builder_t *b, uint16_t cat, uint16_t ctype, uint16_t drsw, uint16_t drhw) {
    size_t at = b->used;
    put16(b, cat);
    put16(b, ctype);
    put16(b, drsw);
    put16(b, drhw);
    return at;
}

// Emit one staged video sResource (leaves first, list last); returns the
// offset of its sResource list for the directory entry.
static size_t put_video_srsrc(declrom_builder_t *b, const staged_vid_t *v, size_t drvr_dir_at) {
    size_t type_at = put_type_record(b, 0x0003 /*CatDisplay*/, 0x0001 /*TypVideo*/, 0x0001 /*DrSWApple*/, v->drhw);
    size_t name_at = put_cstring(b, v->name);
    size_t base_at = b->used;
    put32(b, v->base);
    size_t len_at = b->used;
    put32(b, v->length);

    // Per-mode VPBlocks, then the small mode lists referencing them.
    size_t vp_at[DECLROM_MAX_MODES];
    size_t mode_at[DECLROM_MAX_MODES];
    for (size_t i = 0; i < v->mode_count; i++) {
        const declrom_vidmode_t *m = &v->modes[i];
        vp_at[i] = b->used;
        put32(b, 46); // sBlock size (incl. self): 4 + 42 VPBlock bytes
        put32(b, m->base_offset); // vpBaseOffset
        put16(b, m->row_bytes); // vpRowBytes
        put16(b, 0); // vpBounds top
        put16(b, 0); // vpBounds left
        put16(b, m->height); // vpBounds bottom
        put16(b, m->width); // vpBounds right
        put16(b, 1); // vpVersion
        put16(b, 0); // vpPackType
        put32(b, 0); // vpPackSize
        put32(b, 0x00480000); // vpHRes: 72 dpi Fixed
        put32(b, 0x00480000); // vpVRes
        put16(b, m->pixel_type); // vpPixelType
        put16(b, m->pixel_size); // vpPixelSize
        put16(b, m->cmp_count); // vpCmpCount
        put16(b, m->cmp_size); // vpCmpSize
        put32(b, 0); // vpPlaneBytes
    }
    for (size_t i = 0; i < v->mode_count; i++) {
        mode_at[i] = b->used;
        put_oslst(b, ID_MVIDPARAMS, vp_at[i]);
        put_datlst(b, ID_MPAGECNT, v->modes[i].page_count);
        put_datlst(b, ID_MDEVTYPE, v->modes[i].dev_type);
        put_end_of_list(b);
    }

    // The sResource list itself — entries in ascending-id order.
    size_t list_at = b->used;
    put_oslst(b, ID_SRSRC_TYPE, type_at);
    put_oslst(b, ID_SRSRC_NAME, name_at);
    if (drvr_dir_at)
        put_oslst(b, ID_SRSRC_DRVR_DIR, drvr_dir_at);
    if (v->flags)
        put_datlst(b, ID_SRSRC_FLAGS, v->flags);
    put_datlst(b, ID_SRSRC_HWDEVID, 1);
    put_oslst(b, v->use_major ? ID_MAJOR_BASE : ID_MINOR_BASE, base_at);
    put_oslst(b, v->use_major ? ID_MAJOR_LENGTH : ID_MINOR_LENGTH, len_at);
    for (size_t i = 0; i < v->mode_count; i++)
        put_oslst(b, (uint8_t)(ID_FIRST_VIDMODE + i), mode_at[i]);
    put_end_of_list(b);
    return list_at;
}

// Ascending-spID comparison for the directory ordering invariant.
static int vid_cmp(const void *pa, const void *pb) {
    const staged_vid_t *a = pa, *vb = pb;
    return (int)a->spid - (int)vb->spid;
}

bool declrom_finalise(declrom_builder_t *b, uint8_t byte_lanes) {
    if (!b || b->finalised)
        return false;
    if (byte_lanes != 0x0F) {
        // v1 limit: dense 4-lane images only (declrom.h header note).
        LOG(0, "declrom: unsupported byteLanes $%02X (only $0F is generated)", byte_lanes);
        return false;
    }
    if (!b->board_set) {
        LOG(0, "declrom: finalise without a board sResource (declrom_set_board)");
        return false;
    }

    // Deterministic directory order: video sResources ascending by spID.
    qsort(b->vids, b->vid_count, sizeof(b->vids[0]), vid_cmp);

    // 1. Spliced code fragments, verbatim.
    size_t exec_at[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        if (b->exec_frag[k]) {
            exec_at[k] = b->used;
            put_bytes(b, b->exec_frag[k], b->exec_size[k]);
            if (b->used & 1)
                put8(b, 0);
        }
    }
    size_t drvr_at = 0, drvr_dir_at = 0;
    if (b->drvr_frag) {
        drvr_at = b->used;
        put_bytes(b, b->drvr_frag, b->drvr_size);
        if (b->used & 1)
            put8(b, 0);
        // The shared sDriver directory every video sResource points at.
        drvr_dir_at = b->used;
        put_oslst(b, ID_SMACOS68020, drvr_at);
        put_end_of_list(b);
    }

    // 2. Board sResource leaves, then its list.
    size_t board_type_at = put_type_record(b, 0x0001 /*CatBoard*/, 0, 0, 0);
    size_t board_name_at = put_cstring(b, b->board_name);
    size_t pram_at = 0;
    if (b->pram_set) {
        pram_at = b->used;
        put32(b, 12); // sBlock size (incl. self)
        put8(b, 0); // reserved
        put8(b, 0); // reserved
        put_bytes(b, b->pram_init, 6);
    }
    size_t vendor_at = 0;
    if (b->vendor_id || b->vendor_rev || b->vendor_part) {
        size_t vid_at = b->vendor_id ? put_cstring(b, b->vendor_id) : 0;
        size_t rev_at = b->vendor_rev ? put_cstring(b, b->vendor_rev) : 0;
        size_t part_at = b->vendor_part ? put_cstring(b, b->vendor_part) : 0;
        vendor_at = b->used;
        if (vid_at)
            put_oslst(b, ID_VENDOR_ID, vid_at);
        if (rev_at)
            put_oslst(b, ID_VENDOR_REVLEVEL, rev_at);
        if (part_at)
            put_oslst(b, ID_VENDOR_PARTNUM, part_at);
        put_end_of_list(b);
    }
    size_t board_at = b->used;
    put_oslst(b, ID_SRSRC_TYPE, board_type_at);
    put_oslst(b, ID_SRSRC_NAME, board_name_at);
    put_datlst(b, ID_BOARD_ID, b->board_id);
    if (pram_at)
        put_oslst(b, ID_PRAM_INIT, pram_at);
    // Presence is the staged fragment, NOT exec_at != 0 — the first
    // fragment legitimately sits at image offset 0.
    if (b->exec_frag[DECLROM_PRIMARY_INIT])
        put_oslst(b, ID_PRIMARY_INIT, exec_at[DECLROM_PRIMARY_INIT]);
    if (vendor_at)
        put_oslst(b, ID_VENDOR_INFO, vendor_at);
    if (b->exec_frag[DECLROM_SECONDARY_INIT])
        put_oslst(b, ID_SECONDARY_INIT, exec_at[DECLROM_SECONDARY_INIT]);
    put_end_of_list(b);

    // 3. Functional video sResources.
    size_t vid_list_at[DECLROM_MAX_VIDS];
    for (size_t i = 0; i < b->vid_count; i++)
        vid_list_at[i] = put_video_srsrc(b, &b->vids[i], drvr_dir_at);

    // 4. The directory (board id 1 first, then ascending spIDs).
    size_t dir_at = b->used;
    put_oslst(b, 1, board_at);
    for (size_t i = 0; i < b->vid_count; i++)
        put_oslst(b, b->vids[i].spid, vid_list_at[i]);
    put_end_of_list(b);

    // 5. The trailing Format Block; Length/CRC cover the whole image.
    put_oslst(b, 0, dir_at); // DirectoryOffset (id byte 0, offset form)
    size_t length_field = b->used;
    put32(b, 0); // Length — stamped below
    put32(b, 0); // CRC    — stamped below
    put8(b, 1); // RevisionLevel
    put8(b, 1); // Format (Apple)
    put32(b, DECLROM_TESTPATTERN);
    put8(b, 0); // Reserved
    put8(b, byte_lanes);

    if (b->overflow) {
        LOG(0, "declrom: generated image exceeds the %zu-byte builder buffer", b->size);
        return false;
    }
    // Stamp Length, then the CRC over the finished image.
    uint32_t total = (uint32_t)b->used;
    b->buf[length_field + 0] = (uint8_t)(total >> 24);
    b->buf[length_field + 1] = (uint8_t)(total >> 16);
    b->buf[length_field + 2] = (uint8_t)(total >> 8);
    b->buf[length_field + 3] = (uint8_t)total;
    uint32_t crc = declrom_crc(b->buf, b->used);
    b->buf[length_field + 4] = (uint8_t)(crc >> 24);
    b->buf[length_field + 5] = (uint8_t)(crc >> 16);
    b->buf[length_field + 6] = (uint8_t)(crc >> 8);
    b->buf[length_field + 7] = (uint8_t)crc;

    if (!declrom_image_validate(b->buf, b->used)) {
        LOG(0, "declrom: generated image failed structural validation");
        return false;
    }
    b->finalised = true;
    return true;
}

// === Structural validation ==================================================
// The §5 permanent guard: a walk of the generated (or any dense $0F)
// image that fails loudly instead of handing the Slot Manager a corrupt
// directory.  The zero-offset check specifically fences the silent
// `|`-fold class of assembler/serialiser bugs.

// Read a big-endian long inside the image with bounds checking.
static bool img_be32(const uint8_t *img, size_t size, size_t at, uint32_t *out) {
    if (at + 4 > size)
        return false;
    *out = ((uint32_t)img[at] << 24) | ((uint32_t)img[at + 1] << 16) | ((uint32_t)img[at + 2] << 8) | img[at + 3];
    return true;
}

// Decode a list entry's signed self-relative target; false when the
// offset is zero or lands outside the image.
static bool entry_target(const uint8_t *img, size_t size, size_t entry_at, uint32_t entry, size_t *out) {
    (void)img;
    int32_t rel = (int32_t)(entry & 0x00FFFFFFu);
    if (rel & 0x800000)
        rel -= 0x1000000;
    if (rel == 0)
        return false; // the silent-fold fence: no zero offsets, ever
    int64_t tgt = (int64_t)entry_at + rel;
    if (tgt < 0 || (uint64_t)tgt >= size)
        return false;
    *out = (size_t)tgt;
    return true;
}

// Walk one list (directory or sub-list): ascending ids, in-bounds
// terminator.  Returns the entry count, or -1 on a structural fault.
// Offset-form entries are bounds-checked when `offsets` says so.
static int walk_list(const uint8_t *img, size_t size, size_t at, bool (*is_offset)(uint8_t id), const char *what) {
    int count = 0;
    int last_id = -1;
    for (;;) {
        uint32_t e;
        if (!img_be32(img, size, at, &e)) {
            LOG(0, "declrom validate: %s list at +$%zX runs off the image", what, at);
            return -1;
        }
        uint8_t id = (uint8_t)(e >> 24);
        if (id == ID_END_OF_LIST) {
            if (e != 0xFF000000u) {
                LOG(0, "declrom validate: %s terminator at +$%zX is $%08X", what, at, e);
                return -1;
            }
            return count;
        }
        if ((int)id <= last_id) {
            LOG(0, "declrom validate: %s ids not ascending at +$%zX (id %u after %d)", what, at, id, last_id);
            return -1;
        }
        last_id = id;
        if (is_offset && is_offset(id)) {
            size_t tgt;
            if (!entry_target(img, size, at, e, &tgt)) {
                LOG(0, "declrom validate: %s entry id %u at +$%zX has a zero/out-of-range offset ($%08X)", what, id, at,
                    e);
                return -1;
            }
        }
        at += 4;
        count++;
    }
}

// Which ids are offset-form in each list context.
static bool dir_is_offset(uint8_t id) {
    (void)id;
    return true; // every directory entry is an offset
}
static bool board_is_offset(uint8_t id) {
    return id == ID_SRSRC_TYPE || id == ID_SRSRC_NAME || id == ID_PRAM_INIT || id == ID_PRIMARY_INIT ||
           id == ID_VENDOR_INFO || id == ID_SECONDARY_INIT;
}
static bool vid_is_offset(uint8_t id) {
    return id == ID_SRSRC_TYPE || id == ID_SRSRC_NAME || id == ID_SRSRC_DRVR_DIR || id == ID_MINOR_BASE ||
           id == ID_MINOR_LENGTH || id == ID_MAJOR_BASE || id == ID_MAJOR_LENGTH || id >= ID_FIRST_VIDMODE;
}
static bool mode_is_offset(uint8_t id) {
    return id == ID_MVIDPARAMS;
}

// Check an sExecBlock prologue at `at`: in-range size long, revision $02.
static bool check_exec(const uint8_t *img, size_t size, size_t at, const char *what) {
    uint32_t blksize;
    if (!img_be32(img, size, at, &blksize) || blksize < 12 || at + blksize > size) {
        LOG(0, "declrom validate: %s sExec at +$%zX has a bad size", what, at);
        return false;
    }
    if (img[at + 4] != 0x02) {
        LOG(0, "declrom validate: %s sExec at +$%zX revision byte $%02X != $02", what, at, img[at + 4]);
        return false;
    }
    return true;
}

// Walk the entries of one sResource whose list sits at `at`, recursing
// into mode lists and exec blocks.  `board` selects the id namespace.
static bool validate_srsrc(const uint8_t *img, size_t size, size_t at, bool board) {
    if (walk_list(img, size, at, board ? board_is_offset : vid_is_offset, board ? "board" : "video") < 0)
        return false;
    for (size_t e = at;; e += 4) {
        uint32_t entry;
        if (!img_be32(img, size, e, &entry))
            return false;
        uint8_t id = (uint8_t)(entry >> 24);
        if (id == ID_END_OF_LIST)
            break;
        size_t tgt = 0;
        if (board) {
            if ((id == ID_PRIMARY_INIT || id == ID_SECONDARY_INIT)) {
                if (!entry_target(img, size, e, entry, &tgt) || !check_exec(img, size, tgt, "board"))
                    return false;
            }
        } else {
            if (id >= ID_FIRST_VIDMODE) {
                if (!entry_target(img, size, e, entry, &tgt) || walk_list(img, size, tgt, mode_is_offset, "mode") < 0)
                    return false;
            }
            if (id == ID_SRSRC_DRVR_DIR) {
                // sDriver directory: each entry points at a size-prefixed
                // sBlock that must fit inside the image.
                if (!entry_target(img, size, e, entry, &tgt) || walk_list(img, size, tgt, dir_is_offset, "drvrdir") < 0)
                    return false;
                for (size_t d = tgt;; d += 4) {
                    uint32_t de;
                    if (!img_be32(img, size, d, &de))
                        return false;
                    if ((uint8_t)(de >> 24) == ID_END_OF_LIST)
                        break;
                    size_t blk;
                    uint32_t blksize;
                    if (!entry_target(img, size, d, de, &blk) || !img_be32(img, size, blk, &blksize) || blksize < 8 ||
                        blk + blksize > size) {
                        LOG(0, "declrom validate: driver sBlock out of range");
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool declrom_image_validate(const uint8_t *img, size_t size) {
    if (!img || size < DECLROM_FB_SIZE + 8)
        return false;
    size_t fb = size - DECLROM_FB_SIZE;
    uint32_t testpat = 0, length = 0, dirent = 0;
    if (!img_be32(img, size, fb + 14, &testpat) || testpat != DECLROM_TESTPATTERN) {
        LOG(0, "declrom validate: TestPattern is $%08X, expected $%08X", testpat, DECLROM_TESTPATTERN);
        return false;
    }
    uint8_t lanes = img[size - 1];
    if (((~lanes) & 0x0F) != (lanes >> 4)) {
        LOG(0, "declrom validate: byteLanes $%02X fails the complement rule", lanes);
        return false;
    }
    uint8_t rev = img[fb + 12];
    if (rev < 1 || rev > 9) {
        LOG(0, "declrom validate: RevisionLevel %u outside 1..9", rev);
        return false;
    }
    if (!img_be32(img, size, fb + 4, &length) || length > size || length < DECLROM_FB_SIZE) {
        LOG(0, "declrom validate: Length $%X vs image size %zu", length, size);
        return false;
    }
    uint32_t stored_crc = 0, want_crc = declrom_crc(img, size);
    if (!img_be32(img, size, fb + 8, &stored_crc))
        return false;
    if (length == size && stored_crc != want_crc) {
        // Only checkable when Length covers the whole image (generated
        // images always do; dumped chips may cover a sub-region).
        LOG(0, "declrom validate: CRC $%08X != computed $%08X", stored_crc, want_crc);
        return false;
    }
    if (!img_be32(img, size, fb, &dirent))
        return false;
    size_t dir_at;
    if (!entry_target(img, size, fb, dirent, &dir_at)) {
        LOG(0, "declrom validate: DirectoryOffset $%08X is zero or out of range", dirent);
        return false;
    }
    if (walk_list(img, size, dir_at, dir_is_offset, "directory") < 0)
        return false;
    // Recurse into every sResource the directory names.
    for (size_t e = dir_at;; e += 4) {
        uint32_t entry;
        if (!img_be32(img, size, e, &entry))
            return false;
        uint8_t id = (uint8_t)(entry >> 24);
        if (id == ID_END_OF_LIST)
            break;
        size_t tgt;
        if (!entry_target(img, size, e, entry, &tgt))
            return false;
        if (!validate_srsrc(img, size, tgt, id == 1))
            return false;
    }
    return true;
}

bool declrom_identify_vendor(const uint8_t *img, size_t size, const char *vendor, uint16_t *out_board_id) {
    if (!img || !vendor || size < DECLROM_FB_SIZE + 8)
        return false;
    size_t fb = size - DECLROM_FB_SIZE;
    uint32_t testpat = 0, dirent = 0;
    if (!img_be32(img, size, fb + 14, &testpat) || testpat != DECLROM_TESTPATTERN)
        return false;
    size_t dir_at;
    if (!img_be32(img, size, fb, &dirent) || !entry_target(img, size, fb, dirent, &dir_at))
        return false;
    // Find the board sResource (directory id 1), then its BoardId and
    // VendorInfo entries.
    for (size_t e = dir_at;; e += 4) {
        uint32_t entry;
        if (!img_be32(img, size, e, &entry))
            return false;
        uint8_t id = (uint8_t)(entry >> 24);
        if (id == ID_END_OF_LIST)
            return false; // no board sResource
        if (id != 1)
            continue;
        size_t board;
        if (!entry_target(img, size, e, entry, &board))
            return false;
        bool vendor_ok = false;
        uint16_t board_id = 0;
        bool board_id_seen = false;
        for (size_t be = board;; be += 4) {
            uint32_t bentry;
            if (!img_be32(img, size, be, &bentry))
                return false;
            uint8_t bid = (uint8_t)(bentry >> 24);
            if (bid == ID_END_OF_LIST)
                break;
            if (bid == ID_BOARD_ID) {
                board_id = (uint16_t)(bentry & 0xFFFF);
                board_id_seen = true;
            } else if (bid == ID_VENDOR_INFO) {
                size_t vlist;
                if (!entry_target(img, size, be, bentry, &vlist))
                    return false;
                for (size_t ve = vlist;; ve += 4) {
                    uint32_t ventry;
                    if (!img_be32(img, size, ve, &ventry))
                        return false;
                    uint8_t vid = (uint8_t)(ventry >> 24);
                    if (vid == ID_END_OF_LIST)
                        break;
                    size_t str_at;
                    if (vid == ID_VENDOR_ID && entry_target(img, size, ve, ventry, &str_at)) {
                        size_t maxlen = size - str_at;
                        if (strnlen((const char *)img + str_at, maxlen) < maxlen &&
                            strcmp((const char *)img + str_at, vendor) == 0)
                            vendor_ok = true;
                    }
                }
            }
        }
        if (vendor_ok && board_id_seen && out_board_id)
            *out_board_id = board_id;
        return vendor_ok && board_id_seen;
    }
}

void declrom_install(nubus_card_t *card, const declrom_builder_t *b) {
    if (!card || !b || !b->buf || b->used == 0)
        return;
    free(card->declrom);
    card->declrom = malloc(b->used);
    if (!card->declrom) {
        card->declrom_size = 0;
        return;
    }
    memcpy(card->declrom, b->buf, b->used);
    card->declrom_size = b->used;
}

uint8_t *declrom_load(const char *path, size_t expected_size) {
    if (!path || expected_size == 0)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG(1, "declrom_load: cannot open '%s'", path);
        return NULL;
    }
    uint8_t *buf = malloc(expected_size);
    if (!buf) {
        fclose(f);
        LOG(1, "declrom_load: out of memory allocating %zu bytes for '%s'", expected_size, path);
        return NULL;
    }
    size_t n = fread(buf, 1, expected_size, f);
    // Detect whether the file has more bytes beyond the expected size so we
    // can warn about silent truncation.
    int extra_present = (fgetc(f) != EOF);
    fclose(f);
    if (n != expected_size) {
        LOG(1, "declrom_load: '%s' is %zu bytes, expected %zu — refusing", path, n, expected_size);
        free(buf);
        return NULL;
    }
    if (extra_present)
        LOG(1, "declrom_load: '%s' is larger than expected %zu bytes — trailing data ignored", path, expected_size);
    return buf;
}

// === Shared display-card VROM loader ========================================
//
// Factored out of jmfb.c so both the JMFB (8•24) and the Display Card 24AC
// drivers share the same byte-lane expansion and search-path logic — the
// only thing that differs between them is the file name and the chip/bus
// sizes (both happen to be 32 KB → 128 KB today, but the loader is sized
// from the arguments so a different card could use other sizes).

// Sparse-expand a single-lane chip into a 4×-larger bus-space buffer: each
// chip byte at offset i lands at byte lane `lane` of longword i (bus offset
// i*4 + lane); the other three lanes stay zero.  Apple display-card
// declaration ROMs are 8-bit chips wired to one NuBus byte lane.
void declrom_expand_lane(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf, unsigned lane) {
    for (size_t i = 0; i < chip_size; i++)
        bus_buf[i * 4 + (lane & 3u)] = chip[i];
}

// Back-compat lane-3 wrapper (the JMFB / 24AC layout, byteLanes $78).
void declrom_expand_lane3(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf) {
    declrom_expand_lane(chip, chip_size, bus_buf, 3);
}

bool declrom_layout_chip(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf, size_t bus_size, uint8_t byte_lanes) {
    // Single active byte lane.  In the NuBus format-block convention the low
    // nibble of byteLanes is the lane bitmask and the high nibble its ones-
    // complement; a one-bit mask means the ROM lives entirely on lane N, so
    // sparse-expand it into lane N of the 4× bus buffer.  $78 = lane 3
    // (JMFB / 24AC), $E1 = lane 0 (Display Card 8•24 GC — a wider ROM wired
    // to the low byte lane).
    uint8_t lanes = (uint8_t)(byte_lanes & 0x0Fu);
    bool complement_ok = (uint8_t)((~byte_lanes) & 0x0Fu) == (uint8_t)(byte_lanes >> 4);
    bool single_lane = lanes != 0 && (lanes & (uint8_t)(lanes - 1)) == 0;
    if (complement_ok && single_lane) {
        if (bus_size < chip_size * 4)
            return false;
        unsigned lane = 0;
        while (!(lanes & (1u << lane)))
            lane++;
        declrom_expand_lane(chip, chip_size, bus_buf, lane);
        return true;
    }
    if (byte_lanes == 0x0Fu) {
        // 4-lane layout (e.g. a synthesised ROM) — flat copy into the last
        // chip_size bytes of the bus buffer; lanes 0..3 all carry data.
        if (bus_size < chip_size)
            return false;
        memcpy(bus_buf + bus_size - chip_size, chip, chip_size);
        return true;
    }
    return false;
}

// Read exactly chip_size bytes from `path` into `buf`.  Returns true only
// on an exact-size read (a short file is rejected).
static bool read_chip_exact(const char *path, uint8_t *buf, size_t chip_size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(buf, 1, chip_size, f);
    fclose(f);
    return n == chip_size;
}

// Load the identified chip image at `path` (chip_size bytes) into the TAIL of
// the card's bus-space window, so the Format Block always ends at the slot
// top regardless of chip size — a 32 KB chip in a card whose window was sized
// for a 64 KB one (the 8•24 GC v1.0 in the v1.1-sized window) occupies the
// top half; the leading bytes stay zero, below the ROM's declared length.
static bool load_chip_into_bus(const char *path, size_t chip_size, uint8_t *bus_buf, size_t bus_size) {
    if (chip_size == 0 || bus_size < chip_size)
        return false;
    uint8_t *chip = calloc(1, chip_size);
    if (!chip)
        return false;
    if (!read_chip_exact(path, chip, chip_size)) {
        free(chip);
        return false;
    }
    // The chip's last byte is the byteLanes value (for the single-lane and
    // 4-lane layouts both the spec and these files place it at the highest
    // active-lane address — the chip's final byte).
    uint8_t byte_lanes = chip[chip_size - 1];
    // A single-lane chip sparse-expands to 4× its size in bus space; a 4-lane
    // ($0F) chip is flat-copied and occupies exactly chip_size bytes (the
    // SE/30 onboard-video ROM, whose window equals the chip size).
    size_t footprint = (byte_lanes == 0x0Fu) ? chip_size : chip_size * 4;
    bool ok = bus_size >= footprint &&
              declrom_layout_chip(chip, chip_size, bus_buf + (bus_size - footprint), footprint, byte_lanes);
    if (!ok)
        LOG(0, "declrom_load_vrom_card: '%s' has unsupported byteLanes $%02x (or exceeds the bus window)", path,
            byte_lanes);
    free(chip);
    return ok;
}

bool declrom_install_builtin(const char *card_id, const uint8_t *chip, size_t chip_size, uint8_t *bus_buf,
                             size_t bus_size) {
    if (!card_id || !chip || chip_size < 20 || !bus_buf)
        return false;
    // Lay the blob out exactly like a file-backed chip: byteLanes from the
    // chip's last byte, tail-placed so the Format Block ends at the slot top.
    uint8_t byte_lanes = chip[chip_size - 1];
    size_t footprint = (byte_lanes == 0x0Fu) ? chip_size : chip_size * 4;
    if (bus_size < footprint ||
        !declrom_layout_chip(chip, chip_size, bus_buf + (bus_size - footprint), footprint, byte_lanes)) {
        LOG(0, "declrom_install_builtin: '%s' blob has unsupported byteLanes $%02x (or exceeds the bus window)",
            card_id, byte_lanes);
        return false;
    }
    // Report the pick into the built-from record like any resolved declROM —
    // path-less, identified by the blob's stored Format-Block CRC.
    const uint8_t *t = chip + chip_size - 12; // CRC field, big-endian
    uint32_t crc = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | (uint32_t)t[3];
    char locator[64];
    snprintf(locator, sizeof locator, "builtin:%s", card_id);
    machine_config_note_vrom(card_id, locator, crc, /*explicit_pick*/ false);
    return true;
}

bool declrom_load_vrom_card(const char *card_id, uint8_t *bus_buf, size_t bus_size, char **out_path) {
    if (out_path)
        *out_path = NULL;
    if (!card_id || !bus_buf || bus_size == 0)
        return false;

    // Walk the offer registry's candidates for this card in pick order
    // (explicit vrom.load first, then catalog-preferred, then catalog order —
    // see vrom_offer_find).  Every candidate was already content-identified
    // at offer time; the first one that lays out cleanly wins.  Core never
    // builds a path here — the platform offered every one of these.
    size_t chip_size = 0;
    for (int n = 0;; n++) {
        const char *path = vrom_offer_find(card_id, n, &chip_size);
        if (!path)
            break;
        if (load_chip_into_bus(path, chip_size, bus_buf, bus_size)) {
            // Report the winning pick into the built-from record so
            // machine.config.vroms answers which revision this machine
            // actually runs (proposal-named-args-boot-config §4.2).
            uint32_t crc = 0;
            bool explicit_pick = false;
            vrom_offer_info(path, &crc, &explicit_pick);
            machine_config_note_vrom(card_id, path, crc, explicit_pick);
            if (out_path)
                *out_path = strdup(path);
            return true;
        }
    }
    return false;
}
