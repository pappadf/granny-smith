// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// display_class.c
// See display_class.h.  One framebuffer node, shared by every display source.

#include "display_class.h"

#include "machine_profile.h"

static display_fb_node_t *fb_node(struct object *self) {
    return (display_fb_node_t *)object_data(self);
}

static display_t *node_disp(struct object *self) {
    display_fb_node_t *n = fb_node(self);
    return (n && n->resolve) ? n->resolve(n->owner) : NULL;
}

static value_t fb_attr_base(struct object *self, const member_t *m) {
    (void)m;
    display_fb_node_t *n = fb_node(self);
    return val_uint(4, (n && n->base) ? n->base(n->owner) : 0);
}
static value_t fb_attr_width(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_uint(4, d ? d->width : 0);
}
static value_t fb_attr_height(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_uint(4, d ? d->height : 0);
}
static value_t fb_attr_stride(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_uint(4, d ? d->stride : 0);
}
static value_t fb_attr_depth(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_uint(4, d ? display_bpp(d->format) : 0);
}
static value_t fb_attr_format(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_str(d ? display_format_name(d->format) : "");
}
static value_t fb_attr_raw_size(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_uint(4, d ? (uint64_t)d->stride * d->height : 0);
}

static value_t fb_attr_clut_len(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_uint(4, d ? d->clut_len : 0);
}

static const member_t fb_members[] = {
    {.kind = M_ATTR,
     .name = "base",
     .doc = "Framebuffer base, in the address space the guest reaches it through",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = fb_attr_base}},
    {.kind = M_ATTR,
     .name = "width",
     .doc = "Active width in pixels",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_width}                              },
    {.kind = M_ATTR,
     .name = "height",
     .doc = "Active height in pixels",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_height}                             },
    {.kind = M_ATTR,
     .name = "stride",
     .doc = "Row stride in bytes (rowBytes)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_stride}                             },
    {.kind = M_ATTR,
     .name = "depth",
     .doc = "Bits per pixel",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_depth}                              },
    {.kind = M_ATTR,
     .name = "format",
     .doc = "Pixel encoding",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = fb_attr_format}                           },
    {.kind = M_ATTR,
     .name = "clut_len",
     .doc = "Palette entries the current format uses (0 for a direct format)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_clut_len}                           },
    {.kind = M_ATTR,
     .name = "raw_size",
     .doc = "Active framebuffer size in bytes (stride x height)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_raw_size}                           },
};

const class_desc_t display_fb_class = {
    .name = "framebuffer", .members = fb_members, .n_members = sizeof(fb_members) / sizeof(fb_members[0])};

// The `video` wrapper reads the same descriptor, so `machine.video.width` and
// `machine.video.framebuffer.width` both work -- a card's geometry is
// reachable at `slot[N].card.mode.width` and the built-in chips should not
// need a deeper path for the same fact.
static const class_desc_t display_video_class = {
    .name = "video", .members = fb_members, .n_members = sizeof(fb_members) / sizeof(fb_members[0])};

// --- machine.video, for the soldered-down producers -------------------------

struct object *display_attach_video_node(display_fb_node_t *node, const char *label) {
    if (!node)
        return NULL;
    struct object *video = object_new(&display_video_class, node, "video");
    if (!video)
        return NULL;
    object_set_label(video, label ? label : "Video");
    object_set_order(video, 120);
    struct object *fb = object_new(&display_fb_class, node, "framebuffer");
    if (fb) {
        object_set_label(fb, "Framebuffer");
        object_set_order(fb, 10);
        object_attach(video, fb);
    }
    object_attach(machine_object(), video);
    return video;
}

void display_detach_video_node(struct object *video) {
    if (!video)
        return;
    // ...TREE: this node owns an attached `framebuffer` child, and
    // object_delete frees only the node it is given -- it would orphan the
    // child, one leaked object per machine.boot.  Caught by ASan on a
    // pm9500 boot, where Control attaches one of these.
    object_detach(video);
    object_delete_tree(video);
}
