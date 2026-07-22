// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vrom.c
// VROM content identification, the pre-boot offer registry, and the vrom.*
// object-model surface.
//
// A vROM is a NuBus declaration-ROM chip image (32 KB, or 64 KB for some
// revisions) identified purely by its Format-Block CRC.  The platform offers
// candidate files (vrom_offer) before machine.boot; the card factories match
// among the offers by content (vrom_offer_find via declrom_load_vrom_card).
// Core never fabricates a path and never interprets a filename — a path is
// only ever an opaque handle used to open the file.

#include "vrom.h"

#include "log.h"
#include "machine_profile.h"
#include "object.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

LOG_USE_CATEGORY_NAME("vrom");

// ============================================================================
// File-level helpers
// ============================================================================

bool vrom_probe_file(const char *path, size_t *out_size) {
    if (out_size)
        *out_size = 0;
    if (!path || !*path)
        return false;
    // stat is portable for binary-file sizing; fseek(SEEK_END)+ftell on a
    // binary stream is implementation-defined per ISO C. See [F-354].
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0)
        return false;
    size_t size = (size_t)st.st_size;
    if (out_size)
        *out_size = size;
    return size == VROM_EXPECTED_SIZE;
}

// NuBus declaration-ROM Format Block CRC.  Every declaration ROM — the
// genuine cards and the "fake" SE/30 onboard-video ROM alike — carries a
// 20-byte Format Block at the top of the dense chip image; its 4-byte CRC
// (preceded by the `$5A932BC7` TestPattern) is the intrinsic, Slot-Manager-
// validated checksum.  See docs/core/peripherals/nubus_vrom.md §2.  We read
// it as identity, the direct analog of rom.c keying on the main ROM's
// checksum word — no emulator-invented hash.  Field offsets from EOF of the
// dense chip (high address = end), per §2 / §12:
//   buf[size-1]        ByteLanes
//   buf[size-2]        Reserved
//   buf[size-6..size-3] TestPattern (big-endian $5A 93 2B C7)
//   buf[size-12..size-9] CRC (big-endian)
#define VROM_TESTPATTERN_OFF 6 // bytes from EOF to the first TestPattern byte
#define VROM_CRC_OFF         12 // bytes from EOF to the first (MSB) CRC byte

static uint32_t vrom_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Catalog of known VROM blobs.  Maps the declaration ROM's Format-Block CRC
// to the nubus card-kind id the blob provides — content→hardware facts only,
// no filenames (canonical fixture naming is a tooling concern; see
// scripts/rom_naming.py).  The id is the machine-readable link a UI uses to
// pick the card (machine.nubus.video_card); the human label is owned by the
// card kind (nubus_card_find(id)->display_name) so it never drifts.  The
// `preferred` bit marks the default revision when one card has several ROMs.
// Adding a new VROM = one row here.  Keyed exactly like rom.c's ROM_TABLE
// {checksum -> ...}.
struct vrom_known {
    uint32_t crc;
    size_t chip_size; // dense chip image size on disk
    const char *card_id;
    bool preferred; // default pick among several revisions of one card
};
static const struct vrom_known VROM_CATALOG[] = {
    // Macintosh Display Card 8•24 (ROM rev 341-0868, "Rev B"). Drives the JMFB
    // NuBus card (id "mdc_8_24") on IIcx / IIx / IIfx; loaded by jmfb.c.
    {0xD1629664u, 0x08000, "mdc_8_24",           true },
    // SE/30 onboard video declaration ROM (byteLanes $0F, 4-lane); loaded
    // by builtin_se30_video.c (id "builtin_se30_video").
    {0x4F71FF1Au, 0x08000, "builtin_se30_video", true },
    // Apple Macintosh Display Card 24AC (id "display_card_24ac").
    {0xD8DAAB87u, 0x08000, "display_card_24ac",  true },
    // Apple Macintosh Display Card 8•24 GC ("Dolphin", id "824gc"): the
    // accelerated card.  Its declaration ROMs are byteLanes $E1 (byte lane 0);
    // v1.1 is a 64 KB chip, v1.0 / the alpha are 32 KB.  All three carry the
    // same $5A932BC7 TestPattern, so they identify by Format-Block CRC.
    {0xD722B053u, 0x10000, "824gc",              true }, // part 341-0266, v1.1 (default; 16bpp)
    {0x9E9857E8u, 0x08000, "824gc",              false}, // part 341-0812-02, v1.0 (shipping)
    {0x4740028Du, 0x08000, "824gc",              false}, // "Dolphin" 1.00a16 alpha
};

#define VROM_CATALOG_COUNT (sizeof(VROM_CATALOG) / sizeof(VROM_CATALOG[0]))

// Content-identification core shared by vrom.identify, the offer registry,
// and the card factories' loader (declrom_load_vrom_card).  Reads the file's
// trailing Format Block, gates on the $5A932BC7 TestPattern, and looks the
// CRC up in the catalog.  Result codes let vrom.identify keep its
// error/unrecognised distinction.
enum vrom_id_result {
    VROM_ID_UNREADABLE, // stat/open/read failed
    VROM_ID_WRONG_SIZE, // exists, but not a chip-sized blob
    VROM_ID_UNKNOWN, // right size; *out_crc valid; not a catalog entry (or no TestPattern)
    VROM_ID_KNOWN, // recognised: *out filled from the catalog row
};
static enum vrom_id_result vrom_identify_core(const char *path, vrom_id_t *out, size_t *out_size, uint32_t *out_crc) {
    size_t size = 0;
    vrom_probe_file(path, &size); // fills *size regardless of the 32 KB gate
    if (out_size)
        *out_size = size;
    // vrom_probe_file leaves size == 0 only when stat failed (missing /
    // unreadable).
    if (size == 0)
        return VROM_ID_UNREADABLE;
    // Declaration-ROM chips come in two sizes: 32 KB (SE/30, JMFB, 24AC, the
    // 8•24 GC v1.0 / alpha) and 64 KB (the 8•24 GC v1.1).  The Format Block +
    // CRC live in the trailing bytes either way, so accept both.
    if (size != VROM_EXPECTED_SIZE && size != 2u * VROM_EXPECTED_SIZE)
        return VROM_ID_WRONG_SIZE;

    FILE *f = fopen(path, "rb");
    if (!f)
        return VROM_ID_UNREADABLE;
    // Only the trailing Format Block matters for identity.
    uint8_t tail[VROM_CRC_OFF];
    if (fseek(f, (long)(size - sizeof(tail)), SEEK_SET) != 0 || fread(tail, 1, sizeof(tail), f) != sizeof(tail)) {
        fclose(f);
        return VROM_ID_UNREADABLE;
    }
    fclose(f);

    // Gate on the TestPattern: a right-sized blob without `$5A932BC7` in the
    // Format Block is not a declaration ROM (don't trust a stray CRC match).
    const uint8_t *tp = tail + sizeof(tail) - VROM_TESTPATTERN_OFF;
    bool is_declrom = tp[0] == 0x5Au && tp[1] == 0x93u && tp[2] == 0x2Bu && tp[3] == 0xC7u;
    uint32_t crc = vrom_be32(tail);
    if (out_crc)
        *out_crc = crc;
    if (!is_declrom)
        return VROM_ID_UNKNOWN;
    for (size_t i = 0; i < VROM_CATALOG_COUNT; i++) {
        if (VROM_CATALOG[i].crc == crc) {
            if (out) {
                out->crc = crc;
                out->chip_size = size;
                out->card_id = VROM_CATALOG[i].card_id;
            }
            return VROM_ID_KNOWN;
        }
    }
    return VROM_ID_UNKNOWN;
}

bool vrom_identify_card(const char *path, vrom_id_t *out) {
    if (!path || !*path)
        return false;
    return vrom_identify_core(path, out, NULL, NULL) == VROM_ID_KNOWN;
}

// ============================================================================
// Offer registry
// ============================================================================

// One registered candidate: a recognised file the platform offered, keyed by
// its content identity.  The direct analog of the old single pending-path
// static, generalised to N entries.
struct vrom_offer_entry {
    uint32_t crc; // content identity (registry key)
    size_t chip_size; // actual file size (32 KB or 64 KB)
    const char *card_id; // catalog card-kind id (static storage)
    char *path; // opaque locator (owned)
    bool explicit_pick; // set by vrom.load — wins the pick order
};

static struct vrom_offer_entry *s_offers = NULL;
static size_t s_offer_count = 0;
static size_t s_offer_cap = 0;

// Register one candidate.  `explicit_pick` marks the vrom.load offer, which
// takes priority in vrom_offer_find (at most one entry carries the flag).
static void vrom_offer_add(const char *path, bool explicit_pick) {
    if (!path || !*path)
        return;
    vrom_id_t id;
    if (!vrom_identify_card(path, &id)) {
        // Not a recognised declaration ROM — drop it quietly (the platform
        // offers whole directories; strays are expected, not errors).
        LOG(2, "vrom_offer: '%s' is not a recognised declaration ROM — ignored", path);
        return;
    }
    // Idempotent by content: one entry per CRC.  A re-offer refreshes the
    // path (the newest locator for these bytes) and may promote to explicit.
    struct vrom_offer_entry *e = NULL;
    for (size_t i = 0; i < s_offer_count; i++) {
        if (s_offers[i].crc == id.crc) {
            e = &s_offers[i];
            break;
        }
    }
    if (!e) {
        if (s_offer_count == s_offer_cap) {
            size_t cap = s_offer_cap ? s_offer_cap * 2 : 8;
            struct vrom_offer_entry *grown = realloc(s_offers, cap * sizeof(*grown));
            if (!grown)
                return;
            s_offers = grown;
            s_offer_cap = cap;
        }
        e = &s_offers[s_offer_count];
        memset(e, 0, sizeof(*e));
        s_offer_count++;
    }
    char *dup = strdup(path);
    if (!dup) {
        // Fresh entry with no path is useless — roll the append back.
        if (!e->path)
            s_offer_count--;
        return;
    }
    free(e->path);
    e->path = dup;
    e->crc = id.crc;
    e->chip_size = id.chip_size;
    e->card_id = id.card_id;
    if (explicit_pick) {
        // Only one explicit pick at a time — latest vrom.load wins.
        for (size_t i = 0; i < s_offer_count; i++)
            s_offers[i].explicit_pick = false;
        e->explicit_pick = true;
    }
    LOG(2, "vrom_offer: '%s' provides card '%s' (crc 0x%08x)%s", path, e->card_id, e->crc,
        explicit_pick ? " [explicit]" : "");
}

void vrom_offer(const char *path) {
    vrom_offer_add(path, false);
}

void vrom_offer_clear(void) {
    for (size_t i = 0; i < s_offer_count; i++)
        free(s_offers[i].path);
    free(s_offers);
    s_offers = NULL;
    s_offer_count = 0;
    s_offer_cap = 0;
}

const char *vrom_offer_find(const char *card_id, int idx, size_t *out_chip_size) {
    if (!card_id)
        return NULL;
    // Pick order: the explicit vrom.load offer first, then catalog rows with
    // the `preferred` bit, then the remaining catalog rows in order.  All
    // content-based — no filename ever enters the comparison.
    // Pass 0: the explicit pick (at most one entry carries the flag).
    for (size_t i = 0; i < s_offer_count; i++) {
        if (!s_offers[i].explicit_pick || strcmp(s_offers[i].card_id, card_id) != 0)
            continue;
        if (idx-- == 0) {
            if (out_chip_size)
                *out_chip_size = s_offers[i].chip_size;
            return s_offers[i].path;
        }
    }
    // Passes 1..2: catalog order, preferred rows first.  One offer per CRC
    // (registry invariant), so each row yields at most one candidate.
    for (int want_preferred = 1; want_preferred >= 0; want_preferred--) {
        for (size_t r = 0; r < VROM_CATALOG_COUNT; r++) {
            if (VROM_CATALOG[r].preferred != (bool)want_preferred)
                continue;
            if (strcmp(VROM_CATALOG[r].card_id, card_id) != 0)
                continue;
            for (size_t i = 0; i < s_offer_count; i++) {
                if (s_offers[i].crc != VROM_CATALOG[r].crc || s_offers[i].explicit_pick)
                    continue; // explicit entry was already yielded in pass 0
                if (idx-- == 0) {
                    if (out_chip_size)
                        *out_chip_size = s_offers[i].chip_size;
                    return s_offers[i].path;
                }
            }
        }
    }
    return NULL;
}

bool vrom_offer_info(const char *path, uint32_t *out_crc, bool *out_explicit) {
    if (!path)
        return false;
    for (size_t i = 0; i < s_offer_count; i++) {
        if (strcmp(s_offers[i].path, path) != 0)
            continue;
        if (out_crc)
            *out_crc = s_offers[i].crc;
        if (out_explicit)
            *out_explicit = s_offers[i].explicit_pick;
        return true;
    }
    return false;
}

bool vrom_card_catalogued(const char *card_id) {
    if (!card_id || !*card_id)
        return false;
    for (size_t r = 0; r < VROM_CATALOG_COUNT; r++) {
        if (strcmp(VROM_CATALOG[r].card_id, card_id) == 0)
            return true;
    }
    return false;
}

bool vrom_card_resolvable(const char *card_id) {
    return vrom_offer_find(card_id, 0, NULL) != NULL;
}

// ============================================================================
// Explicit pick (vrom.load)
// ============================================================================

int vrom_set_path(const char *path) {
    if (!path || !*path) {
        printf("vrom: expected a non-empty path\n");
        return -1;
    }
    // The boot document's vrom= explicit pick: an offer that wins the pick
    // order for whichever card its content provides.  An unrecognised file
    // is dropped by the offer (with a log).
    vrom_offer_add(path, true);
    return 0;
}

// ============================================================================
// Object-model class descriptor
// ============================================================================

static value_t vrom_attr_size(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, VROM_EXPECTED_SIZE);
}

// vrom.offer(path) — platform/UI hook into the offer registry, e.g. web2's
// upload ingest offering a freshly stored file so an "(auto)" boot sees it
// without a page reload.  Returns true iff the file was recognised and
// registered; false is "not a vROM", not an error.
static value_t vrom_method_offer(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    vrom_id_t id;
    bool recognised = vrom_identify_card(argv[0].s, &id);
    if (recognised)
        vrom_offer(argv[0].s);
    return val_bool(recognised);
}

// vrom.identify(path) — returns a JSON map of content facts describing the
// file, keyed off the declaration ROM's Format-Block CRC:
//   {
//     "recognised":     bool,
//     "card_id":        "display_card_24ac",     // nubus card-kind id
//     "compatible":     ["display_card_24ac"],   // card ids this blob can drive
//     "size":           32768,
//     "crc":            "0xd8daab87"
//   }
// `compatible` mirrors rom.identify's `compatible:[model_ids]` shape (a list,
// usually length 1).  JS callers use crc to persist the file under a stable
// content-addressed name, and card_id / compatible to pick the card; the
// human-readable name comes from machine.profile, not here.  (The card
// factories load by CONTENT — declrom_load_vrom_card — so the on-disk name
// never matters.)
static value_t vrom_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    vrom_id_t id;
    size_t size = 0;
    uint32_t crc = 0;
    switch (vrom_identify_core(path, &id, &size, &crc)) {
    case VROM_ID_UNREADABLE:
        // Distinguish "can't read the file" from "present, but not a vROM",
        // mirroring rom.identify: a missing/unreadable path is a V_ERROR,
        // while a real file of the wrong size is simply unrecognised.
        return val_err("vrom.identify: cannot read '%s'", path);
    case VROM_ID_WRONG_SIZE:
        return val_str("{\"recognised\":false}");
    case VROM_ID_KNOWN: {
        char out[256];
        snprintf(out, sizeof(out),
                 "{\"recognised\":true,\"card_id\":\"%s\",\"compatible\":[\"%s\"],"
                 "\"size\":%zu,\"crc\":\"0x%08x\"}",
                 id.card_id, id.card_id, size, crc);
        return val_str(out);
    }
    case VROM_ID_UNKNOWN:
    default: {
        // Right size but not a known declaration ROM. Recognised=false so
        // callers route into a normal "unrecognised file" path; still report
        // the CRC so a future catalog entry (or the user) can identify it.
        char out[128];
        snprintf(out, sizeof(out), "{\"recognised\":false,\"size\":%zu,\"crc\":\"0x%08x\"}", size, crc);
        return val_str(out);
    }
    }
}

static const arg_decl_t vrom_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "VROM file path"},
};

static const member_t vrom_members[] = {
    {.kind = M_ATTR,
     .name = "size",
     .doc = "Expected VROM size in bytes (32 KB)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = vrom_attr_size, .set = NULL}},
    {.kind = M_METHOD,
     .name = "offer",
     .doc = "Offer a candidate VROM file; true iff recognised and registered",
     .method = {.args = vrom_path_arg, .nargs = 1, .result = V_BOOL, .fn = vrom_method_offer}},
    {.kind = M_METHOD,
     .name = "identify",
     .doc = "JSON map: {recognised, card_id?, compatible?, size, crc}.",
     .method = {.args = vrom_path_arg, .nargs = 1, .result = V_STRING, .fn = vrom_method_identify}},
};

const class_desc_t vrom_class = {
    .name = "vrom",
    .members = vrom_members,
    .n_members = sizeof(vrom_members) / sizeof(vrom_members[0]),
};

// ============================================================================
// Lifecycle
// ============================================================================

static struct object *s_vrom_object = NULL;

void vrom_init(void) {
    if (s_vrom_object)
        return;
    s_vrom_object = object_new(&vrom_class, NULL, "vrom");
    if (s_vrom_object) {
        object_set_label(s_vrom_object, "Video ROM");
        object_set_order(s_vrom_object, 95);
        object_attach(machine_object(), s_vrom_object);
    }
}

void vrom_delete(void) {
    if (s_vrom_object) {
        object_detach(s_vrom_object);
        object_delete(s_vrom_object);
        s_vrom_object = NULL;
    }
    vrom_offer_clear();
}
