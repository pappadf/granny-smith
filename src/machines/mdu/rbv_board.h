// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rbv_board.h
// The device spine and the RBV glue every RBV-lineage board shares: the IIci
// and IIsi (mdu/) and the V8 family (v8/), whose decoders absorb the same RBV
// register file as their pseudo-VIA2.  After VIA1 each of them builds the
// same run of devices -- ADB, the disk images, the 5380, the sound block,
// SWIM and the ADB/power microcontroller -- and wires them the same way: the
// 5380's IRQ/DRQ and the sound interrupt into the RBV's flags, the RBV's
// combined interrupt to IPL 2, the companion's soft power-off to the
// scheduler.  This is that run, once; each family keeps only what differs
// (the RBV variant and sense strap, the depth/blank/expansion consumers).

#ifndef GS_MACHINES_MDU_RBV_BOARD_H
#define GS_MACHINES_MDU_RBV_BOARD_H

#include "checkpoint.h"
#include "rbv.h" // rbv_variant_t
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

struct asc;
struct cuda;
struct egret;
struct mmu_state;

// The ADB/clock/power microcontroller on VIA1's shift register.
typedef enum rbv_board_companion {
    RBV_BOARD_NO_COMPANION = 0, // the IIci: ADB transceiver and RTC bit-banged on VIA1
    RBV_BOARD_EGRET = 1, // the IIsi, LC II, Classic II, LC III
    RBV_BOARD_CUDA = 2, // the Color Classic, LC 520 family, Color Classic II
} rbv_board_companion_t;

// What differs in the shared run.
typedef struct rbv_board_spec {
    rbv_board_companion_t companion;
    uint8_t asc_version; // the sound block's version byte (0: the ASC's own)
    bool asc_fifo_only; // a sound block with no wavetable, always draining (the V8's)
} rbv_board_spec_t;

// The devices the run builds, by where the family's state keeps them.  `rbv`
// is read later (the 5380's interrupt routes through it), so it must point
// into state that outlives the machine; `egret`/`cuda` may be NULL when the
// spec names no such companion.
typedef struct rbv_board_slots {
    struct rbv **rbv;
    struct asc **asc;
    struct egret **egret;
    struct cuda **cuda;
} rbv_board_slots_t;

// Build ADB, the images, the 5380, the sound block, SWIM and the companion,
// each a checkpoint part, in that order.  Returns -1 when a device cannot be
// constructed.
int rbv_board_build_devices(config_t *cfg, checkpoint_t *cp, const rbv_board_spec_t *spec,
                            const rbv_board_slots_t *slots);

// Build the RBV part (`variant`, `sense` strapped on its monitor-sense lines)
// into *slots->rbv, route its combined interrupt to IPL 2 and the sound
// block's interrupt to its sound flag.  Returns -1 when it cannot be built.
int rbv_board_build_rbv(config_t *cfg, checkpoint_t *cp, rbv_variant_t variant, uint8_t sense,
                        const rbv_board_slots_t *slots);

// Soft power-off (the RBV's RvPowerOff edge, Egret's PwrDown): stop the
// scheduler so the run ends cleanly.  Context: cfg.
void rbv_board_power_off(void *context);

// Delete the family-owned devices (companion before the adb it was handed,
// then the RBV, the MMU and the sound block), clearing each pointer, then the
// config_t-owned chain (machine_teardown.h).  Any argument may be NULL.
void rbv_board_teardown(config_t *cfg, const rbv_board_slots_t *slots, struct mmu_state **mmu);

#endif // GS_MACHINES_MDU_RBV_BOARD_H
