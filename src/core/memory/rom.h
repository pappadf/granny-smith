// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rom.h
// ROM identification: checksum validation and machine-type lookup.

#ifndef ROM_H
#define ROM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ROM identification result
typedef struct rom_info {
    const char *model_id; // machine registry key ("plus", "se30")
    const char *model_name; // human-readable name ("Macintosh Plus Rev 3")
    uint32_t checksum; // expected ROM checksum
    uint32_t rom_size; // expected ROM file size in bytes
} rom_info_t;

// Look up a ROM by its stored checksum (first 4 bytes, big-endian).
// Returns a pointer to a static rom_info_t entry, or NULL if unknown.
const rom_info_t *rom_identify(uint32_t checksum);

// Compute the ROM checksum using the sum-of-16-bit-words algorithm.
// data points to the full ROM image (including the 4-byte checksum header).
// size is the total ROM file size in bytes.
// Returns the calculated checksum (sum of 16-bit big-endian words from offset 4).
uint32_t rom_compute_checksum(const uint8_t *data, size_t size);

// Extract the stored checksum from a ROM image (first 4 bytes, big-endian).
uint32_t rom_stored_checksum(const uint8_t *data);

// Validate a ROM image: compute checksum and compare with stored value.
// Returns true if the computed checksum matches the stored checksum.
bool rom_validate(const uint8_t *data, size_t size);

// Identify a ROM from raw file data: extract checksum, validate, and look up.
// If checksum validation fails, falls back to file-size heuristics.
// out_checksum (if non-NULL) receives the stored checksum regardless of validity.
// Returns a rom_info_t pointer or NULL if completely unrecognised.
const rom_info_t *rom_identify_data(const uint8_t *data, size_t size, uint32_t *out_checksum);

#endif // ROM_H
