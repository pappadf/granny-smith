// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// nubus_class.c
// `nubus.*` object-model surface.  Step-3 status: just the `nubus.cards()`
// method, which walks nubus_card_registry() and returns a list of card-id
// strings — empty until step 4 lands the first card.  The richer surface
// described in proposal §3.5.3 (slot.<n>/ children, .primary, per-card
// width/height/format/decl_rom_path) lands once cfg->nubus is non-NULL,
// which only happens after step 4 wires up nubus_init from glue030_init.

#include "card.h"
#include "display.h"
#include "jmfb.h"
#include "nubus.h"
#include "object.h"
#include "radius24ac.h"
#include "value.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// `nubus.cards()` — list every registered card-driver id.  Returns
// V_LIST<V_STRING>.  Used by the config dialog in step 5 to populate
// the per-slot card-type dropdown without baking the list into the JS.
static value_t nubus_method_cards(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    const nubus_card_kind_t *const *reg = nubus_card_registry();
    size_t n = 0;
    for (const nubus_card_kind_t *const *p = reg; *p; p++)
        n++;
    if (n == 0)
        return val_list(NULL, 0);
    value_t *items = (value_t *)calloc(n, sizeof(value_t));
    if (!items)
        return val_err("nubus.cards: out of memory");
    size_t i = 0;
    for (const nubus_card_kind_t *const *p = reg; *p; p++)
        items[i++] = val_str((*p)->id);
    return val_list(items, n);
}

// `nubus.video_sense` — get/set the JMFB-card monitor sense for the
// next machine.boot.  Reads return the current pending value (which
// the JMFB factory will consume on its next instantiation); writes
// stage the new value.  Valid raw-sense codes are 0..6 per
// JMFBPrimaryInit.a's sense table (0=21" RGB / 1=15" Mono /
// 2=12" RGB / 3=21" B&W / 4=NTSC / 5=15" RGB / 6=13" RGB / 7=no
// connect).  Settable from integration test scripts via
// `nubus.video_sense = N` before `machine.boot`.
static value_t nubus_attr_video_sense_get(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    value_t v = val_uint(1, jmfb_pending_sense_get());
    v.flags |= VAL_HEX;
    return v;
}

static value_t nubus_attr_video_sense_set(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    if (in.kind != V_UINT) {
        value_free(&in);
        return val_err("nubus.video_sense: expected unsigned integer 0..7");
    }
    if (in.u > 7) {
        value_free(&in);
        return val_err("nubus.video_sense: value must be 0..7 (got %llu)", (unsigned long long)in.u);
    }
    jmfb_pending_sense_set((uint8_t)in.u);
    value_free(&in);
    return val_none();
}

// `nubus.video_mode` — get/set a pending high-level video-mode id
// for the next machine.boot.  The id matches one of the entries in
// `machine.profile(...).video_modes[].id` (e.g. "13in_rgb_8bpp").
// When the JMFB factory consumes the pending id it resolves it to a
// (monitor, depth) tuple, overrides nubus.video_sense to the
// monitor's sense_code, and writes the boot-ROM PRAM validity tokens
// plus the slot-PRAMRec bytes so the Slot Manager's GET_SLOT_DEPTH
// lands on the requested mode.  Reading returns the pending id (or
// the empty string when none is set); writing "" clears it.
static value_t nubus_attr_video_mode_get(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const char *id = jmfb_pending_video_mode_get();
    return val_str(id ? id : "");
}

static value_t nubus_attr_video_mode_set(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    if (in.kind != V_STRING) {
        value_free(&in);
        return val_err("nubus.video_mode: expected string id (e.g. \"13in_rgb_8bpp\")");
    }
    const char *id = in.s ? in.s : "";
    // Validate against the catalog so the user gets immediate
    // feedback if they pass a typo instead of waiting until the next
    // machine.boot to discover the id didn't match.  Empty string is
    // permitted and clears any pending selection.
    if (*id && !jmfb_video_mode_lookup(id, NULL, NULL)) {
        value_t err = val_err("nubus.video_mode: unknown video-mode id '%s'", id);
        value_free(&in);
        return err;
    }
    jmfb_pending_video_mode_set(id);
    value_free(&in);
    return val_none();
}

// `nubus.video_card` — get/set the card id to install into the next
// machine.boot's VIDEO slot.  Writes stage a pending pick that nubus_init
// honours iff it is one of the slot's available_cards (else it falls back
// to the slot default).  Reads return the pending id, or "" when none is
// staged.  Settable from integration scripts via
// `machine.nubus.video_card = "radius_24ac"` before machine.boot.  The id
// is validated against the registered card drivers so a typo is caught at
// set time rather than silently falling back at boot.
static value_t nubus_attr_video_card_get(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const char *id = nubus_pending_video_card_get();
    return val_str(id ? id : "");
}

static value_t nubus_attr_video_card_set(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    if (in.kind != V_STRING) {
        value_free(&in);
        return val_err("nubus.video_card: expected card-id string (e.g. \"radius_24ac\")");
    }
    const char *id = in.s ? in.s : "";
    // Empty clears the pending pick; a non-empty id must name a registered
    // card driver (nubus.cards() lists them).
    if (*id && !nubus_card_find(id)) {
        value_t err = val_err("nubus.video_card: unknown card id '%s' (see nubus.cards())", id);
        value_free(&in);
        return err;
    }
    nubus_pending_video_card_set(id);
    value_free(&in);
    return val_none();
}

// === Per-card object-model surface (proposal §3.8) ==========================
//
// machine.nubus.slot[N].card.{framebuffer,declrom,clut,mode[,engine]}.  The
// node objects are created by nubus_objects_build() (called from nubus_init,
// once the cards exist) and live here keyed by slot; nubus_objects_teardown()
// (from nubus_delete) frees the trees before the cards they read go away.
// Every node's instance_data is the nubus_card_t, so the accessors read live
// card state (the display_t, the declrom, the radius engine) and copy nothing.
// Resolution and SYSTEM-tab enumeration both work through object_attach: the
// `slot` indexed member returns the slot wrapper, whose attached `card` child
// (and its attached resource children) the resolver/meta-walker discover.

#define NUBUS_OBJ_SLOTS 16 // slot index space ($0..$F); cards seat in $9..$E
#define NUBUS_OBJ_FIRST 9
#define NUBUS_OBJ_LAST  14

typedef struct {
    struct object *slot; // slot wrapper (returned by the indexed `slot` member)
    struct object *card; // the seated card
    struct object *fb; // framebuffer
    struct object *declrom; // declaration ROM
    struct object *clut; // palette
    struct object *mode; // current monitor / depth
    struct object *engine; // accelerator (radius_24ac only; NULL otherwise)
} nubus_slot_nodes_t;

static nubus_bus_t *g_obj_bus = NULL;
static nubus_slot_nodes_t g_slot_nodes[NUBUS_OBJ_SLOTS];

static nubus_card_t *node_card(struct object *self) {
    return (nubus_card_t *)object_data(self);
}
static display_t *node_disp(struct object *self) {
    nubus_card_t *c = node_card(self);
    return (c && c->ops && c->ops->display) ? c->ops->display(c) : NULL;
}
static uint32_t node_fmt_bpp(pixel_format_t f) {
    switch (f) {
    case PIXEL_1BPP_MSB:
        return 1;
    case PIXEL_2BPP_MSB:
        return 2;
    case PIXEL_4BPP_MSB:
        return 4;
    case PIXEL_8BPP:
        return 8;
    case PIXEL_16BPP_555:
        return 16;
    case PIXEL_32BPP_XRGB:
        return 32;
    default:
        return 0;
    }
}
static const char *node_fmt_name(pixel_format_t f) {
    switch (f) {
    case PIXEL_1BPP_MSB:
        return "1bpp";
    case PIXEL_2BPP_MSB:
        return "2bpp";
    case PIXEL_4BPP_MSB:
        return "4bpp";
    case PIXEL_8BPP:
        return "8bpp_clut";
    case PIXEL_16BPP_555:
        return "16bpp_555";
    case PIXEL_32BPP_XRGB:
        return "32bpp_xrgb";
    default:
        return "?";
    }
}

// --- framebuffer node -------------------------------------------------------
static value_t fb_attr_base(struct object *self, const member_t *m) {
    (void)m;
    nubus_card_t *c = node_card(self);
    return val_uint(4, c ? nubus_slot_base(c->slot) : 0);
}
static value_t fb_attr_width(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_int(d ? (int)d->width : 0);
}
static value_t fb_attr_height(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_int(d ? (int)d->height : 0);
}
static value_t fb_attr_stride(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_uint(4, d ? d->stride : 0);
}
static value_t fb_attr_depth(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_int(d ? (int)node_fmt_bpp(d->format) : 0);
}
static value_t fb_attr_format(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_str(d ? node_fmt_name(d->format) : "");
}
static value_t fb_attr_raw_size(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_uint(4, d ? (uint64_t)d->stride * d->height : 0);
}
static const member_t fb_members[] = {
    {.kind = M_ATTR,
     .name = "base",
     .doc = "Slot-space base address of the framebuffer",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = fb_attr_base}},
    {.kind = M_ATTR,
     .name = "width",
     .doc = "Active width in pixels",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = fb_attr_width}                               },
    {.kind = M_ATTR,
     .name = "height",
     .doc = "Active height in pixels",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = fb_attr_height}                              },
    {.kind = M_ATTR,
     .name = "stride",
     .doc = "Row stride in bytes (rowBytes)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_stride}                             },
    {.kind = M_ATTR,
     .name = "depth",
     .doc = "Bits per pixel",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = fb_attr_depth}                               },
    {.kind = M_ATTR,
     .name = "format",
     .doc = "Pixel encoding",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = fb_attr_format}                           },
    {.kind = M_ATTR,
     .name = "raw_size",
     .doc = "Active framebuffer size in bytes (stride × height)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_raw_size}                           },
};
static const class_desc_t nubus_fb_class = {
    .name = "framebuffer", .members = fb_members, .n_members = sizeof(fb_members) / sizeof(fb_members[0])};

// --- declrom node -----------------------------------------------------------
static value_t declrom_attr_size(struct object *self, const member_t *m) {
    (void)m;
    nubus_card_t *c = node_card(self);
    return val_uint(4, c ? (uint64_t)c->declrom_size : 0);
}
static value_t declrom_attr_present(struct object *self, const member_t *m) {
    (void)m;
    nubus_card_t *c = node_card(self);
    return val_bool(c && c->declrom && c->declrom_size > 0);
}
static const member_t declrom_members[] = {
    {.kind = M_ATTR,
     .name = "size",
     .doc = "Declaration ROM size in bytes (bus-space, byte-lane expanded)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = declrom_attr_size}   },
    {.kind = M_ATTR,
     .name = "present",
     .doc = "True if a declaration ROM is loaded",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = declrom_attr_present}},
};
static const class_desc_t nubus_declrom_class = {
    .name = "declrom", .members = declrom_members, .n_members = sizeof(declrom_members) / sizeof(declrom_members[0])};

// --- clut node --------------------------------------------------------------
static value_t clut_attr_len(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_int(d ? (int)d->clut_len : 0);
}
static const member_t clut_members[] = {
    {.kind = M_ATTR,
     .name = "len",
     .doc = "Number of palette entries",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = clut_attr_len}},
};
static const class_desc_t nubus_clut_class = {
    .name = "clut", .members = clut_members, .n_members = sizeof(clut_members) / sizeof(clut_members[0])};

// --- mode node (current monitor / depth) ------------------------------------
static const member_t mode_members[] = {
    {.kind = M_ATTR,
     .name = "width",
     .doc = "Current monitor width in pixels",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = fb_attr_width}    },
    {.kind = M_ATTR,
     .name = "height",
     .doc = "Current monitor height in pixels",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = fb_attr_height}   },
    {.kind = M_ATTR,
     .name = "depth",
     .doc = "Current pixel depth (bpp)",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = fb_attr_depth}    },
    {.kind = M_ATTR,
     .name = "format",
     .doc = "Current pixel encoding",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = fb_attr_format}},
};
static const class_desc_t nubus_mode_class = {
    .name = "mode", .members = mode_members, .n_members = sizeof(mode_members) / sizeof(mode_members[0])};

// --- engine node (radius_24ac acceleration engine) --------------------------
static value_t eng_attr_enabled_get(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(radius24ac_engine_enabled(node_card(self)));
}
static value_t eng_attr_enabled_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    if (in.kind != V_BOOL) {
        value_free(&in);
        return val_err("engine.enabled: expected a boolean");
    }
    radius24ac_engine_set_enabled(node_card(self), in.b);
    value_free(&in);
    return val_none();
}
static value_t eng_attr_mode(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, radius24ac_engine_mode(node_card(self)));
}
static value_t eng_attr_operand(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, radius24ac_engine_operand(node_card(self)));
}
static const member_t engine_members[] = {
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "Acceleration gate; clear to force the software-fallback path (the oracle)",
     .attr = {.type = V_BOOL, .get = eng_attr_enabled_get, .set = eng_attr_enabled_set}},
    {.kind = M_ATTR,
     .name = "mode",
     .doc = "Latched CONTROL op byte ($01 fill / $03 stretch / $7F copy / ROP)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = eng_attr_mode}},
    {.kind = M_ATTR,
     .name = "operand",
     .doc = "Latched 32-bit fill/pattern operand",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = eng_attr_operand}},
};
static const class_desc_t nubus_engine_class = {
    .name = "engine", .members = engine_members, .n_members = sizeof(engine_members) / sizeof(engine_members[0])};

// --- card node --------------------------------------------------------------
static value_t card_attr_name(struct object *self, const member_t *m) {
    (void)m;
    nubus_card_t *c = node_card(self);
    return val_str((c && c->ops && c->ops->name) ? c->ops->name(c) : "");
}
static value_t card_attr_slot(struct object *self, const member_t *m) {
    (void)m;
    nubus_card_t *c = node_card(self);
    return val_int(c ? c->slot : -1);
}
static const member_t card_members[] = {
    {.kind = M_ATTR,
     .name = "name",
     .doc = "Card display name",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = card_attr_name}                            },
    {.kind = M_ATTR,
     .name = "slot",
     .doc = "NuBus slot number ($9..$E)",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .presentation_flags = VAL_HEX, .get = card_attr_slot}},
};
static const class_desc_t nubus_card_class = {
    .name = "card", .members = card_members, .n_members = sizeof(card_members) / sizeof(card_members[0])};

// --- slot wrapper node ------------------------------------------------------
static value_t slot_attr_number(struct object *self, const member_t *m) {
    (void)m;
    nubus_card_t *c = node_card(self);
    return val_int(c ? c->slot : -1);
}
static const member_t slot_members[] = {
    {.kind = M_ATTR,
     .name = "number",
     .doc = "NuBus slot number ($9..$E)",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .presentation_flags = VAL_HEX, .get = slot_attr_number}},
};
static const class_desc_t nubus_slot_class = {
    .name = "slot", .members = slot_members, .n_members = sizeof(slot_members) / sizeof(slot_members[0])};

// --- indexed `slot` member: enumerate populated slots -----------------------
static struct object *nubus_slot_get(struct object *self, int index) {
    (void)self;
    if (!g_obj_bus || index < 0 || index >= NUBUS_OBJ_SLOTS)
        return NULL;
    if (!nubus_card(g_obj_bus, index))
        return NULL;
    return g_slot_nodes[index].slot;
}
static int nubus_slot_count(struct object *self) {
    (void)self;
    if (!g_obj_bus)
        return 0;
    int n = 0;
    for (int i = NUBUS_OBJ_FIRST; i <= NUBUS_OBJ_LAST; i++)
        if (nubus_card(g_obj_bus, i))
            n++;
    return n;
}
static int nubus_slot_next(struct object *self, int prev_index) {
    (void)self;
    if (!g_obj_bus)
        return -1;
    for (int i = prev_index + 1; i <= NUBUS_OBJ_LAST; i++)
        if (nubus_card(g_obj_bus, i))
            return i;
    return -1;
}

static const member_t nubus_members[] = {
    {.kind = M_CHILD,
     .name = "slot",
     .doc = "Populated NuBus slots ($9..$E); index by slot number, e.g. slot[9].card.framebuffer",
     .label = "Slots",
     .order = 10,
     .child = {.cls = &nubus_slot_class,
               .indexed = true,
               .get = nubus_slot_get,
               .count = nubus_slot_count,
               .next = nubus_slot_next}},
    {.kind = M_METHOD,
     .name = "cards",
     .doc = "List the ids of all registered NuBus card drivers",
     .method = {.args = NULL, .nargs = 0, .result = V_LIST, .fn = nubus_method_cards}},
    {.kind = M_ATTR,
     .name = "video_card",
     .doc = "Pending video-slot card id for next machine.boot (e.g. \"radius_24ac\")",
     .flags = 0,
     .attr = {.type = V_STRING, .get = nubus_attr_video_card_get, .set = nubus_attr_video_card_set}},
    {.kind = M_ATTR,
     .name = "video_sense",
     .doc = "Pending JMFB monitor sense (0..6, 7 = no connect)",
     .flags = 0,
     .attr = {.type = V_UINT, .get = nubus_attr_video_sense_get, .set = nubus_attr_video_sense_set}},
    {.kind = M_ATTR,
     .name = "video_mode",
     .doc = "Pending video-mode id for next machine.boot (matches profile.video_modes[].id)",
     .flags = 0,
     .attr = {.type = V_STRING, .get = nubus_attr_video_mode_get, .set = nubus_attr_video_mode_set}},
};

const class_desc_t nubus_class = {
    .name = "nubus",
    .members = nubus_members,
    .n_members = sizeof(nubus_members) / sizeof(nubus_members[0]),
};

// === Object-tree build / teardown ===========================================

// Attach one resource child under the card node with a label/order/category.
static struct object *attach_resource(struct object *card_node, const class_desc_t *cls, nubus_card_t *card,
                                      const char *name, const char *label, int order, uint16_t category) {
    struct object *o = object_new(cls, card, name);
    if (!o)
        return NULL;
    object_set_label(o, label);
    object_set_order(o, order);
    object_set_category(o, category);
    object_attach(card_node, o);
    return o;
}

void nubus_objects_build(nubus_bus_t *bus) {
    nubus_objects_teardown(); // idempotent — drop any prior trees first
    if (!bus)
        return;
    g_obj_bus = bus;
    for (int i = NUBUS_OBJ_FIRST; i <= NUBUS_OBJ_LAST; i++) {
        nubus_card_t *card = nubus_card(bus, i);
        if (!card)
            continue;
        nubus_slot_nodes_t *n = &g_slot_nodes[i];

        n->slot = object_new(&nubus_slot_class, card, "slot");
        if (!n->slot)
            continue;
        object_set_label(n->slot, "Slot");
        object_set_order(n->slot, i);

        n->card = object_new(&nubus_card_class, card, "card");
        if (n->card) {
            object_set_label(n->card, (card->ops && card->ops->name) ? card->ops->name(card) : "Card");
            object_attach(n->slot, n->card);
            n->fb = attach_resource(n->card, &nubus_fb_class, card, "framebuffer", "Framebuffer", 10, M_CAT_BASIC);
            n->declrom =
                attach_resource(n->card, &nubus_declrom_class, card, "declrom", "Declaration ROM", 20, M_CAT_BASIC);
            n->clut = attach_resource(n->card, &nubus_clut_class, card, "clut", "CLUT", 30, M_CAT_BASIC);
            n->mode = attach_resource(n->card, &nubus_mode_class, card, "mode", "Mode", 40, M_CAT_BASIC);
            // The accelerator is radius_24ac-specific; mark it Advanced so it
            // doesn't clutter the default SYSTEM tree.
            if (radius24ac_is_card(card))
                n->engine =
                    attach_resource(n->card, &nubus_engine_class, card, "engine", "Accelerator", 50, M_CAT_ADVANCED);
        }
    }
}

void nubus_objects_teardown(void) {
    for (int i = 0; i < NUBUS_OBJ_SLOTS; i++) {
        if (g_slot_nodes[i].slot)
            object_delete_tree(g_slot_nodes[i].slot); // frees the slot + attached subtree
        memset(&g_slot_nodes[i], 0, sizeof(g_slot_nodes[i]));
    }
    g_obj_bus = NULL;
}

struct object *nubus_active_framebuffer_object(void) {
    if (!g_obj_bus)
        return NULL;
    nubus_card_t *card = nubus_primary_display_card(g_obj_bus);
    if (!card)
        return NULL;
    for (int i = 0; i < NUBUS_OBJ_SLOTS; i++)
        if (g_slot_nodes[i].slot && nubus_card(g_obj_bus, i) == card)
            return g_slot_nodes[i].fb;
    return NULL;
}
