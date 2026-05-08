// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// nubus.h
// NuBus subsystem — bus controller, slot table, slot-IRQ aggregation, and
// the public API every glue030-family machine uses.  See
// proposal-machine-iicx-iix.md §3.2 for the full design.  Step-3 status:
// types, slot-decl shape, and the public API are in place; the bus
// controller body is a skeleton — no machine creates a bus yet, so the
// registry is empty and nubus_init is unused.  Step 4 lands the first
// card and starts using this from se30_init.

#ifndef NUBUS_H
#define NUBUS_H

#include "card.h"
#include "common.h"
#include <stdbool.h>
#include <stdint.h>

struct config;
struct checkpoint;
struct display;
typedef struct config config_t;
typedef struct checkpoint checkpoint_t;
typedef struct display display_t;

// Slot-table kinds.  See proposal §3.2.2.
typedef enum nubus_slot_kind {
    NUBUS_SLOT_ABSENT = 0, // physical absence — bus errors on access
    NUBUS_SLOT_EMPTY, // physically present, no card seated — GLUE rule
    NUBUS_SLOT_BUILTIN, // machine populates with a fixed card (e.g. SE/30 slot $E)
    NUBUS_SLOT_VIDEO, // single user-configurable video slot — pick card,
                      // monitor, depth from the dialog.  At most one VIDEO
                      // entry per machine in v1.
} nubus_slot_kind_t;

// One entry in a machine's slot table.  Sentinel-terminated arrays end at
// the entry whose `slot` is 0.
typedef struct nubus_slot_decl {
    int slot; // $9..$E (0 ends the array)
    nubus_slot_kind_t kind;
    const char *builtin_card_id; // BUILTIN: card-id resolved via nubus_card_find()
    const char *const *available_cards; // VIDEO: sentinel-terminated card-id list
    const char *default_card; // VIDEO: default when user hasn't picked
} nubus_slot_decl_t;

// Standard slot space base for slot s (s ∈ $9..$E):
//   $9 → $F9000000, $A → $FA000000, …, $E → $FE000000
static inline uint32_t nubus_slot_base(int slot) {
    return 0xF0000000u | ((uint32_t)slot << 24);
}

// Super slot space base for slot s (256 MB each).  Reserved for future
// use; v1 doesn't register these regions for any of the cards we ship.
//   $9 → $90000000, $A → $A0000000, …, $E → $E0000000
static inline uint32_t nubus_super_slot_base(int slot) {
    return ((uint32_t)slot << 28);
}

// Bus controller.  Walks the slot table at init; for each BUILTIN entry
// looks up .builtin_card_id via nubus_card_find() and calls the resolved
// factory.  For the VIDEO entry (at most one per machine) it reads the
// user's pick via the video.* pending statics and resolves through the
// same path.  Returns NULL on failure.
nubus_bus_t *nubus_init(config_t *cfg, const nubus_slot_decl_t *slots, checkpoint_t *cp);
void nubus_delete(nubus_bus_t *bus);

// Per-slot IRQ assertion.  The bus aggregates and drives VIA2 PA[0..5]
// (active-low) plus pulses CA1 on the umbrella transition.
void nubus_assert_irq(nubus_card_t *card);
void nubus_deassert_irq(nubus_card_t *card);

// Look up the active card in a slot, or NULL if unpopulated.
nubus_card_t *nubus_card(nubus_bus_t *bus, int slot);

// Return the primary display — the card whose framebuffer drives the
// canvas.  v1: first slot in declared order whose ops->display() returns
// non-NULL.
const display_t *nubus_primary_display(nubus_bus_t *bus);

// VBL fan-out.  Family code calls this from glue030_trigger_vbl after
// pulsing the GLUE-driven CA1 lines; it iterates the slot table and
// calls each populated card's ops->on_vbl().
void nubus_tick_vbl(nubus_bus_t *bus);

#endif // NUBUS_H
