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
// made of standard fields only, the way a vROM's is its Format-Block CRC and
// a main ROM's its checksum word -- no emulator-invented hash, and no
// filename ever enters the comparison: the PCIR vendor and device ids (which
// card) and the FCode header's own checksum (which programming of its ROM),
// written "vvvv-dddd-cccc".

#include "prom.h"
#include "common.h"
#include "gs_out.h"
#include "offer_registry.h"

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

// The FCode program header (IEEE 1275 §5.2.2.4), big-endian: a start token,
// a format byte, the checksum (the 16-bit sum of the program's bytes after
// the header) and the program length, header included.
#define FCODE_HEADER_LEN 8
#define FCODE_CHECKSUM   0x02 // BE halfword
#define FCODE_LENGTH     0x04 // BE word

// ============================================================================
// The catalog
// ============================================================================

// Known expansion ROMs, keyed by their identity (vendor, device, FCode
// checksum) and mapped to the pci card-kind id the blob provides.
// Content->hardware facts only, no filenames.  The `preferred` bit marks the
// default revision where one card has several programmings.  Adding a card
// ROM is one row here.
struct prom_known {
    uint16_t vendor_id, device_id, fcode_checksum;
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
    {0x1002, 0x4758, 0xC6E8, "mach64_gx", true }, // -104 (chip CRC-32 $437584E0)
    {0x1002, 0x4758, 0xD71A, "mach64_gx", false}, // -101 (chip CRC-32 $8C68216E)
};

#define PROM_CATALOG_COUNT (sizeof(PROM_CATALOG) / sizeof(PROM_CATALOG[0]))

static uint64_t prom_key(uint16_t vendor, uint16_t device, uint16_t checksum) {
    return ((uint64_t)vendor << 32) | ((uint64_t)device << 16) | checksum;
}

static uint64_t prom_row_key(size_t r) {
    return prom_key(PROM_CATALOG[r].vendor_id, PROM_CATALOG[r].device_id, PROM_CATALOG[r].fcode_checksum);
}

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

// Read the whole file.  Expansion ROMs are at most 256 KB, so there is no
// partial-read path worth having.
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
// Every gate must pass before the identity is trusted: an unrecognised blob
// is DROPPED with a log, never guessed at.
static prom_id_result_t prom_validate(const uint8_t *buf, size_t size, prom_id_t *out) {
    if (!prom_plausible_size(size))
        return PROM_ID_WRONG_SIZE;
    if (buf[PROM_SIG0] != 0x55u || buf[PROM_SIG1] != 0xAAu)
        return PROM_ID_NOT_A_PROM;

    // The PCI Data Structure must lie inside the image and carry 'PCIR'.
    uint32_t pcir = RD_LE16(buf + PROM_PCIR_POINTER);
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
    uint32_t fcode_offset = RD_LE16(buf + 0x02);
    if ((size_t)fcode_offset + 8 > size)
        return PROM_ID_NOT_A_PROM;
    uint8_t start = buf[fcode_offset];
    if (start < 0xF0u || start > 0xF3u) // start0 / start1 / start2 / start4
        return PROM_ID_NOT_A_PROM;
    // The program the header describes must lie inside the image.
    uint32_t fcode_len = RD_BE32(buf + fcode_offset + FCODE_LENGTH);
    if (fcode_len < FCODE_HEADER_LEN || (size_t)fcode_offset + fcode_len > size)
        return PROM_ID_NOT_A_PROM;

    if (out) {
        out->image_size = size;
        out->vendor_id = RD_LE16(buf + pcir + PCIR_VENDOR_ID);
        out->device_id = RD_LE16(buf + pcir + PCIR_DEVICE_ID);
        out->class_code = (uint32_t)buf[pcir + PCIR_CLASS_CODE] | ((uint32_t)buf[pcir + PCIR_CLASS_CODE + 1] << 8) |
                          ((uint32_t)buf[pcir + PCIR_CLASS_CODE + 2] << 16);
        out->fcode_offset = fcode_offset;
        out->fcode_checksum = RD_BE16(buf + fcode_offset + FCODE_CHECKSUM);
        uint16_t sum = 0;
        for (uint32_t i = FCODE_HEADER_LEN; i < fcode_len; i++)
            sum = (uint16_t)(sum + buf[fcode_offset + i]);
        out->intact = sum == out->fcode_checksum;
        out->key = prom_key(out->vendor_id, out->device_id, out->fcode_checksum);
        snprintf(out->id, sizeof(out->id), "%04x-%04x-%04x", out->vendor_id, out->device_id, out->fcode_checksum);
        out->card_id = NULL;
    }
    return PROM_ID_UNKNOWN; // structurally valid; the catalog decides
}

prom_id_result_t prom_identify_detail(const char *path, prom_id_t *out, size_t *out_size) {
    prom_id_t local;
    if (!out)
        out = &local;
    memset(out, 0, sizeof(*out));
    if (out_size)
        *out_size = 0;
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

    free(buf);

    // Nothing is named by its identity unless the program verifies.
    for (size_t i = 0; i < PROM_CATALOG_COUNT; i++) {
        if (prom_row_key(i) != out->key)
            continue;
        out->card_id = PROM_CATALOG[i].card_id;
        return out->intact ? PROM_ID_KNOWN : PROM_ID_DAMAGED;
    }
    return PROM_ID_UNKNOWN;
}

bool prom_identify_card(const char *path, prom_id_t *out) {
    return prom_identify_detail(path, out, NULL) == PROM_ID_KNOWN;
}

// ============================================================================
// Offer registry
// ============================================================================

// Identify one candidate for the registry.  This half stays here: what a
// declaration ROM and a PCI expansion ROM have in common is the registry
// shape, and almost nothing else (offer_registry.h).  The per-failure
// diagnostics live here too, because only the identifier knows WHICH kind of
// stray it just rejected -- and the interesting one is a structurally valid
// ROM we do not catalog.
static bool prom_offer_identify(const char *path, uint64_t *out_key, size_t *out_size, const char **out_card_id) {
    prom_id_t id;
    prom_id_result_t r = prom_identify_detail(path, &id, NULL);
    if (r != PROM_ID_KNOWN) {
        // The platform offers whole directories, so strays are expected
        // rather than errors.
        switch (r) {
        case PROM_ID_NOT_OPEN_FIRMWARE:
            LOG(0,
                "prom_offer: '%s' is a PCI expansion ROM but its code type is $%02X, not $01 "
                "(Open Firmware) — a PC/x86 option ROM cannot drive a Macintosh card; ignored",
                path, (unsigned)id.class_code);
            break;
        case PROM_ID_UNKNOWN:
            LOG(1,
                "prom_offer: '%s' is a valid Open Firmware expansion ROM (id %s) but no catalog row claims it; "
                "ignored",
                path, id.id);
            break;
        case PROM_ID_DAMAGED:
            LOG(0,
                "prom_offer: '%s' looks like the %s expansion ROM (id %s), but its FCode checksum does not verify; "
                "ignored",
                path, id.card_id, id.id);
            break;
        default:
            LOG(2, "prom_offer: '%s' is not a recognised PCI expansion ROM — ignored", path);
            break;
        }
        return false;
    }
    *out_key = id.key;
    *out_size = id.image_size;
    *out_card_id = id.card_id;
    return true;
}

static void prom_catalog_row(size_t r, const char **card_id, uint64_t *key, bool *preferred) {
    *card_id = PROM_CATALOG[r].card_id;
    *key = prom_row_key(r);
    *preferred = PROM_CATALOG[r].preferred;
}

static offer_registry_t s_offers = {
    .tag = "prom_offer",
    .identify = prom_offer_identify,
    .catalog = {.count = PROM_CATALOG_COUNT, .row = prom_catalog_row},
};

void prom_offer(const char *path) {
    offer_registry_add(&s_offers, path);
}

void prom_offer_dir(const char *dir, const char *ext) {
    offer_registry_add_dir(&s_offers, dir, ext);
}

void prom_offer_clear(void) {
    offer_registry_clear(&s_offers);
}

const char *prom_offer_find(const char *card_id, int idx, size_t *out_size) {
    return offer_registry_find(&s_offers, card_id, idx, out_size, NULL);
}

bool prom_card_resolvable(const char *card_id, const char *rom) {
    if (rom && *rom) {
        prom_id_t id;
        return prom_identify_card(rom, &id) && strcmp(id.card_id, card_id) == 0;
    }
    return offer_registry_resolvable(&s_offers, card_id);
}

bool prom_load_card(const char *card_id, const char *rom, uint8_t **out_buf, size_t *out_size, char **out_path) {
    if (out_buf)
        *out_buf = NULL;
    if (out_size)
        *out_size = 0;
    if (out_path)
        *out_path = NULL;
    // The slot's own file is the only candidate when the document names one
    // (machine_boot_apply checked it provides this card).
    for (int idx = 0;; idx++) {
        size_t declared = 0;
        const char *path = (rom && *rom) ? (idx == 0 ? rom : NULL) : prom_offer_find(card_id, idx, &declared);
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

// catalog.proms.offer(path) — the platform/UI hook into the registry, so an upload
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

// catalog.proms.identify(path) — a typed map of content facts, mirroring
// catalog.vroms.identify and rom.identify:
//   { "recognised": bool, "card_id"?, "compatible"?, "vendor_id"?,
//     "device_id"?, "id"?, "intact"?, "size", "reason"? }
// `id` is the identity ("vvvv-dddd-cccc") of any structurally valid Open
// Firmware ROM, recognised or not; `intact` whether its FCode checksum
// verifies.
static value_t prom_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    prom_id_t id;
    size_t size = 0;
    prom_id_result_t r = prom_identify_detail(path, &id, &size);
    if (r == PROM_ID_UNREADABLE)
        return val_err("catalog.proms.identify: cannot read '%s'", path);

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
        case PROM_ID_DAMAGED:
            why = "a known expansion ROM, but its FCode checksum does not verify (the dump is probably damaged)";
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
    if (id.id[0]) {
        val_map_put(b, "id", val_str(id.id));
        val_map_put(b, "intact", val_bool(id.intact));
    }
    val_map_put(b, "size", val_int((int64_t)size));
    return val_map_finish(b);
}

static const arg_decl_t prom_path_arg[] = {
    {.name = "path", .kind = V_STRING, .presentation_flags = VAL_PATH, .doc = "PCI expansion-ROM file path"},
};

static const member_t prom_members[] = {
    {.kind = M_METHOD,
     .name = "offer",
     .doc = "Offer a candidate expansion ROM; true iff recognised and registered",
     .method = {.args = prom_path_arg, .nargs = 1, .result = V_BOOL, .fn = prom_method_offer}  },
    {.kind = M_METHOD,
     .name = "identify",
     .doc = "Typed map: {recognised, card_id?, compatible?, vendor_id?, device_id?, id?, intact?, size, reason?}.",
     .method = {.args = prom_path_arg, .nargs = 1, .result = V_MAP, .fn = prom_method_identify}},
};

static const class_desc_t prom_class = {
    .name = "prom",
    .members = prom_members,
    .n_members = sizeof(prom_members) / sizeof(prom_members[0]),
    .doc = "Registry of PCI expansion ROM files",
};

// ============================================================================
// Lifecycle
// ============================================================================

static struct object *s_prom_object = NULL;

void prom_init(struct object *parent) {
    if (s_prom_object)
        return;
    s_prom_object = object_new(&prom_class, NULL, "proms");
    if (s_prom_object) {
        object_set_label(s_prom_object, "PCI expansion ROMs");
        object_set_order(s_prom_object, 21);
        object_set_category(s_prom_object, M_CAT_ADVANCED);
        object_attach(parent, s_prom_object);
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
