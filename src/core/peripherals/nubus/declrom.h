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
// (e.g. the JMFB card with `Apple-341-0868.vrom`) and fall back to the
// builder on miss.  v1 stub — body lands in step 6.
uint8_t *declrom_load(const char *path, size_t expected_size);

#endif // NUBUS_DECLROM_H
