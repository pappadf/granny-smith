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
struct object;
typedef struct config config_t;
typedef struct checkpoint checkpoint_t;
typedef struct display display_t;

// Slot-table kinds.  See proposal §3.2.2 and
// proposal-nubus-computed-card-compatibility.md §5.2.
typedef enum nubus_slot_kind {
    NUBUS_SLOT_ABSENT = 0, // physical absence — bus errors on access
    NUBUS_SLOT_EMPTY, // electrically decoded as empty (GLUE rule) but NOT
                      // user-populatable — no connector (SE/30 $9..$B)
    NUBUS_SLOT_BUILTIN, // machine populates with a fixed card (e.g. SE/30 slot $E)
    NUBUS_SLOT_SOCKET, // physical connector — user-populatable; behaves
                       // exactly like EMPTY when no card is configured.
                       // A machine may declare any number of sockets.
} nubus_slot_kind_t;

// One entry in a machine's slot table.  Sentinel-terminated arrays end at
// the entry whose `slot` is 0.  The machine declares TOPOLOGY only — which
// slots exist, which hold a soldered-down builtin pseudo-card, which are
// user-configurable, and what card a configurable slot ships with by
// default.  Which cards *fit* a configurable slot is not a machine fact:
// it is computed from each card kind's declared attachment
// (nubus_card_fits_socket; proposal-nubus-computed-card-compatibility.md).
typedef struct nubus_slot_decl {
    int slot; // $9..$E (0 ends the array)
    nubus_slot_kind_t kind;
    const char *builtin_card_id; // BUILTIN: card-id resolved via nubus_card_find()
    const char *default_card; // SOCKET: factory-default population when the
                              // user hasn't staged a pick (NULL = ships empty)
} nubus_slot_decl_t;

// True iff card kind `kind` may be seated in slot `s`.  Compatibility is
// COMPUTED — the machine declares topology, the card declares its physical
// attachment (card_attach_t); nobody enumerates (machine, card) pairs.
// Today's rule is the bus standard's: any NuBus-attach card fits any
// user-configurable slot.  Shared by the machine.profile encoder and
// nubus_init's pick validation so the two can never diverge.
bool nubus_card_fits_socket(const nubus_slot_decl_t *s, const nubus_card_kind_t *kind);

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

// === Staged per-slot configuration (proposal §5.6, stage 2) ================
//
// User picks for the NEXT machine.boot live in a small staged table keyed by
// slot number, consumed (and cleared) by nubus_init.  Slot 0 is the WILDCARD
// entry meaning "the machine's first SOCKET" — the machine-independent
// staging channel behind `machine.nubus.video_card` (and the headless
// `video_card=` startup arg), preserved from the single-pending era.
// Concrete slots ($9..$E) are staged via `machine.nubus.slot[N].card_id` /
// `.video_mode`; a concrete entry beats the wildcard for that slot.  At
// boot, a staged card is honoured iff it fits the slot per
// nubus_card_fits_socket (else it logs and the slot falls back to
// default_card); a staged mode is routed to the resolved card kind's
// pending-mode channel just before its factory runs.
#define NUBUS_STAGED_WILDCARD 0 // staged-table key for "first socket"
void nubus_staged_card_set(int slot, const char *id); // NULL/"" clears
const char *nubus_staged_card_get(int slot);
void nubus_staged_mode_set(int slot, const char *id); // NULL/"" clears
const char *nubus_staged_mode_get(int slot);

// Wildcard-entry conveniences — the pre-stage-2 API, kept for the alias
// attribute and the headless startup arg.  Equivalent to
// nubus_staged_card_set/get(NUBUS_STAGED_WILDCARD, id).
void nubus_pending_video_card_set(const char *id);
const char *nubus_pending_video_card_get(void);

// The slot declaration for slot `slot` on the running bus, or NULL when the
// bus doesn't declare it.  Backs the object model's staged-attr validation
// and empty-socket node enumeration.
const nubus_slot_decl_t *nubus_slot_decl_get(nubus_bus_t *bus, int slot);

// Per-slot IRQ assertion.  The bus aggregates and drives VIA2 PA[0..5]
// (active-low) plus pulses CA1 on the umbrella transition.
void nubus_assert_irq(nubus_card_t *card);
void nubus_deassert_irq(nubus_card_t *card);

// Look up the active card in a slot, or NULL if unpopulated.
nubus_card_t *nubus_card(nubus_bus_t *bus, int slot);

// Return the primary display — the card whose framebuffer drives the
// canvas.  v1: first slot in declared order whose ops->display() returns
// non-NULL.
display_t *nubus_primary_display(nubus_bus_t *bus);

// The card behind nubus_primary_display() (same selection rule), or NULL.
// Used by the object model to wire `machine.screen.source` to the active
// card's framebuffer node.
nubus_card_t *nubus_primary_display_card(nubus_bus_t *bus);

// === Object-model surface (proposal §3.8) ===================================
//
// Build / tear down the per-slot `slot[N].card.{framebuffer,declrom,clut,mode}`
// object trees for every populated slot.  nubus_init calls _build after the
// cards exist; nubus_delete calls _teardown before freeing them.  The node
// objects are owned here (object_delete_tree on teardown), not by the bus.
void nubus_objects_build(nubus_bus_t *bus);
void nubus_objects_teardown(void);

// The framebuffer node object of the active (primary-display) card, or NULL —
// the target of the `machine.screen.source` reference edge.  Re-resolved on
// demand so a card swap can never leave it dangling.
struct object *nubus_active_framebuffer_object(void);

// VBL fan-out.  Family code calls this from glue030_trigger_vbl after
// pulsing the GLUE-driven CA1 lines; it iterates the slot table and
// calls each populated card's ops->on_vbl().
void nubus_tick_vbl(nubus_bus_t *bus);

// /RESET fan-out.  system_reset_devices calls this on the 68k RESET
// instruction; it resets each populated card to power-on via ops->reset.
void nubus_reset(nubus_bus_t *bus);

#endif // NUBUS_H
