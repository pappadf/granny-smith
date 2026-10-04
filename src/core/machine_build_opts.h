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
    int n_options;
    slot_option_t options[SLOT_OPTIONS_MAX];
} slot_opts_t;

// One entry per slot the document configures.
#define MACHINE_SLOTS_MAX 16

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
} machine_build_opts_t;

// "No sense chosen" -- distinct from every legal 3-bit code, including 0.
#define MACHINE_SENSE_UNSET (-1)

// A build with nothing chosen: every field at its "caller said nothing" value.
static inline machine_build_opts_t machine_build_opts_default(void) {
    machine_build_opts_t o;
    o.video_sense = MACHINE_SENSE_UNSET;
    o.ram_kb = 0;
    o.rom = (rom_image_t){.data = NULL, .size = 0, .path = NULL};
    o.n_slots = 0;
    return o;
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
static inline const char *slot_opts_option(const slot_opts_t *e, const char *key) {
    for (int i = 0; e && i < e->n_options; i++) {
        if (strcmp(e->options[i].key, key) == 0)
            return e->options[i].value;
    }
    return NULL;
}

#endif // MACHINE_BUILD_OPTS_H
