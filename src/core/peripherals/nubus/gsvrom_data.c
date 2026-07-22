// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gsvrom_data.c
// The built-in GS generic declaration ROM blobs.  The images themselves
// live in the committed generated header gsvrom_blobs.h (regenerate with
// `make -C tools/vrom`); this file is the only translation unit that
// includes it, and exposes the lookup API from gsvrom.h.

#include "gsvrom.h"
#include "gsvrom_blobs.h"

// Per-personality blob table, indexed by gsvrom_personality_t.
static const struct {
    const uint8_t *bytes;
    size_t size;
} s_blobs[] = {
    [GSVROM_JMFB] = {gsvrom_jmfb,   sizeof(gsvrom_jmfb)  },
    [GSVROM_BOOGIE] = {gsvrom_boogie, sizeof(gsvrom_boogie)},
    [GSVROM_MDCGC] = {gsvrom_mdcgc,  sizeof(gsvrom_mdcgc) },
    [GSVROM_SE30] = {gsvrom_se30,   sizeof(gsvrom_se30)  },
};

const uint8_t *gsvrom_blob(gsvrom_personality_t p, size_t *out_size) {
    if ((size_t)p >= sizeof(s_blobs) / sizeof(s_blobs[0]))
        return NULL;
    if (out_size)
        *out_size = s_blobs[p].size;
    return s_blobs[p].bytes;
}

uint32_t gsvrom_blob_crc(gsvrom_personality_t p) {
    size_t size = 0;
    const uint8_t *b = gsvrom_blob(p, &size);
    if (!b || size < 20)
        return 0;
    // The CRC field starts 12 bytes from the end of the Format Block
    // (see docs/core/peripherals/nubus_vrom.md sec. 2), big-endian.
    const uint8_t *t = b + size - 12;
    return ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | (uint32_t)t[3];
}
