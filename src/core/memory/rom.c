// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rom.c
// ROM identification, file I/O, and the rom.* object-model surface.
//
// New machine model: the user picks a machine via machine.boot(model[, ram_kb])
// before loading a ROM. rom.load(path) only writes bytes into the active
// machine; it does NOT pick the machine for you. Multiple machines can share
// the same ROM image (the SE/30, IIcx, and IIx all run the universal ROM),
// so rom.identify(path) returns the *list* of compatible models — the user
// is the source of truth.

#include "rom.h"

#include "cpu.h"
#include "json_encode.h"
#include "memory.h"
#include "mmu.h"
#include "object.h"
#include "system.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ============================================================================
// Compatible-model lists
// ============================================================================

static const char *const PLUS_COMPATIBLE[] = {"plus", NULL};
// Universal ROM: same bytes for IIx, IIcx, SE/30. Listed in machine_register
// order so the *first* entry serves as a UI default (for tests that don't pick
// explicitly), but the API never auto-selects.
static const char *const UNIVERSAL_COMPATIBLE[] = {"se30", "iicx", "iix", NULL};
static const char *const IIFX_COMPATIBLE[] = {"iifx", NULL};
// Dedicated 512 KB Macintosh IIci ("Aurora") ROM — not the universal ROM.
static const char *const IICI_COMPATIBLE[] = {"iici", NULL};
// Apple Lisa 2 (rev H) and Macintosh XL ("3A") interleaved boot ROMs (§4.11 of
// proposal-machine-lisa-xl.md). Each is two 8 KB byte-slice chips interleaved
// into a 16 KB image; see rom_load_lisa_pair / rom_identify_lisa below.
static const char *const LISA_COMPATIBLE[] = {"lisa", NULL};
static const char *const MACXL_COMPATIBLE[] = {"macxl", NULL};
// Dedicated 512 KB Macintosh IIsi ("Erickson") ROM — not the universal ROM.
static const char *const IISI_COMPATIBLE[] = {"iisi", NULL};

// Master ROM signature table.
static const rom_info_t ROM_TABLE[] = {
    {"Macintosh Plus (Rev 1, Lonely Hearts)",   PLUS_COMPATIBLE,      0x4D1EEEE1, 128 * 1024},
    {"Macintosh Plus (Rev 2, Lonely Heifers)",  PLUS_COMPATIBLE,      0x4D1EEAE1, 128 * 1024},
    {"Macintosh Plus (Rev 3, Loud Harmonicas)", PLUS_COMPATIBLE,      0x4D1F8172, 128 * 1024},
    {"Universal IIx/IIcx/SE/30 ROM",            UNIVERSAL_COMPATIBLE, 0x97221136, 256 * 1024},
    {"Macintosh IIfx ROM",                      IIFX_COMPATIBLE,      0x4147DD77, 512 * 1024},
    {"Macintosh IIci ROM",                      IICI_COMPATIBLE,      0x368CADFE, 512 * 1024},
    {"Macintosh IIsi ROM",                      IISI_COMPATIBLE,      0x36B7FB6C, 512 * 1024},
};

#define ROM_TABLE_COUNT (sizeof(ROM_TABLE) / sizeof(ROM_TABLE[0]))

// === Lisa 2 / Macintosh XL boot-ROM signatures ============================
//
// The Lisa boot ROM does NOT follow the Mac checksum convention: its first
// longword is the reset SSP ($00000480), identical across revisions, so it
// cannot be told apart by rom_stored_checksum(). Identify the interleaved
// 16 KB image instead by (size == 16 KB) + the version word at offset $3FFC
// (proposal §4.11 / docs/machines/lisa/lisa.md §16). The `info.checksum` field carries the
// Mac-style *computed* checksum of the combined image so the rom.* object
// surface (rom.checksum, OPFS naming) still has a unique content identifier.
#define LISA_ROM_SIZE           (16 * 1024) // interleaved image size
#define LISA_ROM_VERSION_OFFSET 0x3FFC // version word location
#define LISA_RESET_SSP          0x00000480u // first longword of every Lisa ROM

typedef struct lisa_rom_sig {
    rom_info_t info; // family_name / compatible / computed checksum / size
    uint16_t version_word; // value at LISA_ROM_VERSION_OFFSET
} lisa_rom_sig_t;

static const lisa_rom_sig_t LISA_ROM_TABLE[] = {
    {{"Apple Lisa 2 Boot ROM (rev H)", LISA_COMPATIBLE, 0x098917B2, LISA_ROM_SIZE},   0x0248},
    {{"Macintosh XL Boot ROM (\"3A\")", MACXL_COMPATIBLE, 0x094C82F0, LISA_ROM_SIZE}, 0x0341},
};

#define LISA_ROM_TABLE_COUNT (sizeof(LISA_ROM_TABLE) / sizeof(LISA_ROM_TABLE[0]))

// ============================================================================
// ID helpers
// ============================================================================

const rom_info_t *rom_identify(uint32_t checksum) {
    for (size_t i = 0; i < ROM_TABLE_COUNT; i++) {
        if (ROM_TABLE[i].checksum == checksum)
            return &ROM_TABLE[i];
    }
    return NULL;
}

const rom_info_t *rom_identify_lisa(const uint8_t *data, size_t size) {
    // Lisa/XL ROMs are exactly 16 KB and start with the reset SSP $00000480;
    // require both before trusting the version word (cheap false-positive guard
    // against an unrelated 16 KB blob).
    if (!data || size != LISA_ROM_SIZE)
        return NULL;
    if (rom_stored_checksum(data) != LISA_RESET_SSP)
        return NULL;
    uint16_t version = ((uint16_t)data[LISA_ROM_VERSION_OFFSET] << 8) | data[LISA_ROM_VERSION_OFFSET + 1];
    for (size_t i = 0; i < LISA_ROM_TABLE_COUNT; i++) {
        if (LISA_ROM_TABLE[i].version_word == version)
            return &LISA_ROM_TABLE[i].info;
    }
    return NULL;
}

uint32_t rom_compute_checksum(const uint8_t *data, size_t size) {
    // Real Macintosh ROMs are word-aligned (even size). An odd-sized buffer
    // silently drops the trailing byte; the stored-vs-computed mismatch will
    // flag it, but assert in debug builds so the cause is obvious.
    GS_ASSERTF((size % 2) == 0, "rom_compute_checksum: odd ROM size %zu", size);
    uint32_t sum = 0;
    for (size_t i = 4; i + 1 < size; i += 2) {
        uint16_t word = ((uint16_t)data[i] << 8) | data[i + 1];
        sum += word;
    }
    return sum;
}

uint32_t rom_stored_checksum(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

bool rom_validate(const uint8_t *data, size_t size) {
    if (size < 8)
        return false;
    return rom_compute_checksum(data, size) == rom_stored_checksum(data);
}

const rom_info_t *rom_identify_data(const uint8_t *data, size_t size, uint32_t *out_checksum) {
    if (!data || size < 8)
        return NULL;

    uint32_t stored = rom_stored_checksum(data);
    if (out_checksum)
        *out_checksum = stored;

    const rom_info_t *info = rom_identify(stored);
    if (info) {
        uint32_t computed = rom_compute_checksum(data, size);
        if (computed != stored)
            printf("Warning: ROM checksum mismatch (stored=%08X, computed=%08X)\n", stored, computed);
        return info;
    }

    // Not a Mac ROM — try the Lisa/XL signature path. Lisa ROMs are matched by
    // size + version word, so report the Mac-style *computed* checksum (a
    // stable, unique content id) rather than the meaningless reset-SSP word.
    const rom_info_t *lisa = rom_identify_lisa(data, size);
    if (lisa) {
        if (out_checksum)
            *out_checksum = rom_compute_checksum(data, size);
        return lisa;
    }
    return NULL;
}

int rom_info_compatible_count(const rom_info_t *info) {
    if (!info || !info->compatible)
        return 0;
    int n = 0;
    for (const char *const *p = info->compatible; *p; p++)
        n++;
    return n;
}

// ============================================================================
// File I/O
// ============================================================================

// Read an entire ROM file into a fresh buffer. Caller frees on success.
static uint8_t *read_rom_file(const char *filename, size_t *out_size, bool quiet) {
    // Size via stat — fseek(SEEK_END)+ftell on a binary stream is technically
    // UB per ISO C (offset of end is implementation-defined). stat is portable
    // and matches what the rest of the codebase uses for file sizing.
    struct stat st;
    if (stat(filename, &st) != 0 || st.st_size <= 0) {
        if (!quiet)
            printf("Failed to stat ROM file: %s\n", filename);
        return NULL;
    }
    size_t file_size = (size_t)st.st_size;

    FILE *f = fopen(filename, "rb");
    if (!f) {
        if (!quiet)
            printf("Failed to open ROM file: %s\n", filename);
        return NULL;
    }
    uint8_t *rom_data = malloc(file_size);
    if (!rom_data) {
        fclose(f);
        if (!quiet)
            printf("Failed to allocate memory for ROM\n");
        return NULL;
    }
    size_t n = fread(rom_data, 1, file_size, f);
    fclose(f);
    if (n != file_size) {
        free(rom_data);
        if (!quiet)
            printf("Failed to read ROM file: %s\n", filename);
        return NULL;
    }
    *out_size = file_size;
    return rom_data;
}

int rom_probe_file(const char *path, rom_file_info_t *out) {
    if (!out)
        return -1;
    out->info = NULL;
    out->checksum = 0;
    out->size = 0;
    if (!path || !*path)
        return -1;
    size_t file_size = 0;
    uint8_t *data = read_rom_file(path, &file_size, true);
    if (!data)
        return -1;
    uint32_t checksum = 0;
    const rom_info_t *info = rom_identify_data(data, file_size, &checksum);
    free(data);
    out->info = info;
    out->checksum = checksum;
    out->size = file_size;
    return 0;
}

// ============================================================================
// Lisa / Macintosh XL two-chip ROM interleaving
// ============================================================================

// Interleave two 8 KB byte-slice chips into a 16-bit-wide ROM image:
// even bytes ← high-byte chip (D8–D15), odd bytes ← low-byte chip (D0–D7).
// `out` must hold 2 * min(hi_size, lo_size) bytes. (docs/machines/lisa/lisa.md §16)
void rom_interleave_pair(const uint8_t *hi, size_t hi_size, const uint8_t *lo, size_t lo_size, uint8_t *out) {
    size_t n = hi_size < lo_size ? hi_size : lo_size;
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hi[i]; // even byte = high data byte
        out[2 * i + 1] = lo[i]; // odd byte = low data byte
    }
}

// Read two Lisa/XL ROM chip files and interleave them into a fresh 16 KB image.
// The two chips' high/low roles differ between the H pair (0175=HI, 0176=LO)
// and the 3A pair (0347=HI, 0346=LO), so this loader is order-independent: it
// tries path_a as the high-byte chip first and, if the result is not a valid
// Lisa ROM, swaps the roles. Returns a malloc'd buffer (caller frees) of
// *out_size bytes, or NULL on any read/size/identification failure.
uint8_t *rom_load_lisa_pair(const char *path_a, const char *path_b, size_t *out_size) {
    if (!path_a || !path_b || !out_size)
        return NULL;

    size_t a_size = 0, b_size = 0;
    uint8_t *a = read_rom_file(path_a, &a_size, false);
    if (!a)
        return NULL;
    uint8_t *b = read_rom_file(path_b, &b_size, false);
    if (!b) {
        free(a);
        return NULL;
    }

    uint8_t *combined = NULL;
    // Each chip must be exactly half the 16 KB image.
    if (a_size == LISA_ROM_SIZE / 2 && b_size == LISA_ROM_SIZE / 2) {
        combined = malloc(LISA_ROM_SIZE);
        if (combined) {
            // Try a=HI/b=LO; if that orientation doesn't identify as a Lisa
            // ROM, reinterleave with the chips swapped.
            rom_interleave_pair(a, a_size, b, b_size, combined);
            if (!rom_identify_lisa(combined, LISA_ROM_SIZE))
                rom_interleave_pair(b, b_size, a, a_size, combined);
        }
    } else {
        printf("rom.load_lisa: each chip must be %d bytes (got %zu and %zu)\n", LISA_ROM_SIZE / 2, a_size, b_size);
    }

    free(a);
    free(b);
    if (!combined)
        return NULL;
    if (!rom_identify_lisa(combined, LISA_ROM_SIZE))
        printf("Warning: interleaved image is not a recognised Lisa/XL ROM\n");
    *out_size = LISA_ROM_SIZE;
    return combined;
}

// ============================================================================
// Pending-path tracking (consumed by SE/30 vrom auto-discovery)
// ============================================================================

static char *s_pending_rom_path = NULL;

const char *rom_pending_path(void) {
    return s_pending_rom_path;
}

int rom_pending_set(const char *path) {
    if (!path)
        return -1;
    char *dup = strdup(path);
    if (!dup)
        return -1;
    free(s_pending_rom_path);
    s_pending_rom_path = dup;
    return 0;
}

void rom_pending_clear(void) {
    free(s_pending_rom_path);
    s_pending_rom_path = NULL;
}

// ============================================================================
// Load into active machine
// ============================================================================

// Install already-loaded ROM bytes into the active machine: identify, warn on
// incompatibility / size mismatch, remember the path (for SE/30 vrom
// discovery), copy into the ROM region, invalidate stale SoA/TLB entries, and
// reset the CPU from the ROM's reset vectors. `rom_data` is owned by the
// caller. Returns 0 on success, -1 if no machine is active.
static int install_rom_into_machine(const uint8_t *rom_data, size_t file_size, const char *path) {
    memory_map_t *mem = system_memory();
    if (!mem) {
        printf("rom.load: no machine — call machine.boot(model) first\n");
        return -1;
    }

    uint32_t checksum = 0;
    const rom_info_t *info = rom_identify_data(rom_data, file_size, &checksum);
    if (info)
        printf("ROM: %s (checksum %08X)\n", info->family_name, checksum);
    else
        printf("ROM: unknown (checksum %08X, size %zu bytes)\n", checksum, file_size);

    // Compatibility check against the active machine. Allow load with a
    // warning if the ROM is recognised but the active machine isn't in the
    // compatibility list — useful for swapping between Plus revisions, etc.
    const char *active = system_machine_model_id();
    if (info && active) {
        bool ok = false;
        for (const char *const *p = info->compatible; *p; p++) {
            if (strcmp(*p, active) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok)
            printf("Warning: this ROM is not listed as compatible with %s — loading anyway\n", active);
    }

    if (file_size != memory_rom_size(mem)) {
        printf("Warning: ROM file is %zu bytes, machine expects %u — truncating/padding to fit\n", file_size,
               memory_rom_size(mem));
    }

    // Track the pending path so SE/30 init (which chains through machine
    // reset) can find a sibling SE30.vrom file.
    rom_pending_set(path);

    memory_install_rom(mem, rom_data, file_size, path);

    // Invalidate any SoA / TLB entries that cached the old ROM bytes — without
    // this, an SE/30 reload after the MMU is set up keeps stale host pointers
    // until the next PFLUSH / _SwapMMUMode cycle.
    if (g_mmu)
        mmu_invalidate_tlb(g_mmu);

    // Reset CPU from the ROM reset vectors (SSP at offset 0, PC at offset 4).
    // Read directly from the ROM region — Plus has ROM at 0x400000, not
    // overlaid at address 0, so going through the address bus would miss.
    cpu_t *cpu = system_cpu();
    const uint8_t *rom_base = memory_rom_bytes(mem);
    if (cpu && rom_base && memory_rom_size(mem) >= 8) {
        uint32_t initial_ssp =
            ((uint32_t)rom_base[0] << 24) | ((uint32_t)rom_base[1] << 16) | ((uint32_t)rom_base[2] << 8) | rom_base[3];
        uint32_t initial_pc =
            ((uint32_t)rom_base[4] << 24) | ((uint32_t)rom_base[5] << 16) | ((uint32_t)rom_base[6] << 8) | rom_base[7];
        cpu_set_an(cpu, 7, initial_ssp);
        cpu_set_pc(cpu, initial_pc);
        printf("CPU reset: PC=%08X SSP=%08X\n", initial_pc, initial_ssp);
    }

    printf("ROM loaded successfully from %s\n", path);
    return 0;
}

int rom_load_into_machine(const char *path) {
    if (!path || !*path) {
        printf("rom.load: expected a path\n");
        return -1;
    }
    if (!system_memory()) {
        printf("rom.load: no machine — call machine.boot(model) first\n");
        return -1;
    }
    size_t file_size = 0;
    uint8_t *rom_data = read_rom_file(path, &file_size, false);
    if (!rom_data)
        return -1;
    int rc = install_rom_into_machine(rom_data, file_size, path);
    free(rom_data);
    return rc;
}

int rom_load_lisa_into_machine(const char *path_a, const char *path_b) {
    if (!path_a || !*path_a || !path_b || !*path_b) {
        printf("rom.load_lisa: expected two chip paths\n");
        return -1;
    }
    if (!system_memory()) {
        printf("rom.load_lisa: no machine — call machine.boot(model) first\n");
        return -1;
    }
    size_t size = 0;
    uint8_t *combined = rom_load_lisa_pair(path_a, path_b, &size);
    if (!combined)
        return -1;
    // Record the high-byte chip path so SE/30-style sibling discovery is inert
    // (Lisa has no vrom) but the pending-path bookkeeping stays populated.
    int rc = install_rom_into_machine(combined, size, path_a);
    free(combined);
    return rc;
}

// ============================================================================
// Object-model class descriptor
// ============================================================================

static value_t rom_attr_path(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const char *s = memory_rom_filename(system_memory());
    return val_str(s ? s : "");
}

static value_t rom_attr_loaded(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_bool(memory_rom_filename(system_memory()) != NULL);
}

// rom.checksum → 8 uppercase hex chars of the loaded ROM's stored checksum,
// or empty string when no ROM is loaded.  Surfaced as a string (not a number)
// because checksums are identifiers — never arithmetic — and the OPFS layout
// uses the canonical 8-hex form as the filename.
static value_t rom_attr_checksum(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    memory_map_t *mem = system_memory();
    if (!mem || !memory_rom_filename(mem))
        return val_str("");
    char hex[16];
    snprintf(hex, sizeof(hex), "%08X", memory_rom_checksum(mem));
    return val_str(hex);
}

static value_t rom_attr_size(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, memory_rom_size(system_memory()));
}

// rom.name → family name of the loaded ROM (e.g. "Universal IIx/IIcx/SE/30 ROM"),
// or empty string when no ROM is loaded or the checksum is unrecognised.
static value_t rom_attr_name(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    memory_map_t *mem = system_memory();
    if (!mem || !memory_rom_filename(mem))
        return val_str("");
    // Identify from ROM content rather than the computed checksum: Lisa/XL
    // ROMs aren't keyed by checksum (rom_identify_lisa uses size + version
    // word), and content-based ID gives the same answer for Mac ROMs.
    const rom_info_t *info = rom_identify_data(memory_rom_bytes(mem), memory_rom_size(mem), NULL);
    return val_str(info ? info->family_name : "");
}

static value_t rom_method_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    if (rom_load_into_machine(argv[0].s) != 0)
        return val_err("rom.load: failed");
    return val_bool(true);
}

// rom.load_lisa(chip_a, chip_b) — interleave two 8 KB Lisa/XL byte-slice chip
// files into the 16 KB boot ROM and load it into the active machine. The two
// chips may be given in either order (the loader detects the high/low byte
// orientation by checking for a valid Lisa signature).
static value_t rom_method_load_lisa(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    if (rom_load_lisa_into_machine(argv[0].s, argv[1].s) != 0)
        return val_err("rom.load_lisa: failed");
    return val_bool(true);
}

// rom.reload — load the ROM file the binary was launched with into
// the active machine.  Convenience for integration test scripts that
// `machine.boot` mid-stream to reset state and then need to re-stage
// the same ROM bytes; the script doesn't otherwise have access to the
// startup-time absolute path.  The pending path is set by the platform
// at startup (rom_pending_set) and never cleared, so reload is always
// a no-arg operation.  Errors if no startup ROM was specified or if
// no machine is currently active.
static value_t rom_method_reload(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    const char *path = rom_pending_path();
    if (!path || !*path)
        return val_err("rom.reload: no startup ROM path recorded; pass an explicit path to rom.load instead");
    if (rom_load_into_machine(path) != 0)
        return val_err("rom.reload: failed to reload '%s'", path);
    return val_bool(true);
}

// rom.identify(path) → JSON-encoded info map describing the ROM file:
//   { recognised, compatible, checksum, name, size }
// recognised==false implies compatible==[] and name=="" but the checksum and
// size are still populated from the file.  Returns V_ERROR if the path can
// not be opened (caller treats that as "no info, skip this entry").
static value_t rom_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    rom_file_info_t fi = {0};
    if (rom_probe_file(argv[0].s, &fi) != 0)
        return val_err("rom.identify: cannot read '%s'", argv[0].s);

    json_builder_t *b = json_builder_new();
    if (!b)
        return val_err("rom.identify: out of memory");

    json_open_obj(b);
    json_key(b, "recognised");
    json_bool(b, fi.info != NULL);
    json_key(b, "compatible");
    json_open_arr(b);
    if (fi.info && fi.info->compatible) {
        for (const char *const *p = fi.info->compatible; *p; p++)
            json_str(b, *p);
    }
    json_close_arr(b);
    char hex[16];
    snprintf(hex, sizeof(hex), "%08X", fi.checksum);
    json_key(b, "checksum");
    json_str(b, hex);
    json_key(b, "name");
    json_str(b, fi.info ? fi.info->family_name : "");
    json_key(b, "size");
    json_int(b, (int64_t)fi.size);
    json_close_obj(b);

    return json_finish(b);
}

static const arg_decl_t rom_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "ROM file path"},
};

static const arg_decl_t rom_lisa_pair_args[] = {
    {.name = "chip_a", .kind = V_STRING, .doc = "First Lisa/XL ROM chip file (8 KB)" },
    {.name = "chip_b", .kind = V_STRING, .doc = "Second Lisa/XL ROM chip file (8 KB)"},
};

static const member_t rom_members[] = {
    {.kind = M_ATTR,
     .name = "path",
     .doc = "Path of the currently loaded ROM (empty if none)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = rom_attr_path, .set = NULL}},
    {.kind = M_ATTR,
     .name = "loaded",
     .doc = "True if a ROM has been loaded into the active machine",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = rom_attr_loaded, .set = NULL}},
    {.kind = M_ATTR,
     .name = "checksum",
     .doc = "Stored checksum of the loaded ROM (8 uppercase hex chars, no prefix)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = rom_attr_checksum, .set = NULL}},
    {.kind = M_ATTR,
     .name = "size",
     .doc = "ROM region size in bytes",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = rom_attr_size, .set = NULL}},
    {.kind = M_ATTR,
     .name = "name",
     .doc = "Family name of the loaded ROM (e.g. \"Universal IIx/IIcx/SE/30 ROM\")",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = rom_attr_name, .set = NULL}},
    {.kind = M_METHOD,
     .name = "load",
     .doc = "Load ROM bytes into the active machine and reset the CPU",
     .method = {.args = rom_path_arg, .nargs = 1, .result = V_BOOL, .fn = rom_method_load}},
    {.kind = M_METHOD,
     .name = "load_lisa",
     .doc = "Interleave two Lisa/XL ROM chip files into the 16 KB boot ROM and load it",
     .method = {.args = rom_lisa_pair_args, .nargs = 2, .result = V_BOOL, .fn = rom_method_load_lisa}},
    {.kind = M_METHOD,
     .name = "reload",
     .doc = "Reload the startup-time ROM (path remembered from launch)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = rom_method_reload}},
    {.kind = M_METHOD,
     .name = "identify",
     .doc = "Return a JSON-encoded info map for a ROM file (compatible/checksum/name/size/recognised)",
     .method = {.args = rom_path_arg, .nargs = 1, .result = V_STRING, .fn = rom_method_identify}},
};

const class_desc_t rom_class = {
    .name = "rom",
    .members = rom_members,
    .n_members = sizeof(rom_members) / sizeof(rom_members[0]),
};

// ============================================================================
// Lifecycle
// ============================================================================
//
// ROM is a process-singleton: there is exactly one rom object node at any
// time, regardless of how many emulator instances come and go. Both calls
// are idempotent — system_create on cold boot is the canonical caller, but
// nothing breaks if the call is repeated (e.g. from a checkpoint reload
// that creates a fresh emulator before destroying the old one).

static struct object *s_rom_object = NULL;

void rom_init(void) {
    if (s_rom_object)
        return;
    s_rom_object = object_new(&rom_class, NULL, "rom");
    if (s_rom_object)
        object_attach(object_root(), s_rom_object);
}

void rom_delete(void) {
    if (s_rom_object) {
        object_detach(s_rom_object);
        object_delete(s_rom_object);
        s_rom_object = NULL;
    }
    rom_pending_clear();
}
