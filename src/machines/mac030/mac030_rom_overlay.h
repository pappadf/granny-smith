// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_rom_overlay.h
// The access-triggered ROM-at-zero overlay, shared by the AV and MCU families.

#ifndef MAC030_ROM_OVERLAY_H
#define MAC030_ROM_OVERLAY_H

#include "common.h"
#include "memory.h"

#include <stdbool.h>
#include <stdint.h>

struct config;

// While ARMED, the ROM image is readable at $00000000 and the board's ROM
// aperture is registered as a trigger device.  The first access to the
// aperture DROPS the overlay -- RAM appears at zero, the aperture becomes
// direct ROM pages -- and the triggering access itself still returns ROM data.
//
// The AV and MCU families each carried a private copy of this: 110 lines that
// were byte-identical apart from one log prefix, once the `av_`/`mcu_` names
// were normalised.  They looked like clones differing in the aperture bounds
// and in whether the image mirrors every 1 MiB; neither delta was real -- the bounds already
// came from the board descriptor's rom_base/rom_end in both, and both mirrored
// by `% rom_pages`.  So there was nothing to parameterise except the names.
//
// Three other "ROM at zero" mechanisms remain and are NOT this:
// mac030_glue_set_rom_overlay (a software-toggled page swap, no trigger),
// iifx_set_rom_overlay (a device window over the ROM region) and
// pdm_hmc_remap (a memory-controller remap).  They differ in how the overlay
// is dropped, which is the part that matters.
typedef struct mac030_rom_overlay {
    struct config *cfg;
    uint32_t rom_base; // aperture start, from the board descriptor
    uint32_t rom_end; // aperture end (exclusive)
    void (*map_ram)(struct config *cfg); // the family's RAM-at-zero mapping
    const char *name; // log prefix, e.g. "AV" or "MCU"
    memory_interface_t iface; // the trigger device while armed
    bool armed; // checkpointed by the substrate
} mac030_rom_overlay_t;

// Fill in `ov` and its trigger interface.  Does not arm; call _arm after the
// aperture has been registered with memory_map_add(&ov->iface, ov).
void mac030_rom_overlay_init(mac030_rom_overlay_t *ov, struct config *cfg, uint32_t rom_base, uint32_t rom_end,
                             void (*map_ram)(struct config *cfg), const char *name);

// ROM readable at zero; aperture pages routed to the trigger device.
// Cold boot and hardware RESET both arm.
void mac030_rom_overlay_arm(mac030_rom_overlay_t *ov);

// RAM at zero; aperture pages become direct ROM.  Idempotent.
void mac030_rom_overlay_drop(mac030_rom_overlay_t *ov);

// Point the aperture at direct ROM pages, mirroring the image across it.
void mac030_rom_overlay_fill_aperture(const mac030_rom_overlay_t *ov);

#endif // MAC030_ROM_OVERLAY_H
