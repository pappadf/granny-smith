// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// The PROM catalog, for suites that link pci.c without the ROM layer: no file
// is a known PROM, and a card's PROM can be found only when the suite says
// so (stub_prom_offered).

#include "prom.h"

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
