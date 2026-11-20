// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// glue.h
// The GLUE family (SE/30, IIcx, IIx): its unified machine state, its board
// hooks, the shared init and substrate, and the family's lifecycle leaves.
// It builds on the 68k board runtime (m68k_board.h); its I/O tables are
// glue_io.h.

#ifndef GS_MACHINES_GLUE_GLUE_H
#define GS_MACHINES_GLUE_GLUE_H

#include "checkpoint.h"
#include "glue_io.h"
#include "m68k_board.h"
#include "m68k_board_overlay.h"
#include "memory.h" // memory_interface_t
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

struct adb;
struct asc;
struct floppy;
struct mmu_state;
struct nubus_card;
struct via;

// Unified GLUE-family machine state — the single struct shared by SE/30,
// IIcx and IIx (collapsing the former se30_state_t and iicx_state_t).  It is
// a superset: the SE/30 uses vram/vrom/video_card and
// leaves the IIcx soft-power fields unused; the IIcx/IIx use the soft-power
// fields and leave the video pointers NULL.
typedef struct glue_state {
    struct asc *asc; // (the adb and floppy are cfg->adb / cfg->floppy)

    m68k_board_overlay_t overlay; // ROM at $00000000, following VIA1 PA4
    struct mmu_state *mmu; // 68030 PMMU

    board_io_t glue_io; // device handles for the shared dispatcher

    // The /SLOTIRQ aggregate this family drives onto VIA2 CA1: the OR of
    // every source, kept by the CHIPSET rather than inferred from the NuBus
    // controller's own view.  Quadra 700/900 developer notes: "The ... NuBus
    // interrupt signals ..., the built-in video interrupt signal, and the
    // Ethernet controller interrupt signal are routed through an OR gate to
    // generate a signal called /SLOTIRQ.  This signal is connected to the CA1
    // input of VIA2."  It is a LEVEL, and it can include sources the bus
    // knows nothing about -- which is why the bus's `umbrella_edge` was the
    // wrong abstraction.  Not saved: a restore derives it from the VIA2 pins
    // (slot_irq.h).
    uint8_t slot_pa_mask;

    bool soft_power_armed; // IIcx soft-power detect (unused on se30/iix)

    // SE/30 built-in video (slot $E); NULL on IIcx/IIx (they use NuBus cards).
    uint8_t *vram;
    uint8_t *vrom;
    struct nubus_card *video_card;

    memory_interface_t io_interface; // registered at the $50000000 I/O region
} glue_state_t;

// The GLUE family's machine state (cfg->machine_context).
static inline glue_state_t *glue_st(config_t *cfg) {
    return (glue_state_t *)cfg->machine_context;
}

// Construct the GLUE peripheral set shared by se30/iicx/iix in canonical
// order: ADB, (checkpoint image restore), SCSI (+VIA2), images, ASC (+VIA2),
// SWIM floppy, then bind the I/O dispatcher.  Stores the device handles in
// the unified state and on config_t.  Call after VIA1/VIA2 exist.
int glue_build_peripherals(config_t *cfg, checkpoint_t *cp, glue_state_t *st, const m68k_board_desc_t *desc);

// The I/O window sits directly above the ROM window and is 256 MB on every
// GLUE machine ($50000000-$5FFFFFFF), so its base is desc->rom_end.
#define GLUE_IO_SIZE 0x10000000UL

// The GLUE family's shared memory layout: RAM (with the SIMM address wrap the
// ROM's ram_address_test needs), the ROM window from desc->rom_base/rom_end,
// and the I/O dispatcher.  glue_init calls this, then the board's
// memory_layout_tail.  One motherboard design, one function.
void glue_memory_layout(config_t *cfg, const m68k_board_desc_t *desc);

// A GLUE machine = its board descriptor (data) + a few hooks.
// The shared glue_init() walks this in canonical order; the per-machine
// deltas are the VIA output/shift callbacks, the machine-ID strap sequence, the
// memory layout, and (SE/30 only) the built-in-video wiring.  NULL hooks are
// skipped.
typedef struct glue_board {
    const m68k_board_desc_t *desc; // hardware data (rom window, io map, slots, bus-error)

    void (*via1_output)(void *context, uint8_t port, uint8_t value);
    // VIA1 shift-out routing, NULL on every ADB board (here, the MDU's and the
    // MCU's): adb.c reads the VIA's shift register directly at each port-B ST
    // transition, because in mode 7 the ADB transceiver clocks the shift, not
    // the VIA's internal timer, and the ROM's SR writes during interrupt
    // handling fire the callback spuriously (BUG-004).  via.c tolerates NULL.
    void (*via1_shift_out)(void *context, uint8_t byte);
    void (*via2_output)(void *context, uint8_t port, uint8_t value);
    void (*via2_shift_out)(void *context, uint8_t byte);

    void (*setup_id)(config_t *cfg); // machine-ID strap + VIA2 idle lines

    // Per-board remainder of the memory layout, run right after the shared
    // glue_memory_layout() has done RAM, ROM and the I/O dispatcher:
    // the SE/30 maps its built-in video's VRAM/VROM, the IIcx and IIx fill
    // page entries for NuBus cards' host-backed regions, and both then arm
    // the ROM overlay.  Optional, like the other tail hooks.
    void (*memory_layout_tail)(config_t *cfg);

    void (*pre_devices)(config_t *cfg); // optional: before device construction (SE/30 VBL event type)
    void (*post_nubus)(config_t *cfg); // optional: after nubus_init (SE/30 VRAM/VROM wiring)
    void (*trigger_vbl)(config_t *cfg); // optional VBL override; NULL → the default GLUE NuBus VBL
} glue_board_t;

// The one GLUE-family substrate: SE/30, IIcx and IIx all bind
// this; their per-machine deltas live entirely in their glue_board_t
// (named via hw_profile_t.board) and profile.  The lifecycle methods read the
// board from cfg->machine->board.
extern const machine_substrate_t glue_substrate;

// The shared GLUE init: allocates the unified state, builds the II-family
// core + RTC/SCC/VIA1/VIA2 + peripherals + PMMU + NuBus in canonical order,
// applies the board's deltas via its hooks, and finishes.  A GLUE machine's
// substrate.init is a one-liner that calls this with its board.
int glue_init(config_t *cfg, checkpoint_t *cp, const glue_board_t *board);

// The GLUE machines' RAM range: 8 MB stock, 128 MB the most the 68030 boards
// here take (SE/30, IIcx, IIx profiles).
#define GLUE_RAM_DEFAULT 0x800000u // 8 MB
#define GLUE_RAM_MAX     0x8000000u // 128 MB

// VIA callbacks for a pin set the board does not observe: VIA2 output on
// the SE/30 and IIx, VIA2 shift-out on all three (via_init takes a function,
// and a NULL shift-out changes mode-7 timing -- see via.c).
void glue_via_output_ignored(void *context, uint8_t port, uint8_t output);
void glue_via_shift_out_ignored(void *context, uint8_t byte);

// substrate.nubus_slot_irq for the GLUE family: each slot's /NMRQ is a VIA2
// port-A bit (active-low; slot $9→PA0 .. $E→PA5), and the umbrella OR-line edge
// pulses CA1.  (se30/iicx/iix.)
// One source of the family /SLOTIRQ aggregate on VIA2 port A changes state.
// pa_bit 0-5 are NuBus slots $9-$E; 6 is available for a built-in source the
// bus knows nothing about (the SE/30's video, the MCU's DAFB).
// One 60.15 Hz VBL pulse on a VIA's CA1 line.
void mac_vbl_pulse(struct via *via);

void glue_slot_irq_source(config_t *cfg, int pa_bit, bool active);

void glue_nubus_slot_irq(config_t *cfg, int slot, bool active);

#endif // GS_MACHINES_GLUE_GLUE_H
