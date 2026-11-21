// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_kind.c
// The image kinds and the one open path every device uses -- see image_kind.h
// and docs/internals/core/storage/image_kind.md.

#include "image_kind.h"

#include "checkpoint_machine.h"
#include "image.h"
#include "image_internal.h"
#include "platform_hooks.h"

#include <stdio.h>
#include <string.h>

// One row per kind: everything a kind decides about the media it opens.
typedef struct image_kind_desc {
    const char *name; // wire name (files.images[i].type)
    image_geometry_t geom; // the tags its sectors carry
    bool read_only; // never writable, whatever the caller asks
    bool floppy_sizes; // accepts only the four floppy sizes
} image_kind_desc_t;

static const image_kind_desc_t k_kinds[IMAGE_KIND_COUNT] = {
    [IMAGE_KIND_NONE] = {.name = "none"},
    [IMAGE_KIND_FLOPPY] = {.name = "floppy", .geom = {.gcr_tags = true}, .floppy_sizes = true},
    [IMAGE_KIND_HD] = {.name = "hd"},
    [IMAGE_KIND_CD] = {.name = "cd", .read_only = true},
    [IMAGE_KIND_PROFILE] = {.name = "profile", .geom = {.tag_bytes = 20}},
};

// The row for `kind` (IMAGE_KIND_NONE's for a value out of range).
static const image_kind_desc_t *kind_desc(image_kind_t kind) {
    return (kind >= IMAGE_KIND_NONE && kind < IMAGE_KIND_COUNT) ? &k_kinds[kind] : &k_kinds[IMAGE_KIND_NONE];
}

const char *image_kind_name(image_kind_t kind) {
    return kind_desc(kind)->name;
}

uint64_t image_kind_file_bytes(image_kind_t kind, uint64_t data_bytes) {
    uint32_t t = kind_desc(kind)->geom.tag_bytes; // in-band in the kind's files
    return data_bytes / 512 * (512 + t);
}

const char *image_delta_dir(const char *path) {
    // A base on volatile scratch storage keeps its delta in the image layer's
    // scratch root too (NULL); everything else lives with the machine.
    if (platform_path_is_volatile(path))
        return NULL;
    return checkpoint_machine_dir();
}

// Write the reason into `err` when the caller gave room for one.
static void set_err(char *err, size_t cap, const char *fmt, const char *path) {
    if (err && cap)
        snprintf(err, cap, fmt, path ? path : "");
}

image_t *image_open_kind(const char *path, const image_open_opts_t *o, char *err, size_t err_cap) {
    set_err(err, err_cap, "%s", "");
    if (!o || !path || !*path) {
        set_err(err, err_cap, "no image path%s", "");
        return NULL;
    }
    const image_kind_desc_t *d = kind_desc(o->kind);
    const image_geometry_t geom = d->geom;
    bool writable = o->writable && !d->read_only;
    image_t *img = NULL;
    if (!writable)
        img = image_open_readonly_with_geometry(path, geom);
    else if (o->instance && *o->instance)
        img = image_open_with_geometry(path, o->instance, geom);
    else
        img = image_create_with_geometry(path, o->delta_dir ? o->delta_dir : image_delta_dir(path), geom);
    if (!img) {
        const char *why = image_open_error();
        if (why && *why) {
            if (err && err_cap)
                snprintf(err, err_cap, "'%s' is %s", path, why);
        } else {
            set_err(err, err_cap, "cannot open '%s'", path);
        }
        return NULL;
    }
    // A floppy drive presents only the four floppy sizes.
    if (d->floppy_sizes && image_layout_of_size(img->raw_size) == IMAGE_LAYOUT_NONE) {
        set_err(err, err_cap, "'%s' is not a floppy-sized image", path);
        image_close(img);
        return NULL;
    }
    img->kind = o->kind;
    return img;
}

image_t *image_create_blank_kind(image_kind_t kind, uint64_t blocks, const char *instance) {
    image_t *img = image_create_blank(blocks, kind_desc(kind)->geom, image_delta_dir(NULL), instance);
    if (img)
        img->kind = kind;
    return img;
}
