// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// card_slots.c
// The slot collection NuBus and PCI share (see card_slots.h).

#include "card_slots.h"

#include "machine_profile.h" // machine_object
#include "root.h"
#include "value.h"

#include <stddef.h>

// ============================================================================
// Operations
// ============================================================================

void card_slots_install(card_slot_bus_t *b, struct config *cfg, bool present) {
    if (!cfg || !present)
        return;
    // The bus node: emulated hardware, so under `machine`, not a meta object
    struct object *bus = root_attach_stub(machine_object(), object_new(b->bus_class, cfg, b->name));
    if (!bus)
        return;
    object_set_label(bus, b->label);
    object_set_order(bus, b->order);
    // Its `slot` container; the shared getter reaches the nodes through `b`
    struct object *slots = root_attach_stub(bus, object_collection_new(b->slots_desc, b, "slot"));
    if (slots) {
        object_set_label(slots, "Slots");
        object_cache_set_parent(&b->nodes, slots);
    }
}

struct object *card_slots_entry(struct object *self, int index) {
    const card_slot_bus_t *b = (const card_slot_bus_t *)object_data(self);
    if (!b || !b->owner || index < 0)
        return NULL;
    // A node exists for every declared slot: enumeration keys off it
    return object_cache_find(&b->nodes, index);
}

void card_slots_begin(card_slot_bus_t *b, const void *owner) {
    card_slots_clear(b);
    b->owner = owner;
}

struct object *card_slot_open(card_slot_bus_t *b, card_slot_t *rec, const char *label) {
    rec->seated = false;
    struct object *slot = object_cache_at(&b->nodes, rec->number, rec);
    if (!slot)
        return NULL;
    object_set_label(slot, label ? label : "Slot");
    object_set_order(slot, rec->number);
    return slot;
}

struct object *card_slot_seat(struct object *slot, card_slot_t *rec, const class_desc_t *cls, void *card,
                              const char *label) {
    struct object *node = object_new(cls, card, "card");
    if (!node)
        return NULL;
    if (label)
        object_set_label(node, label);
    object_attach(slot, node);
    rec->seated = true;
    return node;
}

void card_slots_clear(card_slot_bus_t *b) {
    object_cache_clear(&b->nodes); // frees each slot node with its subtree
    b->owner = NULL;
}

bool card_slots_owned_by(const card_slot_bus_t *b, const void *owner) {
    return owner && b->owner == owner;
}

// The slot record a slot node carries.
static const card_slot_t *slot_rec(struct object *self) {
    return (const card_slot_t *)object_data(self);
}

DEF_GETTER(card_slot_attr_number) {
    const card_slot_t *rec = slot_rec(self);
    return val_int(rec ? rec->number : -1);
}

DEF_GETTER(card_slot_attr_empty) {
    const card_slot_t *rec = slot_rec(self);
    return val_bool(!rec || !rec->seated);
}
