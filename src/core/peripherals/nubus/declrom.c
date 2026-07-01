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

void declrom_expand_lane3(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf) {
    for (size_t i = 0; i < chip_size; i++)
        bus_buf[i * 4 + 3] = chip[i];
}

bool declrom_layout_chip(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf, size_t bus_size, uint8_t byte_lanes) {
    if (byte_lanes == 0x78u) {
        // Standard display-card layout — lane 3 only; sparse-expand into 4×.
        if (bus_size < chip_size * 4)
            return false;
        declrom_expand_lane3(chip, chip_size, bus_buf);
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
// on an exact-size read (a short or oversized file is rejected).
static bool read_chip_exact(const char *path, uint8_t *buf, size_t chip_size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(buf, 1, chip_size, f);
    fclose(f);
    return n == chip_size;
}

bool declrom_load_vrom(const char *filename, size_t chip_size, uint8_t *bus_buf, size_t bus_size, char **out_path) {
    if (out_path)
        *out_path = NULL;
    if (!filename || !bus_buf || chip_size == 0)
        return false;

    uint8_t *chip = calloc(1, chip_size);
    if (!chip)
        return false;

    char path[1024];
    char *found_path = NULL;

    // 1. Next to the pending CPU ROM — the directory the integration-test
    //    harness cds into, where sibling vrom files live.
    const char *rom_path = rom_pending_path();
    if (rom_path) {
        const char *slash = strrchr(rom_path, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - rom_path + 1);
            if (dir_len + strlen(filename) + 1 <= sizeof(path)) {
                memcpy(path, rom_path, dir_len);
                strcpy(path + dir_len, filename);
                if (read_chip_exact(path, chip, chip_size))
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
        if (read_chip_exact(path, chip, chip_size))
            found_path = strdup(path);
    }

    if (!found_path) {
        free(chip);
        return false;
    }

    // The chip's last byte is the byteLanes value (for the lane-3-only and
    // 4-lane layouts both the spec and these files place it at the highest
    // active-lane address — the chip's final byte).
    uint8_t byte_lanes = chip[chip_size - 1];
    if (!declrom_layout_chip(chip, chip_size, bus_buf, bus_size, byte_lanes)) {
        LOG(0, "declrom_load_vrom: '%s' has unsupported byteLanes $%02x (only $78 and $0F handled)", found_path,
            byte_lanes);
        free(chip);
        free(found_path);
        return false;
    }

    free(chip);
    if (out_path)
        *out_path = found_path;
    else
        free(found_path);
    return true;
}
