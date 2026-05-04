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

// Path of the ROM passed to the most recent rom_load_into_machine().
// Used by SE/30 init to auto-discover a sibling SE30.vrom file.
// Returns NULL if no ROM has been loaded.
const char *rom_pending_path(void);

// Set the pending ROM path before machine creation. Required when callers
// (e.g. headless_main) want SE/30 vrom auto-discovery to know where the
// ROM came from — otherwise SE/30 init can only fall back to fixed paths.
// rom_load_into_machine() also calls this internally on its way through.
int rom_pending_set(const char *path);

// Clear the pending path (called from system_destroy).
void rom_pending_clear(void);

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
