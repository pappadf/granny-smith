// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rom.c
// ROM content identity, file I/O, and the rom.* object-model surface.  The
// table of known ROMs lives in rom_table.c.
//
// The ROM is a construction argument: machine.boot reads and validates the
// file and the machine is built with it (machine_build_opts_t.rom); a running
// machine's ROM is never swapped.  Multiple machines can share the same ROM
// image (the SE/30, IIcx, and IIx all run the universal ROM), so
// rom.identify(path) returns the *list* of compatible models — the user is the
// source of truth.

#include "rom.h"
#include "gs_out.h"
#include "source.h"

#include "machine_profile.h"
#include "memory.h"
#include "object.h"
#include "system.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ============================================================================
// Content identity
// ============================================================================

#define LISA_ROM_SIZE      (16 * 1024) // interleaved image size
#define LISA_RESET_SSP     0x00000480u // first longword of every Lisa ROM
#define LISA_CHECK_WORD    0x3FFE // ROMTST's LAST: the word that zeroes the sum
#define PPC_ROM_SIZE       (4u * 1024 * 1024) // Old World image size
#define PPC_68K_HALF       0x300000u // end of the span the header sum covers
#define PPC_CONFIGINFO_PTR 0x300080u // BE32 offset of ConfigInfo from 0x300000
#define PPC_CONFIGINFO_LEN 0x28u // eight lane sums + the 64-bit sum
#define PPC_SUM64_OFFSET   0x20u // ROMCheckSum64 within ConfigInfo

// Big-endian 16-bit read.
static uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// Big-endian 32-bit read.
static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Big-endian 64-bit read.
static uint64_t rd_be64(const uint8_t *p) {
    return ((uint64_t)rd_be32(p) << 32) | rd_be32(p + 4);
}

// The classic Mac ROM sum: big-endian 16-bit words over [from, to), mod 2^32.
static uint32_t word_sum(const uint8_t *data, size_t from, size_t to) {
    uint32_t sum = 0;
    for (size_t i = from; i + 1 < to; i += 2)
        sum += rd_be16(data + i); // one word into the running sum
    return sum;
}

// Absolute offset of a 4 MiB image's ConfigInfo block, or 0 when the pointer
// at 0x300080 does not land on a doubleword inside the PowerPC megabyte.
static size_t ppc_configinfo(const uint8_t *data) {
    uint32_t off = rd_be32(data + PPC_CONFIGINFO_PTR);
    if (off % 8 != 0 || (uint64_t)off + PPC_CONFIGINFO_LEN > PPC_ROM_SIZE - PPC_68K_HALF)
        return 0;
    return PPC_68K_HALF + off;
}

// Apple's 64-bit sum: big-endian doublewords over the whole image except the
// 40-byte ConfigInfo record that stores it.
static uint64_t ppc_sum64(const uint8_t *data, size_t ci) {
    uint64_t sum = 0;
    for (size_t i = 0; i < PPC_ROM_SIZE; i += 8) {
        if (i >= ci && i < ci + PPC_CONFIGINFO_LEN)
            continue; // the record itself is not summed
        sum += rd_be64(data + i);
    }
    return sum;
}

// Byte-lane sums over the same range, for naming the chip of a bad dump.
static void ppc_lane_sums(const uint8_t *data, size_t ci, uint32_t lanes[8]) {
    memset(lanes, 0, 8 * sizeof(uint32_t));
    for (size_t i = 0; i < PPC_ROM_SIZE; i++) {
        if (i >= ci && i < ci + PPC_CONFIGINFO_LEN)
            continue;
        lanes[i & 7] += data[i]; // lane = byte position within the doubleword
    }
}

// The Lisa boot ROM's own test (ROMTST): add each word and rotate left one
// bit, then add the check word; an intact ROM leaves zero.
static uint16_t lisa_rotating_sum(const uint8_t *data) {
    uint16_t sum = 0;
    for (size_t i = 0; i < LISA_CHECK_WORD; i += 2) {
        sum = (uint16_t)(sum + rd_be16(data + i)); // ADD (A0)+,D0
        sum = (uint16_t)((sum << 1) | (sum >> 15)); // ROL #1,D0
    }
    return (uint16_t)(sum + rd_be16(data + LISA_CHECK_WORD));
}

// Identity of `data` under the per-kind rule, honouring a matched row's
// exceptions when `row` is given.
static void identity_eval(const uint8_t *data, size_t size, const rom_info_t *row, rom_identity_t *out) {
    memset(out, 0, sizeof(*out));
    if (!data || size < 8 || size % 2 != 0) {
        out->kind = ROM_KIND_NONE;
        snprintf(out->reason, sizeof(out->reason), "not a ROM image");
        return;
    }

    // Lisa / Macintosh XL: 16 KB starting with the reset SSP.
    if (size == LISA_ROM_SIZE && rd_be32(data) == LISA_RESET_SSP) {
        out->kind = ROM_KIND_LISA;
        snprintf(out->id, sizeof(out->id), "%04x", rd_be16(data + LISA_CHECK_WORD));
        out->intact = lisa_rotating_sum(data) == 0;
        if (!out->intact)
            snprintf(out->reason, sizeof(out->reason), "checksum does not verify (check the chip pair)");
        return;
    }

    uint32_t stored = rd_be32(data); // the header sum every Mac ROM starts with

    // Old World PowerPC: 4 MiB with a ConfigInfo record in the last megabyte.
    if (size == PPC_ROM_SIZE) {
        out->kind = ROM_KIND_PPC;
        size_t ci = ppc_configinfo(data);
        if (!ci) {
            snprintf(out->id, sizeof(out->id), "%08x", stored);
            snprintf(out->reason, sizeof(out->reason), "no ConfigInfo");
            return;
        }
        uint64_t stored64 = rd_be64(data + ci + PPC_SUM64_OFFSET);
        snprintf(out->id, sizeof(out->id), "%08x-%016llx", stored, (unsigned long long)stored64);
        bool ok32 = word_sum(data, 4, PPC_68K_HALF) == stored; // the 68k half
        bool ok64 = (row && (row->flags & ROM_F_NO_SUM64)) || ppc_sum64(data, ci) == stored64;
        out->intact = ok32 && ok64;
        if (!ok32) {
            snprintf(out->reason, sizeof(out->reason), "68k section does not verify");
        } else if (!ok64) {
            // Name the byte lanes (chips) whose recomputed sum differs.
            uint32_t lanes[8];
            ppc_lane_sums(data, ci, lanes);
            char list[24] = "";
            size_t n = 0;
            for (int i = 0; i < 8; i++) {
                if (lanes[i] != rd_be32(data + ci + 4 * i))
                    n += (size_t)snprintf(list + n, sizeof(list) - n, "%s%d", n ? ", " : "", i);
            }
            if (n)
                snprintf(out->reason, sizeof(out->reason), "PowerPC section does not verify (byte lanes %s)", list);
            else
                snprintf(out->reason, sizeof(out->reason), "PowerPC section does not verify");
        }
        return;
    }

    // Mac 68k: the header sum over the whole image, or a row's shorter span.
    out->kind = ROM_KIND_MAC68K;
    snprintf(out->id, sizeof(out->id), "%08x", stored);
    size_t span = (row && row->checksum_span && row->checksum_span < size) ? row->checksum_span : size;
    out->intact = word_sum(data, 4, span) == stored;
    if (!out->intact)
        snprintf(out->reason, sizeof(out->reason), "checksum does not verify");
}

void rom_identity_compute(const uint8_t *data, size_t size, rom_identity_t *out) {
    identity_eval(data, size, NULL, out);
}

const rom_info_t *rom_lookup(const char *id, size_t size) {
    if (!id || !*id)
        return NULL;
    for (size_t i = 0; i < rom_table_count; i++) {
        if (rom_table[i].rom_size == size && strcmp(rom_table[i].id, id) == 0)
            return &rom_table[i];
    }
    return NULL;
}

const rom_info_t *rom_identify_data(const uint8_t *data, size_t size, rom_identity_t *out) {
    rom_identity_t local;
    rom_identity_t *id = out ? out : &local;
    identity_eval(data, size, NULL, id);
    const rom_info_t *info = rom_lookup(id->id, size);
    // A known ROM whose own checksum covers less than the plain rule assumes
    // is re-verified under its row's exception.
    if (info && (info->checksum_span || info->flags))
        identity_eval(data, size, info, id);
    return info;
}

bool rom_is_supported(const rom_info_t *info) {
    return info && info->compatible && info->compatible[0];
}

const char *rom_kind_name(rom_kind_t kind) {
    switch (kind) {
    case ROM_KIND_MAC68K:
        return "mac68k";
    case ROM_KIND_PPC:
        return "ppc";
    case ROM_KIND_LISA:
        return "lisa";
    default:
        return "";
    }
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

// Largest file read as a ROM: far above any Mac ROM (4 MiB), well below
// anything that would strain memory.
#define ROM_FILE_MAX (64u * 1024u * 1024u)

// Read an entire ROM file into a fresh buffer. Caller frees on success.  The
// path goes through the VFS, so a ROM may be a member of an archive or a
// file inside a disk image (roms.zip/Plus.rom).
uint8_t *rom_read_file(const char *filename, size_t *out_size, bool quiet) {
    uint8_t *rom_data = NULL;
    size_t file_size = 0;
    int rc = gs_read_path(filename, ROM_FILE_MAX, &rom_data, &file_size);
    if (rc != 0 || file_size == 0) {
        free(rom_data);
        if (!quiet)
            gs_outf("Failed to read ROM file: %s\n", filename);
        return NULL;
    }
    *out_size = file_size;
    return rom_data;
}

int rom_probe_file(const char *path, rom_file_info_t *out) {
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!path || !*path)
        return -1;
    size_t file_size = 0;
    uint8_t *data = rom_read_file(path, &file_size, true);
    if (!data)
        return -1;
    out->info = rom_identify_data(data, file_size, &out->identity);
    free(data);
    out->size = file_size;
    return 0;
}

// ============================================================================
// Lisa / Macintosh XL two-chip ROM interleaving
// ============================================================================

// Interleave two 8 KB byte-slice chips into a 16-bit-wide ROM image:
// even bytes ← high-byte chip (D8–D15), odd bytes ← low-byte chip (D0–D7).
// `out` must hold 2 * min(hi_size, lo_size) bytes. (docs/reference/machines/lisa/lisa.md §16)
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
    uint8_t *a = rom_read_file(path_a, &a_size, false);
    if (!a)
        return NULL;
    uint8_t *b = rom_read_file(path_b, &b_size, false);
    if (!b) {
        free(a);
        return NULL;
    }

    uint8_t *combined = NULL;
    // Each chip must be exactly half the 16 KB image.
    if (a_size == LISA_ROM_SIZE / 2 && b_size == LISA_ROM_SIZE / 2) {
        combined = malloc(LISA_ROM_SIZE);
        if (combined) {
            // Try a=HI/b=LO; if that orientation fails the boot ROM's own
            // self-check, reinterleave with the chips swapped (the check
            // covers both chips, so a swapped pair never passes it).
            rom_identity_t id;
            rom_interleave_pair(a, a_size, b, b_size, combined);
            rom_identity_compute(combined, LISA_ROM_SIZE, &id);
            if (id.kind != ROM_KIND_LISA || !id.intact) {
                rom_interleave_pair(b, b_size, a, a_size, combined);
                rom_identity_compute(combined, LISA_ROM_SIZE, &id);
                // Neither orientation verifies: keep the order as given.
                if (id.kind != ROM_KIND_LISA || !id.intact)
                    rom_interleave_pair(a, a_size, b, b_size, combined);
            }
        }
    } else {
        gs_outf("Lisa ROM: each chip must be %d bytes (got %zu and %zu)\n", LISA_ROM_SIZE / 2, a_size, b_size);
    }

    free(a);
    free(b);
    if (!combined)
        return NULL;
    rom_identity_t id;
    rom_identity_compute(combined, LISA_ROM_SIZE, &id);
    if (id.kind != ROM_KIND_LISA || !id.intact)
        gs_outf("Warning: interleaved image does not pass the Lisa/XL boot ROM self-check\n");
    *out_size = LISA_ROM_SIZE;
    return combined;
}

// ============================================================================
// Object-model class descriptor
// ============================================================================

static DEF_GETTER(rom_attr_path) {
    const char *s = memory_rom_filename(system_memory());
    return val_str(s ? s : "");
}

static DEF_GETTER(rom_attr_loaded) {
    return val_bool(memory_rom_filename(system_memory()) != NULL);
}

// Identity of the loaded ROM's bytes; false when no ROM is loaded.
static bool loaded_rom_identity(rom_identity_t *id) {
    memory_map_t *mem = system_memory();
    if (!mem || !memory_rom_filename(mem))
        return false;
    rom_identify_data(memory_rom_bytes(mem), memory_rom_size(mem), id);
    return true;
}

// rom.id → the loaded ROM's content id (rom.h), or "" when none is loaded.
// Answered from the same identification pass as rom.identify, so it always
// equals the id the ROM file identified as.
static DEF_GETTER(rom_attr_id) {
    rom_identity_t id;
    return val_str(loaded_rom_identity(&id) ? id.id : "");
}

// rom.intact → the loaded ROM's own checksum verifies.
static DEF_GETTER(rom_attr_intact) {
    rom_identity_t id;
    return val_bool(loaded_rom_identity(&id) && id.intact);
}

static DEF_GETTER(rom_attr_size) {
    return val_uint(4, memory_rom_size(system_memory()));
}

// rom.name → family name of the loaded ROM (e.g. "Universal IIx/IIcx/SE/30 ROM"),
// or empty string when no ROM is loaded or the ROM is not known.
static DEF_GETTER(rom_attr_name) {
    memory_map_t *mem = system_memory();
    if (!mem || !memory_rom_filename(mem))
        return val_str("");
    // Identify from ROM content, the same pass rom.identify runs on a file.
    const rom_info_t *info = rom_identify_data(memory_rom_bytes(mem), memory_rom_size(mem), NULL);
    return val_str(info ? info->family_name : "");
}

// rom.identify(path) → typed info map describing the ROM file:
//   { recognised, supported, compatible, name, variant, size, kind, id, intact,
//     reason }
// recognised: the id is in the ROM table; supported: its row names at least
// one emulated model (compatible).  An unrecognised file still reports its
// kind, id, intact and reason.  id is the only field anything names a file by,
// and only when intact.  Returns V_ERROR if the path can not be opened (caller
// treats that as "no info, skip this entry").
static DEF_METHOD(rom_method_identify) {
    rom_file_info_t fi = {0};
    if (rom_probe_file(argv[0].s, &fi) != 0)
        return val_err("rom.identify: cannot read '%s'", argv[0].s);

    value_map_builder_t *b = val_map_new();
    val_map_put(b, "recognised", val_bool(fi.info != NULL));
    val_map_put(b, "supported", val_bool(rom_is_supported(fi.info)));
    value_t *compat = NULL;
    size_t n_compat = 0, cap_compat = 0;
    if (fi.info && fi.info->compatible) {
        for (const char *const *p = fi.info->compatible; *p; p++)
            val_list_push(&compat, &n_compat, &cap_compat, val_str(*p));
    }
    val_map_put(b, "compatible", val_list(compat, n_compat));
    val_map_put(b, "name", val_str(fi.info ? fi.info->family_name : ""));
    // What tells this ROM apart from the other ROMs of its models; "" when
    // no other known ROM boots any of them.
    val_map_put(b, "variant", val_str(fi.info && fi.info->variant ? fi.info->variant : ""));
    val_map_put(b, "size", val_int((int64_t)fi.size));
    val_map_put(b, "kind", val_str(rom_kind_name(fi.identity.kind)));
    val_map_put(b, "id", val_str(fi.identity.id));
    val_map_put(b, "intact", val_bool(fi.identity.intact));
    val_map_put(b, "reason", val_str(fi.identity.reason));
    return val_map_finish(b);
}

static const arg_decl_t rom_path_arg[] = {
    {.name = "path", .kind = V_STRING, .presentation_flags = VAL_PATH, .doc = "ROM file path"},
};

static const member_t rom_members[] = {
    {.kind = M_ATTR,
     .name = "path",
     .doc = "Path of the currently loaded ROM (empty if none)",
     .attr = {.type = V_STRING, .get = rom_attr_path, .set = NULL}                                                                                                                  },
    {.kind = M_ATTR,
     .name = "loaded",
     .doc = "True if a ROM has been loaded into the active machine",
     .attr = {.type = V_BOOL, .get = rom_attr_loaded, .set = NULL}                                                                                                                  },
    {.kind = M_ATTR,
     .name = "id",
     .doc = "Content id of the loaded ROM (its own stored checksum fields, lowercase hex)",
     .attr = {.type = V_STRING, .get = rom_attr_id, .set = NULL}                                                                                                                    },
    {.kind = M_ATTR,
     .name = "intact",
     .doc = "True if the loaded ROM's own checksum verifies",
     .attr = {.type = V_BOOL, .get = rom_attr_intact, .set = NULL}                                                                                                                  },
    {.kind = M_ATTR,
     .name = "size",
     .doc = "ROM region size in bytes",
     .attr = {.type = V_UINT, .get = rom_attr_size, .set = NULL}                                                                                                                    },
    {.kind = M_ATTR,
     .name = "name",
     .doc = "Family name of the loaded ROM (e.g. \"Universal IIx/IIcx/SE/30 ROM\")",
     .attr = {.type = V_STRING, .get = rom_attr_name, .set = NULL}                                                                                                                  },
    {.kind = M_METHOD,
     .name = "identify",
     .doc = "Return a typed info map for a ROM file "
            "(recognised/supported/compatible/name/variant/size/kind/id/intact/reason)",    .method = {.args = rom_path_arg, .nargs = 1, .result = V_MAP, .fn = rom_method_identify}},
};

static const class_desc_t rom_class = {
    .name = "rom",
    .doc = "The machine ROM: identity and integrity",
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
    if (s_rom_object) {
        object_set_label(s_rom_object, "ROM");
        object_set_order(s_rom_object, 30);
        object_attach(machine_object(), s_rom_object);
    }
}

void rom_delete(void) {
    if (s_rom_object) {
        object_detach(s_rom_object);
        object_delete(s_rom_object);
        s_rom_object = NULL;
    }
}
