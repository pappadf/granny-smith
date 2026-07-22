// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// declrom.c
// Declaration-ROM builder + loader skeleton.  Step-3 status: builder
// allocates / frees, exposes its bytes, and supports declrom_install
// onto a card.  The structured "add a board sResource / video sResource"
// helpers and the Format-Header finaliser are stubs — they land in
// step 4 alongside the SE/30 built-in card migration.  declrom_load is a
// minimal "open + slurp" today; the full byte-lane-aware reader lands in
// step 6 when the JMFB card needs it.

#include "declrom.h"
#include "card.h"
#include "log.h"
#include "machine_config.h" // resolved-pick reporting into the built-from record
#include "vrom.h" // content identification + the platform-fed offer registry

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("nubus");

struct declrom_builder {
    uint8_t *buf;
    size_t size;
};

declrom_builder_t *declrom_builder_new(size_t size) {
    if (size == 0)
        return NULL;
    declrom_builder_t *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->buf = calloc(1, size);
    if (!b->buf) {
        free(b);
        return NULL;
    }
    b->size = size;
    return b;
}

void declrom_builder_free(declrom_builder_t *b) {
    if (!b)
        return;
    free(b->buf);
    free(b);
}

const uint8_t *declrom_builder_bytes(const declrom_builder_t *b, size_t *out_size) {
    if (!b)
        return NULL;
    if (out_size)
        *out_size = b->size;
    return b->buf;
}

void declrom_set_board(declrom_builder_t *b, const char *name, uint8_t board_id) {
    // Step-3 stub: full body lands when the SE/30 hand-rolled VROM in
    // se30.c migrates onto these helpers in step 4.
    (void)b;
    (void)name;
    (void)board_id;
}

void declrom_finalise(declrom_builder_t *b, uint8_t byte_lanes) {
    // Step-3 stub: emits the Format Header test pattern, byte-lane mask,
    // length, and CRC over the populated region.  Body lands in step 4.
    (void)b;
    (void)byte_lanes;
}

void declrom_install(nubus_card_t *card, const declrom_builder_t *b) {
    if (!card || !b || !b->buf || b->size == 0)
        return;
    free(card->declrom);
    card->declrom = malloc(b->size);
    if (!card->declrom) {
        card->declrom_size = 0;
        return;
    }
    memcpy(card->declrom, b->buf, b->size);
    card->declrom_size = b->size;
}

uint8_t *declrom_load(const char *path, size_t expected_size) {
    if (!path || expected_size == 0)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG(1, "declrom_load: cannot open '%s'", path);
        return NULL;
    }
    uint8_t *buf = malloc(expected_size);
    if (!buf) {
        fclose(f);
        LOG(1, "declrom_load: out of memory allocating %zu bytes for '%s'", expected_size, path);
        return NULL;
    }
    size_t n = fread(buf, 1, expected_size, f);
    // Detect whether the file has more bytes beyond the expected size so we
    // can warn about silent truncation.
    int extra_present = (fgetc(f) != EOF);
    fclose(f);
    if (n != expected_size) {
        LOG(1, "declrom_load: '%s' is %zu bytes, expected %zu — refusing", path, n, expected_size);
        free(buf);
        return NULL;
    }
    if (extra_present)
        LOG(1, "declrom_load: '%s' is larger than expected %zu bytes — trailing data ignored", path, expected_size);
    return buf;
}

// === Shared display-card VROM loader ========================================
//
// Factored out of jmfb.c so both the JMFB (8•24) and the Display Card 24AC
// drivers share the same byte-lane expansion and search-path logic — the
// only thing that differs between them is the file name and the chip/bus
// sizes (both happen to be 32 KB → 128 KB today, but the loader is sized
// from the arguments so a different card could use other sizes).

// Sparse-expand a single-lane chip into a 4×-larger bus-space buffer: each
// chip byte at offset i lands at byte lane `lane` of longword i (bus offset
// i*4 + lane); the other three lanes stay zero.  Apple display-card
// declaration ROMs are 8-bit chips wired to one NuBus byte lane.
void declrom_expand_lane(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf, unsigned lane) {
    for (size_t i = 0; i < chip_size; i++)
        bus_buf[i * 4 + (lane & 3u)] = chip[i];
}

// Back-compat lane-3 wrapper (the JMFB / 24AC layout, byteLanes $78).
void declrom_expand_lane3(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf) {
    declrom_expand_lane(chip, chip_size, bus_buf, 3);
}

bool declrom_layout_chip(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf, size_t bus_size, uint8_t byte_lanes) {
    // Single active byte lane.  In the NuBus format-block convention the low
    // nibble of byteLanes is the lane bitmask and the high nibble its ones-
    // complement; a one-bit mask means the ROM lives entirely on lane N, so
    // sparse-expand it into lane N of the 4× bus buffer.  $78 = lane 3
    // (JMFB / 24AC), $E1 = lane 0 (Display Card 8•24 GC — a wider ROM wired
    // to the low byte lane).
    uint8_t lanes = (uint8_t)(byte_lanes & 0x0Fu);
    bool complement_ok = (uint8_t)((~byte_lanes) & 0x0Fu) == (uint8_t)(byte_lanes >> 4);
    bool single_lane = lanes != 0 && (lanes & (uint8_t)(lanes - 1)) == 0;
    if (complement_ok && single_lane) {
        if (bus_size < chip_size * 4)
            return false;
        unsigned lane = 0;
        while (!(lanes & (1u << lane)))
            lane++;
        declrom_expand_lane(chip, chip_size, bus_buf, lane);
        return true;
    }
    if (byte_lanes == 0x0Fu) {
        // 4-lane layout (e.g. a synthesised ROM) — flat copy into the last
        // chip_size bytes of the bus buffer; lanes 0..3 all carry data.
        if (bus_size < chip_size)
            return false;
        memcpy(bus_buf + bus_size - chip_size, chip, chip_size);
        return true;
    }
    return false;
}

// Read exactly chip_size bytes from `path` into `buf`.  Returns true only
// on an exact-size read (a short file is rejected).
static bool read_chip_exact(const char *path, uint8_t *buf, size_t chip_size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(buf, 1, chip_size, f);
    fclose(f);
    return n == chip_size;
}

// Load the identified chip image at `path` (chip_size bytes) into the TAIL of
// the card's bus-space window, so the Format Block always ends at the slot
// top regardless of chip size — a 32 KB chip in a card whose window was sized
// for a 64 KB one (the 8•24 GC v1.0 in the v1.1-sized window) occupies the
// top half; the leading bytes stay zero, below the ROM's declared length.
static bool load_chip_into_bus(const char *path, size_t chip_size, uint8_t *bus_buf, size_t bus_size) {
    if (chip_size == 0 || bus_size < chip_size)
        return false;
    uint8_t *chip = calloc(1, chip_size);
    if (!chip)
        return false;
    if (!read_chip_exact(path, chip, chip_size)) {
        free(chip);
        return false;
    }
    // The chip's last byte is the byteLanes value (for the single-lane and
    // 4-lane layouts both the spec and these files place it at the highest
    // active-lane address — the chip's final byte).
    uint8_t byte_lanes = chip[chip_size - 1];
    // A single-lane chip sparse-expands to 4× its size in bus space; a 4-lane
    // ($0F) chip is flat-copied and occupies exactly chip_size bytes (the
    // SE/30 onboard-video ROM, whose window equals the chip size).
    size_t footprint = (byte_lanes == 0x0Fu) ? chip_size : chip_size * 4;
    bool ok = bus_size >= footprint &&
              declrom_layout_chip(chip, chip_size, bus_buf + (bus_size - footprint), footprint, byte_lanes);
    if (!ok)
        LOG(0, "declrom_load_vrom_card: '%s' has unsupported byteLanes $%02x (or exceeds the bus window)", path,
            byte_lanes);
    free(chip);
    return ok;
}

bool declrom_install_builtin(const char *card_id, const uint8_t *chip, size_t chip_size, uint8_t *bus_buf,
                             size_t bus_size) {
    if (!card_id || !chip || chip_size < 20 || !bus_buf)
        return false;
    // Lay the blob out exactly like a file-backed chip: byteLanes from the
    // chip's last byte, tail-placed so the Format Block ends at the slot top.
    uint8_t byte_lanes = chip[chip_size - 1];
    size_t footprint = (byte_lanes == 0x0Fu) ? chip_size : chip_size * 4;
    if (bus_size < footprint ||
        !declrom_layout_chip(chip, chip_size, bus_buf + (bus_size - footprint), footprint, byte_lanes)) {
        LOG(0, "declrom_install_builtin: '%s' blob has unsupported byteLanes $%02x (or exceeds the bus window)",
            card_id, byte_lanes);
        return false;
    }
    // Report the pick into the built-from record like any resolved declROM —
    // path-less, identified by the blob's stored Format-Block CRC.
    const uint8_t *t = chip + chip_size - 12; // CRC field, big-endian
    uint32_t crc = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | (uint32_t)t[3];
    char locator[64];
    snprintf(locator, sizeof locator, "builtin:%s", card_id);
    machine_config_note_vrom(card_id, locator, crc, /*explicit_pick*/ false);
    return true;
}

bool declrom_load_vrom_card(const char *card_id, uint8_t *bus_buf, size_t bus_size, char **out_path) {
    if (out_path)
        *out_path = NULL;
    if (!card_id || !bus_buf || bus_size == 0)
        return false;

    // Walk the offer registry's candidates for this card in pick order
    // (explicit vrom.load first, then catalog-preferred, then catalog order —
    // see vrom_offer_find).  Every candidate was already content-identified
    // at offer time; the first one that lays out cleanly wins.  Core never
    // builds a path here — the platform offered every one of these.
    size_t chip_size = 0;
    for (int n = 0;; n++) {
        const char *path = vrom_offer_find(card_id, n, &chip_size);
        if (!path)
            break;
        if (load_chip_into_bus(path, chip_size, bus_buf, bus_size)) {
            // Report the winning pick into the built-from record so
            // machine.config.vroms answers which revision this machine
            // actually runs (proposal-named-args-boot-config §4.2).
            uint32_t crc = 0;
            bool explicit_pick = false;
            vrom_offer_info(path, &crc, &explicit_pick);
            machine_config_note_vrom(card_id, path, crc, explicit_pick);
            if (out_path)
                *out_path = strdup(path);
            return true;
        }
    }
    return false;
}
