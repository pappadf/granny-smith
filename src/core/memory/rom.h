// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rom.h
// ROM identification and loading. Owns the system ROM file: identify-by-
// checksum, list of machine models a given ROM is compatible with, file
// I/O, and the rom.* object-model surface.

#ifndef ROM_H
#define ROM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Forward declarations ==================================================
struct class_desc;
struct object;

// === ROM identification =====================================================
//
// A single ROM image (identified by checksum + size) maps to one or more
// machine model_ids. The 256 KB Universal ROM, for example, is shared by
// the SE/30, IIcx, and IIx — there is no way to derive the machine from the
// ROM alone, so the user must pick one of `compatible[]`.
typedef struct rom_info {
    const char *family_name; // Human-readable ("Universal IIx/IIcx/SE/30 ROM")
    const char *const *compatible; // NULL-terminated list of compatible model_ids
    uint32_t checksum; // Stored checksum (first 4 bytes, big-endian)
    uint32_t rom_size; // Expected file size in bytes
    // Note: canonical fixture filenames are a tooling concern (scripts/
    // rom_naming.py maps content identity → name for the gs-test-data repo);
    // core reasons only in content identities and never knows a filename.
} rom_info_t;

// Look up a ROM by its stored checksum.
// Returns a pointer to a static rom_info_t entry, or NULL if unknown.
const rom_info_t *rom_identify(uint32_t checksum);

// Compute the ROM checksum (sum of 16-bit big-endian words from offset 4).
uint32_t rom_compute_checksum(const uint8_t *data, size_t size);

// Extract the stored checksum from a ROM image (first 4 bytes, big-endian).
uint32_t rom_stored_checksum(const uint8_t *data);

// Validate a ROM image by comparing stored vs. computed checksum.
bool rom_validate(const uint8_t *data, size_t size);

// Identify a ROM from raw file data; falls back to file-size heuristics for
// unrecognised checksums. *out_checksum (if non-NULL) is set unconditionally.
const rom_info_t *rom_identify_data(const uint8_t *data, size_t size, uint32_t *out_checksum);

// Identify an interleaved 16 KB Apple Lisa 2 / Macintosh XL boot ROM by size +
// the version word at offset $3FFC ($0248 = Lisa 2 rev H, $0341 = Mac XL "3A").
// Returns a static rom_info_t, or NULL if `data`/`size` aren't a Lisa ROM.
const rom_info_t *rom_identify_lisa(const uint8_t *data, size_t size);

// === Lisa / Macintosh XL two-chip ROM interleaving ==========================

// Interleave two byte-slice chips into a 16-bit-wide ROM image: even bytes ←
// `hi` (high data byte), odd bytes ← `lo` (low data byte). `out` must hold
// 2 * min(hi_size, lo_size) bytes.
void rom_interleave_pair(const uint8_t *hi, size_t hi_size, const uint8_t *lo, size_t lo_size, uint8_t *out);

// Read two Lisa/XL ROM chip files and interleave them into a fresh 16 KB image.
// Order-independent: tries both high/low orientations and keeps the one that
// identifies as a Lisa ROM. Returns a malloc'd buffer (caller frees) of
// *out_size bytes, or NULL on read/size failure.
uint8_t *rom_load_lisa_pair(const char *path_a, const char *path_b, size_t *out_size);

// Number of entries in info->compatible (NULL-terminated walk).
int rom_info_compatible_count(const rom_info_t *info);

// === File-level helpers =====================================================

typedef struct rom_file_info {
    const rom_info_t *info; // NULL if checksum unrecognised
    uint32_t checksum;
    size_t size;
} rom_file_info_t;

// Probe a file at `path`. Returns 0 on success (file readable, *out filled),
// non-zero on read error. `out` is always zero-initialised.
int rom_probe_file(const char *path, rom_file_info_t *out);

// === Loading into the active machine ========================================

// Load a ROM file into the currently-active machine's memory and reset the
// CPU. Caller must have created a machine (via machine.boot) first; this
// function does NOT pick a machine for you. Returns 0 on success, -1 on
// failure (no machine, file unreadable, OOM). Size mismatch with the
// machine's expected ROM size produces a warning but is not fatal — the
// truncating copy matches historical behaviour for Plus ROMs.
int rom_load_into_machine(const char *path);

// Like rom_load_into_machine, but for the Lisa/XL two-chip boot ROM: interleave
// `path_a` and `path_b` (either order) into 16 KB and install. Returns 0 on
// success, -1 on failure (no machine, unreadable/wrong-size chips).
int rom_load_lisa_into_machine(const char *path_a, const char *path_b);

// === Lifecycle =============================================================
//
// rom_init() creates the singleton `rom` object node and attaches it under
// the root; rom_delete() detaches/frees it and clears the pending path.
// Both are idempotent — repeated calls are safe and turn into no-ops.
// Called from system_create / system_destroy alongside the other subsystems.
extern const struct class_desc rom_class;

void rom_init(void);
void rom_delete(void);

#endif // ROM_H
