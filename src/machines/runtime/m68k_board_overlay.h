// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// m68k_board_overlay.h
// The ROM-at-zero overlay of the 68000/030/040 boards, one object with a
// policy for how it goes away:
//   M68K_BOARD_OVERLAY_DROP_ON_ACCESS  the AV, MCU, IIfx, V8 and the compact
//                                  BBU boards: the first access to the ROM
//                                  aperture drops it (a trip-wire)
//   M68K_BOARD_OVERLAY_FOLLOW_LINE     GLUE and MDU: it follows VIA1's Overlay
//                                  output (PA4), set by the board's VIA code
//   M68K_BOARD_OVERLAY_FOLLOW_READS    the compact PAL boards: it follows the
//                                  line too, but steers reads only

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
// The GLUE/MDU page swap, the IIfx's own trip-wire (which re-implemented
// this one, ROM-window refill included) and the compact PAL boards'
// read-side-only steer became policies of this object.  One "ROM at zero"
// mechanism remains outside it: pdm_hmc_remap (a memory-controller remap).

// How the overlay goes away (see above).
typedef enum {
    M68K_BOARD_OVERLAY_DROP_ON_ACCESS,
    M68K_BOARD_OVERLAY_FOLLOW_LINE,
    // FOLLOW_LINE with the decoder steering READS only: while armed a write
    // to the overlaid pages still lands in RAM (the PAL boards), so arming
    // and dropping re-point each page's read side and leave its write side.
    M68K_BOARD_OVERLAY_FOLLOW_READS,
} m68k_board_overlay_policy_t;

typedef struct m68k_board_overlay {
    struct config *cfg;
    uint32_t rom_base; // aperture start, from the board descriptor
    uint32_t rom_end; // aperture end (exclusive)
    // An optional alias of the aperture (0/0: none), filled and routed
    // exactly like it: a board whose ROM answers in two windows (a 24-bit one
    // and the 32-bit ROM space).  m68k_board_overlay_set_alias.
    uint32_t rom_base2;
    uint32_t rom_end2;
    void (*map_ram)(struct config *cfg); // the family's RAM-at-zero mapping
    // Optional: called after every arm (true) and every effective drop
    // (false), for what else the board's overlay map changes (the PAL boards'
    // RAM image at $600000).
    void (*on_change)(struct config *cfg, bool armed);
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
// pages (m68k_board_overlay_map_ram_identity).  Does not arm.
void m68k_board_overlay_init_line(m68k_board_overlay_t *ov, struct config *cfg, const char *name);

// The same for the FOLLOW_READS policy (the compact PAL boards), with an
// optional on_change hook.  Does not arm.
void m68k_board_overlay_init_line_reads(m68k_board_overlay_t *ov, struct config *cfg, const char *name,
                                        void (*on_change)(struct config *cfg, bool armed));

// Add an alias of the ROM aperture, [base2, end2) (DROP_ON_ACCESS): every
// arm routes its pages to the trigger device too (offsets relative to
// base2), every drop (and m68k_board_overlay_fill_aperture) fills it with
// direct ROM pages, mirroring the image across it from base2.  Call after
// m68k_board_overlay_init, before the first arm.  For the V8's 32-bit ROM
// space beside its 24-bit window; no board sets one yet.
void m68k_board_overlay_set_alias(m68k_board_overlay_t *ov, uint32_t base2, uint32_t end2);

// RAM back at zero: the pages the overlay covered (the ROM's size from zero),
// each at its own offset wrapped into the installed RAM.  The FOLLOW_LINE
// default, and a DROP_ON_ACCESS board's map_ram when it needs no more.
void m68k_board_overlay_map_ram_identity(struct config *cfg);

// ROM readable (read-only) at zero; under DROP_ON_ACCESS the aperture pages
// are routed to the trigger device.  Cold boot and hardware RESET both arm.
// Unconditional: it re-points the pages even while armed.
void m68k_board_overlay_arm(m68k_board_overlay_t *ov);

// RAM at zero; under DROP_ON_ACCESS the aperture pages become direct ROM.
// Idempotent.
void m68k_board_overlay_drop(m68k_board_overlay_t *ov);

// FOLLOW_LINE: the board's Overlay line changed -- arm while it is high, drop
// when it falls.  No-op while the level is unchanged.
void m68k_board_overlay_set_line(m68k_board_overlay_t *ov, bool on);

// Point the aperture (both, with a second one) at direct ROM pages,
// mirroring the image across it.
void m68k_board_overlay_fill_aperture(const m68k_board_overlay_t *ov);

#endif // GS_MACHINES_RUNTIME_M68K_BOARD_OVERLAY_H
