// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// card_registry.h
// The pieces of a card-kind registry NuBus (nubus.c) and PCI (pci.c) share:
// find a kind by id, suggest the id a near-miss typo meant, format a
// slot-check refusal, and check and resolve the monitor a slot entry plugs
// into its card (slot_opts_t.monitor against the kind's monitor rows).
//
// Both buses keep their kinds in an explicit, NULL-terminated array of
// pointers to a kind struct whose FIRST member is `const char *id`
// (nubus_card_kind_t, pci_card_kind_t), so one body serves both: a pointer
// to a struct converts to a pointer to its first member.  Each bus wraps
// these in its typed nubus_card_* / pci_card_* entry points.

#ifndef CARD_REGISTRY_H
#define CARD_REGISTRY_H

// === Includes ===

#include "machine_build_opts.h"
#include "value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nubus_monitor; // nubus/card.h: one monitor row of a card kind

// === Type Definitions ===

// A bus's slot-entry check (nubus_slot_entry_check, pci_slot_entry_check)
// over its own slot-declaration table, passed as `slots`.
typedef bool (*card_entry_check_fn)(const void *slots, const char *model, const slot_opts_t *e, bool roms_final,
                                    char *why, size_t why_len);

// === Operations ===

// The kind in `registry` whose id is `id`, or NULL (also for a NULL or empty
// id).  `registry` is a NULL-terminated array of kind pointers, each kind
// starting with its `const char *id`.
const void *card_registry_find(const void *const *registry, const char *id);

// For an unknown-id error: the registered id that differs from `id` only by
// underscores ("8_24gc" for "824gc"), or NULL.
const char *card_registry_suggest(const void *const *registry, const char *id);

// Every registered id, as VK_LIST<VK_STRING> (catalog.nubus_cards,
// catalog.pci_cards); `what` names the attribute in an out-of-memory error.
value_t card_registry_list(const void *const *registry, const char *what);

// Validates a restored slot table of `n_slots` entries: each is sanitized
// (slot_opts_sanitize) and, when it seats a card, run through `check` as a
// boot would, bar the ROM file (a card's ROM comes from its own checkpoint
// block, not a path).  Returns -1 when every entry holds, else the first bad
// slot with the reason in `why` ("" for a malformed entry).
int card_slot_table_check(slot_opts_t *table, int n_slots, card_entry_check_fn check, const void *slots,
                          const char *model, char *why, size_t why_len);

// The monitor rows a slot entry may name: a card kind's `monitors` (a
// sentinel-terminated nubus_monitor_t array, NULL for a card without video).

// Does `monitor` (slot_opts_t.monitor) name a row of `rows`?  "" (the card's
// default) and "none" (nothing plugged in) always do.  The one check of an
// entry's monitor, for both buses.
bool card_monitor_check(const struct nubus_monitor *rows, const char *monitor);

// The row an entry's `monitor` names: its row, the first row for "" (the
// card's default), NULL for "none" or a card without rows.
const struct nubus_monitor *card_monitor_row(const struct nubus_monitor *rows, const char *monitor);

// Plug `row` into the entry: its monitor and sense become the row's, or
// "none" and MACHINE_SENSE_NONE for NULL.
void card_entry_set_monitor(slot_opts_t *e, const struct nubus_monitor *row);

// The 6-bit extended sense code `row` answers the extended probe with: its
// ext_sense, else what its strap alone reads (an undriven line reads its own
// strapped level).  NULL -- no cable -- reads $3F, every line high.
uint8_t card_monitor_ext_sense(const struct nubus_monitor *row);

// Formats a slot-check refusal into `why` and returns false.
__attribute__((format(printf, 3, 4))) bool card_refuse(char *why, size_t len, const char *fmt, ...);

#endif // CARD_REGISTRY_H
