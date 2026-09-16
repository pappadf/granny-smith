// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// offer_registry.h
// The shape a ROM offer registry has, once.
//
// The platform hands the emulator whole directories of candidate ROM files and
// the machine later asks "is there an image for this card?".  Between those two
// moments sits a registry: fixed shape, content-keyed, duplicate-collapsing,
// with one explicit pick that wins the order and catalog order deciding the
// rest.  vrom.c (NuBus declaration ROMs) and prom.c (PCI Open Firmware
// expansion ROMs) each had their own copy of it -- 86 of 123 lines identical,
// and the improvements made to one never reached the other (04-video F-09).
//
// What genuinely differs between the two is IDENTIFICATION: the validation
// gates, size classes and identity spans of a declaration ROM and a PCI
// expansion ROM have almost nothing in common, and prom.c says so in its own
// header.  That half stays where it is, behind the `identify` callback -- which
// is also where each side's per-failure diagnostics belong, since only the
// identifier knows what kind of stray it just rejected.

#ifndef OFFER_REGISTRY_H
#define OFFER_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One registered candidate, keyed by its content identity.
typedef struct offer_entry {
    uint32_t crc;
    size_t size; // the image/chip size the identifier reported
    const char *card_id; // catalog row (static storage)
    char *path; // opaque locator (owned)
    bool explicit_pick; // the boot document's explicit pick
} offer_entry_t;

// What the registry needs to know about its catalog, without knowing the row
// type: how many rows, and how to read the three fields the pick order uses.
typedef struct offer_catalog {
    size_t count;
    void (*row)(size_t r, const char **card_id, uint32_t *crc, bool *preferred);
} offer_catalog_t;

// A registry instance.  `entries`/`count`/`cap` are owned here; the rest is
// the caller's static configuration.
typedef struct offer_registry {
    offer_entry_t *entries;
    size_t count, cap;
    const char *tag; // log prefix, e.g. "vrom_offer"
    // Identify a candidate file.  Returns true to register it, filling crc,
    // size and card_id (which must be static storage).  On false the callback
    // has ALREADY logged why -- only it knows which kind of stray this is.
    bool (*identify)(const char *path, uint32_t *out_crc, size_t *out_size, const char **out_card_id);
    offer_catalog_t catalog;
} offer_registry_t;

// Register one candidate.  Idempotent by content: one entry per CRC, and a
// re-offer refreshes the path (the newest locator for these bytes) and may
// promote the entry to the explicit pick.  At most one entry is explicit.
void offer_registry_add(offer_registry_t *r, const char *path, bool explicit_pick);

// Drop every entry and free the registry's storage.
void offer_registry_clear(offer_registry_t *r);

// The idx'th candidate path for `card_id`, or NULL past the end.  Pick order:
// the explicit pick first, then catalog rows with the `preferred` bit, then the
// remaining rows in catalog order.  All content-based -- no filename ever
// enters the comparison.  `out_size` receives the identified size.
const char *offer_registry_find(const offer_registry_t *r, const char *card_id, int idx, size_t *out_size);

// Facts about an already-registered path.
bool offer_registry_info(const offer_registry_t *r, const char *path, uint32_t *out_crc, bool *out_explicit);

// Does the catalog have a row for this card at all (independent of whether any
// file has been offered for it)?
bool offer_registry_catalogued(const offer_registry_t *r, const char *card_id);

// Is there at least one offered file for this card?
bool offer_registry_resolvable(const offer_registry_t *r, const char *card_id);

#endif // OFFER_REGISTRY_H
