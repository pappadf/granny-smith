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

// Borrowed accessor.  Returns the underlying buffer and writes its size
// to *out_size.  Lifetime is the builder's; ownership stays with the
// builder.
const uint8_t *declrom_builder_bytes(const declrom_builder_t *b, size_t *out_size);

// Fill in the standard board sResource (catBoard + name + BoardId).  v1
// stub — full body lands in step 4 once the SE/30 hand-rolled VROM moves
// onto these helpers.
void declrom_set_board(declrom_builder_t *b, const char *name, uint8_t board_id);

// Fill the trailing Format Header (byte-lane mask, length, CRC).  Writes
// the test pattern `$5A932BC7`, the supplied byte-lanes value, the CRC
// over the populated region, and resets `directory_offset` /
// `length` per the NuBus spec.  v1 stub — body lands in step 4.
void declrom_finalise(declrom_builder_t *b, uint8_t byte_lanes);

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
