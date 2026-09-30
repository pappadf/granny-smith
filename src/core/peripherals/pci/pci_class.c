// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pci_class.c
// The `machine.pci.*` object-model surface (docs/internals/core/peripherals/pci.md,
// "Object model").
//
// The nubus_class.c shape, ported — with its two warts fixed.  A slot node
// exists for EVERY declared socket and builtin, populated or not, because
// an empty socket's staged `card_id` attribute is how the next boot gets
// configured; a populated slot grows a `card` subtree whose `config` child
// exposes the live header (command/status and the six BARs plus the
// expansion-ROM BAR).  Card-specific children are attached through the
// KIND's attach_objects() hook, never by identity tests here.

#include "config_space.h"
#include "machine_profile.h"
#include "object.h"
#include "pci.h"
#include "root.h"
#include "system_config.h"
#include "value.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PCI_OBJ_SLOTS 16 // slot numbers are 1-based; [0] is the wildcard key

// One declared slot's node tree.  `dev` is NULL for an empty socket — its
// staged attrs are the whole point of the node existing.
typedef struct pci_slot_nodes {
    struct object *slot;
    struct object *card;
    struct object *config;
    struct object *bar[PCI_BAR_SLOTS];
    struct object *fb; // the card's nominated framebuffer node, if any
    pci_device_t *dev;
    int number; // instance data for the slot wrapper
    int bar_index[PCI_BAR_SLOTS]; // instance data for the bar nodes
} pci_slot_nodes_t;

static pci_root_t *g_obj_root = NULL;
static pci_slot_nodes_t g_slot_nodes[PCI_OBJ_SLOTS];

// === catalog.pci_cards ======================================================

// Every registered PCI card-driver id, as V_LIST<V_STRING> (catalog.pci_cards).
value_t pci_cards_list(void) {
    const pci_card_kind_t *const *reg = pci_card_registry();
    size_t n = 0;
    for (const pci_card_kind_t *const *p = reg; *p; p++)
        n++;
    if (n == 0)
        return val_list(NULL, 0);
    value_t *items = (value_t *)calloc(n, sizeof(value_t));
    if (!items)
        return val_err("catalog.pci_cards: out of memory");
    size_t i = 0;
    for (const pci_card_kind_t *const *p = reg; *p; p++)
        items[i++] = val_str((*p)->id);
    return val_list(items, n);
}

// === BAR nodes ==============================================================

// The slot record a config/bar node belongs to (both carry it as their
// instance data, so their accessors reach the live device and BAR index).
static pci_slot_nodes_t *node_rec(struct object *self) {
    return (pci_slot_nodes_t *)object_data(self);
}

// The BAR index a bar node stands for (its instance data is the int inside
// the owning slot record).
static int bar_index_of(struct object *self, pci_slot_nodes_t **rec_out) {
    const int *idx = (const int *)object_data(self);
    if (!idx)
        return -1;
    for (int s = 0; s < PCI_OBJ_SLOTS; s++) {
        pci_slot_nodes_t *n = &g_slot_nodes[s];
        for (int b = 0; b < PCI_BAR_SLOTS; b++) {
            if (&n->bar_index[b] == idx) {
                if (rec_out)
                    *rec_out = n;
                return b;
            }
        }
    }
    return -1;
}

static value_t bar_attr_index(struct object *self, const member_t *m) {
    (void)m;
    return val_int(bar_index_of(self, NULL));
}

static value_t bar_attr_base(struct object *self, const member_t *m) {
    (void)m;
    pci_slot_nodes_t *rec = NULL;
    int b = bar_index_of(self, &rec);
    return val_uint(4, (rec && rec->dev && b >= 0) ? pci_cfg_bar_base(rec->dev, b) : 0);
}

static value_t bar_attr_size(struct object *self, const member_t *m) {
    (void)m;
    pci_slot_nodes_t *rec = NULL;
    int b = bar_index_of(self, &rec);
    return val_uint(4, (rec && rec->dev && b >= 0) ? pci_cfg_bar_size(rec->dev, b) : 0);
}

static value_t bar_attr_mapped(struct object *self, const member_t *m) {
    (void)m;
    pci_slot_nodes_t *rec = NULL;
    int b = bar_index_of(self, &rec);
    return val_bool(rec && rec->dev && b >= 0 && pci_cfg_bar_enabled(rec->dev, b));
}

static value_t bar_attr_kind(struct object *self, const member_t *m) {
    (void)m;
    pci_slot_nodes_t *rec = NULL;
    int b = bar_index_of(self, &rec);
    if (!rec || !rec->dev || b < 0 || !rec->dev->decl)
        return val_str("");
    if (b == PCI_ROM_BAR_INDEX)
        return val_str("rom");
    switch (rec->dev->decl->bar[b].kind) {
    case PCI_BAR_MEM:
        return val_str("mem");
    case PCI_BAR_MEM_PREFETCH:
        return val_str("mem_prefetch");
    case PCI_BAR_IO:
        return val_str("io");
    case PCI_BAR_NONE:
    default:
        return val_str("");
    }
}

static const member_t bar_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .doc = "BAR number (6 = the expansion-ROM BAR at config $30)",
     .attr = {.type = V_INT, .get = bar_attr_index}                               },
    {.kind = M_ATTR,
     .name = "base",
     .doc = "Decoded base address assigned by the guest's firmware",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = bar_attr_base}},
    {.kind = M_ATTR,
     .name = "size",
     .doc = "Region size in bytes (what the $FFFFFFFF sizing probe reports)",
     .attr = {.type = V_UINT, .get = bar_attr_size}                               },
    {.kind = M_ATTR,
     .name = "kind",
     .doc = "Space this BAR decodes: mem / mem_prefetch / io / rom",
     .attr = {.type = V_STRING, .get = bar_attr_kind}                             },
    {.kind = M_ATTR,
     .name = "mapped",
     .doc = "True while the device actually decodes this region",
     .attr = {.type = V_BOOL, .get = bar_attr_mapped}                             },
};
static const class_desc_t pci_bar_class = {
    .name = "bar", .members = bar_members, .n_members = sizeof(bar_members) / sizeof(bar_members[0])};

// === config node ============================================================

static value_t cfg_attr_command(struct object *self, const member_t *m) {
    (void)m;
    pci_slot_nodes_t *n = node_rec(self);
    return val_uint(2, (n && n->dev) ? n->dev->cfg.command : 0);
}
static value_t cfg_attr_status(struct object *self, const member_t *m) {
    (void)m;
    pci_slot_nodes_t *n = node_rec(self);
    return val_uint(2, (n && n->dev) ? n->dev->cfg.status : 0);
}
static value_t cfg_attr_cache_line(struct object *self, const member_t *m) {
    (void)m;
    pci_slot_nodes_t *n = node_rec(self);
    return val_uint(1, (n && n->dev) ? n->dev->cfg.cache_line_size : 0);
}
static value_t cfg_attr_int_line(struct object *self, const member_t *m) {
    (void)m;
    pci_slot_nodes_t *n = node_rec(self);
    return val_int((n && n->dev) ? n->dev->cfg.interrupt_line : 0);
}
static value_t cfg_attr_rom_bar(struct object *self, const member_t *m) {
    (void)m;
    pci_slot_nodes_t *n = node_rec(self);
    return val_uint(4, (n && n->dev) ? n->dev->cfg.rom_bar : 0);
}
static value_t cfg_attr_rom_size(struct object *self, const member_t *m) {
    (void)m;
    pci_slot_nodes_t *n = node_rec(self);
    return val_uint(4, (n && n->dev) ? (uint64_t)n->dev->rom_size : 0);
}

// A bar node exists for every BAR the device declares, plus the ROM BAR
// when it carries one; enumeration keys off node existence.
static struct object *pci_bar_get(struct object *self, int index) {
    pci_slot_nodes_t *n = node_rec(self);
    if (!n || index < 0 || index >= PCI_BAR_SLOTS)
        return NULL;
    return n->bar[index];
}

static const member_t config_members[] = {
    {.kind = M_ATTR,
     .name = "command",
     .doc = "Config $04 command register (bit 0 = I/O, 1 = memory, 2 = bus master)",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = cfg_attr_command}},
    {.kind = M_ATTR,
     .name = "status",
     .doc = "Config $06 status register",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = cfg_attr_status} },
    {.kind = M_ATTR,
     .name = "cache_line",
     .doc = "Config $0C cache line size, in longwords",
     .attr = {.type = V_UINT, .get = cfg_attr_cache_line}                            },
    {.kind = M_ATTR,
     .name = "interrupt_line",
     .doc = "Config $3C interrupt line — the controller line number the OS stored",
     .attr = {.type = V_INT, .get = cfg_attr_int_line}                               },
    {.kind = M_ATTR,
     .name = "rom_bar",
     .doc = "Config $30 expansion-ROM BAR (bit 0 = decode enable)",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = cfg_attr_rom_bar}},
    {.kind = M_ATTR,
     .name = "rom_size",
     .doc = "Expansion-ROM image size in bytes (0 = no ROM)",
     .attr = {.type = V_UINT, .get = cfg_attr_rom_size}                              },
};
// `config.bar` -- the BAR collection: a container whose entries are the bar
// nodes.  Its instance data is the slot record, like the config node's.
static const member_t pci_bars_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .doc = "Base address registers; index 0..5, plus 6 for the expansion ROM",
     .child = {.cls = &pci_bar_class, .indexed = true, .get = pci_bar_get, .slots = PCI_BAR_SLOTS}},
};
static const class_desc_t pci_bars_class = {.name = "pci_bars",
                                            .members = pci_bars_members,
                                            .n_members = sizeof(pci_bars_members) / sizeof(pci_bars_members[0])};

static const class_desc_t pci_config_class = {
    .name = "config", .members = config_members, .n_members = sizeof(config_members) / sizeof(config_members[0])};

// === card node ==============================================================

static pci_device_t *card_dev(struct object *self) {
    return (pci_device_t *)object_data(self);
}

static value_t card_attr_name(struct object *self, const member_t *m) {
    (void)m;
    pci_device_t *d = card_dev(self);
    return val_str((d && d->ops && d->ops->name) ? d->ops->name(d) : "");
}
static value_t card_attr_vendor(struct object *self, const member_t *m) {
    (void)m;
    pci_device_t *d = card_dev(self);
    return val_uint(2, (d && d->decl) ? d->decl->vendor_id : 0);
}
static value_t card_attr_device(struct object *self, const member_t *m) {
    (void)m;
    pci_device_t *d = card_dev(self);
    return val_uint(2, (d && d->decl) ? d->decl->device_id : 0);
}
static value_t card_attr_class(struct object *self, const member_t *m) {
    (void)m;
    pci_device_t *d = card_dev(self);
    return val_uint(4, (d && d->decl) ? d->decl->class_code : 0);
}

static const member_t card_members[] = {
    {.kind = M_ATTR, .name = "name", .doc = "Device display name", .attr = {.type = V_STRING, .get = card_attr_name}},
    {.kind = M_ATTR,
     .name = "vendor_id",
     .doc = "PCI vendor id (config $00)",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = card_attr_vendor}                               },
    {.kind = M_ATTR,
     .name = "device_id",
     .doc = "PCI device id (config $02)",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = card_attr_device}                               },
    {.kind = M_ATTR,
     .name = "class_code",
     .doc = "24-bit class / subclass / prog-if (config $09..$0B)",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = card_attr_class}                                },
};
static const class_desc_t pci_card_class = {
    .name = "card", .members = card_members, .n_members = sizeof(card_members) / sizeof(card_members[0])};

// === slot wrapper node ======================================================

static int node_slot_number(struct object *self) {
    const int *n = (const int *)object_data(self);
    return n ? *n : -1;
}

static const pci_slot_decl_t *node_slot_decl(struct object *self) {
    return pci_slot_decl_get(g_obj_root, node_slot_number(self));
}

static value_t slot_attr_number(struct object *self, const member_t *m) {
    (void)m;
    return val_int(node_slot_number(self));
}
static value_t slot_attr_label(struct object *self, const member_t *m) {
    (void)m;
    const pci_slot_decl_t *d = node_slot_decl(self);
    return val_str((d && d->label) ? d->label : "");
}
static value_t slot_attr_bus(struct object *self, const member_t *m) {
    (void)m;
    const pci_slot_decl_t *d = node_slot_decl(self);
    return val_int(d ? d->bus : -1);
}
static value_t slot_attr_device(struct object *self, const member_t *m) {
    (void)m;
    const pci_slot_decl_t *d = node_slot_decl(self);
    return val_int(d ? d->device : -1);
}
static value_t slot_attr_irq(struct object *self, const member_t *m) {
    (void)m;
    const pci_slot_decl_t *d = node_slot_decl(self);
    return val_int(d ? d->int_line : -1);
}

// `slot[N].card_id` — stage a card pick for THIS slot for the next
// machine.boot (the concrete-slot sibling of machine.boot's `pci_card=`
// wildcard; a concrete entry beats the wildcard).  Only SOCKET slots
// accept a pick; "" clears.  Consumed and cleared by pci_seat_slots.
static value_t slot_attr_card_id_get(struct object *self, const member_t *m) {
    (void)m;
    const char *id = pci_staged_card_get(node_slot_number(self));
    return val_str(id ? id : "");
}

static value_t slot_attr_card_id_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    int slot = node_slot_number(self);
    if (in.kind != V_STRING) {
        value_free(&in);
        return val_err("slot[%d].card_id: expected a card-id string (see catalog.pci_cards)", slot);
    }
    const char *id = in.s ? in.s : "";
    const pci_slot_decl_t *decl = pci_slot_decl_get(g_obj_root, slot);
    if (!decl || decl->kind != PCI_SLOT_SOCKET) {
        value_free(&in);
        return val_err("slot[%d].card_id: slot is not a user-configurable socket", slot);
    }
    if (*id && !pci_card_find(id)) {
        const char *near = pci_card_suggest(id);
        value_t err = near ? val_err("slot[%d].card_id: unknown card id '%s' — did you mean '%s'? "
                                     "(see catalog.pci_cards)",
                                     slot, id, near)
                           : val_err("slot[%d].card_id: unknown card id '%s' (see catalog.pci_cards)", slot, id);
        value_free(&in);
        return err;
    }
    pci_staged_card_set(slot, id);
    value_free(&in);
    return val_none();
}

static const member_t slot_members[] = {
    {.kind = M_ATTR,
     .name = "number",
     .doc = "Logical slot number (1-based, in the machine's declared order)",
     .attr = {.type = V_INT, .get = slot_attr_number}},
    {.kind = M_ATTR,
     .name = "label",
     .doc = "Slot name silkscreened on the board (\"A1\", \"VCI\")",
     .attr = {.type = V_STRING, .get = slot_attr_label}},
    {.kind = M_ATTR,
     .name = "bus",
     .doc = "Host-bridge bus index this slot sits on",
     .attr = {.type = V_INT, .get = slot_attr_bus}},
    {.kind = M_ATTR,
     .name = "device",
     .doc = "PCI device number (IDSEL AD line) on that bus",
     .attr = {.type = V_INT, .get = slot_attr_device}},
    {.kind = M_ATTR,
     .name = "irq",
     .doc = "Interrupt-controller line the slot's strapped INTA-D reaches",
     .attr = {.type = V_INT, .get = slot_attr_irq}},
    {.kind = M_ATTR,
     .name = "card_id",
     .doc = "Staged card pick for this socket for the next machine.boot (\"\" = none)",
     .flags = 0,
     .attr = {.type = V_STRING, .get = slot_attr_card_id_get, .set = slot_attr_card_id_set}},
};
static const class_desc_t pci_slot_class = {
    .name = "slot", .members = slot_members, .n_members = sizeof(slot_members) / sizeof(slot_members[0])};

// --- indexed `slot` member ---------------------------------------------------

static struct object *pci_slot_get(struct object *self, int index) {
    (void)self;
    if (!g_obj_root || index < 0 || index >= PCI_OBJ_SLOTS)
        return NULL;
    return g_slot_nodes[index].slot;
}

// `machine.pci.slot` -- the slot collection: a container (attached under
// `machine.pci` by the install hook below) whose entries are the declared slots.
static const member_t pci_slots_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .doc = "Declared PCI slots; index by slot number, e.g. slot[1].card.config",
     .child = {.cls = &pci_slot_class, .indexed = true, .get = pci_slot_get, .slots = PCI_OBJ_SLOTS}},
};

static const class_desc_t pci_slots_class = {
    .name = "pci_slots",
    .doc = "PCI slots, by slot number",
    .members = pci_slots_members,
    .n_members = sizeof(pci_slots_members) / sizeof(pci_slots_members[0]),
};

// `machine.pci` itself carries no members of its own: its `slot` child is
// the container above.
static const class_desc_t pci_class = {
    .name = "pci",
    .doc = "The PCI expansion bus: slots and their cards",
    .members = NULL,
    .n_members = 0,
};

static struct object *g_slots_container = NULL;

// The container is freed by root_uninstall; forget it then.
static void slots_container_dtor(struct object *o) {
    if (g_slots_container == o)
        g_slots_container = NULL;
}

static void pci_objects_adopt(struct object *slots) {
    g_slots_container = slots;
    if (slots)
        object_set_destructor(slots, slots_container_dtor);
    for (int i = 0; i < PCI_OBJ_SLOTS; i++)
        if (g_slot_nodes[i].slot)
            object_set_logical_parent(g_slot_nodes[i].slot, slots, NULL, i, NULL);
}

// `machine.pci` and its slot collection, under the machine node (they are
// emulated hardware, not meta objects), on a machine with that bus.
static void pci_root_install(struct config *cfg) {
    if (!cfg || !cfg->pci)
        return;
    struct object *bus = root_attach_stub(machine_object(), object_new(&pci_class, cfg, "pci"));
    if (!bus)
        return;
    object_set_label(bus, "PCI");
    object_set_order(bus, 101);
    struct object *slots = root_attach_stub(bus, object_new(&pci_slots_class, cfg, "slot"));
    if (slots) {
        object_set_label(slots, "Slots");
        pci_objects_adopt(slots);
    }
}

// === Object-tree build / teardown ===========================================

void pci_objects_build(pci_root_t *root) {
    pci_objects_teardown(); // idempotent — drop any prior trees first
    if (!root)
        return;
    root_register_install(pci_root_install, NULL); // idempotent
    g_obj_root = root;
    for (int i = 0; i < PCI_OBJ_SLOTS; i++) {
        const pci_slot_decl_t *decl = pci_slot_decl_get(root, i);
        if (!decl)
            continue; // only DECLARED slots get nodes
        pci_slot_nodes_t *n = &g_slot_nodes[i];
        n->number = i;
        n->dev = pci_slot_device(root, i);
        n->slot = object_new(&pci_slot_class, &n->number, "slot");
        if (!n->slot)
            continue;
        object_set_label(n->slot, decl->label ? decl->label : "Slot");
        object_set_order(n->slot, i);
        if (g_slots_container)
            object_set_logical_parent(n->slot, g_slots_container, NULL, i, NULL);
        if (!n->dev)
            continue; // empty socket: the wrapper plus its staged attrs

        n->card = object_new(&pci_card_class, n->dev, "card");
        if (!n->card)
            continue;
        object_set_label(n->card, (n->dev->ops && n->dev->ops->name) ? n->dev->ops->name(n->dev) : "Card");
        object_attach(n->slot, n->card);

        // The live config header, Advanced so it doesn't clutter the
        // default SYSTEM tree.  Its instance data is the slot record: the
        // BAR children reach the device through it.
        n->config = object_new(&pci_config_class, n, "config");
        if (n->config) {
            object_set_label(n->config, "Config space");
            object_set_order(n->config, 10);
            object_set_category(n->config, M_CAT_ADVANCED);
            object_attach(n->card, n->config);
            struct object *bars = object_new(&pci_bars_class, n, "bar");
            if (bars) {
                object_set_label(bars, "BARs");
                object_set_order(bars, 10);
                object_attach(n->config, bars);
            }
            for (int b = 0; b < PCI_BAR_SLOTS; b++) {
                if (!pci_cfg_bar_size(n->dev, b))
                    continue;
                n->bar_index[b] = b;
                n->bar[b] = object_new(&pci_bar_class, &n->bar_index[b], "bar");
                if (!n->bar[b])
                    continue;
                object_set_label(n->bar[b], "BAR");
                object_set_order(n->bar[b], b);
                object_set_logical_parent(n->bar[b], bars, NULL, b, NULL);
            }
        }

        // Card-specific children, through the KIND that actually seated
        // this slot — pci_class.c never tests a device's identity.
        const pci_card_kind_t *kind = pci_slot_kind(root, i);
        if (kind && kind->attach_objects)
            kind->attach_objects(n->dev, n->card);
    }
}

void pci_card_set_framebuffer_object(pci_device_t *dev, struct object *obj) {
    if (!dev)
        return;
    for (int i = 0; i < PCI_OBJ_SLOTS; i++) {
        if (g_slot_nodes[i].dev == dev) {
            g_slot_nodes[i].fb = obj;
            return;
        }
    }
}

struct object *pci_active_framebuffer_object(void) {
    if (!g_obj_root)
        return NULL;
    pci_device_t *dev = pci_primary_display_card(g_obj_root);
    if (!dev)
        return NULL;
    for (int i = 0; i < PCI_OBJ_SLOTS; i++)
        if (g_slot_nodes[i].dev == dev)
            return g_slot_nodes[i].fb;
    return NULL;
}

void pci_objects_teardown(void) {
    for (int i = 0; i < PCI_OBJ_SLOTS; i++) {
        // The bar nodes are not attached to the config node (the indexed
        // child member serves them), so free them explicitly first.
        for (int b = 0; b < PCI_BAR_SLOTS; b++) {
            if (g_slot_nodes[i].bar[b])
                object_delete_tree(g_slot_nodes[i].bar[b]);
        }
        if (g_slot_nodes[i].slot)
            object_delete_tree(g_slot_nodes[i].slot); // slot + attached subtree
        memset(&g_slot_nodes[i], 0, sizeof(g_slot_nodes[i]));
    }
    g_obj_root = NULL;
}

void pci_objects_teardown_owned(pci_root_t *root) {
    // On checkpoint restore the new machine's tree is built BEFORE the old
    // machine is destroyed; the old root's teardown must not rip it down.
    if (g_obj_root == root)
        pci_objects_teardown();
}
