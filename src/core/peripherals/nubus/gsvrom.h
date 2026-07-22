// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gsvrom.h
// Access to the built-in GS generic declaration ROM images (see
// tools/vrom/ and proposal-generic-nubus-vrom.md).  One blob per card
// personality; the generic sibling card kinds install these instead of
// consulting the vROM offer registry.

#ifndef NUBUS_GSVROM_H
#define NUBUS_GSVROM_H

#include <stddef.h>
#include <stdint.h>

// Card personalities with a built-in declaration ROM.
typedef enum gsvrom_personality {
    GSVROM_JMFB = 0, // 8•24 (JMFB) register model
    GSVROM_BOOGIE, // Display Card 24AC register model
    GSVROM_MDCGC, // 8•24 GC register model
    GSVROM_SE30, // SE/30 built-in video (framebuffer only)
} gsvrom_personality_t;

// Returns the personality's chip image (dense 4-lane, Format Block at the
// end) and its size.  The pointer is static storage — never freed.
const uint8_t *gsvrom_blob(gsvrom_personality_t p, size_t *out_size);

// Returns the Format-Block CRC stored in a blob (its content identity, as
// listed by machine.config.vroms and recognisable by vrom.identify).
uint32_t gsvrom_blob_crc(gsvrom_personality_t p);

#endif // NUBUS_GSVROM_H
