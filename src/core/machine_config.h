// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_config.h
// Construction-time boot document and the per-machine "built-from" record
// (proposal-named-args-boot-config §4).
//
// The boot document carries every construction-time setting of one
// machine.boot call; the record is the live machine's immutable birth
// certificate — written by boot (plus the rom.load write-back), read by
// the read-only machine.config object, inherited by argument-less
// reboots, and serialized into checkpoints.

#ifndef GS_MACHINE_CONFIG_H
#define GS_MACHINE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

struct object;

// Path capacity for record entries. Checkpoint serialization writes the
// struct verbatim, so these are fixed-size fields, not pointers.
#define MC_PATH_MAX 512
#define MC_ID_MAX   40

// One resolved declaration-ROM pick, reported by the card loader during
// machine construction (declrom_load_vrom_card).
typedef struct {
    char card_id[MC_ID_MAX];
    char path[MC_PATH_MAX];
    uint32_t crc; // Format-Block CRC (content identity)
    bool explicit_pick; // true when the vrom= / vrom.load explicit pick won
} machine_config_vrom_t;

#define MC_MAX_VROMS 8

// The built-from record. POD by design — checkpoints store it verbatim
// (build-ID-gated, so layout changes are safe across builds).
typedef struct machine_config_record {
    bool valid; // a machine built through the boot document exists
    char model[MC_ID_MAX];
    uint32_t ram_kb;
    char rom[MC_PATH_MAX];
    uint32_t rom_crc;
    char rom2[MC_PATH_MAX]; // Lisa second chip ("" = single-file ROM)
    char vrom[MC_PATH_MAX]; // explicit vrom= pick ("" = auto-resolve)
    char video_card[MC_ID_MAX]; // wildcard-socket card id ("" = slot default)
    int32_t video_sense; // -1 = unset
    char video_mode[MC_ID_MAX]; // wildcard video-mode id ("" = card default)
    char custom_mode[MC_ID_MAX]; // "WxHxD" custom resolution ("" = none)
    char created[24]; // ISO8601 UTC, stamped by boot
    machine_config_vrom_t vroms[MC_MAX_VROMS]; // resolved picks, in load order
    int32_t n_vroms;
} machine_config_record_t;

// The in-flight boot document: pointers borrow from the caller; NULL/0/-1
// mean "not given" (inheritance fills them from the record).
typedef struct boot_config {
    const char *model;
    uint32_t ram_kb; // 0 = inherit / profile default
    const char *rom;
    const char *rom2;
    const char *vrom;
    const char *video_card;
    int video_sense; // -1 = not given
    const char *video_mode;
    const char *custom_mode; // "WxHxD" custom resolution (NULL = none)
} boot_config_t;

// Read-only view of the live record (never NULL; check ->valid).
const machine_config_record_t *machine_config_record(void);

// Mutable access for boot / checkpoint restore (machine.c / system.c).
machine_config_record_t *machine_config_record_mut(void);

// Clear the resolved-vROM list ahead of machine construction so the
// loader's reports rebuild it for the new machine.
void machine_config_reset_vroms(void);

// Report one resolved declaration-ROM pick (called by the card loader
// while the machine is being constructed).
void machine_config_note_vrom(const char *card_id, const char *path, uint32_t crc, bool explicit_pick);

// rom.load write-back: keep the record answering "how do I recreate
// what I'm looking at" after a live ROM swap.
void machine_config_note_rom(const char *path, uint32_t crc);

// Attach the read-only `machine.config` child object (idempotent).
void machine_config_object_init(struct object *machine_obj);

// Apply one boot document: inherit → validate → tear down → construct →
// record (defined in machines/machine.c; shared by machine.boot and
// headless startup).  Returns V_NONE on success; V_ERROR — with the old
// machine still running — on rejection.
value_t machine_boot_apply(const boot_config_t *doc);

#ifdef __cplusplus
}
#endif

#endif // GS_MACHINE_CONFIG_H
