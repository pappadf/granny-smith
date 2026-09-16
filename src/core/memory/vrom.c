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
#include "common.h"
#include "declrom.h" // structural recognition of generated GS images
#include "offer_registry.h"

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
    uint32_t crc = RD_BE32(tail);
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
    // Not a catalogued Apple dump — recognise a dumped copy of one of our
    // own GENERATED generic images structurally: the runtime-generated GS
    // vROM has no fixed CRC to match (its content varies with the mode
    // set and the toolchain that assembled the fragments), so identity is
    // the board sResource's "granny-smith" VendorId plus its BoardId
    // (proposal-nubus-runtime-vrom §4).
    static const struct {
        uint16_t board_id;
        const char *card_id;
    } gs_boards[] = {
        {0x0027, "8_24"  },
        {0x05FA, "24ac"  },
        {0x002C, "8_24gc"},
        {0x000C, "se30"  },
    };
    uint8_t *whole = malloc(size);
    if (whole) {
        f = fopen(path, "rb");
        bool read_ok = f && fread(whole, 1, size, f) == size;
        if (f)
            fclose(f);
        uint16_t board_id = 0;
        if (read_ok && declrom_identify_vendor(whole, size, "granny-smith", &board_id)) {
            for (size_t i = 0; i < sizeof(gs_boards) / sizeof(gs_boards[0]); i++) {
                if (gs_boards[i].board_id == board_id) {
                    if (out) {
                        out->crc = crc;
                        out->chip_size = size;
                        out->card_id = gs_boards[i].card_id;
                    }
                    free(whole);
                    return VROM_ID_KNOWN;
                }
            }
        }
        free(whole);
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

// Identify one candidate for the registry.  The vROM side's validation gates,
// size classes and identity spans are nothing like the PCI side's, which is
// exactly why this half is NOT shared (offer_registry.h).
static bool vrom_offer_identify(const char *path, uint32_t *out_crc, size_t *out_size, const char **out_card_id) {
    vrom_id_t id;
    if (!vrom_identify_card(path, &id)) {
        // Not a recognised declaration ROM — drop it quietly (the platform
        // offers whole directories; strays are expected, not errors).
        LOG(2, "vrom_offer: '%s' is not a recognised declaration ROM — ignored", path);
        return false;
    }
    *out_crc = id.crc;
    *out_size = id.chip_size;
    *out_card_id = id.card_id;
    return true;
}

static void vrom_catalog_row(size_t r, const char **card_id, uint32_t *crc, bool *preferred) {
    *card_id = VROM_CATALOG[r].card_id;
    *crc = VROM_CATALOG[r].crc;
    *preferred = VROM_CATALOG[r].preferred;
}

static offer_registry_t s_offers = {
    .tag = "vrom_offer",
    .identify = vrom_offer_identify,
    .catalog = {.count = VROM_CATALOG_COUNT, .row = vrom_catalog_row},
};

void vrom_offer(const char *path) {
    offer_registry_add(&s_offers, path, false);
}

void vrom_offer_clear(void) {
    offer_registry_clear(&s_offers);
}

const char *vrom_offer_find(const char *card_id, int idx, size_t *out_chip_size) {
    return offer_registry_find(&s_offers, card_id, idx, out_chip_size);
}

bool vrom_offer_info(const char *path, uint32_t *out_crc, bool *out_explicit) {
    return offer_registry_info(&s_offers, path, out_crc, out_explicit);
}

bool vrom_card_catalogued(const char *card_id) {
    return offer_registry_catalogued(&s_offers, card_id);
}

bool vrom_card_resolvable(const char *card_id) {
    return offer_registry_resolvable(&s_offers, card_id);
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
    offer_registry_add(&s_offers, path, true);
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
    case VROM_ID_WRONG_SIZE: {
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "recognised", val_bool(false));
        return val_map_finish(b);
    }
    case VROM_ID_KNOWN: {
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "recognised", val_bool(true));
        val_map_put(b, "card_id", val_str(id.card_id));
        value_t *compat = NULL;
        size_t n_compat = 0, cap_compat = 0;
        val_list_push(&compat, &n_compat, &cap_compat, val_str(id.card_id));
        val_map_put(b, "compatible", val_list(compat, n_compat));
        val_map_put(b, "size", val_int((int64_t)size));
        char hex[16];
        snprintf(hex, sizeof(hex), "0x%08x", crc);
        val_map_put(b, "crc", val_str(hex));
        return val_map_finish(b);
    }
    case VROM_ID_UNKNOWN:
    default: {
        // Right size but not a known declaration ROM. Recognised=false so
        // callers route into a normal "unrecognised file" path; still report
        // the CRC so a future catalog entry (or the user) can identify it.
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "recognised", val_bool(false));
        val_map_put(b, "size", val_int((int64_t)size));
        char hex[16];
        snprintf(hex, sizeof(hex), "0x%08x", crc);
        val_map_put(b, "crc", val_str(hex));
        return val_map_finish(b);
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
     .doc = "Typed map: {recognised, card_id?, compatible?, size, crc}.",
     .method = {.args = vrom_path_arg, .nargs = 1, .result = V_MAP, .fn = vrom_method_identify}},
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
