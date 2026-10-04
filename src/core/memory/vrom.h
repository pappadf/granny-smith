// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vrom.h
// VROM (NuBus declaration-ROM) handling: content identification plus the
// pre-boot offer registry.  A path is a HANDLE, not a fact: core may open a
// path it was handed and identify the bytes, but it never fabricates a path
// or interprets a filename.  The platform — which owns the filesystem —
// enumerates candidate files and *offers* them via vrom_offer(); the card
// factories then match, by content, among the offered candidates
// (declrom_load_vrom_card).

#ifndef VROM_H
#define VROM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct class_desc;
struct object;

// The common declaration-ROM chip size (32 KB); some revisions are 64 KB
// (2 * VROM_EXPECTED_SIZE) — vrom_identify_card accepts both.
#define VROM_EXPECTED_SIZE (32 * 1024)

// True if a file at `path` is the common 32 KB chip size. *out_size
// (if non-NULL) gets the file size regardless of validity.
bool vrom_probe_file(const char *path, size_t *out_size);

// === Content-based identification (the declaration-ROM catalog) ============
//
// Identity is the NuBus Format-Block CRC of the chip image — the same key
// catalog.vroms.identify exposes to the UI.  Filenames are never inspected; these
// helpers let the card factories load whatever file actually provides their
// card, wherever the user put it (see declrom_load_vrom_card).

// One identified declaration ROM.
typedef struct {
    uint32_t crc; // Format-Block CRC (big-endian, as stored)
    size_t chip_size; // dense chip image size (32 KB or 64 KB today)
    const char *card_id; // nubus card-kind id the blob provides (static)
} vrom_id_t;

// Identify the file at `path` by content.  True iff it is a *recognised*
// declaration ROM (right size, $5A932BC7 TestPattern, catalog CRC); fills
// *out.  False for anything else (missing, wrong size, unknown CRC).
bool vrom_identify_card(const char *path, vrom_id_t *out);

// === The offer registry =====================================================
//
// The platform hands core candidate vROM files before machine.boot.  Core
// opens each, identifies it by content, and remembers recognised ones keyed
// by their content identity (Format-Block CRC).  It NEVER enumerates a
// directory or builds a path.  `path` is opaque: used to open the file and
// stored for round-trip reporting.  Unrecognised offers are dropped (with a
// debug log), not errors.  Offers persist across machine.boot; the registry
// is process-lifetime state torn down by vrom_offer_clear()/vrom_delete().

// Add one candidate (idempotent by content).
void vrom_offer(const char *path);
// Offer every file in `dir` ending in `ext` (NULL: any), skipping dotfiles.
void vrom_offer_dir(const char *dir, const char *ext);

// Drop every registered offer (teardown).
void vrom_offer_clear(void);

// Enumerate the offered candidates that provide the card `card_id`, in pick
// order: catalog `preferred` rows, then remaining catalog order.  Returns the
// idx'th candidate's path (borrowed; valid until the registry changes), its
// chip size via *out_chip_size and its Format-Block CRC via *out_crc (both
// optional), or NULL when exhausted.
const char *vrom_offer_find(const char *card_id, int idx, size_t *out_chip_size, uint32_t *out_crc);

// True iff the catalog lists a declaration ROM for this card id — i.e. the
// card needs a vROM and boot's strict-resolution validation applies.
bool vrom_card_catalogued(const char *card_id);

// True iff card `card_id` has a declaration ROM: `rom` (the slot's own file,
// NULL for none) when given -- it must identify as this card's -- else an
// offered candidate.  Boot validation rejects a configuration whose named
// catalogued cards cannot all resolve.  No side effect.
bool vrom_card_resolvable(const char *card_id, const char *rom);

// Create the catalog.vroms registry node under `parent` (the catalog).
struct object;
void vrom_init(struct object *parent);
void vrom_delete(void);

#endif // VROM_H
