// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// declrom.h
// NuBus declaration-ROM helpers — used both to *build* a synthesised ROM
// (the SE/30 built-in card today; pseudo_video later) and to *parse* a
// real ROM image (the JMFB / Display Card 8•24 in step 6).
//
// Step-3 status: the type and the public API exist as a skeleton; the
// builder actually emits a working Format Header and CRC, but the higher-
// level "add a video sResource" / "add a board sResource" helpers are
// stubs because the SE/30 built-in card hasn't moved over yet.  Step 4
// fleshes those out as the SE/30's hand-rolled VROM in se30.c migrates
// onto the helpers.
//
// Byte-lane mask — v1 limit (per proposal §3.2.4): every shipped card
// uses byte-lane mask `$0F` (all four lanes valid; bytes are contiguous
// when read across longwords).  Sparser masks ($01, $05, …) are a future
// follow-up; the helpers assert mask = $0F today.

#ifndef NUBUS_DECLROM_H
#define NUBUS_DECLROM_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nubus_card;
typedef struct nubus_card nubus_card_t;

struct declrom_builder;
typedef struct declrom_builder declrom_builder_t;

// Allocate a new builder backed by a `size`-byte buffer.  Typical size:
// 32 KB for a real card declaration ROM, 8 KB for a minimal synth.
declrom_builder_t *declrom_builder_new(size_t size);

// Free the builder and its backing buffer.
void declrom_builder_free(declrom_builder_t *b);

// Borrowed accessor.  Returns the underlying buffer and writes the
// number of image bytes appended so far to *out_size — after a
// successful declrom_finalise, that is the finished image (Format Block
// in the last 20 bytes).  Lifetime is the builder's; ownership stays
// with the builder.
const uint8_t *declrom_builder_bytes(const declrom_builder_t *b, size_t *out_size);

// --- Structured builder inputs (proposal-nubus-runtime-vrom §3.3) ----------
//
// The builder collects declarative inputs (board identity, functional
// video sResources, spliced 68K code fragments) and serialises the whole
// image in declrom_finalise: leaf records first, then the lists that
// reference them, then the directory, then the trailing Format Block —
// every stored offset is a backward self-relative reference, so no
// patching pass exists to get wrong.  Record order is deterministic
// (directory ascending by spID) so checkpoint regeneration and pixel
// goldens are stable.

// One video mode (pixel depth) of a functional video sResource: the
// VPBlock fields that vary per depth plus its mode-list data entries
// (nubus_vrom.md §6.1/§6.2).
typedef struct declrom_vidmode {
    uint32_t base_offset; // vpBaseOffset: page-0 offset from the FB base
    uint16_t row_bytes; // vpRowBytes
    uint16_t width; // vpBounds right
    uint16_t height; // vpBounds bottom
    uint16_t pixel_type; // 0 = chunky indexed, $10 = chunky direct
    uint16_t pixel_size; // bits per pixel
    uint16_t cmp_count; // 1 indexed / 3 direct
    uint16_t cmp_size; // bits per component
    uint16_t dev_type; // mDevType: 0 CLUT / 1 fixed / 2 direct
    uint16_t page_count; // mPageCnt
} declrom_vidmode_t;

// One functional video sResource: identity + address window + mode set.
typedef struct declrom_vidsrsrc {
    const char *name; // sRsrcName cString ("Display_Video_...")
    uint16_t drhw; // sRsrcType DrHW value (card-design id)
    uint16_t flags; // sRsrcFlags word; 0 = omit the entry
    bool use_major; // true: Major (super-slot) base/length entries
    uint32_t base; // MinorBaseOS / MajorBaseOS long
    uint32_t length; // MinorLength / MajorLength long
    const declrom_vidmode_t *modes; // per-depth entries, mode-list order
    size_t mode_count;
} declrom_vidsrsrc_t;

// Which board-sResource entry a spliced sExecBlock is wired to.
typedef enum declrom_exec_kind {
    DECLROM_PRIMARY_INIT = 0, // board entry 34
    DECLROM_SECONDARY_INIT, // board entry 38
} declrom_exec_kind_t;

// Stage the board sResource identity (catBoard type record + name +
// BoardId).  BoardId is the 16-bit DTS-assigned product id — the one
// datum that can single-handedly invalidate the slot.
bool declrom_set_board(declrom_builder_t *b, const char *name, uint16_t board_id);

// Stage the VendorInfo sub-list strings (any may be NULL to omit).
void declrom_set_vendor(declrom_builder_t *b, const char *vendor_id, const char *rev_level, const char *part_num);

// Stage the PRAMInitData sBlock: default values for the slot's 6
// modifiable PRAM bytes (b1..b6; nubus_vrom.md §5.2).
void declrom_set_pram_init(declrom_builder_t *b, const uint8_t bytes[6]);

// Stage one functional video sResource under directory id `spid`
// (2..254 — the documented designer range starts at 128, but Apple's
// own 24AC uses $6B..$6D).  The descriptor and its mode array are
// copied.
bool declrom_add_video_srsrc(declrom_builder_t *b, uint8_t spid, const declrom_vidsrsrc_t *v);

// Splice a pre-assembled, self-contained sExecBlock (PrimaryInit /
// SecondaryInit) verbatim; the builder only wires the board-sResource
// entry that points at it.  The fragment must start with its sExec
// header (long size incl. self, revision $02, CPU id).
bool declrom_add_exec(declrom_builder_t *b, declrom_exec_kind_t kind, const uint8_t *frag, size_t size);

// Splice the DRVR sBlock (long size prefix + DRVR header + code)
// verbatim; every video sResource's sRsrcDrvrDir points at it under
// the sMacOS68020 id.
bool declrom_add_drvr(declrom_builder_t *b, const uint8_t *frag, size_t size);

// Serialise the staged inputs and stamp the trailing Format Block:
// DirectoryOffset, Length (covering the whole image), RevisionLevel,
// Format, TestPattern `$5A932BC7`, ByteLanes, and the rotate-left-add
// CRC computed over the finished image (CRC field read as zero) —
// crc.py's job, moved into the emulator.  Only byte_lanes $0F (dense
// 4-lane) is supported.  Runs declrom_image_validate on the result and
// fails (loudly) if the structural invariants don't hold.  After
// success, declrom_builder_bytes returns the finished image.
bool declrom_finalise(declrom_builder_t *b, uint8_t byte_lanes);

// Structural validation of a finished declaration-ROM image (§5 of the
// runtime-vrom proposal): Format Block sanity, directory walk
// terminates with ascending IDs, NO offset-form entry carries a zero
// offset (the fence against gas's silent `|`-fold hazard), every
// offset lands inside the image, and exec blocks carry the sExec
// revision $02 prologue.  `img` is a dense chip image with the Format
// Block in its last 20 bytes.
bool declrom_image_validate(const uint8_t *img, size_t size);

// Copy the builder's buffer into the card's declrom slot.  Increments
// `card->declrom_size` and stashes the bytes; ownership of the bytes
// transfers to the card.
void declrom_install(nubus_card_t *card, const declrom_builder_t *b);

// Load a real declaration-ROM binary from disk into a freshly-allocated
// buffer of size `expected_size`.  Returns the buffer (caller owns) on
// success, NULL on miss.  Used by cards that prefer a real ROM file
// (e.g. the JMFB card with `mdc-8-24-revb-d1629664.vrom`) and fall back to the
// builder on miss.  v1 stub — body lands in step 6.
uint8_t *declrom_load(const char *path, size_t expected_size);

// Expand a chip image whose byteLanes value is `$78` (lane 3 only — the
// layout every Apple 32 KB display-card declaration ROM uses) into
// a 4×-larger bus-space buffer.  Each chip byte at offset i lands at bus
// offset i*4 + 3 (lane 3 of longword i); lanes 0..2 stay zero.  This is
// what the Slot Manager expects when it reads the format block at the high
// end of slot space.  `bus_buf` must hold at least chip_size*4 bytes.
void declrom_expand_lane3(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf);

// Lay a freshly-read chip image into a bus-space buffer according to its
// byteLanes byte (the chip's last byte): `$78` → lane-3 sparse expand
// (chip occupies bus_size = chip_size*4); `$0F` → flat copy into the top
// chip_size bytes (all four lanes carry data, e.g. a synthesised ROM).
// Returns true on a recognised layout, false otherwise (caller logs).
bool declrom_layout_chip(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf, size_t bus_size, uint8_t byte_lanes);

// Find the declaration-ROM chip image that provides the card `card_id` —
// by CONTENT (the vrom.c Format-Block-CRC catalog), never by a filename —
// read it, and lay it out into the TAIL of `bus_buf` (bus_size bytes) per
// its byteLanes byte (see declrom_layout_chip; the Format Block always ends
// at the slot top, so a smaller ROM revision occupies the top of a window
// sized for the largest one).  Candidates come exclusively from the offer
// registry the platform populated before boot (vrom_offer / vrom.load — see
// vrom.h): they are tried in pick order (explicit vrom.load first, then the
// catalog's preferred revision, then catalog order).  Core never fabricates
// a search path.
// On success returns true and stores a freshly-strdup'd copy of the path it
// loaded from in *out_path (caller frees); on miss returns false and leaves
// *out_path NULL.  Shared by every card with a real ROM file (JMFB, 24AC,
// 8•24 GC, SE/30 built-in video).
bool declrom_load_vrom_card(const char *card_id, uint8_t *bus_buf, size_t bus_size, char **out_path);

// Install a BUILT-IN declaration ROM image (a gsvrom.h blob) into the tail
// of the card's bus window, exactly as declrom_load_vrom_card lays out a
// file-backed chip — byteLanes read from the chip's last byte — and report
// the pick into the built-from record with the "builtin:<card_id>" locator
// (path-less, identified by CRC; proposal-generic-nubus-vrom.md sec. 6.2).
// Returns true on success.
bool declrom_install_builtin(const char *card_id, const uint8_t *chip, size_t chip_size, uint8_t *bus_buf,
                             size_t bus_size);

#endif // NUBUS_DECLROM_H
