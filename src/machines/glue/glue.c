// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// glue.c
// The GLUE family's init, peripherals, memory layout and substrate — see
// glue.h.

#include "glue.h"

#include "config_seed.h"
#include "machine_teardown.h"

#include "mac_host_io.h" // mac_fd_*/mac_input_* substrate methods (shared by all Macs)
#include "machine_profile.h" // machine_substrate_t

#include "adb.h"
#include "asc.h"
#include "checkpoint.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "machine_checkpoint.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rtc.h"
#include "scsi.h"
#include "slot_irq.h"
#include "via.h"

#include <stdlib.h>

LOG_USE_CATEGORY_NAME("system");

// Construct the GLUE peripheral set in canonical order — see header.
int glue_build_peripherals(config_t *cfg, checkpoint_t *cp, glue_state_t *st, const m68k_board_desc_t *desc) {
    machine_part_begin(cfg, cp, "adb");
    cfg->adb = adb_init(cfg->via1, cfg->scheduler, cp);
    machine_part(cfg, cp, "adb", part_save_adb, cfg->adb);

    // The image list before the devices that reference it.
    machine_part_images(cfg, cp);

    machine_part_begin(cfg, cp, "scsi");
    cfg->scsi = machine_scsi_bus_init(cfg, cp, "scsi");
    // SE/30, IIcx and IIx: an NCR 5380 behind the glue's own decode.
    scsi_5380_attach(cfg->scsi, cp);
    machine_part(cfg, cp, "scsi", part_save_scsi, cfg->scsi);
    scsi_set_via(cfg->scsi, cfg->via2);

    machine_part_begin(cfg, cp, "asc");
    st->asc = asc_init(NULL, cfg->scheduler, cp);
    machine_part(cfg, cp, "asc", part_save_asc, st->asc);
    asc_set_via(st->asc, cfg->via2);
    asc_set_mix(st->asc, desc->asc_mix); // board speaker fold (not checkpointed)

    machine_part_begin(cfg, cp, "floppy");
    cfg->floppy =
        floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, machine_floppy_count(cfg), cp, config_images(cfg));
    machine_part(cfg, cp, "floppy", part_save_floppy, cfg->floppy);

    glue_io_bind(&st->glue_io, cfg, desc, st->asc, cfg->floppy);
    return 0;
}

// The GLUE family's memory layout — RAM, ROM and the I/O dispatcher.
//
// The SE/30, IIcx and IIx are one motherboard design with one GLUE, so this
// was three copies of the same function differing only in the constant NAMES
// (SE30_ROM_START vs IICX_ROM_START, both $40000000) and in a short tail.
// The tail is what actually differs and stays per-board (memory_layout_tail):
// the SE/30 maps its built-in video's VRAM/VROM; the IIcx and IIx fill page
// entries for NuBus cards' host-backed regions.  Both then arm the overlay.
//
// The ROM window comes from the board descriptor, which already carried it
// for m68k_board_build_mmu — so the duplicated per-machine #defines are gone.
// I/O is the $10000000 window immediately above the ROM window on all three.
void glue_memory_layout(config_t *cfg, const m68k_board_desc_t *desc) {
    glue_state_t *st = glue_st(cfg);

    uint32_t ram_size = cfg->ram_size;
    uint32_t rom_size = cfg->machine->rom_size;
    uint8_t *ram_base = ram_native_pointer(cfg->memory_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->memory_map, ram_size); // ROM follows RAM in the flat buffer

    // --- RAM, with the SIMM address-line wrap the ROM's test depends on ---
    //
    // SIMMs ignore address bits above their capacity, so the byte at
    // <ram_size> is the same cell as the byte at 0.  The ROM's
    // ram_address_test writes to the top-of-RAM address and checks whether the
    // pattern appears at a lower alias; without the mirror the write falls
    // into unmapped space and the test reports a spurious address-bus error.
    //
    // Its table has two kinds of row: "BMI" rows (alias = $FFFFFFFF) that
    // expect NO aliasing, and non-BMI rows that expect the wrap.  BMI rows are
    // 1, 4 and 16 MB; every other total (2, 5, 8, 32, 64 …) expects the wrap,
    // so those get one extra mirror.
    uint32_t ram_pages = ram_size >> MEM_PAGE_SHIFT;
    bool standard_bank = (ram_size == 1 * 1024 * 1024 || ram_size == 4 * 1024 * 1024 || ram_size == 16 * 1024 * 1024);
    uint32_t map_end_page = standard_bank ? ram_pages : (ram_pages * 2);
    for (uint32_t p = 0; p < map_end_page && p < g_page_count; p++)
        page_fill_68k(p, ram_base + ((p % ram_pages) << MEM_PAGE_SHIFT), true);

    // --- ROM, mirrored across the board's window (read-only) ---
    uint32_t rom_pages = rom_size >> MEM_PAGE_SHIFT;
    uint32_t rom_start_page = desc->rom_base >> MEM_PAGE_SHIFT;
    uint32_t rom_end_page = desc->rom_end >> MEM_PAGE_SHIFT;
    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && p < g_page_count; p++)
            page_fill_68k(p, rom_data + (((p - rom_start_page) % rom_pages) << MEM_PAGE_SHIFT), false);
    }

    // --- I/O dispatcher, the window directly above the ROM window ---
    board_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->memory_map, desc->rom_end, GLUE_IO_SIZE, "I/O", &st->io_interface, &st->glue_io);
}

// The shared GLUE init — board-driven (see header).  Order is the canonical
// se30/iicx/iix init spine; per-machine deltas come from the board's data and
// hooks.  TT1 is uniform across the GLUE family ($F0..$FF supervisor identity);
// it is set right after the PMMU is built (no MMU walk happens during
// construction, so the exact moment is immaterial).
int glue_init(config_t *cfg, checkpoint_t *cp, const glue_board_t *board) {
    glue_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;

    m68k_board_build_core(cfg, board->desc, cp);
    if (board->pre_devices)
        board->pre_devices(cfg);
    machine_part_irq(cfg, cp);

    m68k_board_build_lowspeed(cfg, cp, NULL); // NULL: the family-default SCC IRQ

    // Derived, not the literal 20 this used to pass.  Every GLUE machine is
    // 15.6672 MHz so the number is unchanged -- but 20 was exactly the stale
    // inherited divisor mdu.c:68-72 and mcu.c:639-641 both record being burned
    // by ("correct for the 16 MHz IIcx this code was adapted from, and 1.6x
    // too fast on a IIci"), and a GLUE sibling on another clock would inherit
    // it the same way.
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    machine_part_begin(cfg, cp, "via1");
    cfg->via1 = via_init(NULL, cfg->scheduler, via_ff, "via1", board->via1_output, board->via1_shift_out,
                         m68k_board_via1_irq, cfg, cp);
    machine_part(cfg, cp, "via1", part_save_via, cfg->via1);
    machine_part_begin(cfg, cp, "via2");
    cfg->via2 = via_init(NULL, cfg->scheduler, via_ff, "via2", board->via2_output, board->via2_shift_out,
                         m68k_board_via2_irq, cfg, cp);
    machine_part(cfg, cp, "via2", part_save_via, cfg->via2);
    // The family saves no /SLOTIRQ mask: a restore derives it from the VIA2
    // port-A pins it just read, or a source still requesting at save time
    // would drop out of the OR on the next other source's deassert.
    if (cp)
        st->slot_pa_mask = slot_irq_mask_from_via(cfg->via2);
    rtc_set_via(cfg->rtc, cfg->via1);

    board->setup_id(cfg);

    if (glue_build_peripherals(cfg, cp, st, board->desc) != 0)
        return -1;

    st->mmu = m68k_board_build_mmu(cfg, board->desc->rom_base, board->desc->rom_end);
    if (!st->mmu)
        return -1; // m68k_board_build_mmu reported the reason
    st->mmu->tt1 = st->mmu->tt1_board = M68K_BOARD_TT1_NUBUS_SUPER;

    cfg->nubus = nubus_init(cfg, cfg->machine->nubus_slots, cp);
    if (board->post_nubus)
        board->post_nubus(cfg);

    glue_memory_layout(cfg, board->desc);
    if (board->memory_layout_tail)
        board->memory_layout_tail(cfg);

    // VIA2 drives GLUE outputs too: re-driven once the PMMU is bound
    machine_part_pmmu(cfg, cp, st->mmu, cfg->via1, cfg->via2);

    m68k_board_finish(cfg, cp, &st->glue_io);
    return 0;
}

// A VIA output the board does not observe.
void glue_via_output_ignored(void *context, uint8_t port, uint8_t output) {
    (void)context;
    (void)port;
    (void)output;
}

// A VIA shift-out the board does not observe.
void glue_via_shift_out_ignored(void *context, uint8_t byte) {
    (void)context;
    (void)byte;
}

// substrate.nubus_slot_irq (GLUE): each NuBus slot's /NMRQ line maps to a VIA2
// port-A bit (active-low; slot $9→PA0 .. $E→PA5); the umbrella OR-line edge
// (no slot asserted ↔ any slot asserted) pulses CA1.  Verbatim from the former
// nubus.c VIA2 fast-path — now reached uniformly through the substrate so
// nubus.c carries no cfg->via2.
// One source of the family's /SLOTIRQ aggregate changes state.
//
// The chipset keeps the OR, not the bus.  GLUE used to pulse CA1 only when
// the NuBus controller reported an `umbrella_edge` computed from its own
// slot bitmap; the MCU ignored that flag and re-drove CA1 from its own
// mask, and the MCU was right -- its aggregate includes DAFB on PA6 and SONIC
// on PA0, sources the NuBus controller knows nothing about.  The SE/30's
// built-in video is the same shape.  A bus that cannot see every contributor
// cannot compute the edge.  The OR itself is shared with the MCU (slot_irq.h).
void glue_slot_irq_source(config_t *cfg, int pa_bit, bool active) {
    glue_state_t *st = glue_st(cfg);
    if (!st)
        return;
    slot_irq_source(cfg->via2, &st->slot_pa_mask, pa_bit, active);
}

void glue_nubus_slot_irq(config_t *cfg, int slot, bool active) {
    int pa_bit = slot - 0x9;
    if (pa_bit < 0 || pa_bit > 5)
        return;
    glue_slot_irq_source(cfg, pa_bit, active);
}

// ============================================================
// The shared GLUE-family substrate
// ============================================================
//
// SE/30, IIcx and IIx all bind this one substrate.  Each machine's deltas live
// in its glue_board_t (reached via cfg->machine->board) + its profile;
// the per-machine init/reset/teardown/checkpoint clones are gone.

static inline const glue_board_t *glue_board(config_t *cfg) {
    return (const glue_board_t *)cfg->machine->board;
}

static int glue_substrate_init(config_t *cfg, checkpoint_t *cp) {
    if (glue_init(cfg, cp, glue_board(cfg)) != 0)
        return -1;
    return 0;
}

static void glue_bus_reset(config_t *cfg) {
    glue_state_t *st = glue_st(cfg);
    m68k_board_bus_reset(cfg, &st->overlay);
}

static void glue_teardown(config_t *cfg) {
    glue_state_t *st = glue_st(cfg);
    if (st) {
        // Borrowed slot-$E video pointers (SE/30 only; already NULL on IIcx/IIx).
        // system_destroy ran nubus_delete first, so the card buffer is gone.
        st->vram = NULL;
        st->vrom = NULL;
        st->video_card = NULL;
        if (st->mmu) {
            mmu_delete(st->mmu); // also clears g_mmu if it matches
            st->mmu = NULL;
        }
        if (st->asc) {
            asc_delete(st->asc);
            st->asc = NULL;
        }
    }
    machine_teardown_config_devices(cfg); // floppy, adb, scsi, VIAs, ... (machine_teardown.h)
}

// One 60.15 Hz VBL pulse on a VIA's CA1.  The lines idle high (via_init parks
// them there), so the active transition is the falling edge and the line is
// left back at rest.
void mac_vbl_pulse(struct via *via) {
    via_input_c(via, /*port A*/ 0, /*CA1*/ 0, false);
    via_input_c(via, 0, 0, true);
}

static void glue_trigger_vbl(config_t *cfg) {
    const glue_board_t *board = glue_board(cfg);
    if (board->trigger_vbl) {
        board->trigger_vbl(cfg); // SE/30: built-in slot-$E video VBL
        return;
    }
    // Default GLUE VBL (IIcx/IIx, NuBus video): the 60.15 Hz interrupt on
    // VIA1 CA1, then fan the VBL out to the NuBus cards.
    //
    // VIA1 ONLY.  This used to pulse VIA2's CA1 too, "as the GLUE chip does"
    // -- but VIA2 CA1 is /SLOTIRQ, the slot-interrupt aggregate, so every
    // frame forged a slot interrupt and desynchronised the umbrella level a
    // real card may be holding.  Guide to the Macintosh Family Hardware 2e,
    // p.211: the vSync slot interrupt "is distinct from the 60.15 Hz
    // interrupt (VBL) request, WHICH IS SENT BY VIA2 TO VIA1."  Two different
    // interrupts; only one of them lands on a CA1 pin here.
    mac_vbl_pulse(cfg->via1);
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

const machine_substrate_t glue_substrate = {
    .init = glue_substrate_init,
    .bus_reset = glue_bus_reset,
    .teardown = glue_teardown,
    .seed = mac_seed_rtc_pram,
    .trigger_vbl = glue_trigger_vbl,
    .nubus_slot_irq = glue_nubus_slot_irq,
    MAC_SUBSTRATE_HOST_IO,
    MAC_SUBSTRATE_MEDIA_STD,
};
