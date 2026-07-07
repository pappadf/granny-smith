// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vrom.c
// VROM file handling and the vrom.* object-model surface.
//
// VROM is a 32 KB blob the SE/30 needs alongside the main ROM. Because the
// main ROM is only loaded after machine creation and the VROM has to be
// available during machine init, the design is: vrom.load(path) just
// records a pending path; se30 init reads pending_path() and loads bytes
// directly into video RAM.

#include "vrom.h"

#include "machine_profile.h"
#include "object.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

// ============================================================================
// Pending-path tracking
// ============================================================================

static char *s_pending_vrom_path = NULL;

int vrom_set_path(const char *path) {
    if (!path || !*path) {
        printf("vrom.load: expected a non-empty path\n");
        return -1;
    }
    char *dup = strdup(path);
    if (!dup)
        return -1;
    free(s_pending_vrom_path);
    s_pending_vrom_path = dup;
    // Note: no log line here on success — the path is just stashed for later
    // consumption by machine init; the user gets `true` back from vrom.load().
    return 0;
}

const char *vrom_pending_path(void) {
    return s_pending_vrom_path;
}

void vrom_pending_clear(void) {
    free(s_pending_vrom_path);
    s_pending_vrom_path = NULL;
}

// ============================================================================
// Object-model class descriptor
// ============================================================================

static value_t vrom_attr_path(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const char *p = vrom_pending_path();
    return val_str(p ? p : "");
}

static value_t vrom_attr_loaded(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_bool(vrom_pending_path() != NULL);
}

static value_t vrom_attr_size(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, VROM_EXPECTED_SIZE);
}

static value_t vrom_method_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    if (vrom_set_path(argv[0].s) != 0)
        return val_err("vrom.load: failed");
    return val_bool(true);
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
// to the canonical on-disk filename the C-side card factories expect (e.g.
// jmfb.c hardcodes "Apple-341-0868.vrom") and the nubus card-kind id the
// blob provides.  The id is the machine-readable link a UI uses to pick the
// card (machine.nubus.video_card); the human label is owned by the card kind
// (nubus_card_find(id)->display_name) so it never drifts.  Adding a new VROM
// = one row here.  Keyed exactly like rom.c's ROM_TABLE {checksum -> ...}.
struct vrom_known {
    uint32_t crc;
    const char *canonical_name;
    const char *card_id;
};
static const struct vrom_known VROM_CATALOG[] = {
    // Macintosh Display Card 8•24 (ROM rev 341-0868). Drives the JMFB
    // NuBus card (id "mdc_8_24") on IIcx / IIx / IIfx; loaded by jmfb.c.
    {0xD1629664u, "Apple-341-0868.vrom",    "mdc_8_24"          },
    // SE/30 onboard video declaration ROM (byteLanes $0F, 4-lane); loaded
    // by builtin_se30_video.c (id "builtin_se30_video").
    {0x4F71FF1Au, "SE30.vrom",              "builtin_se30_video"},
    // Apple Macintosh Display Card 24AC (id "display_card_24ac").
    {0xD8DAAB87u, "display-card-24ac.vrom", "display_card_24ac" },
    // Apple Macintosh Display Card 8•24 GC ("Dolphin", id "824gc"): the
    // accelerated card.  Its declaration ROMs are byteLanes $E1 (byte lane 0);
    // v1.1 is a 64 KB chip, v1.0 / the alpha are 32 KB.  All three carry the
    // same $5A932BC7 TestPattern, so they identify by Format-Block CRC.
    {0xD722B053u, "Apple-341-0266.vrom",    "824gc"             }, // v1.1 (default)
    {0x9E9857E8u, "341-0812-02_1.0.vrom",   "824gc"             }, // v1.0 (shipping)
    {0x4740028Du, "Dolphin_1.0A16.vrom",    "824gc"             }, // 1.00a16 alpha
};

// vrom.identify(path) — returns a JSON map describing the file, keyed off
// the declaration ROM's Format-Block CRC:
//   {
//     "recognised":     bool,
//     "canonical_name": "display-card-24ac.vrom",     // present when recognised
//     "card_id":        "display_card_24ac",          // nubus card-kind id
//     "compatible":     ["display_card_24ac"],         // card ids this blob can drive
//     "size":           32768,
//     "crc":            "0xd8daab87"
//   }
// `compatible` mirrors rom.identify's `compatible:[model_ids]` shape (a list,
// usually length 1).  JS callers use canonical_name to persist the file under
// the path the C-side card factories expect, and card_id / compatible to pick
// the card; the human-readable name comes from machine.profile, not here.
static value_t vrom_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    size_t size = 0;
    vrom_probe_file(path, &size); // fills *size regardless of SE/30's 32 KB gate
    // Distinguish "can't read the file" from "present, but not a vROM",
    // mirroring rom.identify: a missing/unreadable path is a V_ERROR, while a
    // real file of the wrong size is simply unrecognised.  vrom_probe_file
    // leaves *size == 0 only when stat failed (nonexistent / unreadable).
    if (size == 0)
        return val_err("vrom.identify: cannot read '%s'", path);
    // Declaration-ROM chips come in two sizes: 32 KB (SE/30, JMFB, 24AC, the
    // 8•24 GC v1.0 / alpha) and 64 KB (the 8•24 GC v1.1).  The Format Block +
    // CRC live in the trailing bytes either way, so accept both.
    bool size_ok = (size == VROM_EXPECTED_SIZE) || (size == 2u * VROM_EXPECTED_SIZE);
    if (!size_ok)
        return val_str("{\"recognised\":false}");

    // size == VROM_EXPECTED_SIZE (32 KB) from here; slurp the chip image —
    // the Format Block is its trailing 20 bytes.
    FILE *f = fopen(path, "rb");
    if (!f)
        return val_err("vrom.identify: cannot open '%s'", path);
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        fclose(f);
        return val_err("vrom.identify: out of memory");
    }
    size_t got = fread(buf, 1, size, f);
    fclose(f);
    if (got != size) {
        free(buf);
        return val_err("vrom.identify: short read from '%s'", path);
    }

    // Gate on the TestPattern: a right-sized blob without `$5A932BC7` in the
    // Format Block is not a declaration ROM (don't trust a stray CRC match).
    const uint8_t *tp = buf + size - VROM_TESTPATTERN_OFF;
    bool is_declrom = tp[0] == 0x5Au && tp[1] == 0x93u && tp[2] == 0x2Bu && tp[3] == 0xC7u;
    uint32_t crc = vrom_be32(buf + size - VROM_CRC_OFF);
    free(buf);

    if (is_declrom) {
        for (size_t i = 0; i < sizeof(VROM_CATALOG) / sizeof(VROM_CATALOG[0]); i++) {
            if (VROM_CATALOG[i].crc == crc) {
                char out[256];
                snprintf(out, sizeof(out),
                         "{\"recognised\":true,\"canonical_name\":\"%s\",\"card_id\":\"%s\",\"compatible\":[\"%s\"],"
                         "\"size\":%zu,\"crc\":\"0x%08x\"}",
                         VROM_CATALOG[i].canonical_name, VROM_CATALOG[i].card_id, VROM_CATALOG[i].card_id, size, crc);
                return val_str(out);
            }
        }
    }

    // Right size but not a known declaration ROM. Recognised=false so callers
    // route into a normal "unrecognised file" path; still report the CRC so a
    // future catalog entry (or the user) can identify it.
    char out[128];
    snprintf(out, sizeof(out), "{\"recognised\":false,\"size\":%zu,\"crc\":\"0x%08x\"}", size, crc);
    return val_str(out);
}

static const arg_decl_t vrom_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "VROM file path"},
};

static const member_t vrom_members[] = {
    {.kind = M_ATTR,
     .name = "path",
     .doc = "Path of the pending VROM (empty if none)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = vrom_attr_path, .set = NULL}},
    {.kind = M_ATTR,
     .name = "loaded",
     .doc = "True if a VROM path has been set",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = vrom_attr_loaded, .set = NULL}},
    {.kind = M_ATTR,
     .name = "size",
     .doc = "Expected VROM size in bytes (32 KB)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = vrom_attr_size, .set = NULL}},
    {.kind = M_METHOD,
     .name = "load",
     .doc = "Set the VROM path; consumed at the next machine init",
     .method = {.args = vrom_path_arg, .nargs = 1, .result = V_BOOL, .fn = vrom_method_load}},
    {.kind = M_METHOD,
     .name = "identify",
     .doc = "JSON map: {recognised, canonical_name?, card_id?, compatible?, size, crc}.",
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
    vrom_pending_clear();
}
