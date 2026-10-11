// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rom.h
// ROM identification and loading. Owns the system ROM file: content
// identity, the table of known ROMs and the models each boots, file I/O,
// and the rom.* object-model surface.

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
// A ROM's identity (`id`) is the checksum field(s) the ROM itself carries,
// read verbatim, and its integrity (`intact`) is that checksum recomputed over
// the bytes.  The id is a stored label, so a damaged dump still says which ROM
// it was meant to be; nothing may be named by its id unless it is intact.
//
//   kind    id                                      self-check
//   MAC68K  %08x of the header sum at offset 0      header sum over the image
//   PPC     %08x-%016llx: header sum, ConfigInfo    header sum over the 68k half,
//           64-bit sum (4 MiB Old World images)     64-bit sum over the whole image
//   LISA    %04x of the check word at $3FFE         the boot ROM's rotating sum
//
// One table (rom_table.c) lists every ROM we know about.  A row's
// `compatible` list names the emulated models it boots; an empty list is a
// real ROM for a machine Granny Smith does not emulate.

// Which checksum layout an image carries (a content rule, not a table lookup).
typedef enum {
    ROM_KIND_NONE, // not a ROM (odd size, too small)
    ROM_KIND_MAC68K, // classic 68k header sum
    ROM_KIND_PPC, // 4 MiB Old World image with a ConfigInfo block
    ROM_KIND_LISA, // 16 KB interleaved Lisa / Macintosh XL boot ROM
} rom_kind_t;

// Longest id text plus NUL ("xxxxxxxx-xxxxxxxxxxxxxxxx").
#define ROM_ID_MAX 32

// Row flag: a PPC ROM whose ConfigInfo sums cover a range we do not know, so
// only its 68k half can be verified.
#define ROM_F_NO_SUM64 0x1u

// One ROM we know about.
typedef struct rom_info {
    const char *family_name; // Human-readable ("Universal IIx/IIcx/SE/30 ROM")
    const char *const *compatible; // NULL-terminated emulated model_ids; empty = not emulated
    const char *id; // Content id, lowercase hex (see above)
    // Sizes here are of the FILE (host bytes), so size_t like every other
    // host buffer size in this header; sizes on the guest bus (the memory
    // map's ROM region) stay uint32_t, the width of the address space.
    size_t rom_size; // Expected file size in bytes
    // MAC68K only: bytes the header sum covers when it is not the whole image
    // (the Classic's sum stops where its ROM disk starts); 0 = whole image.
    size_t checksum_span;
    uint32_t flags; // ROM_F_*
    // Short label telling this ROM apart from the other ROMs that boot the
    // same model ("Win NT", "Rev 2", "v2"); NULL when no other known ROM
    // shares a model with it.  UIs show it after the model name, as
    // "<model> (<variant>)", so every model/ROM pair reads differently.
    const char *variant;
} rom_info_t;

// The identity of one image: its kind, stored id and self-check verdict.
typedef struct rom_identity {
    rom_kind_t kind;
    char id[ROM_ID_MAX]; // "" for ROM_KIND_NONE
    bool intact; // the ROM's own checksum verifies
    char reason[80]; // which part does not verify; "" when intact
} rom_identity_t;

// The ROM table (rom_table.c).
extern const rom_info_t rom_table[];
extern const size_t rom_table_count;

// Kind, stored id and self-check of `data` by the plain per-kind rule.
void rom_identity_compute(const uint8_t *data, size_t size, rom_identity_t *out);

// The table row for `id` whose size is `size`, or NULL if not known.
const rom_info_t *rom_lookup(const char *id, size_t size);

// Identify raw ROM bytes: compute the identity, look it up, and re-verify
// with the matched row's exceptions (checksum_span, ROM_F_NO_SUM64).
// `out` may be NULL.  Returns the row, or NULL if the ROM is not known.
const rom_info_t *rom_identify_data(const uint8_t *data, size_t size, rom_identity_t *out);

// True when a known ROM boots at least one emulated model.
bool rom_is_supported(const rom_info_t *info);

// Short name of a kind: "mac68k", "ppc", "lisa", or "" for ROM_KIND_NONE.
const char *rom_kind_name(rom_kind_t kind);

// === Lisa / Macintosh XL two-chip ROM interleaving ==========================

// Interleave two byte-slice chips into a 16-bit-wide ROM image: even bytes ←
// `hi` (high data byte), odd bytes ← `lo` (low data byte). `out` must hold
// 2 * min(hi_size, lo_size) bytes.
void rom_interleave_pair(const uint8_t *hi, size_t hi_size, const uint8_t *lo, size_t lo_size, uint8_t *out);

// Read two Lisa/XL ROM chip files and interleave them into a fresh 16 KB image.
// Order-independent: tries both high/low orientations and keeps the one whose
// interleave passes the boot ROM's own self-check. Returns a malloc'd buffer (caller frees) of
// *out_size bytes, or NULL on read/size failure.
uint8_t *rom_load_lisa_pair(const char *path_a, const char *path_b, size_t *out_size);

// Number of entries in info->compatible (NULL-terminated walk).
int rom_info_compatible_count(const rom_info_t *info);

// === File-level helpers =====================================================

// What a ROM file is: its table row (if known), identity and size.
typedef struct rom_file_info {
    const rom_info_t *info; // NULL if the ROM is not known
    rom_identity_t identity;
    size_t size;
} rom_file_info_t;

// Probe a file at `path`. Returns 0 on success (file readable, *out filled),
// non-zero on read error. `out` is always zero-initialised.
int rom_probe_file(const char *path, rom_file_info_t *out);

// Read an entire ROM file (through the VFS: a ROM may be an archive member)
// into a fresh buffer the caller frees.  NULL on failure, saying why unless
// `quiet`.
uint8_t *rom_read_file(const char *path, size_t *out_size, bool quiet);

void rom_init(void);
void rom_delete(void);

#endif // ROM_H
