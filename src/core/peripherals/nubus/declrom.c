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
#include "rom.h" // rom_pending_path() — sibling-vrom search
#include "vrom.h" // content identification (Format-Block CRC catalog)

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
    if (chip_size == 0 || bus_size < chip_size * 4)
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
    size_t footprint = chip_size * 4; // single-lane expansion (the common case)
    bool ok = declrom_layout_chip(chip, chip_size, bus_buf + (bus_size - footprint), footprint, byte_lanes);
    if (!ok)
        LOG(0, "declrom_load_vrom_card: '%s' has unsupported byteLanes $%02x", path, byte_lanes);
    free(chip);
    return ok;
}

bool declrom_load_vrom_card(const char *card_id, uint8_t *bus_buf, size_t bus_size, char **out_path) {
    if (out_path)
        *out_path = NULL;
    if (!card_id || !bus_buf || bus_size == 0)
        return false;

    // 0. The explicitly loaded vROM (machine.vrom.load <path> — how the web
    //    UI hands over an uploaded file).  Content-verified: the file must
    //    identify (Format-Block CRC) to THIS card; the filename is irrelevant.
    const char *pending = vrom_pending_path();
    vrom_id_t id;
    if (pending && vrom_identify_card(pending, &id)) {
        if (strcmp(id.card_id, card_id) == 0) {
            if (load_chip_into_bus(pending, id.chip_size, bus_buf, bus_size)) {
                if (out_path)
                    *out_path = strdup(pending);
                return true;
            }
        } else {
            LOG(2, "vrom.load path '%s' provides card '%s', not '%s' — searching instead", pending, id.card_id,
                card_id);
        }
    }

    // 1..4. Search the standard locations for EVERY catalog filename that
    //    provides this card (a card can have several ROM revisions, with
    //    different chip sizes — e.g. the 8•24 GC's v1.1/v1.0/alpha), still
    //    content-verifying each hit before loading it.
    char path[1024];
    for (int n = 0;; n++) {
        const char *filename = vrom_catalog_name(card_id, n, NULL);
        if (!filename)
            break;

        // 1. Next to the pending CPU ROM — the directory the integration-test
        //    harness cds into, where sibling vrom files live.
        char *found_path = NULL;
        const char *rom_path = rom_pending_path();
        if (rom_path) {
            const char *slash = strrchr(rom_path, '/');
            if (slash) {
                size_t dir_len = (size_t)(slash - rom_path + 1);
                if (dir_len + strlen(filename) + 1 <= sizeof(path)) {
                    memcpy(path, rom_path, dir_len);
                    strcpy(path + dir_len, filename);
                    if (vrom_identify_card(path, &id) && strcmp(id.card_id, card_id) == 0)
                        found_path = strdup(path);
                }
            }
        }

        // 2..4. Standard search prefixes, in priority order.
        static const char *const prefixes[] = {
            "/opfs/images/vrom/",
            "tests/data/roms/",
            "",
            NULL,
        };
        for (const char *const *pre = prefixes; !found_path && *pre; pre++) {
            if (strlen(*pre) + strlen(filename) + 1 > sizeof(path))
                continue;
            strcpy(path, *pre);
            strcat(path, filename);
            if (vrom_identify_card(path, &id) && strcmp(id.card_id, card_id) == 0)
                found_path = strdup(path);
        }

        if (found_path) {
            // id was filled by the successful identify; its chip_size is the
            // actual file size.
            if (load_chip_into_bus(found_path, id.chip_size, bus_buf, bus_size)) {
                if (out_path)
                    *out_path = found_path;
                else
                    free(found_path);
                return true;
            }
            free(found_path);
        }
    }
    return false;
}
