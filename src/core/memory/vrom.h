// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vrom.h
// VROM (NuBus declaration-ROM) handling: content identification plus the
// pre-boot offer registry.  A path is a HANDLE, not a fact: core may open a
// path it was handed and identify the bytes, but it never fabricates a path
// or interprets a filename.  The platform — which owns the filesystem —
// enumerates candidate files and *offers* them via vrom_offer(); the card
// factories then match, by content, among the offered candidates
// (declrom_load_vrom_card).  See proposal-content-addressed-rom-provisioning.md.

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
// vrom.identify exposes to the UI.  Filenames are never inspected; these
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

// Drop every registered offer (teardown).
void vrom_offer_clear(void);

// Enumerate the offered candidates that provide the card `card_id`, in pick
// order: the explicit vrom.load offer first, then catalog `preferred` rows,
// then remaining catalog order.  Returns the idx'th candidate's path
// (borrowed; valid until the registry changes) and its chip size via
// *out_chip_size (optional), or NULL when exhausted.
const char *vrom_offer_find(const char *card_id, int idx, size_t *out_chip_size);

// Content facts for a registered offer, looked up by its path (as returned
// from vrom_offer_find).  Used by the card loader to report the resolved
// pick into the built-from record.  Returns false if the path is not a
// registered offer.
bool vrom_offer_info(const char *path, uint32_t *out_crc, bool *out_explicit);

// True iff the catalog lists a declaration ROM for this card id — i.e. the
// card needs a vROM and boot's strict-resolution validation applies
// (proposal-named-args-boot-config §4.1).
bool vrom_card_catalogued(const char *card_id);

// True iff an offered candidate resolves for this card id (pick order as
// vrom_offer_find).  Boot validation rejects a configuration whose
// catalogued cards cannot all resolve.
bool vrom_card_resolvable(const char *card_id);

// Register the boot document's vrom= explicit pick: an offer that is also
// the *preferred* candidate for whichever card its content provides
// (proposal-named-args-boot-config §4.1).  Returns 0 on success, -1 on an
// empty path.
int vrom_set_path(const char *path);

// === Lifecycle =============================================================
//
// vrom_init() creates the singleton `vrom` object node and attaches it under
// the root; vrom_delete() detaches/frees it and clears the pending path and
// the offer registry.  Both are idempotent.
extern const struct class_desc vrom_class;

void vrom_init(void);
void vrom_delete(void);

#endif // VROM_H
