// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_class.c
// The `image` node -- see image_class.h.

#include "image_class.h"

#include "image.h"
#include "image_kind.h"
#include "value.h"

#include <stddef.h>
#include <stdint.h>

// The export runs as an I/O job (io_leaf.h): the disk is snapshotted and
// write-locked here, streamed to the file on the I/O worker, and the answer
// comes when it is done.  Weak: a unit suite of one drive links no I/O
// leaves, and exports here and now.
value_t io_leaf_export_image(struct image *img, const char *dest, const char *codec, const char *what)
    __attribute__((weak));

// The node an image-class object is.
static const image_node_t *node_of(struct object *self) {
    return (const image_node_t *)object_data(self);
}

struct image *image_node_image(struct object *self) {
    const image_node_t *n = node_of(self);
    return (n && n->ops && n->ops->image) ? n->ops->image(n->dev, n->slot) : NULL;
}

// The kind names, indexed by image_kind_t (image_kind_name spells them).
static const char *const IMAGE_KIND_NAMES[IMAGE_KIND_COUNT] = {
    [IMAGE_KIND_NONE] = "none", [IMAGE_KIND_FLOPPY] = "floppy",   [IMAGE_KIND_HD] = "hd",
    [IMAGE_KIND_CD] = "cd",     [IMAGE_KIND_PROFILE] = "profile",
};

static DEF_GETTER(image_attr_kind) {
    return val_enum((int)image_get_kind(image_node_image(self)), IMAGE_KIND_NAMES, IMAGE_KIND_COUNT);
}

static DEF_GETTER(image_attr_source) {
    const char *s = image_get_filename(image_node_image(self));
    return val_str(s ? s : "");
}

static DEF_GETTER(image_attr_encoding) {
    const char *s = image_get_format(image_node_image(self));
    return val_str(s ? s : "");
}

static DEF_GETTER(image_attr_size) {
    return val_uint(8, disk_size(image_node_image(self)));
}

static DEF_GETTER(image_attr_sectors) {
    return val_uint(8, disk_size(image_node_image(self)) / 512u);
}

static DEF_GETTER(image_attr_tag_bytes) {
    return val_uint(4, image_get_tag_bytes(image_node_image(self)));
}

static DEF_GETTER(image_attr_writable) {
    return val_bool(image_is_writable(image_node_image(self)));
}

static DEF_GETTER(image_attr_delta) {
    const char *s = image_path(image_node_image(self));
    return val_str(s ? s : "");
}

static DEF_GETTER(image_attr_reads) {
    return val_uint(8, image_get_reads(image_node_image(self)));
}

static DEF_GETTER(image_attr_writes) {
    return val_uint(8, image_get_writes(image_node_image(self)));
}

static DEF_GETTER(image_attr_read_errors) {
    return val_uint(8, image_get_read_errors(image_node_image(self)));
}

// `export(path, [codec])` -- write the image as it is now (base + delta, and
// the sector tags for a DiskCopy or MOOF destination) to a NEW file; the
// name picks the format (image_export_to), and a .dmg is compressed with
// `codec` (zlib or xz).  Never overwrites, never touches the source.
static DEF_METHOD(image_method_export) {
    struct image *img = image_node_image(self);
    if (!img)
        return val_err("image.export: no image");
    const char *dest = argv[0].s;
    const char *codec = argc > 1 && argv[1].kind == VK_STRING ? argv[1].s : NULL;
    if (io_leaf_export_image)
        return io_leaf_export_image(img, dest, codec, "image.export");
    if (image_export_to_codec(img, dest, codec) != 0)
        return val_err("image.export: failed to write '%s' (refuses to overwrite an existing file)", dest);
    return val_bool(true);
}

// `eject()` -- the drive's eject: the image goes back to the machine's image
// list, which closes it.
static DEF_METHOD(image_method_eject) {
    const image_node_t *n = node_of(self);
    if (!n || !n->ops || !n->ops->eject)
        return val_err("image.eject: eject it from its drive (machine.eject_media)");
    switch (n->ops->eject(n->dev, n->slot)) {
    case IMAGE_EJECT_OK:
        return val_none();
    case IMAGE_EJECT_LOCKED:
        return val_err("image.eject: the guest has locked the drive (PREVENT MEDIUM REMOVAL)");
    default:
        return val_err("image.eject: no image to eject");
    }
}

static const arg_decl_t image_export_args[] = {
    {.name = "path",
     .kind = VK_STRING,
     .presentation_flags = VFLAG_PATH,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "New file: .dmg = UDIF, .dc42/.diskcopy/.image = DiskCopy 4.2 with tags (floppies), .moof = MOOF (GCR "
            "floppies), else raw"},
    {.name = "codec",
     .kind = VK_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "A .dmg's compression: zlib (UDZO) or xz (ULMO: 14-16% smaller on system disks, decodes ~4x slower)",
     .default_doc = "zlib"       },
};

static const member_t image_members[] = {
    {.kind = MK_ATTR,
     .name = "kind",
     .doc = "What its drive presents it as: floppy, hd, cd or profile",
     .attr = {.type = VK_ENUM, .get = image_attr_kind, .set = NULL}                                             },
    {.kind = MK_ATTR,
     .name = "source",
     .doc = "The path the image was opened from (\"\" for a blank one)",
     .attr = {.type = VK_STRING, .get = image_attr_source, .set = NULL}                                         },
    {.kind = MK_ATTR,
     .name = "encoding",
     .doc = "Wrapper layers peeled to reach the disk, outermost first: raw, dc42, udif, bin+ndif, moof+dc42, ...",
     .attr = {.type = VK_STRING, .get = image_attr_encoding, .set = NULL}                                       },
    {.kind = MK_ATTR,
     .name = "size",
     .doc = "The disk's data bytes (512 per sector; tags not counted)",
     .attr = {.type = VK_UINT, .get = image_attr_size, .set = NULL}                                             },
    {.kind = MK_ATTR,
     .name = "sectors",
     .doc = "512-byte sectors",
     .attr = {.type = VK_UINT, .get = image_attr_sectors, .set = NULL}                                          },
    {.kind = MK_ATTR,
     .name = "tag_bytes",
     .doc = "Tag bytes beside each sector: 12 on a 400K/800K GCR floppy, 20 on a ProFile, else 0",
     .attr = {.type = VK_UINT, .get = image_attr_tag_bytes, .set = NULL}                                        },
    {.kind = MK_ATTR,
     .name = "writable",
     .doc = "True when the guest may write it (false: write-protected)",
     .attr = {.type = VK_BOOL, .get = image_attr_writable, .set = NULL}                                         },
    {.kind = MK_ATTR,
     .name = "delta",
     .doc = "The delta+journal stem of a writable image (\"\" when read-only)",
     .attr = {.type = VK_STRING, .get = image_attr_delta, .set = NULL}                                          },
    {.kind = MK_ATTR,
     .name = "reads",
     .doc = "Drive reads served from the image since it was opened (what lights the activity light)",
     .attr = {.type = VK_UINT, .get = image_attr_reads, .set = NULL, .presentation_flags = VFLAG_VOLATILE}      },
    {.kind = MK_ATTR,
     .name = "writes",
     .doc = "Drive writes to the image since it was opened",
     .attr = {.type = VK_UINT, .get = image_attr_writes, .set = NULL, .presentation_flags = VFLAG_VOLATILE}     },
    {.kind = MK_ATTR,
     .name = "read_errors",
     .doc = "Reads the backing image could not serve (the guest saw a read error for each)",
     .attr = {.type = VK_UINT, .get = image_attr_read_errors, .set = NULL, .presentation_flags = VFLAG_VOLATILE}},
    {.kind = MK_METHOD,
     .name = "export",
     .doc = "Save the image as it is now to a new file (Save As)",
     .method = {.args = image_export_args,
                .nargs = 2,
                .result = VK_BOOL,
                .fn = image_method_export,
                .ui_flags = MM_MUTATE | MM_IO,
                .verb_label = "Save image…"}                                                                  },
    {.kind = MK_METHOD,
     .name = "eject",
     .doc = "Take the image out of its drive",
     .method = {.args = NULL,
                .nargs = 0,
                .result = VK_NONE,
                .fn = image_method_eject,
                .ui_flags = MM_DESTRUCTIVE | MM_MUTATE}                                                         },
};

const class_desc_t image_class = {
    .name = "image",
    .members = image_members,
    .n_members = sizeof(image_members) / sizeof(image_members[0]),
    .doc = "A disk image in a drive: the same node under every drive and in files.images",
};
