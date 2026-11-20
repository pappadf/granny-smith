// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// m68k_board_overlay.h
// The ROM-at-zero overlay of the 68030/040 boards, one object with a policy
// for how it goes away:
//   M68K_BOARD_OVERLAY_DROP_ON_ACCESS  the AV, MCU and IIfx: the first access to
//                                  the ROM aperture drops it (a trip-wire)
//   M68K_BOARD_OVERLAY_FOLLOW_LINE     GLUE and MDU: it follows VIA1's Overlay
//                                  output (PA4), set by the board's VIA code

#ifndef GS_MACHINES_RUNTIME_M68K_BOARD_OVERLAY_H
#define GS_MACHINES_RUNTIME_M68K_BOARD_OVERLAY_H

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
// The GLUE/MDU page swap and the IIfx's own trip-wire (which re-implemented
// this one, ROM-window refill included) became policies of this object.  Two
// "ROM at zero" mechanisms remain outside it: the Plus's read-side-only steer
// (24-bit, no MMU) and pdm_hmc_remap (a memory-controller remap).

// How the overlay goes away (see above).
typedef enum {
    M68K_BOARD_OVERLAY_DROP_ON_ACCESS,
    M68K_BOARD_OVERLAY_FOLLOW_LINE,
} m68k_board_overlay_policy_t;

typedef struct m68k_board_overlay {
    struct config *cfg;
    uint32_t rom_base; // aperture start, from the board descriptor
    uint32_t rom_end; // aperture end (exclusive)
    void (*map_ram)(struct config *cfg); // the family's RAM-at-zero mapping
    const char *name; // log prefix, e.g. "AV" or "MCU"
    m68k_board_overlay_policy_t policy;
    memory_interface_t iface; // the trigger device while armed (DROP_ON_ACCESS)
    bool armed; // checkpointed by the MCU substrate; GLUE/MDU re-derive it from VIA1
} m68k_board_overlay_t;

// Fill in `ov` and its trigger interface.  Does not arm; call _arm after the
// aperture has been registered with memory_map_add(&ov->iface, ov).
void m68k_board_overlay_init(m68k_board_overlay_t *ov, struct config *cfg, uint32_t rom_base, uint32_t rom_end,
                             void (*map_ram)(struct config *cfg), const char *name);

// Fill in `ov` for the FOLLOW_LINE policy (GLUE, MDU): no trigger device and
// no aperture of its own; RAM at zero is the identity map of the overlaid
// pages.  Does not arm.
void m68k_board_overlay_init_line(m68k_board_overlay_t *ov, struct config *cfg, const char *name);

// ROM readable (read-only) at zero; under DROP_ON_ACCESS the aperture pages
// are routed to the trigger device.  Cold boot and hardware RESET both arm.
void m68k_board_overlay_arm(m68k_board_overlay_t *ov);

// RAM at zero; under DROP_ON_ACCESS the aperture pages become direct ROM.
// Idempotent.
void m68k_board_overlay_drop(m68k_board_overlay_t *ov);

// FOLLOW_LINE: the board's Overlay line changed -- arm while it is high, drop
// when it falls.  No-op while the level is unchanged.
void m68k_board_overlay_set_line(m68k_board_overlay_t *ov, bool on);

// Point the aperture at direct ROM pages, mirroring the image across it.
void m68k_board_overlay_fill_aperture(const m68k_board_overlay_t *ov);

#endif // GS_MACHINES_RUNTIME_M68K_BOARD_OVERLAY_H
