// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vrom.h
// VROM handling for machines with discrete VROM (currently SE/30 only).
// The user picks the path via vrom.load(path); the actual blit into video
// memory happens during machine init (machines/se30.c::se30_load_vrom),
// which calls vrom_pending_path() to retrieve the path.

#ifndef VROM_H
#define VROM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct class_desc;
struct object;

// VROM is a fixed 32 KB image for the SE/30. A file passes the probe if
// it has the right size; we do not validate content beyond that.
#define VROM_EXPECTED_SIZE (32 * 1024)

// True if a file at `path` is the right size to be a VROM image. *out_size
// (if non-NULL) gets the file size regardless of validity.
bool vrom_probe_file(const char *path, size_t *out_size);

// === Content-based identification (the declaration-ROM catalog) ============
//
// Identity is the NuBus Format-Block CRC of the chip image — the same key
// vrom.identify exposes to the UI.  Filenames are only ever hints; these
// helpers let the card factories load whatever file actually provides their
// card, wherever the user put it (see declrom_load_vrom_card).

// One identified declaration ROM.
typedef struct {
    uint32_t crc; // Format-Block CRC (big-endian, as stored)
    size_t chip_size; // dense chip image size (32 KB or 64 KB today)
    const char *canonical_name; // catalog filename (static storage)
    const char *card_id; // nubus card-kind id the blob provides (static)
} vrom_id_t;

// Identify the file at `path` by content.  True iff it is a *recognised*
// declaration ROM (right size, $5A932BC7 TestPattern, catalog CRC); fills
// *out.  False for anything else (missing, wrong size, unknown CRC).
bool vrom_identify_card(const char *path, vrom_id_t *out);

// Enumerate the catalog's canonical filenames for a card id: returns the
// idx'th matching row's canonical_name (and its chip size via *out_chip_size,
// optional), or NULL when exhausted.  idx counts matches, not catalog rows.
const char *vrom_catalog_name(const char *card_id, int idx, size_t *out_chip_size);

// Set the pending VROM path. Takes effect at the next machine init.
// Returns 0 on success, -1 on OOM.
int vrom_set_path(const char *path);

// Path most recently set by vrom_set_path(), or NULL if none.
// Consumed by se30_load_vrom() during SE/30 machine init.
const char *vrom_pending_path(void);

// Clear the pending path (called from system_destroy).
void vrom_pending_clear(void);

// === Lifecycle =============================================================
//
// vrom_init() creates the singleton `vrom` object node and attaches it under
// the root; vrom_delete() detaches/frees it and clears the pending path.
// Both are idempotent. Called from system_create / system_destroy.
extern const struct class_desc vrom_class;

void vrom_init(void);
void vrom_delete(void);

#endif // VROM_H
