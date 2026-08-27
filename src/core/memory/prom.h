// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// prom.h
// PROM (PCI expansion-ROM / IEEE 1275 FCode image) handling: content
// identification plus the pre-boot offer registry.
//
// Same contract as vrom.h, and deliberately the same SHAPE: a path is a
// HANDLE, not a fact.  Core may open a path it was handed and identify the
// bytes, but it never fabricates a path and never interprets a filename.
// The PLATFORM — which owns the filesystem — enumerates candidate files and
// offers them via prom_offer(); the card factories then match, by content,
// among the offered candidates (prom_load_card).
//
// Two small parallel modules beat one abstract one: what a declaration ROM
// and a PCI expansion ROM have in common is the offer-registry shape, and
// almost nothing else.  A vROM is identified by a NuBus Format-Block CRC in
// its trailing bytes and comes in two fixed sizes; a PROM is identified by
// a PCI Data Structure near its head, spans five size classes, and has to
// tell an Open Firmware image apart from an x86 VGA BIOS.  The validation
// gates, the size classes and the identity spans genuinely differ, so they
// are written out twice rather than parameterised into one thing that
// serves neither well.

#ifndef PROM_H
#define PROM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct class_desc;
struct object;

// Expansion-ROM chip sizes we accept (PCI 2.x allows up to 16 MB; real
// Mac display-card chips are 32 KB or 64 KB).  The gate is "a power of two
// in this range", not a fixed list, because the image length the PCI Data
// Structure declares is independent of the chip it was burned onto.
#define PROM_MIN_SIZE (2u * 1024u)
#define PROM_MAX_SIZE (256u * 1024u)

// One identified expansion ROM.
typedef struct {
    uint32_t crc; // CRC-32 of the whole chip image (the identity)
    size_t image_size; // bytes on the chip
    uint16_t vendor_id; // from the PCI Data Structure
    uint16_t device_id;
    uint32_t class_code; // 24-bit class / subclass / prog-if
    uint32_t fcode_offset; // where the FCode program starts
    const char *card_id; // pci card-kind id the blob provides (static)
} prom_id_t;

// Why a candidate was rejected.  Kept as a result code rather than a bool
// because "this is an x86 VGA BIOS for a PC Mach64" is the predictable user
// error and deserves its own message, not a shrug.
typedef enum prom_id_result {
    PROM_ID_UNREADABLE, // stat/open/read failed
    PROM_ID_WRONG_SIZE, // exists, but not a plausible chip image
    PROM_ID_NOT_A_PROM, // no $55AA / no PCIR / no FCode start token
    PROM_ID_NOT_OPEN_FIRMWARE, // a real expansion ROM, but code type != 1
    PROM_ID_UNKNOWN, // structurally valid; CRC is not in the catalog
    PROM_ID_KNOWN, // recognised: *out filled from the catalog row
} prom_id_result_t;

// Identify the file at `path` by content.  True iff it is a *recognised*
// expansion ROM (structurally valid AND a catalog CRC); fills *out.
bool prom_identify_card(const char *path, prom_id_t *out);

// The same, with the reason for a rejection — what prom.identify reports.
prom_id_result_t prom_identify_detail(const char *path, prom_id_t *out, size_t *out_size, uint32_t *out_crc);

// === The offer registry =====================================================
//
// The platform hands core candidate .prom files before machine.boot.  Core
// opens each, identifies it by content, and remembers the recognised ones
// keyed by content identity (CRC-32).  Unrecognised offers are dropped with
// a log, not errors — the platform offers whole directories and strays are
// expected.  Offers persist across machine.boot.

void prom_offer(const char *path);
void prom_offer_clear(void);

// Enumerate the offered candidates providing `card_id`, in pick order: the
// explicit machine.boot `prom=` pick first, then catalog `preferred` rows,
// then remaining catalog order.  Returns the idx'th path (borrowed) or NULL.
const char *prom_offer_find(const char *card_id, int idx, size_t *out_size);

// Content facts for a registered offer, looked up by its path.
bool prom_offer_info(const char *path, uint32_t *out_crc, bool *out_explicit);

// True iff the catalog lists an expansion ROM for this card id — i.e. the
// card needs one and boot's strict-resolution validation applies.
bool prom_card_catalogued(const char *card_id);

// True iff an offered candidate resolves for this card id.
bool prom_card_resolvable(const char *card_id);

// Register the boot document's explicit `prom=` pick.  0 on success, -1 on
// an empty path.
int prom_set_path(const char *path);

// Load the resolved image for `card_id` into a malloc'd buffer the caller
// (a card factory) hands to pci_device_t.rom / .rom_size.  *out_path (if
// non-NULL) receives a malloc'd copy of the resolved path for the
// built-from record.  False when nothing resolves.
bool prom_load_card(const char *card_id, uint8_t **out_buf, size_t *out_size, char **out_path);

// === Lifecycle =============================================================

extern const struct class_desc prom_class;

void prom_init(void);
void prom_delete(void);

#endif // PROM_H
