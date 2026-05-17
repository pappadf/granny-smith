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
#include "object.h"
#include "system.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Compatible-model lists
// ============================================================================

static const char *const PLUS_COMPATIBLE[] = {"plus", NULL};
// Universal ROM: same bytes for IIx, IIcx, SE/30. Listed in machine_register
// order so the *first* entry serves as a UI default (for tests that don't pick
// explicitly), but the API never auto-selects.
static const char *const UNIVERSAL_COMPATIBLE[] = {"se30", "iicx", "iix", NULL};
static const char *const IIFX_COMPATIBLE[] = {"iifx", NULL};

// Master ROM signature table.
static const rom_info_t ROM_TABLE[] = {
    {"Macintosh Plus (Rev 1, Lonely Hearts)",   PLUS_COMPATIBLE,      0x4D1EEEE1, 128 * 1024},
    {"Macintosh Plus (Rev 2, Lonely Heifers)",  PLUS_COMPATIBLE,      0x4D1EEAE1, 128 * 1024},
    {"Macintosh Plus (Rev 3, Loud Harmonicas)", PLUS_COMPATIBLE,      0x4D1F8172, 128 * 1024},
    {"Universal IIx/IIcx/SE/30 ROM",            UNIVERSAL_COMPATIBLE, 0x97221136, 256 * 1024},
    {"Macintosh IIfx ROM",                      IIFX_COMPATIBLE,      0x4147DD77, 512 * 1024},
};

#define ROM_TABLE_COUNT (sizeof(ROM_TABLE) / sizeof(ROM_TABLE[0]))

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

uint32_t rom_compute_checksum(const uint8_t *data, size_t size) {
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
    FILE *f = fopen(filename, "rb");
    if (!f) {
        if (!quiet)
            printf("Failed to open ROM file: %s\n", filename);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(f);
        if (!quiet)
            printf("Failed to read ROM file: %s\n", filename);
        return NULL;
    }
    uint8_t *rom_data = malloc((size_t)file_size);
    if (!rom_data) {
        fclose(f);
        if (!quiet)
            printf("Failed to allocate memory for ROM\n");
        return NULL;
    }
    size_t n = fread(rom_data, 1, (size_t)file_size, f);
    fclose(f);
    if ((long)n != file_size) {
        free(rom_data);
        if (!quiet)
            printf("Failed to read ROM file: %s\n", filename);
        return NULL;
    }
    *out_size = (size_t)file_size;
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

int rom_load_into_machine(const char *path) {
    if (!path || !*path) {
        printf("rom.load: expected a path\n");
        return -1;
    }

    memory_map_t *mem = system_memory();
    if (!mem) {
        printf("rom.load: no machine — call machine.boot(model) first\n");
        return -1;
    }

    size_t file_size = 0;
    uint8_t *rom_data = read_rom_file(path, &file_size, false);
    if (!rom_data)
        return -1;

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
    free(rom_data);

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
    const rom_info_t *info = rom_identify(memory_rom_checksum(mem));
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
