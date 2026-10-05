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

// Longest identity text plus NUL ("vvvv-dddd-cccc").
#define PROM_ID_MAX 16

// One identified expansion ROM.  Its identity is made only of fields the
// standards define, nothing hashed by us: the PCI Data Structure's vendor
// and device ids say which card the ROM is for, and the IEEE 1275 FCode
// header's own checksum says which programming of that card's ROM it is.
// The checksum also verifies the program (`intact`).
typedef struct {
    uint64_t key; // vendor << 32 | device << 16 | FCode checksum
    char id[PROM_ID_MAX]; // the same as text, lowercase hex: "1002-4758-c6e8"
    bool intact; // the FCode checksum verifies over the program
    size_t image_size; // bytes on the chip
    uint16_t vendor_id; // from the PCI Data Structure
    uint16_t device_id;
    uint16_t fcode_checksum; // from the FCode header (stored, not computed)
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
    PROM_ID_UNKNOWN, // structurally valid; its identity is not in the catalog
    PROM_ID_DAMAGED, // a catalogued identity whose FCode checksum does not verify
    PROM_ID_KNOWN, // recognised and intact: *out filled from the catalog row
} prom_id_result_t;

// Identify the file at `path` by content.  True iff it is a *recognised*
// expansion ROM (structurally valid, a catalogued identity, intact); fills *out.
bool prom_identify_card(const char *path, prom_id_t *out);

// The same, with the reason for a rejection — what catalog.proms.identify reports.
prom_id_result_t prom_identify_detail(const char *path, prom_id_t *out, size_t *out_size);

// === The offer registry =====================================================
//
// The platform hands core candidate .prom files before machine.boot.  Core
// opens each, identifies it by content, and remembers the recognised ones
// keyed by content identity (prom_id_t.key).  Unrecognised offers are dropped with
// a log, not errors — the platform offers whole directories and strays are
// expected.  Offers persist across machine.boot.

void prom_offer(const char *path);
// Offer every file in `dir` ending in `ext` (NULL: any), skipping dotfiles.
void prom_offer_dir(const char *dir, const char *ext);
void prom_offer_clear(void);

// Enumerate the offered candidates providing `card_id`, in pick order:
// catalog `preferred` rows, then remaining catalog order.  Returns the idx'th
// path (borrowed) or NULL.
const char *prom_offer_find(const char *card_id, int idx, size_t *out_size);

// True iff card `card_id` has an expansion ROM: `rom` (the slot's own file,
// NULL for none) when given -- it must identify as this card's -- else an
// offered candidate.  No side effect.
bool prom_card_resolvable(const char *card_id, const char *rom);

// Load the image for `card_id` into a malloc'd buffer the caller (a card
// factory) hands to pci_device_t.rom / .rom_size: `rom` (the slot's own
// file) when given, else the offered candidates in pick order.  *out_path
// (if non-NULL) receives a malloc'd copy of the path it came from.  False
// when nothing loads.
bool prom_load_card(const char *card_id, const char *rom, uint8_t **out_buf, size_t *out_size, char **out_path);

// === Lifecycle =============================================================

// Create the catalog.proms registry node under `parent` (the catalog).
struct object;
void prom_init(struct object *parent);
void prom_delete(void);

#endif // PROM_H
