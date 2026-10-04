// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pci_class.c
// The `machine.pci.*` object-model surface (docs/internals/core/peripherals/pci.md,
// "Object model").
//
// The nubus_class.c shape, ported.  A slot node exists for every declared
// slot and describes the board: its declaration (number, label, bus, device,
// irq) and, when populated, a `card` subtree whose `config` child exposes the
// live header (command/status and the six BARs plus the
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

#define PCI_OBJ_SLOTS 16 // slot numbers are 1-based

// One declared slot's node tree.  `dev` is NULL for an empty socket.
typedef struct pci_slot_nodes {
    struct object *slot;
    struct object *card;
    struct object *config;
    object_cache_t bars; // the bar nodes, by BAR index; their data is this record
    struct object *fb; // the card's nominated framebuffer node, if any
    pci_device_t *dev;
    int number; // instance data for the slot wrapper
} pci_slot_nodes_t;

static pci_root_t *g_obj_root = NULL;
static pci_slot_nodes_t g_slot_nodes[PCI_OBJ_SLOTS];
// The slot wrapper objects, by slot number.
static const class_desc_t pci_slot_class;
static object_cache_t g_slot_objects = OBJECT_CACHE(&pci_slot_class, "slot");

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

// The BAR index a bar node stands for (its cache index), and the slot record
// it belongs to (its instance data).
static int bar_index_of(struct object *self, pci_slot_nodes_t **rec_out) {
    if (rec_out)
        *rec_out = node_rec(self);
    return object_entry_index(self);
}

static DEF_GETTER(bar_attr_index) {
    return val_int(bar_index_of(self, NULL));
}

static DEF_GETTER(bar_attr_base) {
    pci_slot_nodes_t *rec = NULL;
    int b = bar_index_of(self, &rec);
    return val_uint(4, (rec && rec->dev && b >= 0) ? pci_cfg_bar_base(rec->dev, b) : 0);
}

static DEF_GETTER(bar_attr_size) {
    pci_slot_nodes_t *rec = NULL;
    int b = bar_index_of(self, &rec);
    return val_uint(4, (rec && rec->dev && b >= 0) ? pci_cfg_bar_size(rec->dev, b) : 0);
}

static DEF_GETTER(bar_attr_mapped) {
    pci_slot_nodes_t *rec = NULL;
    int b = bar_index_of(self, &rec);
    return val_bool(rec && rec->dev && b >= 0 && pci_cfg_bar_enabled(rec->dev, b));
}

static DEF_GETTER(bar_attr_kind) {
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

static DEF_GETTER(cfg_attr_command) {
    pci_slot_nodes_t *n = node_rec(self);
    return val_uint(2, (n && n->dev) ? n->dev->cfg.command : 0);
}
static DEF_GETTER(cfg_attr_status) {
    pci_slot_nodes_t *n = node_rec(self);
    return val_uint(2, (n && n->dev) ? n->dev->cfg.status : 0);
}
static DEF_GETTER(cfg_attr_cache_line) {
    pci_slot_nodes_t *n = node_rec(self);
    return val_uint(1, (n && n->dev) ? n->dev->cfg.cache_line_size : 0);
}
static DEF_GETTER(cfg_attr_int_line) {
    pci_slot_nodes_t *n = node_rec(self);
    return val_int((n && n->dev) ? n->dev->cfg.interrupt_line : 0);
}
static DEF_GETTER(cfg_attr_rom_bar) {
    pci_slot_nodes_t *n = node_rec(self);
    return val_uint(4, (n && n->dev) ? n->dev->cfg.rom_bar : 0);
}
static DEF_GETTER(cfg_attr_rom_size) {
    pci_slot_nodes_t *n = node_rec(self);
    return val_uint(4, (n && n->dev) ? (uint64_t)n->dev->rom_size : 0);
}

// A bar node exists for every BAR the device declares, plus the ROM BAR
// when it carries one; enumeration keys off node existence.
static struct object *pci_bar_get(struct object *self, int index) {
    pci_slot_nodes_t *n = node_rec(self);
    if (!n || index < 0 || index >= PCI_BAR_SLOTS)
        return NULL;
    return object_cache_find(&n->bars, index);
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
static const collection_desc_t pci_bars = {
    .entry = &pci_bar_class,
    .by_index = {.get = pci_bar_get, .slots = PCI_BAR_SLOTS},
    .name = "pci_bars",
    .entries_doc = "Base address registers; index 0..5, plus 6 for the expansion ROM",
};

static const class_desc_t pci_config_class = {
    .name = "config", .members = config_members, .n_members = sizeof(config_members) / sizeof(config_members[0])};

// === card node ==============================================================

static pci_device_t *card_dev(struct object *self) {
    return (pci_device_t *)object_data(self);
}

static DEF_GETTER(card_attr_name) {
    pci_device_t *d = card_dev(self);
    return val_str((d && d->ops && d->ops->name) ? d->ops->name(d) : "");
}
static DEF_GETTER(card_attr_id) {
    pci_device_t *d = card_dev(self);
    const pci_card_kind_t *k = d && g_obj_root ? pci_slot_kind(g_obj_root, d->slot_index) : NULL;
    return val_str(k && k->id ? k->id : "");
}
static DEF_GETTER(card_attr_vendor) {
    pci_device_t *d = card_dev(self);
    return val_uint(2, (d && d->decl) ? d->decl->vendor_id : 0);
}
static DEF_GETTER(card_attr_device) {
    pci_device_t *d = card_dev(self);
    return val_uint(2, (d && d->decl) ? d->decl->device_id : 0);
}
static DEF_GETTER(card_attr_class) {
    pci_device_t *d = card_dev(self);
    return val_uint(4, (d && d->decl) ? d->decl->class_code : 0);
}

static const member_t card_members[] = {
    {.kind = M_ATTR,
     .name = "id",
     .doc = "Card kind id (catalog.pci_cards)",
     .attr = {.type = V_STRING, .get = card_attr_id}                                                                },
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

static DEF_GETTER(slot_attr_number) {
    return val_int(node_slot_number(self));
}
static DEF_GETTER(slot_attr_label) {
    const pci_slot_decl_t *d = node_slot_decl(self);
    return val_str((d && d->label) ? d->label : "");
}
static DEF_GETTER(slot_attr_firmware_name) {
    const pci_slot_decl_t *d = node_slot_decl(self);
    return val_str((d && d->detail) ? d->detail : "");
}
static DEF_GETTER(slot_attr_bus) {
    const pci_slot_decl_t *d = node_slot_decl(self);
    return val_int(d ? d->bus : -1);
}
static DEF_GETTER(slot_attr_device) {
    const pci_slot_decl_t *d = node_slot_decl(self);
    return val_int(d ? d->device : -1);
}
static DEF_GETTER(slot_attr_irq) {
    const pci_slot_decl_t *d = node_slot_decl(self);
    return val_int(d ? d->int_line : -1);
}

static const member_t slot_members[] = {
    {.kind = M_ATTR,
     .name = "number",
     .doc = "Logical slot number (1-based, in the machine's declared order)",
     .attr = {.type = V_INT, .get = slot_attr_number}          },
    {.kind = M_ATTR,
     .name = "label",
     .doc = "The slot's name, as Apple's documentation gives it (\"PCI slot A1\")",
     .attr = {.type = V_STRING, .get = slot_attr_label}        },
    {.kind = M_ATTR,
     .name = "firmware_name",
     .doc = "The firmware's own name for the slot (\"A1\", \"SLOT1_PCI0\", \"VCI\")",
     .attr = {.type = V_STRING, .get = slot_attr_firmware_name}},
    {.kind = M_ATTR,
     .name = "bus",
     .doc = "Host-bridge bus index this slot sits on",
     .attr = {.type = V_INT, .get = slot_attr_bus}             },
    {.kind = M_ATTR,
     .name = "device",
     .doc = "PCI device number (IDSEL AD line) on that bus",
     .attr = {.type = V_INT, .get = slot_attr_device}          },
    {.kind = M_ATTR,
     .name = "irq",
     .doc = "Interrupt-controller line the slot's strapped INTA-D reaches",
     .attr = {.type = V_INT, .get = slot_attr_irq}             },
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
// `machine.pci` by the install hook below) whose entries are the declared
// slots.
static const collection_desc_t pci_slots = {
    .entry = &pci_slot_class,
    .by_index = {.get = pci_slot_get, .slots = PCI_OBJ_SLOTS},
    .name = "pci_slots",
    .doc = "PCI slots, by slot number",
    .entries_doc = "Declared PCI slots; index by slot number, e.g. slot[1].card.config",
};

// `machine.pci` itself carries no members of its own: its `slot` child is
// the container above.
static const class_desc_t pci_class = {
    .name = "pci",
    .doc = "The PCI expansion bus: slots and their cards",
    .members = NULL,
    .n_members = 0,
};

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
    struct object *slots = root_attach_stub(bus, object_collection_new(&pci_slots, cfg, "slot"));
    if (slots) {
        object_set_label(slots, "Slots");
        object_cache_set_parent(&g_slot_objects, slots);
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
        n->slot = object_cache_at(&g_slot_objects, i, &n->number);
        if (!n->slot)
            continue;
        object_set_label(n->slot, decl->label ? decl->label : "Slot");
        object_set_order(n->slot, i);
        if (!n->dev)
            continue; // empty socket: its declaration only

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
            struct object *bars = object_collection_new(&pci_bars, n, "bar");
            if (bars) {
                object_set_label(bars, "BARs");
                object_set_order(bars, 10);
                object_attach(n->config, bars);
            }
            n->bars = (object_cache_t)OBJECT_CACHE(&pci_bar_class, "bar");
            object_cache_set_parent(&n->bars, bars);
            for (int b = 0; b < PCI_BAR_SLOTS; b++) {
                if (!pci_cfg_bar_size(n->dev, b))
                    continue;
                struct object *bar = object_cache_at(&n->bars, b, n);
                if (!bar)
                    continue;
                object_set_label(bar, "BAR");
                object_set_order(bar, b);
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
    // The bar nodes are not attached to the config node (the collection
    // serves them), so free them first; then each slot with its subtree.
    for (int i = 0; i < PCI_OBJ_SLOTS; i++)
        object_cache_clear(&g_slot_nodes[i].bars);
    object_cache_clear(&g_slot_objects);
    memset(g_slot_nodes, 0, sizeof(g_slot_nodes));
    g_obj_root = NULL;
}

void pci_objects_teardown_owned(pci_root_t *root) {
    // On checkpoint restore the new machine's tree is built BEFORE the old
    // machine is destroyed; the old root's teardown must not rip it down.
    if (g_obj_root == root)
        pci_objects_teardown();
}
