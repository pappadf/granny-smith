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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (!f)
        return NULL;
    uint8_t *buf = malloc(expected_size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, expected_size, f);
    fclose(f);
    if (n != expected_size) {
        free(buf);
        return NULL;
    }
    return buf;
}
