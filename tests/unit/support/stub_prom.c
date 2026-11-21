// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// The PROM catalog, for suites that link pci.c without the ROM layer: no file
// is a known PROM, and a card's PROM can be found only when the suite says
// so (stub_prom_offered).

#include "prom.h"

#include <stdlib.h>
#include <string.h>

// The image prom_load_card hands back when a PROM is offered.
#define STUB_PROM_SIZE 0x8000u

bool stub_prom_offered = false;

bool prom_identify_card(const char *path, prom_id_t *out) {
    (void)path;
    (void)out;
    return false;
}

bool prom_card_resolvable(const char *card_id, const char *rom) {
    (void)card_id;
    (void)rom;
    return stub_prom_offered;
}

// Loading one hands back a blank 32 KB image (all $FF, an unprogrammed chip):
// whether a card's PROM is offered is decided before the bus loads it
// (prom_card_resolvable, the seam the suites steer), so the load itself
// always finds something.
bool prom_load_card(const char *card_id, const char *rom, uint8_t **out_buf, size_t *out_size, char **out_path) {
    (void)card_id;
    (void)rom;
    *out_buf = NULL;
    *out_size = 0;
    if (out_path)
        *out_path = NULL;
    *out_buf = malloc(STUB_PROM_SIZE);
    if (!*out_buf)
        return false;
    memset(*out_buf, 0xFF, STUB_PROM_SIZE);
    *out_size = STUB_PROM_SIZE;
    return true;
}
