// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_config.h
// Construction-time boot document and the per-machine "built-from" record.
//
// The boot document carries every construction-time setting of one
// machine.boot call; the record is the live machine's immutable birth
// certificate — written by boot, read by the read-only machine.config object,
// and serialized into checkpoints, whose restore builds the machine from it.

#ifndef GS_MACHINE_CONFIG_H
#define GS_MACHINE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "machine_build_opts.h"
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
} machine_config_vrom_t;

#define MC_MAX_VROMS 8

// Which expansion bus a resolved slot pick belongs to.  The two bus
// registries are separate by construction (a NuBus card can never appear
// on a PCI socket), so the record has to say which one a slot number means.
typedef enum mc_bus_kind {
    MC_BUS_NUBUS = 0,
    MC_BUS_PCI = 1,
} mc_bus_kind_t;

// One seated card, reported by a bus controller during machine construction.
typedef struct {
    uint8_t bus_kind; // mc_bus_kind_t
    int16_t slot; // slot number within that bus's numbering
    char card_id[MC_ID_MAX];
} machine_config_slot_card_t;

#define MC_MAX_SLOT_CARDS 12

// The built-from record. POD by design — checkpoints store it verbatim
// (build-ID-gated, so layout changes are safe across builds).
typedef struct machine_config_record {
    bool valid; // a machine built through the boot document exists
    char model[MC_ID_MAX];
    uint32_t ram_kb;
    char rom[MC_PATH_MAX];
    char rom_id[32]; // content id of the installed ROM (rom.h ROM_ID_MAX)
    char rom2[MC_PATH_MAX]; // Lisa second chip ("" = single-file ROM)
    char video_card[MC_ID_MAX]; // wildcard-socket card id ("" = slot default)
    int32_t video_sense; // -1 = unset
    char video_mode[MC_ID_MAX]; // wildcard video-mode id ("" = card default)
    char custom_mode[MC_ID_MAX]; // "WxHxD" custom resolution ("" = none)
    char monitor[MC_ID_MAX]; // built-in monitor strap ("" = machine default)
    char pci_card[MC_ID_MAX]; // wildcard PCI-socket card id ("" = slot default)
    char pci_option[MC_PATH_MAX]; // "key=value[,key=value]" for that card
    char created[24]; // ISO8601 UTC, stamped by boot
    machine_config_vrom_t vroms[MC_MAX_VROMS]; // resolved picks, in load order
    int32_t n_vroms;
    // The cards the slot walks seated, across both expansion buses.
    machine_config_slot_card_t slot_cards[MC_MAX_SLOT_CARDS];
    int32_t n_slot_cards;
    // The expansion-slot entries the machine was built with (the boot
    // document's slots resolved by machine_slots_resolve): a checkpoint
    // restore builds the slots from these.
    int32_t n_slots;
    slot_opts_t slots[MACHINE_SLOTS_MAX];
} machine_config_record_t;

// The in-flight boot document: pointers borrow from the caller; NULL/0/-1
// mean "not given" (the model's own defaults fill them; model and rom are
// required).
typedef struct boot_config {
    const char *model;
    uint32_t ram_kb; // 0 = the profile's ram_default
    const char *rom;
    const char *rom2;
    const char *vrom;
    const char *video_card;
    int video_sense; // -1 = not given
    const char *video_mode;
    const char *custom_mode; // "WxHxD" custom resolution (NULL = none)
    // Which monitor is strapped to the machine's BUILT-IN video port.
    // "none" leaves it unconnected, which switches built-in video off and
    // hands the screen to a NuBus card (machine_profile_t.builtin_video).
    const char *monitor; // NULL = machine default
    // Card id for the machine's FIRST PCI socket, mirroring video_card= for
    // NuBus.  Other slots are configured through slots=.
    const char *pci_card;
    // PCI expansion-ROM file, the sibling of vrom= for FCode cards: the ROM
    // of every slot whose card it provides.  NULL resolves from the offered
    // .prom files.
    const char *prom;
    // Options for that same card, as "key=value" pairs separated by commas
    // ("vram=4m", "vram=4m,monitor=15in_multi").  Which keys mean anything
    // is the CARD's business: the kind's accepts_option() hook accepts or
    // rejects each one before the boot begins, so no card identity leaks
    // into the boot path.
    const char *pci_option;
    // Per-slot configuration: "SLOT=CARD[,key=value]*;..." (machine_slots.c).
    // The sugar above (video_card=, video_mode=, custom_mode=, pci_card=,
    // pci_option=, vrom=, prom=) resolves into the same per-slot entries.
    const char *slots;
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
void machine_config_note_vrom(const char *card_id, const char *path, uint32_t crc);

// Clear / report the seated cards.  A bus controller calls the reporter once
// per slot it actually populates.
void machine_config_reset_slot_cards(void);
void machine_config_note_slot_card(int bus_kind, int slot, const char *card_id);

// Attach the read-only `machine.config` child object (idempotent).
void machine_config_object_init(struct object *machine_obj);

// Apply one boot document: validate → tear down → construct → record
// (defined in machines/machine.c; shared by machine.boot
// and headless startup).  Returns V_NONE on success; V_ERROR — with the
// old machine still running — on rejection.
value_t machine_boot_apply(const boot_config_t *doc);

#ifdef __cplusplus
}
#endif

#endif // GS_MACHINE_CONFIG_H
