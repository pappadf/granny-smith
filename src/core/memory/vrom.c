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

// FNV-1a 32-bit hash, same constants the IOP firmware checks use
// elsewhere in core. Cheap, content-derived, good enough to identify a
// 32 KB VROM blob among a small known set.
static uint32_t vrom_fnv1a32(const uint8_t *data, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

// Catalog of known VROM blobs. Maps FNV-1a32 over the whole file to the
// canonical on-disk filename the C-side card factories expect (e.g.
// jmfb.c hardcodes "Apple-341-0868.vrom") and a human-readable card
// label that the UI can display. Adding a new VROM = one row here.
struct vrom_known {
    uint32_t fnv1a;
    const char *canonical_name;
    const char *card_name;
};
static const struct vrom_known VROM_CATALOG[] = {
    // Macintosh Display Card 8•24 (ROM rev 341-0868). Drives the JMFB
    // NuBus card on IIcx / IIx / IIfx; loaded by jmfb.c.
    {0x00c90c2eu, "Apple-341-0868.vrom",    "Macintosh Display Card 8•24"},
    // SE/30 onboard video declaration ROM; loaded by
    // builtin_se30_video.c.
    {0xe22959a9u, "SE30.vrom",              "Macintosh SE/30 onboard video"},
    // Macintosh Display Card 4•8 (24AC variant).
    {0x41135c6au, "display-card-24ac.vrom", "Macintosh Display Card 4•8" },
};

// vrom.identify(path) — returns a JSON map describing the file:
//   {
//     "recognised": bool,
//     "canonical_name": "Apple-341-0868.vrom",    // present when recognised
//     "card_name":      "Macintosh Display Card 8•24",
//     "size":           32768,
//     "fnv1a":          "0x00c90c2e"
//   }
// JS callers use canonical_name to persist the file under the path
// the C-side card factories expect (rather than the user's original
// browser-renamed filename), and card_name as the human-readable label.
// Mirrors rom.identify's JSON-shape contract.
static value_t vrom_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    size_t size = 0;
    bool size_ok = vrom_probe_file(path, &size);
    if (!size_ok)
        return val_str("{\"recognised\":false}");

    // Read the whole VROM into memory and compute FNV-1a32. 32 KB is
    // tiny — no need to stream.
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
    uint32_t h = vrom_fnv1a32(buf, size);
    free(buf);

    for (size_t i = 0; i < sizeof(VROM_CATALOG) / sizeof(VROM_CATALOG[0]); i++) {
        if (VROM_CATALOG[i].fnv1a == h) {
            char out[256];
            snprintf(out, sizeof(out),
                     "{\"recognised\":true,\"canonical_name\":\"%s\",\"card_name\":\"%s\",\"size\":%zu,\"fnv1a\":\"0x%"
                     "08x\"}",
                     VROM_CATALOG[i].canonical_name, VROM_CATALOG[i].card_name, size, h);
            return val_str(out);
        }
    }

    // Right size but unknown contents. Recognised=false so callers
    // route into a normal "unrecognised file" path; still report the
    // hash so the user (or a future catalog entry) can identify it.
    char out[128];
    snprintf(out, sizeof(out), "{\"recognised\":false,\"size\":%zu,\"fnv1a\":\"0x%08x\"}", size, h);
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
     .doc = "JSON map: {recognised, canonical_name?, card_name?, size, fnv1a}.",
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
