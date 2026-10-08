// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// nubus.h
// NuBus subsystem — bus controller, slot table, slot-IRQ aggregation, and
// the public API every glue030-family machine uses.

#ifndef NUBUS_H
#define NUBUS_H

#include "card.h"
#include "checkpoint.h"
#include "common.h"
#include "value.h"
#include <stdbool.h>
#include <stdint.h>

struct config;
struct checkpoint;
struct display;
struct object;
typedef struct config config_t;
typedef struct checkpoint checkpoint_t;
typedef struct display display_t;

// Slot-table kinds.
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
// (nubus_card_fits_socket).
typedef struct nubus_slot_decl {
    int slot; // $9..$E (0 ends the array)
    nubus_slot_kind_t kind;
    const char *builtin_card_id; // BUILTIN: card-id resolved via nubus_card_find()
    const char *default_card; // SOCKET: factory-default population when the
                              // boot document names no card (NULL = ships empty)
    // BUILTIN video that scans out of main RAM (the IIci's and IIsi's RBV):
    // its frame buffer is the bottom of RAM, which the card takes when it is
    // built, before its checkpoint part is read.
    bool fb_in_ram;
    // SOCKET: Apple's name for it on this machine ("NuBus slot 4" on a IIci,
    // "NuBus slot C" on a Quadra), what catalog.profile shows; the slot ID
    // becomes the detail.
    const char *label;
    // SOCKET: Apple's recommended order for the slot an added card goes in
    // (lowest first); 0 = after every slot that has one, in table order.
    int fill_order;
    // SOCKET: another slot ($9..$E) that cannot be used while this one is
    // (a shared opening or connector); 0 = none.
    int excludes;
} nubus_slot_decl_t;

// True iff card kind `kind` may be seated in slot `s`.  Compatibility is
// COMPUTED — the machine declares topology, the card declares its physical
// attachment (card_attach_t); nobody enumerates (machine, card) pairs.
// Today's rule is the bus standard's: any NuBus-attach card fits any
// user-configurable slot.  Shared by the catalog.profile encoder and
// the boot document's slot validation so the two can never diverge.
bool nubus_card_fits_socket(const nubus_slot_decl_t *s, const nubus_card_kind_t *kind);

// True iff `kind` may be seated in slot `s`, socket or built in: a socket
// takes any card that fits it, a built-in slot only its declared card.  The
// one rule catalog.profile lists choices by and the slot entries are checked
// against.
bool nubus_card_fits_slot(const nubus_slot_decl_t *s, const nubus_card_kind_t *kind);

// Check one slot entry against the machine's slot table: the slot takes a
// card, the card exists and fits, its video mode / custom geometry / ROM /
// monitor are its own, it takes no options -- and with `roms_final`, that a
// card the entry names finds a declaration ROM, Apple's or its substitute.
// machine.boot checks the document's entries with it, a restore the entries
// its checkpoint carries.  On false, `why` says what is wrong (`model` names
// the machine in it).
bool nubus_slot_entry_check(const nubus_slot_decl_t *slots, const char *model, const slot_opts_t *e, bool roms_final,
                            char *why, size_t why_len);

// Does a card of kind `k` built from entry `e` run its substitute ROM?  When
// the entry asks for it, its custom geometry needs it, or Apple's ROM is not
// offered -- and never for a kind without one.  The bus decides it once, as
// it seats the card; afterwards the entry's `substitute` says.
bool nubus_entry_substitute(const nubus_card_kind_t *k, const slot_opts_t *e);

// The monitors kind `k` drives with the ROM it runs: the substitute ROM's
// when that carries fewer than the card.
const nubus_monitor_t *nubus_kind_monitors(const nubus_card_kind_t *k, bool substitute);

// The monitor row entry `e` names (its `monitor`), or NULL with none plugged
// in or for a card without video.
const nubus_monitor_t *nubus_entry_monitor(const nubus_card_kind_t *k, const slot_opts_t *e);

// Give entry `e` a monitor when it names none yet: its video mode's row,
// else the first row its card drives, with that row's sense code.  A card
// without video keeps none.
void nubus_entry_default_monitor(const nubus_card_kind_t *k, slot_opts_t *e);

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

// The board bus-error window, as every NuBus Macintosh draws it: an access
// with no responder anywhere in expansion space ends in the watchdog's bus
// error rather than floating to $FF.
//
// LO IS STANDARD SLOT SPACE, and an attempt to extend it to super-slot space
// was REFUTED BY TEST -- worth recording, because the argument for extending
// it was good and still lost.
//
// It is tempting to read the three different windows as "three separate
// policies for the same architectural question", and the tree looks
// like it has already decided: the AV pair start at $A0, covering super-slot
// space, and PDM's BART explicitly claims super-slot space for empty slots so
// they fault (bart.c:318) -- with bart.c:300 recording "there read $FF
// instead of faulting" as a bug it had to fix.  Two families modelling it,
// one of them with a fixed bug behind it, reads like the other nine being a
// gap.
//
// They are not.  Lowering the nine boards to $90000000 breaks suite-se30,
// suite-iicx and suite-iici outright -- iicx-gc-beep and iici-701-fd stop
// matching -- so something on those machines legitimately reads super-slot
// space and expects the bus to float.  Taking PDM's fact and applying it to
// nine other boards is the classic mistake of carrying one machine's evidence
// to another, in the direction that is harder to see: the evidence was real,
// it was just evidence about a different machine.  The per-board question
// belongs to a float-or-fault audit that reads each board's own address map.
//
// These are the same arithmetic as nubus_slot_base above, spelled as constant
// expressions because a board descriptor is a static initialiser and a
// `static inline` call is not constant.  The _Static_asserts in nubus.c keep
// the two spellings honest.
#define NUBUS_BERR_LO 0xF9000000u // == nubus_slot_base(0x9)

// Standard slot space through slot $E inclusive.
#define NUBUS_BERR_HI 0xFEFFFFFFu // == nubus_slot_base(0xE) + 0xFFFFFF

// ...except where slot $E is not expansion space.  The SE/30's $E is its PDS,
// and the machine has always excluded it.
#define NUBUS_BERR_HI_EXCL_SLOT_E 0xFDFFFFFFu // == nubus_slot_base(0xD) + 0xFFFFFF

// Bus controller.  Walks the slot table at init: each slot seats the card
// the boot document's entry for it names (cfg->build_opts, validated by
// machine_slots_resolve), else its declared builtin / default card, and the
// card's init gets the slot's entry.  Returns NULL on failure.
nubus_bus_t *nubus_init(config_t *cfg, const nubus_slot_decl_t *slots, checkpoint_t *cp);

void nubus_delete(nubus_bus_t *bus);

// Parse/validate a "WxHxD" custom-mode spec (shared by boot validation
// and card_init).  Returns false with *err set (static string) on a
// malformed or out-of-range spec.
bool nubus_custom_mode_parse(const char *spec, uint32_t *out_w, uint32_t *out_h, uint32_t *out_d, const char **err);

// True iff `id` names a video mode in any registered card's catalog.
// Boot-document validation for machine.boot's video_mode= argument.
bool nubus_video_mode_known(const char *id);

// The slot declaration for slot `slot` on the running bus, or NULL when the
// bus doesn't declare it.
const nubus_slot_decl_t *nubus_slot_decl_get(nubus_bus_t *bus, int slot);

// Per-slot IRQ assertion.  The bus aggregates and drives VIA2 PA[0..5]
// (active-low) plus pulses CA1 on the umbrella transition.
void nubus_assert_irq(nubus_card_t *card);
void nubus_deassert_irq(nubus_card_t *card);

// Look up the active card in a slot, or NULL if unpopulated.
nubus_card_t *nubus_card(nubus_bus_t *bus, int slot);

// The KIND that seated `slot`, or NULL.  How the object layer reaches a
// card's attach_objects hook without knowing which cards exist.
const nubus_card_kind_t *nubus_slot_kind(nubus_bus_t *bus, int slot);

// What `slot` was built from (its seat: card, ROM, monitor, sense), or NULL
// for a slot out of range.
const slot_opts_t *nubus_seat(nubus_bus_t *bus, int slot);

// The seeding step's record for `slot`: the 8-byte slot PRAM record
// (at $46 + (slot - 9) * 8) that starts its card in the video mode its seat
// chose.  False when the slot seats no card with a startup mode, chose none,
// or has no monitor plugged in.
bool nubus_startup_record(nubus_bus_t *bus, int slot, uint8_t rec[8]);

// The display of the card the configuration connected the monitor to (its
// slot entry's `connected`), or NULL when no card has it -- and the card,
// which the object model wires `machine.screen.source` to.
display_t *nubus_connected_display(nubus_bus_t *bus);
nubus_card_t *nubus_connected_display_card(nubus_bus_t *bus);

// === Object-model surface ===================================================
//
// Build / tear down the per-slot `slot[N].card.{framebuffer,declrom,clut,mode}`
// object trees for every populated slot.  nubus_init calls _build after the
// cards exist; nubus_delete calls _teardown before freeing them.  The node
// objects are owned here (object_delete_tree on teardown), not by the bus.
// `machine.nubus` and its slot collection are installed from nubus_class.c
// with every NuBus machine (root_register_install).
void nubus_objects_build(nubus_bus_t *bus);
void nubus_objects_teardown(void);
// Every registered card-driver id (catalog.nubus_cards).
value_t nubus_cards_list(void);
// Teardown only if the trees describe `bus` (checkpoint-restore ordering:
// the new machine's tree is built before the old machine is destroyed).
void nubus_objects_teardown_owned(nubus_bus_t *bus);

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

// Power-cycle fan-out (system_machine_power_cycle, ahead of the /RESET):
// each populated card's ops->power_on, which drops what power loses (VRAM).
void nubus_power_on(nubus_bus_t *bus);

#endif // NUBUS_H
