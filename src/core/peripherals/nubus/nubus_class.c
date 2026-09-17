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
#include "display_class.h"
#include "nubus.h"
#include "object.h"
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

// Does any registered card offer this "monitor_Nbpp" id?  Asked of the
// REGISTRY rather than of three named cards: every display kind carries its
// own catalogue in `.monitors`, and nubus_monitor_mode_lookup answers the same
// question against any of them.  The named-card version meant core knew which
// display cards exist -- and silently gave the wrong answer for a fourth
// (04-video F-10).
static bool video_mode_id_known(const char *id) {
    for (const nubus_card_kind_t *const *k = nubus_card_registry(); k && *k; k++)
        if (nubus_monitor_mode_lookup((*k)->monitors, id, NULL, NULL))
            return true;
    return false;
}

// Exported for boot-document validation (machine.boot video_mode=).
bool nubus_video_mode_known(const char *id) {
    return video_mode_id_known(id);
}

// === Per-card object-model surface (proposal §3.8) ==========================
//
// machine.nubus.slot[N].card.{framebuffer,declrom,clut,mode[,engine]}.  The
// node objects are created by nubus_objects_build() (called from nubus_init,
// once the cards exist) and live here keyed by slot; nubus_objects_teardown()
// (from nubus_delete) frees the trees before the cards they read go away.
// The card node and its resource children carry the nubus_card_t as
// instance_data, so their accessors read live card state (the display_t,
// the declrom, the card engine) and copy nothing; the slot wrapper carries
// only its slot number (an empty socket has no card to point at).
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
    display_fb_node_t fb_node; // instance data for the shared framebuffer class
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

// --- framebuffer node -------------------------------------------------------
// The node itself is display_class.c's, shared with every other display
// source so `machine.screen.source` means the same thing on either bus and on
// the built-in chips (04-video F-15/F-16).  All this side supplies is how to
// reach a card's live descriptor and where its framebuffer sits.
static display_t *nubus_fb_resolve(void *owner) {
    nubus_card_t *c = (nubus_card_t *)owner;
    return (c && c->ops && c->ops->display) ? c->ops->display(c) : NULL;
}
static uint64_t nubus_fb_base(void *owner) {
    nubus_card_t *c = (nubus_card_t *)owner;
    return c ? nubus_slot_base(c->slot) : 0;
}

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
// Same numbers as the framebuffer node, under the card's own `mode` child --
// this one carries the CARD as instance data, so it reads the descriptor
// through node_disp rather than through a display_fb_node_t.
static value_t mode_attr_width(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_int(d ? (int)d->width : 0);
}
static value_t mode_attr_height(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_int(d ? (int)d->height : 0);
}
static value_t mode_attr_depth(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_int(d ? (int)display_bpp(d->format) : 0);
}
static value_t mode_attr_format(struct object *self, const member_t *m) {
    (void)m;
    display_t *d = node_disp(self);
    return val_str(d ? display_format_name(d->format) : "");
}
static const member_t mode_members[] = {
    {.kind = M_ATTR,
     .name = "width",
     .doc = "Current monitor width in pixels",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = mode_attr_width}    },
    {.kind = M_ATTR,
     .name = "height",
     .doc = "Current monitor height in pixels",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = mode_attr_height}   },
    {.kind = M_ATTR,
     .name = "depth",
     .doc = "Current pixel depth (bpp)",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = mode_attr_depth}    },
    {.kind = M_ATTR,
     .name = "format",
     .doc = "Current pixel encoding",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = mode_attr_format}},
};
static const class_desc_t nubus_mode_class = {
    .name = "mode", .members = mode_members, .n_members = sizeof(mode_members) / sizeof(mode_members[0])};

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
//
// Slot nodes exist for every declared SOCKET (populated or not) and every
// BUILTIN slot, so an empty socket can be configured for the next boot via
// its staged attrs.  Instance data is a per-slot int (the slot number) —
// NOT the card — because empty sockets have no card; the card CHILD node
// keeps the card pointer as before.
static int s_slot_numbers[NUBUS_OBJ_SLOTS]; // instance data for slot nodes

static int node_slot_number(struct object *self) {
    const int *n = (const int *)object_data(self);
    return n ? *n : -1;
}

static value_t slot_attr_number(struct object *self, const member_t *m) {
    (void)m;
    return val_int(node_slot_number(self));
}

// `slot[N].card_id` — stage a card pick for THIS slot for the next
// machine.boot (the concrete-slot sibling of the `nubus.video_card`
// wildcard alias; a concrete entry beats the wildcard).  Only SOCKET slots
// accept a pick; reads return the staged id ("" when none, and always ""
// on builtin slots).  Cleared when nubus_init consumes it.
static value_t slot_attr_card_id_get(struct object *self, const member_t *m) {
    (void)m;
    const char *id = nubus_staged_card_get(node_slot_number(self));
    return val_str(id ? id : "");
}

static value_t slot_attr_card_id_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    int slot = node_slot_number(self);
    if (in.kind != V_STRING) {
        value_free(&in);
        return val_err("slot[%X].card_id: expected card-id string (e.g. \"824gc\")", slot);
    }
    const char *id = in.s ? in.s : "";
    const nubus_slot_decl_t *decl = nubus_slot_decl_get(g_obj_bus, slot);
    if (!decl || decl->kind != NUBUS_SLOT_SOCKET) {
        value_free(&in);
        return val_err("slot[%X].card_id: slot is not a user-configurable socket", slot);
    }
    // "" clears; a non-empty id must name a registered card (typo guard).
    if (*id && !nubus_card_find(id)) {
        const char *near = nubus_card_suggest(id);
        value_t err = near ? val_err("slot[%X].card_id: unknown card id '%s' — did you mean '%s'? (see nubus.cards())",
                                     slot, id, near)
                           : val_err("slot[%X].card_id: unknown card id '%s' (see nubus.cards())", slot, id);
        value_free(&in);
        return err;
    }
    nubus_staged_card_set(slot, id);
    value_free(&in);
    return val_none();
}

// `slot[N].video_mode` — stage a video-mode id for THIS slot for the next
// machine.boot (concrete-slot sibling of the `nubus.video_mode` alias).
// At boot the id is routed into the slot's resolved card kind; a mode that
// doesn't belong to that card logs and is ignored.
static value_t slot_attr_video_mode_get(struct object *self, const member_t *m) {
    (void)m;
    const char *id = nubus_staged_mode_get(node_slot_number(self));
    return val_str(id ? id : "");
}

static value_t slot_attr_video_mode_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    int slot = node_slot_number(self);
    if (in.kind != V_STRING) {
        value_free(&in);
        return val_err("slot[%X].video_mode: expected string id (e.g. \"gc_640x480_8bpp\")", slot);
    }
    const char *id = in.s ? in.s : "";
    const nubus_slot_decl_t *decl = nubus_slot_decl_get(g_obj_bus, slot);
    if (!decl || decl->kind != NUBUS_SLOT_SOCKET) {
        value_free(&in);
        return val_err("slot[%X].video_mode: slot is not a user-configurable socket", slot);
    }
    if (*id && !video_mode_id_known(id)) {
        value_t err = val_err("slot[%X].video_mode: unknown video-mode id '%s'", slot, id);
        value_free(&in);
        return err;
    }
    nubus_staged_mode_set(slot, id);
    value_free(&in);
    return val_none();
}

static const member_t slot_members[] = {
    {.kind = M_ATTR,
     .name = "number",
     .doc = "NuBus slot number ($9..$E)",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .presentation_flags = VAL_HEX, .get = slot_attr_number}             },
    {.kind = M_ATTR,
     .name = "card_id",
     .doc = "Staged card pick for this socket for the next machine.boot (\"\" = none)",
     .flags = 0,
     .attr = {.type = V_STRING, .get = slot_attr_card_id_get, .set = slot_attr_card_id_set}      },
    {.kind = M_ATTR,
     .name = "video_mode",
     .doc = "Staged video-mode id for this socket for the next machine.boot (\"\" = none)",
     .flags = 0,
     .attr = {.type = V_STRING, .get = slot_attr_video_mode_get, .set = slot_attr_video_mode_set}},
};
static const class_desc_t nubus_slot_class = {
    .name = "slot", .members = slot_members, .n_members = sizeof(slot_members) / sizeof(slot_members[0])};

// --- indexed `slot` member: enumerate declared slots -------------------------
// A slot node exists for every populated slot AND every declared (possibly
// empty) SOCKET — nubus_objects_build decides; enumeration keys off the
// node's existence so the two can't disagree.
static struct object *nubus_slot_get(struct object *self, int index) {
    (void)self;
    if (!g_obj_bus || index < 0 || index >= NUBUS_OBJ_SLOTS)
        return NULL;
    return g_slot_nodes[index].slot;
}
static int nubus_slot_count(struct object *self) {
    (void)self;
    if (!g_obj_bus)
        return 0;
    int n = 0;
    for (int i = NUBUS_OBJ_FIRST; i <= NUBUS_OBJ_LAST; i++)
        if (g_slot_nodes[i].slot)
            n++;
    return n;
}
static int nubus_slot_next(struct object *self, int prev_index) {
    (void)self;
    if (!g_obj_bus)
        return -1;
    for (int i = prev_index + 1; i <= NUBUS_OBJ_LAST; i++)
        if (g_slot_nodes[i].slot)
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
};

const class_desc_t nubus_class = {
    .name = "nubus",
    .members = nubus_members,
    .n_members = sizeof(nubus_members) / sizeof(nubus_members[0]),
};

// === Object-tree build / teardown ===========================================

// Attach one resource child under the card node with a label/order/category.
// `data` is the node's instance data -- the nubus_card_t for every class
// defined here, and a display_fb_node_t for the shared framebuffer class, so
// it is typed as the void the object model actually stores.
static struct object *attach_resource(struct object *card_node, const class_desc_t *cls, void *data, const char *name,
                                      const char *label, int order, uint16_t category) {
    struct object *o = object_new(cls, data, name);
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
        // A node exists for every populated slot and every declared SOCKET
        // (even when empty — its staged card_id/video_mode attrs are how an
        // empty socket gets configured for the next boot).
        const nubus_slot_decl_t *decl = nubus_slot_decl_get(bus, i);
        if (!card && (!decl || decl->kind != NUBUS_SLOT_SOCKET))
            continue;
        nubus_slot_nodes_t *n = &g_slot_nodes[i];

        s_slot_numbers[i] = i;
        n->slot = object_new(&nubus_slot_class, &s_slot_numbers[i], "slot");
        if (!n->slot)
            continue;
        object_set_label(n->slot, "Slot");
        object_set_order(n->slot, i);

        if (!card)
            continue; // empty socket: just the wrapper + staged attrs

        n->card = object_new(&nubus_card_class, card, "card");
        if (n->card) {
            object_set_label(n->card, (card->ops && card->ops->name) ? card->ops->name(card) : "Card");
            object_attach(n->slot, n->card);
            n->fb_node = (display_fb_node_t){.owner = card, .resolve = nubus_fb_resolve, .base = nubus_fb_base};
            n->fb =
                attach_resource(n->card, &display_fb_class, &n->fb_node, "framebuffer", "Framebuffer", 10, M_CAT_BASIC);
            n->declrom =
                attach_resource(n->card, &nubus_declrom_class, card, "declrom", "Declaration ROM", 20, M_CAT_BASIC);
            n->clut = attach_resource(n->card, &nubus_clut_class, card, "clut", "CLUT", 30, M_CAT_BASIC);
            n->mode = attach_resource(n->card, &nubus_mode_class, card, "mode", "Mode", 40, M_CAT_BASIC);
            // Card-specific children, through the KIND that seated this slot.
            // This file never tests a card's identity -- the accelerator nodes
            // that used to live here behind is_card() belong to the cards
            // (04-video F-10), the same way pci_class.c has always done it.
            const nubus_card_kind_t *kind = nubus_slot_kind(bus, i);
            if (kind && kind->attach_objects)
                kind->attach_objects(card, n->card);
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

void nubus_objects_teardown_owned(nubus_bus_t *bus) {
    // Tear down the node trees only when they describe THIS bus.  On
    // checkpoint restore the new machine (and its tree) is built BEFORE the
    // old machine is destroyed; the old bus's teardown must not rip down
    // the new machine's freshly built tree.
    if (g_obj_bus == bus)
        nubus_objects_teardown();
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
