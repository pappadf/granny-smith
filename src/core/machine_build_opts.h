// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_build_opts.h
// Options that must be known BEFORE a device exists, delivered as an argument.

#ifndef MACHINE_BUILD_OPTS_H
#define MACHINE_BUILD_OPTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// A ROM image to build a machine with: its bytes, their size and the file
// they came from.  The bytes are borrowed: the caller owns them for the
// length of system_create, which copies them into the ROM region.
typedef struct rom_image {
    const uint8_t *data;
    size_t size;
    const char *path;
} rom_image_t;

// Some choices cannot be made by writing a device after the machine is built,
// because they decide what the device IS.  The monitor sense is the clearest
// case: at construction it fixes the display geometry and format, the
// generated declaration-ROM bytes for the generic card kinds, the seeded slot
// PRAM, and on the SE/30 which host regions get mapped.  Setting it afterwards
// is a card rebuild, not an attribute write.
//
// Such options are arguments of construction: machine_boot_apply resolves and
// validates the boot document into this struct, system_create keeps it as
// cfg->build_opts, and each device reads its part while it is built.  Nothing
// reaches a constructor through a global.

// One card option (a PCI card's vram=, memory=, raster=, monitor=...).
#define SLOT_OPTION_KEY_MAX   24
#define SLOT_OPTION_VALUE_MAX 48
typedef struct slot_option {
    char key[SLOT_OPTION_KEY_MAX];
    char value[SLOT_OPTION_VALUE_MAX];
} slot_option_t;

// What the document says about one slot of the machine's expansion bus
// (NuBus or PCI -- no machine has both): the card to seat, how to build it,
// and the ROM it carries.  A slot the document says nothing about has no
// entry and seats what the machine declares there.
#define SLOT_OPTIONS_MAX  4
#define SLOT_ROM_PATH_MAX 512
typedef struct slot_opts {
    int slot; // the slot number, as machine.nubus.slot[N] / machine.pci.slot[N]
    char card[32]; // card-kind id; "" = the slot's declared card
    bool empty; // the document leaves this socket empty
    char video_mode[40]; // "monitor_Nbpp" mode id; "" = the card's default
    char custom_mode[40]; // "WxHxD" custom geometry (generic cards); "" = none
    char rom[SLOT_ROM_PATH_MAX]; // declaration ROM / FCode PROM file; "" = the catalog
    // The monitor sense on the card's video connector, MACHINE_SENSE_UNSET
    // for the card's default.  Not a document field: the bus fills it from
    // the document's video_sense= on a boot, and its block carries it.
    int video_sense;
    // The monitor on this card is the one the machine shows (the document's
    // "connected" display device).  At most one entry, or the built-in video,
    // has it; with none set and no built-in video the machine has no screen.
    bool connected;
    int n_options;
    slot_option_t options[SLOT_OPTIONS_MAX];
} slot_opts_t;

// One entry per slot the document configures.
#define MACHINE_SLOTS_MAX 16

// One scalar machine option the document chose, by option id (memory travels
// as ram_kb).  A family reads its own with machine_build_opts_option().
#define MACHINE_OPTIONS_MAX   8
#define MACHINE_OPTION_ID_MAX 24
typedef struct machine_option_value {
    char id[MACHINE_OPTION_ID_MAX];
    char value[MACHINE_OPTION_ID_MAX];
} machine_option_value_t;

// One storage device the document configures: a drive of `type`
// (STORAGE_DEV_HD / STORAGE_DEV_CD, machine_config_decl.h) at `unit` on the
// bus the profile names `bus`.  CD-ROM drives are built with the bus; a hard
// disk is a position its image is attached to after the boot.
#define MACHINE_STORAGE_MAX     24
#define MACHINE_STORAGE_BUS_MAX 12
typedef struct machine_storage_dev {
    char bus[MACHINE_STORAGE_BUS_MAX];
    int unit;
    unsigned type;
} machine_storage_dev_t;

// The startup device the document names: the device the family's seeding
// step records in its parameter memory before the first instruction.
typedef struct machine_startup {
    bool none; // "no default -- let the ROM search"
    char bus[MACHINE_STORAGE_BUS_MAX]; // "" with !none: nothing to seed
    int unit;
    unsigned type;
} machine_startup_t;

typedef struct machine_build_opts {
    // Monitor sense code for the machine's video, or MACHINE_SENSE_UNSET when
    // the caller did not choose one and the board default applies.
    int video_sense;
    // RAM size in KB, already validated against the profile's ram_options and
    // defaulted by the caller; never 0 by the time system_create sees it.
    uint32_t ram_kb;
    // The ROM, read and validated by the caller (identified, compatible with
    // the model, the model's size).  memory_map_init creates the ROM region
    // filled, so every constructor after it sees the real ROM.  Empty on a
    // restore, which reads the ROM from the memory map's checkpoint block.
    rom_image_t rom;
    // The expansion-bus slots the document configures, validated against
    // the model (machine_boot_apply); the bus seats each from its entry.
    int n_slots;
    slot_opts_t slots[MACHINE_SLOTS_MAX];
    // The scalar options the document chose (appletalk, power_supplies, …),
    // every declared option present with its value.
    int n_options;
    machine_option_value_t options[MACHINE_OPTIONS_MAX];
    // How many floppy drives the machine is built with (its leading floppy
    // positions that hold a drive).  -1: the profile's own count.
    int n_floppies;
    // The storage devices, and whether the document gave them.  Without a
    // document (a restore) the buses carry their devices in their own blocks.
    bool storage_given;
    int n_storage;
    machine_storage_dev_t storage[MACHINE_STORAGE_MAX];
    machine_startup_t startup;
    // The built-in video's display is the connected one (no card is).
    bool builtin_connected;
} machine_build_opts_t;

// "No sense chosen" -- distinct from every legal 3-bit code, including 0.
#define MACHINE_SENSE_UNSET (-1)

// The monitor strap a video device is constructed with: `sense` (the build's
// video_sense, or a slot entry's) when it is a code the device can strap --
// one below `codes` -- else the device's default `dflt`.  The one place the
// rule lives; every video model's construction asks it.
static inline uint8_t machine_sense_or(int sense, unsigned codes, uint8_t dflt) {
    return (sense >= 0 && (unsigned)sense < codes) ? (uint8_t)sense : dflt;
}

// The build's sense as a slot entry carries it: a 3-bit code, or unset.  A
// Quadra's DAFB takes indexed codes past 7, which no card's connector straps.
static inline int machine_slot_sense(const machine_build_opts_t *o) {
    return (o->video_sense >= 0 && o->video_sense <= 7) ? o->video_sense : MACHINE_SENSE_UNSET;
}

// A build with nothing chosen: every field at its "caller said nothing" value.
static inline machine_build_opts_t machine_build_opts_default(void) {
    machine_build_opts_t o;
    o.video_sense = MACHINE_SENSE_UNSET;
    o.ram_kb = 0;
    o.rom = (rom_image_t){.data = NULL, .size = 0, .path = NULL};
    o.n_slots = 0;
    o.n_options = 0;
    o.n_floppies = -1;
    o.storage_given = false;
    o.n_storage = 0;
    o.startup = (machine_startup_t){.none = false, .bus = "", .unit = 0, .type = 0};
    o.builtin_connected = true;
    return o;
}

// The value the document chose for option `id`, or NULL.
static inline const char *machine_build_opts_option(const machine_build_opts_t *o, const char *id) {
    for (int i = 0; o && i < o->n_options; i++) {
        if (strcmp(o->options[i].id, id) == 0)
            return o->options[i].value;
    }
    return NULL;
}

// The entry for `slot`, or NULL when the document says nothing about it.
static inline const slot_opts_t *machine_build_opts_slot(const machine_build_opts_t *o, int slot) {
    for (int i = 0; o && i < o->n_slots; i++) {
        if (o->slots[i].slot == slot)
            return &o->slots[i];
    }
    return NULL;
}

// The value of option `key` in a slot entry, or NULL.
// Make an entry a checkpoint carried safe to read -- every string field
// terminated, the option count in range -- and say whether its fixed facts
// hold: the slot it names is `slot` (when it seats a card) and the monitor
// sense is a sense code.  The bus then checks what it seats as a boot would.
static inline bool slot_opts_sanitize(slot_opts_t *e, int slot) {
    e->card[sizeof e->card - 1] = '\0';
    e->video_mode[sizeof e->video_mode - 1] = '\0';
    e->custom_mode[sizeof e->custom_mode - 1] = '\0';
    e->rom[sizeof e->rom - 1] = '\0';
    if (e->n_options < 0 || e->n_options > SLOT_OPTIONS_MAX)
        return false;
    for (int i = 0; i < e->n_options; i++) {
        e->options[i].key[sizeof e->options[i].key - 1] = '\0';
        e->options[i].value[sizeof e->options[i].value - 1] = '\0';
    }
    if (e->video_sense != MACHINE_SENSE_UNSET && (e->video_sense < 0 || e->video_sense > 7))
        return false;
    return !e->card[0] || e->slot == slot;
}

static inline const char *slot_opts_option(const slot_opts_t *e, const char *key) {
    for (int i = 0; e && i < e->n_options; i++) {
        if (strcmp(e->options[i].key, key) == 0)
            return e->options[i].value;
    }
    return NULL;
}

#endif // MACHINE_BUILD_OPTS_H
