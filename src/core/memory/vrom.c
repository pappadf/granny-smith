// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vrom.c
// Video-ROM file handling and the vrom.* object-model surface.
//
// VROM is a 32 KB blob the SE/30 needs alongside the main ROM. Because the
// main ROM is only loaded after machine creation and the VROM has to be
// available during machine init, the design is: vrom.load(path) just
// records a pending path; se30 init reads pending_path() and loads bytes
// directly into video RAM.

#include "vrom.h"

#include "object.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// File-level helpers
// ============================================================================

bool vrom_probe_file(const char *path, size_t *out_size) {
    if (out_size)
        *out_size = 0;
    if (!path || !*path)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (size <= 0)
        return false;
    if (out_size)
        *out_size = (size_t)size;
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
    printf("VROM path set: %s\n", s_pending_vrom_path);
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
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("vrom.load: expected (path)");
    if (vrom_set_path(argv[0].s) != 0)
        return val_err("vrom.load: failed");
    return val_bool(true);
}

// vrom.identify(path) — the only check today is "is the file 32 KB?". Returns
// V_BOOL so callers can write `assert ${vrom.identify(...)} "..."`.
static value_t vrom_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("vrom.identify: expected (path)");
    return val_bool(vrom_probe_file(argv[0].s, NULL));
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
     .doc = "True if the file is a recognised VROM image (32 KB)",
     .method = {.args = vrom_path_arg, .nargs = 1, .result = V_BOOL, .fn = vrom_method_identify}},
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
    if (s_vrom_object)
        object_attach(object_root(), s_vrom_object);
}

void vrom_delete(void) {
    if (s_vrom_object) {
        object_detach(s_vrom_object);
        object_delete(s_vrom_object);
        s_vrom_object = NULL;
    }
    vrom_pending_clear();
}
