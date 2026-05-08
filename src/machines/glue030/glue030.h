// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// glue030.h
// Family-shared init/teardown/checkpoint scaffolding for the "GLUE + 68030 +
// Universal $0178 ROM" family — SE/30, IIx, IIcx (and a future Mac II port,
// modulo the original Mac II's external 68851 PMMU and ROM $0075).
//
// Step-2 status (per proposal-machine-iicx-iix.md §4 step 2): the directory
// and the declarative `glue030_init_t` shape exist, plus the image-list
// checkpoint loop that today appears verbatim in se30.c lands here as the
// first concrete extraction.  Subsequent steps move the I/O dispatcher,
// memory map, and full lifecycle into glue030_*; for now, individual machines
// (se30.c) reach into this header for the helpers that have already moved.

#ifndef GLUE030_H
#define GLUE030_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct config;
struct checkpoint;
typedef struct config config_t;
typedef struct checkpoint checkpoint_t;

// Forward declaration for the NuBus slot table; full type in nubus.h.  Held
// here so glue030_init_t can reference a slot table without pulling the NuBus
// subsystem into every machine that doesn't yet use it.
struct nubus_slot_decl;
typedef struct nubus_slot_decl nubus_slot_decl_t;

// Forward declaration for the I/O device map; full type in glue030_io.h once
// the I/O dispatcher extraction lands.  NULL for now means "use the family
// default".
struct glue030_io_map;

// Per-machine extra-decode hooks.  Family code routes VIA1 / VIA2 port
// outputs through its own decoder first; bits the family doesn't claim fall
// through to these callbacks so the IIcx can implement v2PowerOff and the
// SE/30 can implement vMystery-PB6 VBL-enable while sharing the rest.
typedef void (*glue030_via_extra_fn)(config_t *cfg, uint8_t output);

// Declarative init spec for a glue030-family machine.  The machine fills
// in the struct, then calls glue030_init.  Step-2 surface is intentionally
// thin — fields ratchet in as the family-shared init grows.
typedef struct glue030_init {
    config_t *cfg;
    checkpoint_t *checkpoint; // NULL on cold boot

    int cpu_model; // CPU_MODEL_68030 for v1
    uint32_t cpu_freq; // Hz; mirrors profile.freq
    uint32_t via_freq_factor; // typically 20 on the glue030 family

    const struct glue030_io_map *io_map; // NULL → family default

    // Memory map sizes — usually derived from the profile but kept explicit
    // so non-default machines can adjust without touching family code.
    uint32_t ram_end; // typically 0x40000000
    uint32_t rom_start; // typically 0x40000000
    uint32_t rom_end; // typically 0x50000000

    // Machine-ID inputs driven into the VIA shadow so the ROM identifies
    // this model on PA6 / PB3 reads (see docs/iicx.md §"Machine
    // Identification").
    bool machine_id_pa6;
    bool machine_id_pb3;

    // NuBus slot table (sentinel-terminated).  Step 3 wires this up; for
    // step 2 the field exists but nothing reads it yet.
    const nubus_slot_decl_t *slots;

    // Per-machine VIA extra-decode hooks (NULL = no extra handling).
    glue030_via_extra_fn on_via1_extra;
    glue030_via_extra_fn on_via2_extra;
} glue030_init_t;

// Run the family-shared init sequence.  Step 2 ships a non-functional stub
// (returns 0); the body lights up incrementally as code moves out of
// per-machine files.
int glue030_init(const glue030_init_t *spec);

// Counterpart teardown.  Step 2 ships a stub; later steps walk the family-
// owned peripherals in inverse init order.
void glue030_teardown(config_t *cfg);

// Image-list checkpoint helpers — extracted from se30.c, identical loop
// today appears in plus.c too (proposal §1.1).  Per §3.1.1 the Plus stays
// standalone for now, so v1 only se30.c calls these; folding plus.c onto
// them is a follow-up once the 24-bit / 68000 conditionals are tractable.
//
// _save writes (count, [name_len, name, writable, raw_size, instance_len,
// instance_path] x count) into the checkpoint stream.  _restore reads the
// same shape, opens / creates the corresponding image_t, and attaches it
// to cfg->images via add_image().  Both fail loudly via
// checkpoint_set_error on partial reads / failed image opens so the caller
// sees a marked-error checkpoint.
void glue030_checkpoint_save_images(config_t *cfg, checkpoint_t *cp);
void glue030_checkpoint_restore_images(config_t *cfg, checkpoint_t *cp);

#endif // GLUE030_H
