// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// nubus_class.c
// `machine.nubus.*` object-model surface: the per-slot
// `machine.nubus.slot[N].card.*` node trees, and nubus_cards_list() behind
// catalog.nubus_cards, which walks nubus_card_registry().

#include "card.h"
#include "card_registry.h"
#include "card_slots.h"
#include "display.h"
#include "display_class.h"
#include "machine_profile.h"
#include "nubus.h"
#include "object.h"
#include "object_order.h"
#include "root.h"
#include "system_internal.h"
#include "value.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Every registered card-driver id, as VK_LIST<VK_STRING> (catalog.nubus_cards).
// The config dialog populates the per-slot card-type dropdown from it
// without baking the list into the JS.
value_t nubus_cards_list(void) {
    return card_registry_list((const void *const *)nubus_card_registry(), "catalog.nubus_cards");
}

// Does any registered card offer this "monitor_Nbpp" id?  Asked of the
// REGISTRY rather than of three named cards: every display kind carries its
// own catalogue in `.monitors`, and nubus_monitor_mode_lookup answers the same
// question against any of them.  The named-card version meant core knew which
// display cards exist -- and silently gave the wrong answer for a fourth.
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

// === Per-card object-model surface ==========================================
//
// machine.nubus.slot[N].card.{framebuffer,declrom,clut,mode[,engine]}.  The
// node objects are created by nubus_objects_build() (called from nubus_init,
// once the cards exist) and live here keyed by slot; nubus_objects_teardown()
// (from nubus_delete) frees the trees before the cards they read go away.
// The slot nodes are the shared slot collection's (card_slots.h, the same
// as PCI's): one per slot the machine declares (socket or built in),
// `empty` with no `card` child when nothing is seated.  The card node and
// its resource children carry the nubus_card_t as instance_data, so their
// accessors read live card state (the display_t, the declrom, the card
// engine) and copy nothing; the slot node carries only its card_slot_t.
// Resolution and SYSTEM-tab enumeration both work through object_attach: the
// `slot` indexed member returns the slot node, whose attached `card` child
// (and its attached resource children) the resolver/meta-walker discover.

#define NUBUS_OBJ_SLOTS 16 // slot index space ($0..$F); cards seat in $9..$E
#define NUBUS_OBJ_FIRST 9
#define NUBUS_OBJ_LAST  14

typedef struct {
    card_slot_t rec; // the slot node's instance data
    struct object *slot; // slot node (returned by the indexed `slot` member)
    struct object *card; // the seated card
    struct object *fb; // framebuffer
    struct object *declrom; // declaration ROM
    struct object *clut; // palette
    struct object *mode; // current monitor / depth
    char card_label[96]; // the card node's label (borrowed by the object)
    display_fb_node_t fb_node; // instance data for the shared framebuffer class
} nubus_slot_nodes_t;

static nubus_slot_nodes_t g_slot_nodes[NUBUS_OBJ_SLOTS];
// The slot collection (machine.nubus and its slot nodes), the shared shape.
static const class_desc_t nubus_class, nubus_slot_class;
static const collection_desc_t nubus_slots;
static card_slot_bus_t g_slots = {
    .name = "nubus",
    .label = "NuBus",
    .order = OBJ_ORDER_NUBUS,
    .bus_class = &nubus_class,
    .slots_desc = &nubus_slots,
    .nodes = CARD_SLOT_NODES(&nubus_slot_class),
};

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
// the built-in chips.  All this side supplies is how to reach a card's live
// descriptor and where its framebuffer sits.
static display_t *nubus_fb_resolve(void *owner) {
    nubus_card_t *c = (nubus_card_t *)owner;
    return (c && c->ops && c->ops->display) ? c->ops->display(c) : NULL;
}
static uint64_t nubus_fb_base(void *owner) {
    nubus_card_t *c = (nubus_card_t *)owner;
    return (c && c->ops && c->ops->fb_base) ? c->ops->fb_base(c) : 0;
}

// --- declrom node -----------------------------------------------------------
static DEF_GETTER(declrom_attr_size) {
    nubus_card_t *c = node_card(self);
    return val_uint(4, c ? (uint64_t)c->declrom_size : 0);
}
static DEF_GETTER(declrom_attr_present) {
    nubus_card_t *c = node_card(self);
    return val_bool(c && c->declrom && c->declrom_size > 0);
}
static DEF_GETTER(declrom_attr_path) {
    nubus_card_t *c = node_card(self);
    return val_str(c && c->rom_path ? c->rom_path : "");
}
static DEF_GETTER(declrom_attr_crc) {
    nubus_card_t *c = node_card(self);
    return val_uint(4, c ? c->rom_crc : 0);
}
static const member_t declrom_members[] = {
    {.kind = MK_ATTR,
     .name = "path",
     .doc = "The file the declaration ROM was loaded from; \"builtin:<card>\" for a generated one",
     .attr = {.type = VK_STRING, .get = declrom_attr_path}                              },
    {.kind = MK_ATTR,
     .name = "crc",
     .doc = "The declaration ROM's Format-Block CRC: which revision the card runs",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_HEX, .get = declrom_attr_crc}},
    {.kind = MK_ATTR,
     .name = "size",
     .doc = "Declaration ROM size in bytes (bus-space, byte-lane expanded)",
     .attr = {.type = VK_UINT, .get = declrom_attr_size}                                },
    {.kind = MK_ATTR,
     .name = "present",
     .doc = "True if a declaration ROM is loaded",
     .attr = {.type = VK_BOOL, .get = declrom_attr_present}                             },
};
static const class_desc_t nubus_declrom_class = {
    .name = "declrom", .members = declrom_members, .n_members = sizeof(declrom_members) / sizeof(declrom_members[0])};

// --- clut node --------------------------------------------------------------
static DEF_GETTER(clut_attr_len) {
    display_t *d = node_disp(self);
    return val_int(d ? (int)d->clut_len : 0);
}
static const member_t clut_members[] = {
    {.kind = MK_ATTR,
     .name = "len",
     .doc = "Number of palette entries",
     .attr = {.type = VK_INT, .get = clut_attr_len}},
};
static const class_desc_t nubus_clut_class = {
    .name = "clut", .members = clut_members, .n_members = sizeof(clut_members) / sizeof(clut_members[0])};

// --- mode node (current monitor / depth) ------------------------------------
// Same numbers as the framebuffer node, under the card's own `mode` child --
// this one carries the CARD as instance data, so it reads the descriptor
// through node_disp rather than through a display_fb_node_t.
static DEF_GETTER(mode_attr_width) {
    display_t *d = node_disp(self);
    return val_int(d ? (int)d->width : 0);
}
static DEF_GETTER(mode_attr_height) {
    display_t *d = node_disp(self);
    return val_int(d ? (int)d->height : 0);
}
static DEF_GETTER(mode_attr_depth) {
    display_t *d = node_disp(self);
    return val_int(d ? (int)display_bpp(d->format) : 0);
}
static DEF_GETTER(mode_attr_format) {
    display_t *d = node_disp(self);
    return val_str(d ? display_format_name(d->format) : "");
}
static const member_t mode_members[] = {
    {.kind = MK_ATTR,
     .name = "width",
     .doc = "Current monitor width in pixels",
     .attr = {.type = VK_INT, .get = mode_attr_width}    },
    {.kind = MK_ATTR,
     .name = "height",
     .doc = "Current monitor height in pixels",
     .attr = {.type = VK_INT, .get = mode_attr_height}   },
    {.kind = MK_ATTR,
     .name = "depth",
     .doc = "Current pixel depth (bpp)",
     .attr = {.type = VK_INT, .get = mode_attr_depth}    },
    {.kind = MK_ATTR,
     .name = "format",
     .doc = "Current pixel encoding",
     .attr = {.type = VK_STRING, .get = mode_attr_format}},
};
static const class_desc_t nubus_mode_class = {
    .name = "mode", .members = mode_members, .n_members = sizeof(mode_members) / sizeof(mode_members[0])};

// --- card node --------------------------------------------------------------
// The card's display name, and which declaration ROM it runs.
static void card_label(const nubus_card_t *c, char *buf, size_t len) {
    const nubus_card_kind_t *k = c ? nubus_seated_kind(c->bus, c->slot) : NULL;
    snprintf(buf, len, "%s%s", k && k->display_name ? k->display_name : "Card",
             c && c->substitute ? " (substitute ROM)" : "");
}
static DEF_GETTER(card_attr_name) {
    char name[96];
    card_label(node_card(self), name, sizeof name);
    return val_str(name);
}
static DEF_GETTER(card_attr_substitute) {
    nubus_card_t *c = node_card(self);
    return val_bool(c && c->substitute);
}
static DEF_GETTER(card_attr_monitor) {
    nubus_card_t *c = node_card(self);
    const slot_opts_t *e = c ? nubus_seat(c->bus, c->slot) : NULL;
    return val_str(e ? e->monitor : "");
}
static DEF_GETTER(card_attr_sense) {
    nubus_card_t *c = node_card(self);
    const slot_opts_t *e = c ? nubus_seat(c->bus, c->slot) : NULL;
    return val_uint(1, e ? e->sense : MACHINE_SENSE_NONE);
}
static DEF_GETTER(card_attr_slot) {
    nubus_card_t *c = node_card(self);
    return val_int(c ? c->slot : -1);
}
static DEF_GETTER(card_attr_id) {
    nubus_card_t *c = node_card(self);
    const nubus_card_kind_t *k = c ? nubus_seated_kind(c->bus, c->slot) : NULL;
    return val_str(k && k->id ? k->id : "");
}
static const member_t card_members[] = {
    {.kind = MK_ATTR,
     .name = "id",
     .doc = "Card kind id (catalog.nubus_cards)",
     .attr = {.type = VK_STRING, .get = card_attr_id}                                                                                      },
    {.kind = MK_ATTR, .name = "name", .doc = "Card display name",                        .attr = {.type = VK_STRING, .get = card_attr_name}},
    {.kind = MK_ATTR,
     .name = "substitute",
     .doc = "Runs the emulator's substitute declaration ROM instead of Apple's",
     .attr = {.type = VK_BOOL, .get = card_attr_substitute}                                                                                },
    {.kind = MK_ATTR,
     .name = "monitor",
     .doc = "The monitor on the card's connector: one of its monitor rows, or \"none\"",
     .attr = {.type = VK_STRING, .get = card_attr_monitor}                                                                                 },
    {.kind = MK_ATTR,
     .name = "sense",
     .doc = "The code the connector's monitor-sense lines read (7: nothing connected)",
     .attr = {.type = VK_UINT, .get = card_attr_sense}                                                                                     },
    {.kind = MK_ATTR,
     .name = "slot",
     .doc = "NuBus slot number ($9..$E)",
     .attr = {.type = VK_INT, .presentation_flags = VFLAG_HEX, .get = card_attr_slot}                                                      },
};
static const class_desc_t nubus_card_class = {
    .name = "card", .members = card_members, .n_members = sizeof(card_members) / sizeof(card_members[0])};

// --- slot node ----------------------------------------------------------------
//
// One per declared slot (card_slots.h): its number, whether it is empty, and
// the card child when it is not.

static const member_t slot_members[] = {
    {.kind = MK_ATTR,
     .name = "number",
     .doc = "NuBus slot number ($9..$E)",
     .attr = {.type = VK_INT, .presentation_flags = VFLAG_HEX, .get = card_slot_attr_number}},
    {.kind = MK_ATTR,
     .name = "empty",
     .doc = "True when no card is seated in the slot (it then has no `card` child)",
     .attr = {.type = VK_BOOL, .get = card_slot_attr_empty}                                 },
};
static const class_desc_t nubus_slot_class = {
    .name = "slot", .members = slot_members, .n_members = sizeof(slot_members) / sizeof(slot_members[0])};

// `machine.nubus.slot` -- the slot collection: a container (attached under
// `machine.nubus` by the install hook below) whose entries are the declared
// slots.
static const collection_desc_t nubus_slots = {
    .entry = &nubus_slot_class,
    .by_index = {.get = card_slots_entry, .slots = NUBUS_OBJ_LAST + 1},
    .name = "nubus_slots",
    .doc = "NuBus slots, by slot number",
    .entries_doc = "Declared NuBus slots ($9..$E), populated or empty; index by slot number, e.g. "
                   "slot[9].card.framebuffer",
};

// `machine.nubus` itself carries no members of its own: its `slot` child is
// the container above.
static const class_desc_t nubus_class = {
    .name = "nubus",
    .doc = "The NuBus expansion bus: slots and their cards",
    .members = NULL,
    .n_members = 0,
};

// `machine.nubus` and its slot collection, on a machine with that bus.
static void nubus_root_install(struct config *cfg) {
    card_slots_install(&g_slots, cfg, cfg && cfg->nubus);
}

// === Object-tree build / teardown ===========================================

void nubus_objects_build(nubus_bus_t *bus) {
    nubus_objects_teardown(); // idempotent — drop any prior trees first
    if (!bus)
        return;
    root_register_install(nubus_root_install, NULL); // idempotent
    card_slots_begin(&g_slots, bus);
    for (int i = NUBUS_OBJ_FIRST; i <= NUBUS_OBJ_LAST; i++) {
        const nubus_slot_decl_t *decl = nubus_slot_decl_get(bus, i);
        if (!decl || (decl->kind != NUBUS_SLOT_SOCKET && decl->kind != NUBUS_SLOT_BUILTIN))
            continue; // a node exists for every slot that takes a card
        nubus_slot_nodes_t *n = &g_slot_nodes[i];
        n->rec.number = i;
        n->slot = card_slot_open(&g_slots, &n->rec, decl->label);
        nubus_card_t *card = nubus_card(bus, i);
        if (!n->slot || !card)
            continue; // empty: the slot node only

        // The label is the node's own: object_set_label borrows the string
        card_label(card, n->card_label, sizeof n->card_label);
        n->card = card_slot_seat(n->slot, &n->rec, &nubus_card_class, card, n->card_label);
        if (n->card) {
            n->fb_node = (display_fb_node_t){.owner = card, .resolve = nubus_fb_resolve, .base = nubus_fb_base};
            n->fb = object_attach_new(n->card, &display_fb_class, &n->fb_node, "framebuffer", "Framebuffer", 10);
            n->declrom = object_attach_new(n->card, &nubus_declrom_class, card, "declrom", "Declaration ROM", 20);
            n->clut = object_attach_new(n->card, &nubus_clut_class, card, "clut", "CLUT", 30);
            n->mode = object_attach_new(n->card, &nubus_mode_class, card, "mode", "Mode", 40);
            // Card-specific children, through the KIND that seated this slot.
            // This file never tests a card's identity -- the accelerator nodes
            // that used to live here behind is_card() belong to the cards,
            // the same way pci_class.c has always done it.
            const nubus_card_kind_t *kind = nubus_seated_kind(bus, i);
            if (kind && kind->attach_objects)
                kind->attach_objects(card, n->card);
        }
    }
}

void nubus_objects_teardown(void) {
    card_slots_clear(&g_slots); // frees each slot + attached subtree
    memset(g_slot_nodes, 0, sizeof(g_slot_nodes));
}

void nubus_objects_teardown_owned(nubus_bus_t *bus) {
    // Only when the trees describe THIS bus (card_slots_owned_by)
    if (card_slots_owned_by(&g_slots, bus))
        nubus_objects_teardown();
}

struct object *nubus_active_framebuffer_object(void) {
    nubus_bus_t *bus = (nubus_bus_t *)g_slots.owner;
    if (!bus)
        return NULL;
    nubus_card_t *card = nubus_connected_display_card(bus);
    if (!card)
        return NULL;
    for (int i = 0; i < NUBUS_OBJ_SLOTS; i++)
        if (g_slot_nodes[i].card && nubus_card(bus, i) == card)
            return g_slot_nodes[i].fb;
    return NULL;
}
