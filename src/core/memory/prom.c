// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// prom.c
// PCI expansion-ROM content identification, the pre-boot offer registry,
// and the prom.* object-model surface.  See prom.h for the contract and
// why this is a sibling of vrom.c rather than a generalisation of it.
//
// A PROM is a PCI expansion ROM (PCI 2.x §6.3) carrying an IEEE 1275 FCode
// image: a $55AA signature, a PCI Data Structure ("PCIR") giving the
// vendor/device ids and the code type, and an FCode program.  Identity is
// the CRC-32 of the whole chip image, the way a vROM's identity is its
// Format-Block CRC and a main ROM's is its checksum word — no
// emulator-invented hash, and no filename ever enters the comparison.

#include "prom.h"

#include "log.h"
#include "machine_profile.h"
#include "object.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

LOG_USE_CATEGORY_NAME("prom");

// ============================================================================
// Expansion-ROM structure
// ============================================================================

// Offsets within the image (PCI 2.x §6.3.1).  All multi-byte fields in the
// container are LITTLE-endian; the FCode program inside is big-endian.
#define PROM_SIG0          0x00 // $55
#define PROM_SIG1          0x01 // $AA
#define PROM_PCIR_POINTER  0x18 // LE halfword: offset of the PCI Data Structure
#define PCIR_VENDOR_ID     0x04 // LE halfword
#define PCIR_DEVICE_ID     0x06 // LE halfword
#define PCIR_CLASS_CODE    0x0D // 3 bytes, LE
#define PCIR_IMAGE_LENGTH  0x10 // LE halfword, in 512-byte blocks
#define PCIR_CODE_TYPE     0x14 // $00 = x86 BIOS, $01 = Open Firmware
#define PCIR_MIN_LENGTH    0x18
#define PROM_CODE_TYPE_X86 0x00
#define PROM_CODE_TYPE_OF  0x01

static uint16_t prom_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

// CRC-32 (IEEE), the identity key.  Bit-serial: this runs a handful of
// times per boot over at most 256 KB, so a table would be pure weight.
static uint32_t prom_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

// ============================================================================
// The catalog
// ============================================================================

// Known expansion ROMs, keyed by CRC-32 of the whole chip image and mapped
// to the pci card-kind id the blob provides.  Content->hardware facts only,
// no filenames.  The `preferred` bit marks the default revision where one
// card has several dumps.  Adding a card ROM is one row here.
struct prom_known {
    uint32_t crc;
    size_t image_size; // chip image size on disk
    const char *card_id;
    bool preferred;
};

static const struct prom_known PROM_CATALOG[] = {
    // Apple Accelerated PCI Graphics Card — ATI Mach64 GX ("Spinnaker"),
    // the display card the Power Macintosh 9500 shipped with.  Both
    // revisions declare device-name "ATY,mach64" and publish an ndrv as
    // `driver,AAPL,MacOS,PowerPC`, so Mac OS drives the card with nothing
    // installed from disk.
    //
    // -104 is the shipping revision: it carries real Apple part numbers in
    // its ATY,Rom#/Mem#/Card# properties, and two independent archives
    // hold it byte-identically (it is also dumped as -004).  -101 is an
    // earlier programming whose part-number strings are all
    // "000-00000-000"; kept because it is a distinct dump, not preferred.
    {0x437584E0u, 0x8000, "mach64_gx", true },
    {0x8C68216Eu, 0x8000, "mach64_gx", false},
};

#define PROM_CATALOG_COUNT (sizeof(PROM_CATALOG) / sizeof(PROM_CATALOG[0]))

// ============================================================================
// Identification
// ============================================================================

// A plausible chip image: a power of two in the range real expansion ROMs
// occupy.  The declared IMAGE length inside is independent of this — a
// 31 232-byte image lives happily on a 32 KB chip — so the file size is
// gated on the chip, not on the image.
static bool prom_plausible_size(size_t size) {
    if (size < PROM_MIN_SIZE || size > PROM_MAX_SIZE)
        return false;
    return (size & (size - 1)) == 0;
}

// Read the whole file.  Expansion ROMs are at most 256 KB, and the CRC
// covers all of it, so there is no partial-read path worth having.
static uint8_t *prom_read_file(const char *path, size_t *out_size) {
    *out_size = 0;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0)
        return NULL;
    size_t size = (size_t)st.st_size;
    if (size > PROM_MAX_SIZE)
        return NULL; // refuse to slurp something that cannot be a ROM
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    bool ok = fread(buf, 1, size, f) == size;
    fclose(f);
    if (!ok) {
        free(buf);
        return NULL;
    }
    *out_size = size;
    return buf;
}

// Structural validation, in the order that makes a rejection informative.
// Every gate must pass before the CRC is trusted: an unrecognised blob is
// DROPPED with a log, never guessed at.
static prom_id_result_t prom_validate(const uint8_t *buf, size_t size, prom_id_t *out) {
    if (!prom_plausible_size(size))
        return PROM_ID_WRONG_SIZE;
    if (buf[PROM_SIG0] != 0x55u || buf[PROM_SIG1] != 0xAAu)
        return PROM_ID_NOT_A_PROM;

    // The PCI Data Structure must lie inside the image and carry 'PCIR'.
    uint32_t pcir = prom_le16(buf + PROM_PCIR_POINTER);
    if ((size_t)pcir + PCIR_MIN_LENGTH > size)
        return PROM_ID_NOT_A_PROM;
    if (memcmp(buf + pcir, "PCIR", 4) != 0)
        return PROM_ID_NOT_A_PROM;

    uint8_t code_type = buf[pcir + PCIR_CODE_TYPE];
    if (code_type != PROM_CODE_TYPE_OF) {
        // Recognised as a real expansion ROM, just not one this machine can
        // execute.  Reported separately because "I flashed the ROM off a PC
        // Mach64" is the predictable user error and deserves to be told
        // apart from "that file is not a ROM at all".
        if (out)
            out->class_code = code_type;
        return PROM_ID_NOT_OPEN_FIRMWARE;
    }

    // The FCode program starts where the image's own halfword at $02 says,
    // and must begin with a start token (1275 §5.2.2.4).
    uint32_t fcode_offset = prom_le16(buf + 0x02);
    if ((size_t)fcode_offset + 8 > size)
        return PROM_ID_NOT_A_PROM;
    uint8_t start = buf[fcode_offset];
    if (start < 0xF0u || start > 0xF3u) // start0 / start1 / start2 / start4
        return PROM_ID_NOT_A_PROM;

    if (out) {
        out->image_size = size;
        out->vendor_id = prom_le16(buf + pcir + PCIR_VENDOR_ID);
        out->device_id = prom_le16(buf + pcir + PCIR_DEVICE_ID);
        out->class_code = (uint32_t)buf[pcir + PCIR_CLASS_CODE] | ((uint32_t)buf[pcir + PCIR_CLASS_CODE + 1] << 8) |
                          ((uint32_t)buf[pcir + PCIR_CLASS_CODE + 2] << 16);
        out->fcode_offset = fcode_offset;
        out->card_id = NULL;
    }
    return PROM_ID_UNKNOWN; // structurally valid; the catalog decides
}

prom_id_result_t prom_identify_detail(const char *path, prom_id_t *out, size_t *out_size, uint32_t *out_crc) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (out_size)
        *out_size = 0;
    if (out_crc)
        *out_crc = 0;
    if (!path || !*path)
        return PROM_ID_UNREADABLE;

    size_t size = 0;
    uint8_t *buf = prom_read_file(path, &size);
    if (!buf) {
        // Distinguish "cannot read" from "present but implausible": a file
        // that exists and is simply the wrong size is not an error.
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0) {
            if (out_size)
                *out_size = (size_t)st.st_size;
            return PROM_ID_WRONG_SIZE;
        }
        return PROM_ID_UNREADABLE;
    }
    if (out_size)
        *out_size = size;

    prom_id_result_t r = prom_validate(buf, size, out);
    if (r != PROM_ID_UNKNOWN) {
        free(buf);
        return r;
    }

    uint32_t crc = prom_crc32(buf, size);
    free(buf);
    if (out_crc)
        *out_crc = crc;
    if (out)
        out->crc = crc;

    for (size_t i = 0; i < PROM_CATALOG_COUNT; i++) {
        if (PROM_CATALOG[i].crc != crc)
            continue;
        if (out)
            out->card_id = PROM_CATALOG[i].card_id;
        return PROM_ID_KNOWN;
    }
    return PROM_ID_UNKNOWN;
}

bool prom_identify_card(const char *path, prom_id_t *out) {
    return prom_identify_detail(path, out, NULL, NULL) == PROM_ID_KNOWN;
}

// ============================================================================
// Offer registry
// ============================================================================

// One registered candidate, keyed by its content identity.
struct prom_offer_entry {
    uint32_t crc;
    size_t image_size;
    const char *card_id; // catalog row (static storage)
    char *path; // opaque locator (owned)
    bool explicit_pick; // the boot document's prom= pick
};

static struct prom_offer_entry *s_offers = NULL;
static size_t s_offer_count = 0;
static size_t s_offer_cap = 0;

static void prom_offer_add(const char *path, bool explicit_pick) {
    if (!path || !*path)
        return;
    prom_id_t id;
    prom_id_result_t r = prom_identify_detail(path, &id, NULL, NULL);
    if (r != PROM_ID_KNOWN) {
        // The platform offers whole directories, so strays are expected
        // rather than errors — but say WHICH kind of stray, because the
        // interesting one is a structurally valid ROM we do not catalog.
        switch (r) {
        case PROM_ID_NOT_OPEN_FIRMWARE:
            LOG(0,
                "prom_offer: '%s' is a PCI expansion ROM but its code type is $%02X, not $01 "
                "(Open Firmware) — a PC/x86 option ROM cannot drive a Macintosh card; ignored",
                path, (unsigned)id.class_code);
            break;
        case PROM_ID_UNKNOWN:
            LOG(1,
                "prom_offer: '%s' is a valid Open Firmware expansion ROM (vendor $%04X device $%04X, crc $%08X) "
                "but no catalog row claims it; ignored",
                path, id.vendor_id, id.device_id, id.crc);
            break;
        default:
            LOG(2, "prom_offer: '%s' is not a recognised PCI expansion ROM — ignored", path);
            break;
        }
        return;
    }
    // Idempotent by content: one entry per CRC.  A re-offer refreshes the
    // path (the newest locator for these bytes) and may promote to explicit.
    struct prom_offer_entry *e = NULL;
    for (size_t i = 0; i < s_offer_count; i++) {
        if (s_offers[i].crc == id.crc) {
            e = &s_offers[i];
            break;
        }
    }
    if (!e) {
        if (s_offer_count == s_offer_cap) {
            size_t cap = s_offer_cap ? s_offer_cap * 2 : 8;
            struct prom_offer_entry *grown = realloc(s_offers, cap * sizeof(*grown));
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
        if (!e->path)
            s_offer_count--; // fresh entry with no path is useless
        return;
    }
    free(e->path);
    e->path = dup;
    e->crc = id.crc;
    e->image_size = id.image_size;
    e->card_id = id.card_id;
    if (explicit_pick) {
        for (size_t i = 0; i < s_offer_count; i++)
            s_offers[i].explicit_pick = false; // latest pick wins
        e->explicit_pick = true;
    }
    LOG(2, "prom_offer: '%s' provides card '%s' (crc $%08X)%s", path, e->card_id, e->crc,
        explicit_pick ? " [explicit]" : "");
}

void prom_offer(const char *path) {
    prom_offer_add(path, false);
}

void prom_offer_clear(void) {
    for (size_t i = 0; i < s_offer_count; i++)
        free(s_offers[i].path);
    free(s_offers);
    s_offers = NULL;
    s_offer_count = 0;
    s_offer_cap = 0;
}

const char *prom_offer_find(const char *card_id, int idx, size_t *out_size) {
    if (!card_id)
        return NULL;
    // Pick order: the explicit pick first, then catalog rows with the
    // `preferred` bit, then the remaining rows in catalog order.  All
    // content-based — no filename enters the comparison.
    for (size_t i = 0; i < s_offer_count; i++) {
        if (!s_offers[i].explicit_pick || strcmp(s_offers[i].card_id, card_id) != 0)
            continue;
        if (idx-- == 0) {
            if (out_size)
                *out_size = s_offers[i].image_size;
            return s_offers[i].path;
        }
    }
    for (int want_preferred = 1; want_preferred >= 0; want_preferred--) {
        for (size_t r = 0; r < PROM_CATALOG_COUNT; r++) {
            if (PROM_CATALOG[r].preferred != (bool)want_preferred)
                continue;
            if (strcmp(PROM_CATALOG[r].card_id, card_id) != 0)
                continue;
            for (size_t i = 0; i < s_offer_count; i++) {
                if (s_offers[i].crc != PROM_CATALOG[r].crc || s_offers[i].explicit_pick)
                    continue; // the explicit entry was yielded above
                if (idx-- == 0) {
                    if (out_size)
                        *out_size = s_offers[i].image_size;
                    return s_offers[i].path;
                }
            }
        }
    }
    return NULL;
}

bool prom_offer_info(const char *path, uint32_t *out_crc, bool *out_explicit) {
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

bool prom_card_catalogued(const char *card_id) {
    if (!card_id || !*card_id)
        return false;
    for (size_t r = 0; r < PROM_CATALOG_COUNT; r++) {
        if (strcmp(PROM_CATALOG[r].card_id, card_id) == 0)
            return true;
    }
    return false;
}

bool prom_card_resolvable(const char *card_id) {
    return prom_offer_find(card_id, 0, NULL) != NULL;
}

int prom_set_path(const char *path) {
    if (!path || !*path) {
        printf("prom: expected a non-empty path\n");
        return -1;
    }
    prom_offer_add(path, true);
    return 0;
}

bool prom_load_card(const char *card_id, uint8_t **out_buf, size_t *out_size, char **out_path) {
    if (out_buf)
        *out_buf = NULL;
    if (out_size)
        *out_size = 0;
    if (out_path)
        *out_path = NULL;
    for (int idx = 0;; idx++) {
        size_t declared = 0;
        const char *path = prom_offer_find(card_id, idx, &declared);
        if (!path)
            break;
        size_t size = 0;
        uint8_t *buf = prom_read_file(path, &size);
        if (!buf) {
            // The file was readable when it was offered and is not now.
            LOG(0, "prom_load_card('%s'): '%s' can no longer be read; trying the next candidate", card_id, path);
            continue;
        }
        if (out_buf)
            *out_buf = buf;
        else
            free(buf);
        if (out_size)
            *out_size = size;
        if (out_path)
            *out_path = strdup(path);
        LOG(1, "card '%s' takes its expansion ROM from '%s' (%zu bytes)", card_id, path, size);
        return true;
    }
    LOG(0, "card '%s' needs a PCI expansion ROM but no offered .prom file provides it", card_id);
    return false;
}

// ============================================================================
// Object-model class descriptor
// ============================================================================

// prom.offer(path) — the platform/UI hook into the registry, so an upload
// ingest can offer a freshly stored file without a reload.  True iff the
// file was recognised and registered; false is "not a PROM", not an error.
static value_t prom_method_offer(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    prom_id_t id;
    bool recognised = prom_identify_card(argv[0].s, &id);
    if (recognised)
        prom_offer(argv[0].s);
    return val_bool(recognised);
}

// prom.identify(path) — a typed map of content facts, mirroring
// vrom.identify and rom.identify:
//   { "recognised": bool, "card_id"?, "compatible"?, "vendor_id"?,
//     "device_id"?, "size", "crc", "reason"? }
static value_t prom_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    prom_id_t id;
    size_t size = 0;
    uint32_t crc = 0;
    prom_id_result_t r = prom_identify_detail(path, &id, &size, &crc);
    if (r == PROM_ID_UNREADABLE)
        return val_err("prom.identify: cannot read '%s'", path);

    value_map_builder_t *b = val_map_new();
    val_map_put(b, "recognised", val_bool(r == PROM_ID_KNOWN));
    if (r == PROM_ID_KNOWN) {
        val_map_put(b, "card_id", val_str(id.card_id));
        value_t *compat = NULL;
        size_t n_compat = 0, cap_compat = 0;
        val_list_push(&compat, &n_compat, &cap_compat, val_str(id.card_id));
        val_map_put(b, "compatible", val_list(compat, n_compat));
    } else {
        // Say WHY, so a user who flashed the wrong image is told what they
        // actually have rather than just "no".
        const char *why = "not a PCI expansion ROM";
        switch (r) {
        case PROM_ID_WRONG_SIZE:
            why = "not a plausible expansion-ROM chip size";
            break;
        case PROM_ID_NOT_OPEN_FIRMWARE:
            why = "an expansion ROM, but its code type is not Open Firmware "
                  "(a PC/x86 option ROM cannot drive a Macintosh card)";
            break;
        case PROM_ID_UNKNOWN:
            why = "a valid Open Firmware expansion ROM, but no catalog row claims it";
            break;
        default:
            break;
        }
        val_map_put(b, "reason", val_str(why));
    }
    if (id.vendor_id || id.device_id) {
        val_map_put(b, "vendor_id", val_uint(2, id.vendor_id));
        val_map_put(b, "device_id", val_uint(2, id.device_id));
    }
    val_map_put(b, "size", val_int((int64_t)size));
    char hex[16];
    snprintf(hex, sizeof(hex), "0x%08x", crc);
    val_map_put(b, "crc", val_str(hex));
    return val_map_finish(b);
}

static const arg_decl_t prom_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "PCI expansion-ROM file path"},
};

static const member_t prom_members[] = {
    {.kind = M_METHOD,
     .name = "offer",
     .doc = "Offer a candidate expansion ROM; true iff recognised and registered",
     .method = {.args = prom_path_arg, .nargs = 1, .result = V_BOOL, .fn = prom_method_offer}  },
    {.kind = M_METHOD,
     .name = "identify",
     .doc = "Typed map: {recognised, card_id?, compatible?, vendor_id?, device_id?, size, crc, reason?}.",
     .method = {.args = prom_path_arg, .nargs = 1, .result = V_MAP, .fn = prom_method_identify}},
};

const class_desc_t prom_class = {
    .name = "prom",
    .members = prom_members,
    .n_members = sizeof(prom_members) / sizeof(prom_members[0]),
};

// ============================================================================
// Lifecycle
// ============================================================================

static struct object *s_prom_object = NULL;

void prom_init(void) {
    if (s_prom_object)
        return;
    s_prom_object = object_new(&prom_class, NULL, "prom");
    if (s_prom_object) {
        object_set_label(s_prom_object, "PCI expansion ROM");
        object_set_order(s_prom_object, 96); // beside vrom (95)
        object_attach(machine_object(), s_prom_object);
    }
}

void prom_delete(void) {
    if (s_prom_object) {
        object_detach(s_prom_object);
        object_delete(s_prom_object);
        s_prom_object = NULL;
    }
    prom_offer_clear();
}
