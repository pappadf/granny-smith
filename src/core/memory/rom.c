// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rom.c
// ROM identification: checksum computation, validation, and machine-type lookup
// using a master table of known Macintosh ROM signatures.

#include "rom.h"

#include <stdio.h>
#include <string.h>

// Master ROM signature table.
// Checksums and model assignments from the Macintosh ROM Signatures reference.
static const rom_info_t ROM_TABLE[] = {
    // Macintosh Plus — three revisions, all 128 KB
    {"plus", "Macintosh Plus (Rev 1, Lonely Hearts)",   0x4D1EEEE1, 128 * 1024},
    {"plus", "Macintosh Plus (Rev 2, Lonely Heifers)",  0x4D1EEAE1, 128 * 1024},
    {"plus", "Macintosh Plus (Rev 3, Loud Harmonicas)", 0x4D1F8172, 128 * 1024},

    // Universal IIx/IIcx/SE30 ROM — 256 KB, defaults to SE/30
    {"se30", "Macintosh SE/30 (Universal ROM)",         0x97221136, 256 * 1024},
};

// Number of entries in the ROM table
#define ROM_TABLE_COUNT (sizeof(ROM_TABLE) / sizeof(ROM_TABLE[0]))

// Look up a ROM by its stored checksum
const rom_info_t *rom_identify(uint32_t checksum) {
    for (size_t i = 0; i < ROM_TABLE_COUNT; i++) {
        if (ROM_TABLE[i].checksum == checksum)
            return &ROM_TABLE[i];
    }
    return NULL;
}

// Compute ROM checksum: sum of big-endian 16-bit words starting at byte 4
uint32_t rom_compute_checksum(const uint8_t *data, size_t size) {
    uint32_t sum = 0;
    // Start at byte 4 (skip the stored checksum), step by 2
    for (size_t i = 4; i + 1 < size; i += 2) {
        uint16_t word = ((uint16_t)data[i] << 8) | data[i + 1];
        sum += word;
    }
    return sum;
}

// Extract stored checksum from first 4 bytes (big-endian)
uint32_t rom_stored_checksum(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

// Validate a ROM image by comparing computed vs stored checksum
bool rom_validate(const uint8_t *data, size_t size) {
    if (size < 8)
        return false; // too small to contain checksum + any data
    return rom_compute_checksum(data, size) == rom_stored_checksum(data);
}

// Identify ROM from raw data with fallback to file-size heuristics
const rom_info_t *rom_identify_data(const uint8_t *data, size_t size, uint32_t *out_checksum) {
    if (!data || size < 8)
        return NULL;

    uint32_t stored = rom_stored_checksum(data);
    if (out_checksum)
        *out_checksum = stored;

    // Try exact checksum match first
    const rom_info_t *info = rom_identify(stored);
    if (info) {
        // Validate that computed checksum matches stored
        uint32_t computed = rom_compute_checksum(data, size);
        if (computed != stored) {
            printf("Warning: ROM checksum mismatch (stored=%08X, computed=%08X)\n", stored, computed);
        }
        return info;
    }

    // Fallback: use file-size heuristics for unknown ROMs
    if (size == 128 * 1024) {
        printf("Warning: Unknown ROM checksum %08X, assuming Macintosh Plus (128 KB)\n", stored);
        return &ROM_TABLE[2]; // Plus Rev 3 as default Plus entry
    }
    if (size == 256 * 1024) {
        printf("Warning: Unknown ROM checksum %08X, assuming SE/30 (256 KB)\n", stored);
        return &ROM_TABLE[3]; // SE/30 Universal ROM entry
    }

    printf("Warning: Unknown ROM checksum %08X, size %zu bytes — cannot identify\n", stored, size);
    return NULL;
}
