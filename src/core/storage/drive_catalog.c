// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// drive_catalog.c
// Single source of truth for known SCSI hard disk drive models.

#include "drive_catalog.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Known SCSI hard disk models, sorted by size ascending.
// Sizes must be multiples of 512 (SCSI block size) so that
// image_create_empty/image_open produce valid block-aligned images.
static const struct drive_model catalog[] = {
    {"HD20SC",  "MINISCRB", "8425S",    21307392 },
    {"HD20SC",  " SEAGATE", "ST225N",   21411840 },
    {"HD40SC",  "CONNER",   "CP3040",   42881536 },
    {"HD80SC",  "QUANTUM",  "PRODRIVE", 81222144 },
    {"HD160SC", "QUANTUM",  "LPS170S",  177269760},
};

// number of entries in the catalog
static const int catalog_count = sizeof(catalog) / sizeof(catalog[0]);

// return the number of known drive models
int drive_catalog_count(void) {
    return catalog_count;
}

// return the drive model at index i (0-based)
const struct drive_model *drive_catalog_get(int i) {
    if (i < 0 || i >= catalog_count)
        return NULL;
    return &catalog[i];
}

// find the closest model whose size >= the given size
const struct drive_model *drive_catalog_find_closest(size_t size) {
    for (int i = 0; i < catalog_count; i++) {
        if (size <= catalog[i].size)
            return &catalog[i];
    }
    // fall back to the largest model
    return &catalog[catalog_count - 1];
}

// check if str ends with a case-insensitive suffix
static int ends_with_ci(const char *str, const char *suffix) {
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    if (slen < xlen)
        return 0;
    return strcasecmp(str + slen - xlen, suffix) == 0;
}

// parse a human-friendly size string into exact drive model bytes
size_t drive_catalog_parse_size(const char *str) {
    if (!str || !*str)
        return 0;

    // try model label lookup first (e.g. "HD20SC")
    for (int i = 0; i < catalog_count; i++) {
        if (strcasecmp(str, catalog[i].label) == 0)
            return catalog[i].size;
    }

    // try "mb" / "gb" suffix -> model-aware rounding
    if (ends_with_ci(str, "mb") || ends_with_ci(str, "gb")) {
        char *end = NULL;
        unsigned long long val = strtoull(str, &end, 10);
        if (end == str || val == 0)
            return 0;
        // compute target bytes from the human-friendly unit
        size_t target;
        if (ends_with_ci(end, "gb"))
            target = (size_t)(val * 1000ULL * 1000ULL * 1000ULL);
        else
            target = (size_t)(val * 1000ULL * 1000ULL);
        // find the closest model >= target
        const struct drive_model *m = drive_catalog_find_closest(target);
        return m->size;
    }

    // try exact binary suffixes: K/k, M/m (backward-compatible)
    char *end = NULL;
    unsigned long long val = strtoull(str, &end, 10);
    if (end == str || val == 0)
        return 0;
    if (*end == 'K' || *end == 'k') {
        val *= 1024;
        end++;
    } else if (*end == 'M' || *end == 'm') {
        val *= 1024 * 1024;
        end++;
    }
    // reject trailing garbage
    if (*end != '\0')
        return 0;
    return (size_t)val;
}
