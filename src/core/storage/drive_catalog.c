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
// Where possible, use the earliest OEM mechanism for backward compatibility.
// Source: Apple product brochures (Jul 1987, Feb 1989), Quantum/Seagate manuals.
//
// The three entries at 230 MB and above are Apple-shipped OEM mechanisms whose
// INQUIRY strings and block counts come from bitsavers' apple_scsi_ident.txt;
// their sizes are (blocks x 512) exactly as those drives report via READ
// CAPACITY.  They exist because Apple's own formatters gate on capacity as
// well as on the "APPLE COMPUTER, INC." MODE SENSE page 0x30 signature (see
// scsi.c), and because Copland's installer demands a >= 230 MB target volume.
// The pre-existing sub-230 MB entries keep revision "1.0" - the value the
// attach path hard-coded before this field existed - so their INQUIRY response
// is unchanged.
static const struct drive_model catalog[] = {
    {"HD20SC",   "MINISCRB", "8425S",            "1.0",  21307392  },
    {"HD20SC",   " SEAGATE", "ST225N",           "1.0",  21411840  },
    {"HD40SC",   "QUANTUM ", "Q250",             "1.0",  40061952  },
    {"HD80SC",   "QUANTUM ", "Q280",             "1.0",  80061440  },
    {"HD160SC",  "QUANTUM ", "ELS170S",          "1.0",  177269760 },
    // Quantum ProDrive LPS 240S - 479,350 blocks
    {"HD230SC",  "QUANTUM ", "LP240S GM240S01X", "6.3 ", 245427200 },
    // Quantum ProDrive LPS 540S, Apple P/N 655-0202 - 1,057,616 blocks
    {"HD500SC",  "QUANTUM ", "LPS540S",          "590A", 541499392 },
    // IBM DPES-31080, Apple P/N 655-0141 - 2,118,144 blocks
    {"HD1000SC", "IBM     ", "DPES-31080",       "S31K", 1084489728},
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

// Parse a human-friendly size string into exact drive model bytes.
//
// Two unit conventions are accepted intentionally, matching how period
// product brochures and Unix-side tooling each use them:
//   - "mb" / "gb"  -> decimal (10^6 / 10^9) -- catalog lookup, snaps to
//                     the closest known drive model size (>= target).
//                     Apple advertised these drives by decimal MB.
//   - "k" / "m"    -> binary  (2^10 / 2^20) -- exact byte count, no
//                     model rounding.  Matches dd / mkfs / hdiutil
//                     long-standing Unix tooling convention.
// The two differ by ~5 % at "20mb" vs "20m"; this is deliberate.
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
