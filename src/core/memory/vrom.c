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
#include "out.h"

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

// The declaration-ROM chip sizes the catalog holds: 32 KB (SE/30, JMFB, 24AC,
// the 8•24 GC v1.0 / alpha) and 64 KB (the 8•24 GC v1.1).  Facts of these
// cards' chips, private to the identifier -- a card factory that loads a ROM
// learns its size from the identification, not from a constant.
#define VROM_CHIP_32K (32u * 1024u)
#define VROM_CHIP_64K (64u * 1024u)

// Size of the file at `path` in bytes; 0 when it is missing or unreadable.
// Only a size: the content checks are vrom_identify_image's.
static size_t vrom_file_size(const char *path) {
    if (!path || !*path)
        return 0;
    // stat is portable for binary-file sizing; fseek(SEEK_END)+ftell on a
    // binary stream is implementation-defined per ISO C.
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0)
        return 0;
    return (size_t)st.st_size;
}

// NuBus declaration-ROM Format Block CRC.  Every declaration ROM — the
// genuine cards and the "fake" SE/30 onboard-video ROM alike — carries a
// 20-byte Format Block at the top of the dense chip image; its 4-byte CRC
// (preceded by the `$5A932BC7` TestPattern) is the intrinsic, Slot-Manager-
// validated checksum.  See docs/reference/hardware/nubus/declaration-rom.md §2.  We read
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
// no filenames (a file's name is never evidence of what it is).  The id is the machine-readable link a UI uses to
// pick the card (a `slots=` entry's card id); the human label is owned by the
// card kind (nubus_card_find(id)->display_name) so it never drifts.  The
// `preferred` bit marks the default revision when one card has several ROMs.
// Adding a new VROM = one row here.  Keyed by content, like rom_table.c's
// {id -> ...}.
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

// Content-identification core shared by catalog.vroms.identify, the offer registry,
// and the card factories' loader (declrom_load_vrom_card).  Reads the file's
// trailing Format Block, gates on the $5A932BC7 TestPattern, and looks the
// CRC up in the catalog.  Result codes let catalog.vroms.identify keep its
// error/unrecognised distinction.
enum vrom_id_result {
    VROM_ID_UNREADABLE, // stat/open/read failed
    VROM_ID_WRONG_SIZE, // exists, but not a chip-sized blob
    VROM_ID_UNKNOWN, // right size; *out_crc valid; not a catalog entry (or no TestPattern)
    VROM_ID_KNOWN, // recognised: *out filled from the catalog row
};
// Identify a whole chip image already in memory: gate on size and the
// TestPattern, look the CRC up in the catalog, else recognise one of our own
// generated images by its board sResource.
static enum vrom_id_result vrom_identify_image(const uint8_t *img, size_t size, vrom_id_t *out, uint32_t *out_crc) {
    // Declaration-ROM chips come in two sizes: 32 KB (SE/30, JMFB, 24AC, the
    // 8•24 GC v1.0 / alpha) and 64 KB (the 8•24 GC v1.1).  The Format Block +
    // CRC live in the trailing bytes either way, so accept both.
    if (size != VROM_CHIP_32K && size != VROM_CHIP_64K)
        return VROM_ID_WRONG_SIZE;
    // Only the trailing Format Block matters for identity.
    const uint8_t *tail = img + size - VROM_CRC_OFF;

    // Gate on the TestPattern: a right-sized blob without `$5A932BC7` in the
    // Format Block is not a declaration ROM (don't trust a stray CRC match).
    const uint8_t *tp = tail + VROM_CRC_OFF - VROM_TESTPATTERN_OFF;
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
    // own GENERATED substitute images (the card's, a ROM for that card)
    // structurally: the runtime-generated GS
    // vROM has no fixed CRC to match (its content varies with the mode
    // set and the toolchain that assembled the fragments), so identity is
    // the board sResource's "granny-smith" VendorId plus its BoardId.
    static const struct {
        uint16_t board_id;
        const char *card_id;
    } vrom_boards[] = {
        {0x0027, "mdc_8_24"          },
        {0x05FA, "display_card_24ac" },
        {0x002C, "824gc"             },
        {0x000C, "builtin_se30_video"},
    };
    uint16_t board_id = 0;
    if (declrom_identify_vendor(img, size, "granny-smith", &board_id)) {
        for (size_t i = 0; i < sizeof(vrom_boards) / sizeof(vrom_boards[0]); i++) {
            if (vrom_boards[i].board_id == board_id) {
                if (out) {
                    out->crc = crc;
                    out->chip_size = size;
                    out->card_id = vrom_boards[i].card_id;
                }
                return VROM_ID_KNOWN;
            }
        }
    }
    return VROM_ID_UNKNOWN;
}

// The file form: read the chip and identify it.
static enum vrom_id_result vrom_identify_core(const char *path, vrom_id_t *out, size_t *out_size, uint32_t *out_crc) {
    size_t size = vrom_file_size(path);
    if (out_size)
        *out_size = size;
    // Zero only when stat failed (missing / unreadable).
    if (size == 0)
        return VROM_ID_UNREADABLE;
    if (size != VROM_CHIP_32K && size != VROM_CHIP_64K)
        return VROM_ID_WRONG_SIZE;
    uint8_t *img = malloc(size);
    if (!img)
        return VROM_ID_UNREADABLE;
    FILE *f = fopen(path, "rb");
    bool read_ok = f && fread(img, 1, size, f) == size;
    if (f)
        fclose(f);
    enum vrom_id_result r = read_ok ? vrom_identify_image(img, size, out, out_crc) : VROM_ID_UNREADABLE;
    free(img);
    return r;
}

bool vrom_identify_bytes(const uint8_t *img, size_t size, vrom_id_t *out) {
    return img && vrom_identify_image(img, size, out, NULL) == VROM_ID_KNOWN;
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
static bool vrom_offer_identify(const char *path, uint64_t *out_key, size_t *out_size, const char **out_card_id) {
    vrom_id_t id;
    if (!vrom_identify_card(path, &id)) {
        // Not a recognised declaration ROM — drop it quietly (the platform
        // offers whole directories; strays are expected, not errors).
        LOG(2, "vrom_offer: '%s' is not a recognised declaration ROM — ignored", path);
        return false;
    }
    *out_key = id.crc; // a declaration ROM's identity is its Format-Block CRC
    *out_size = id.chip_size;
    *out_card_id = id.card_id;
    return true;
}

static void vrom_catalog_row(size_t r, const char **card_id, uint64_t *key, bool *preferred) {
    *card_id = VROM_CATALOG[r].card_id;
    *key = VROM_CATALOG[r].crc;
    *preferred = VROM_CATALOG[r].preferred;
}

static offer_registry_t s_offers = {
    .tag = "vrom_offer",
    .identify = vrom_offer_identify,
    .catalog = {.count = VROM_CATALOG_COUNT, .row = vrom_catalog_row},
};

void vrom_offer(const char *path) {
    offer_registry_add(&s_offers, path);
}

void vrom_offer_dir(const char *dir, const char *ext) {
    offer_registry_add_dir(&s_offers, dir, ext);
}

void vrom_offer_clear(void) {
    offer_registry_clear(&s_offers);
}

const char *vrom_offer_find(const char *card_id, int idx, size_t *out_chip_size, uint32_t *out_crc) {
    uint64_t key = 0;
    const char *path = offer_registry_find(&s_offers, card_id, idx, out_chip_size, &key);
    if (out_crc)
        *out_crc = (uint32_t)key;
    return path;
}

bool vrom_card_catalogued(const char *card_id) {
    return offer_registry_catalogued(&s_offers, card_id);
}

bool vrom_card_resolvable(const char *card_id, const char *rom) {
    if (rom && *rom) {
        vrom_id_t id;
        return vrom_identify_card(rom, &id) && strcmp(id.card_id, card_id) == 0;
    }
    return offer_registry_resolvable(&s_offers, card_id);
}

// ============================================================================
// Object-model class descriptor
// ============================================================================

static DEF_GETTER(vrom_attr_size) {
    return val_uint(4, VROM_CHIP_32K);
}

// catalog.vroms.offer(path) — platform/UI hook into the offer registry, e.g. web2's
// upload ingest offering a freshly stored file so an "(auto)" boot sees it
// without a page reload.  Returns true iff the file was recognised and
// registered; false is "not a vROM", not an error.
static DEF_METHOD(vrom_method_offer) {
    vrom_id_t id;
    bool recognised = vrom_identify_card(argv[0].s, &id);
    if (recognised)
        vrom_offer(argv[0].s);
    return val_bool(recognised);
}

// catalog.vroms.identify(path) — returns a JSON map of content facts describing the
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
// human-readable name comes from catalog.profile, not here.  (The card
// factories load by CONTENT — declrom_load_vrom_card — so the on-disk name
// never matters.)
static DEF_METHOD(vrom_method_identify) {
    const char *path = argv[0].s;
    vrom_id_t id;
    size_t size = 0;
    uint32_t crc = 0;
    switch (vrom_identify_core(path, &id, &size, &crc)) {
    case VROM_ID_UNREADABLE:
        // Distinguish "can't read the file" from "present, but not a vROM",
        // mirroring rom.identify: a missing/unreadable path is a VK_ERROR,
        // while a real file of the wrong size is simply unrecognised.
        return val_err("catalog.vroms.identify: cannot read '%s'", path);
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
    {.name = "path", .kind = VK_STRING, .presentation_flags = VFLAG_PATH, .doc = "VROM file path"},
};

static const member_t vrom_members[] = {
    {.kind = MK_ATTR,
     .name = "size",
     .doc = "Expected VROM size in bytes (32 KB)",
     .attr = {.type = VK_UINT, .get = vrom_attr_size, .set = NULL}                              },
    {.kind = MK_METHOD,
     .name = "offer",
     .doc = "Offer a candidate VROM file; true iff recognised and registered",
     .method = {.args = vrom_path_arg, .nargs = 1, .result = VK_BOOL, .fn = vrom_method_offer}  },
    {.kind = MK_METHOD,
     .name = "identify",
     .doc = "Typed map: {recognised, card_id?, compatible?, size, crc}.",
     .method = {.args = vrom_path_arg, .nargs = 1, .result = VK_MAP, .fn = vrom_method_identify}},
};

static const class_desc_t vrom_class = {
    .name = "vrom",
    .members = vrom_members,
    .n_members = sizeof(vrom_members) / sizeof(vrom_members[0]),
    .doc = "Registry of video (NuBus declaration) ROM files",
};

// ============================================================================
// Lifecycle
// ============================================================================

static struct object *s_vrom_object = NULL;

void vrom_init(struct object *parent) {
    if (s_vrom_object)
        return;
    s_vrom_object = object_new(&vrom_class, NULL, "vroms");
    if (s_vrom_object) {
        object_set_label(s_vrom_object, "Video ROMs");
        object_set_order(s_vrom_object, 20);
        object_set_category(s_vrom_object, M_CAT_ADVANCED);
        object_attach(parent, s_vrom_object);
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
