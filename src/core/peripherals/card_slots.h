// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// card_slots.h
// The slot collection NuBus (nubus_class.c) and PCI (pci_class.c) build the
// same way, the object-model half of the card-registry layer
// (card_registry.h): the bus node `machine.<bus>` with its `slot`
// container, one slot node per slot the machine declares, and the `card`
// child of a populated one.
//
// Every declared slot has a node, populated or not.  An empty slot is its
// node with `empty` true and no `card` child; a populated one has `empty`
// false and its card under `card`.  Each bus adds its own members to the
// slot class (PCI's declaration: label, bus, device, irq) and its own card
// subtree; the shared part is how the nodes come into being and what
// `number` and `empty` say.

#ifndef CARD_SLOTS_H
#define CARD_SLOTS_H

// === Includes ===

#include "object.h"

#include <stdbool.h>

struct config;

// === Type Definitions ===

// One slot node's instance data: which slot it is, and whether a card node
// sits under it.  Each bus keeps one per slot.
typedef struct card_slot {
    int number; // the slot number, as `slot[N]` indexes it
    bool seated; // a `card` child is attached
} card_slot_t;

// One bus's slot collection.  The bus fills the descriptor half statically;
// the live half is this module's.
typedef struct card_slot_bus {
    const char *name; // the bus node's path segment ("nubus", "pci")
    const char *label; // its SYSTEM-tree label ("NuBus", "PCI")
    int order; // its object_order under `machine` (OBJ_ORDER_*)
    const class_desc_t *bus_class; // the bus node's class
    const collection_desc_t *slots_desc; // the `slot` container; its by_index.get is card_slots_entry
    object_cache_t nodes; // live: the slot nodes, by slot number
    const void *owner; // live: the bus the nodes describe (NULL: none built)
} card_slot_bus_t;

// The slot cache's initialiser, for the descriptor's `nodes` member.
#define CARD_SLOT_NODES(slot_cls) OBJECT_CACHE((slot_cls), "slot")

// === Operations ===

// The root-install body (a bus's root_install_fn): attaches `machine.<bus>`
// and its `slot` container under the machine node when `present` (the
// machine has the bus).
void card_slots_install(card_slot_bus_t *b, struct config *cfg, bool present);

// The `slot` container's by_index getter, shared: the slot node for `index`,
// or NULL for a slot the machine does not declare.
struct object *card_slots_entry(struct object *self, int index);

// Start a build of `owner`'s slot nodes, dropping any earlier ones.
void card_slots_begin(card_slot_bus_t *b, const void *owner);

// The node of declared slot `rec->number` (made now, `rec` its instance
// data), labelled `label` ("Slot" when NULL) and ordered by its number;
// empty until card_slot_seat.  NULL when it cannot be made.
struct object *card_slot_open(card_slot_bus_t *b, card_slot_t *rec, const char *label);

// Seat a card node under `slot` (made by card_slot_open with `rec`): class
// `cls` over `card`, named `card`, labelled `label`.  The slot reads
// populated from here on.  Returns the card node, or NULL.
struct object *card_slot_seat(struct object *slot, card_slot_t *rec, const class_desc_t *cls, void *card,
                              const char *label);

// Free every slot node and its subtree.
void card_slots_clear(card_slot_bus_t *b);

// Do the nodes describe `owner`?  On checkpoint restore the new machine's
// tree is built BEFORE the old machine is destroyed, so the old bus's
// teardown asks first and leaves the new tree alone.
bool card_slots_owned_by(const card_slot_bus_t *b, const void *owner);

// The slot node's `number` and `empty` attributes, for each bus's slot class.
value_t card_slot_attr_number(struct object *self, const struct member *m);
value_t card_slot_attr_empty(struct object *self, const struct member *m);

#endif // CARD_SLOTS_H
