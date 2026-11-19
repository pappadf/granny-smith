// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue.c
// Shared GLUE-family lifecycle leaves — see mac030_glue.h.

#include "mac030_glue.h"

#include "appletalk.h"
#include "config_seed.h"
#include "machine_teardown.h"

#include "mac_host_io.h" // mac_fd_*/mac_input_* substrate methods (shared by all Macs)
#include "machine_profile.h" // machine_substrate_t

#include "adb.h"
#include "asc.h"
#include "checkpoint.h"
#include "cpu.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "machine_checkpoint.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "slot_irq.h"
#include "via.h"

#include <assert.h>
#include <stdlib.h>

LOG_USE_CATEGORY_NAME("system");

// Construct the GLUE peripheral set in canonical order — see header.
int mac030_glue_build_peripherals(config_t *cfg, checkpoint_t *cp, mac030_glue_state_t *st,
                                  const mac030_board_desc_t *desc) {
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

    mac030_glue_io_bind(&st->glue_io, cfg, desc, st->asc, cfg->floppy);
    return 0;
}

// Create + attach the 68030 PMMU over a board's ROM window — see header.
struct mmu_state *mac030_build_mmu(config_t *cfg, uint32_t rom_base, uint32_t rom_end) {
    uint8_t *ram_base = ram_native_pointer(cfg->memory_map, 0);
    uint32_t ram_size = cfg->ram_size;
    uint8_t *rom_data = ram_native_pointer(cfg->memory_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    mmu_state_t *mmu = mmu_init(ram_base, ram_size, cfg->machine->ram_max, rom_data, rom_size, rom_base, rom_end);
    if (!mmu) {
        LOG(0, "Error: out of memory constructing the PMMU");
        return NULL; // mac030_build_mmu returns the MMU, not a status
    }
    // Set both, always together: the fault hook runs on the map's PMMU and
    // the 68030 bus-error path reads cpu->mmu (asserted equal there).
    memory_map_set_pmmu(cfg->memory_map, mmu);
    cpu_attach_mmu(cfg->cpu, mmu);
    return mmu;
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
// for mac030_build_mmu — so the duplicated per-machine #defines are gone.
// I/O is the $10000000 window immediately above the ROM window on all three.
void mac030_glue_memory_layout(config_t *cfg, const mac030_board_desc_t *desc) {
    mac030_glue_state_t *st = (mac030_glue_state_t *)cfg->machine_context;

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
    mac030_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->memory_map, desc->rom_end, MAC030_GLUE_IO_SIZE, "I/O", &st->io_interface, &st->glue_io);
}

// Build the low-speed spine every 68k family shares: the RTC, the SCC at the
// Mac's clocks, and the machine's connection to the AppleTalk network on its
// LocalTalk channel.
//
// Each of the three is a checkpoint part, registered as it is built
// (machine_parts.h).
//
// `scc_irq` is the only genuine per-family variation at this level; the VIAs
// below it differ enough (one or two, different hooks, different IRQ sinks)
// that they stay with each family.
void mac030_build_lowspeed(config_t *cfg, checkpoint_t *cp, void (*scc_irq)(void *, bool)) {
    machine_part_begin(cfg, cp, "rtc");
    cfg->rtc = rtc_init(cfg->scheduler, cp, true, cfg->machine->pram);
    machine_part(cfg, cp, "rtc", part_save_rtc, cfg->rtc);
    machine_part_begin(cfg, cp, "scc");
    cfg->scc = scc_init(NULL, cfg->scheduler, scc_irq ? scc_irq : mac030_glue_scc_irq, cfg, cp);
    machine_part(cfg, cp, "scc", part_save_scc, cfg->scc);
    // 3.6864 MHz PCLK / 7.8336 MHz RTxC -- the same pair on every 68k Mac.
    scc_set_clocks(cfg->scc, 7833600, 3686400);
    machine_part_begin(cfg, cp, "appletalk");
    cfg->atalk = atalk_conn_new(appletalk_network(), cfg->scheduler, cfg->scc, cp);
    machine_part(cfg, cp, "appletalk", part_save_atalk, cfg->atalk);
    machine_part_imagewriter(cfg, cp, false);
}

// Finish init: debugger, scheduler start, cold-boot IRQ/IPL reset.
void mac030_glue_finish(config_t *cfg, checkpoint_t *cp, const mac030_io_t *io) {
    // Every family's I/O table is checked here, once the machine is fully
    // built.  This is a parameter rather than a lookup so a new family cannot
    // quietly skip it -- see mac030_io_validate.
    mac030_io_validate(io, cfg->machine->id);
    cfg->debugger = debug_init();
    machine_irq_cold_reset(cfg, cp);
}

// The shared GLUE init — board-driven (see header).  Order is the canonical
// se30/iicx/iix init spine; per-machine deltas come from the board's data and
// hooks.  TT1 is uniform across the GLUE family ($F0..$FF supervisor identity);
// it is set right after the PMMU is built (no MMU walk happens during
// construction, so the exact moment is immaterial).
int mac030_glue_init(config_t *cfg, checkpoint_t *cp, const mac030_glue_board_t *board) {
    mac030_glue_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;

    mac030_build_core(cfg, board->desc, cp);
    if (board->pre_devices)
        board->pre_devices(cfg);
    machine_part_irq(cfg, cp);

    mac030_build_lowspeed(cfg, cp, NULL); // NULL: the family-default SCC IRQ

    // Derived, not the literal 20 this used to pass.  Every GLUE machine is
    // 15.6672 MHz so the number is unchanged -- but 20 was exactly the stale
    // inherited divisor mdu.c:68-72 and mcu.c:639-641 both record being burned
    // by ("correct for the 16 MHz IIcx this code was adapted from, and 1.6x
    // too fast on a IIci"), and a GLUE sibling on another clock would inherit
    // it the same way.
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    machine_part_begin(cfg, cp, "via1");
    cfg->via1 = via_init(NULL, cfg->scheduler, via_ff, "via1", board->via1_output, board->via1_shift_out,
                         mac030_glue_via1_irq, cfg, cp);
    machine_part(cfg, cp, "via1", part_save_via, cfg->via1);
    machine_part_begin(cfg, cp, "via2");
    cfg->via2 = via_init(NULL, cfg->scheduler, via_ff, "via2", board->via2_output, board->via2_shift_out,
                         mac030_glue_via2_irq, cfg, cp);
    machine_part(cfg, cp, "via2", part_save_via, cfg->via2);
    // The family saves no /SLOTIRQ mask: a restore derives it from the VIA2
    // port-A pins it just read, or a source still requesting at save time
    // would drop out of the OR on the next other source's deassert.
    if (cp)
        st->slot_pa_mask = slot_irq_mask_from_via(cfg->via2);
    rtc_set_via(cfg->rtc, cfg->via1);

    board->setup_id(cfg);

    if (mac030_glue_build_peripherals(cfg, cp, st, board->desc) != 0)
        return -1;

    st->mmu = mac030_build_mmu(cfg, board->desc->rom_base, board->desc->rom_end);
    if (!st->mmu)
        return -1; // mac030_build_mmu reported the reason
    st->mmu->tt1 = st->mmu->tt1_board = MAC030_TT1_NUBUS_SUPER;

    cfg->nubus = nubus_init(cfg, cfg->machine->nubus_slots, cp);
    if (board->post_nubus)
        board->post_nubus(cfg);

    mac030_glue_memory_layout(cfg, board->desc);
    if (board->memory_layout_tail)
        board->memory_layout_tail(cfg);

    // VIA2 drives GLUE outputs too: re-driven once the PMMU is bound
    machine_part_pmmu(cfg, cp, st->mmu, cfg->via1, cfg->via2);

    mac030_glue_finish(cfg, cp, &st->glue_io);
    return 0;
}

// Build the shared II-family construction prefix.  Reads the CPU model from
// the profile (single source of truth), not a hardcoded constant.
void mac030_build_core(config_t *cfg, const struct mac030_board_desc *desc, checkpoint_t *cp) {
    // The board's NuBus bus-error window is part of the bus it builds.
    const memory_bus_err_window_t bus_err = {.lo = desc->bus_err_lo, .hi = desc->bus_err_hi};
    machine_part_begin(cfg, cp, "memory");
    cfg->memory_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, bus_err,
                                      &cfg->build_opts.rom, cp);
    machine_part(cfg, cp, "memory", part_save_memory, cfg->memory_map);
    machine_part_begin(cfg, cp, "cpu");
    cfg->cpu = cpu_init(cfg->machine->cpu_model, cp);
    machine_part(cfg, cp, "cpu", part_save_cpu, cfg->cpu);
    machine_part_begin(cfg, cp, "scheduler");
    sched_cpu_if_t cpu_if = cpu_sched_if(cfg->cpu); // the 68K main-CPU seam adapter
    cfg->scheduler = scheduler_init(&cpu_if, cp);
    machine_part(cfg, cp, "scheduler", part_save_scheduler, cfg->scheduler);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    // CPI by CPU, both from Apple's "Macintosh Family: MIPS Ratings" (Tech
    // Info Library, Oct 1996) read as MHz / MIPS, which folds in the memory
    // system's wait states as this one constant must.  The 68030 machines sit
    // at 4.0 (IIx/IIcx/SE/30 3.9 MIPS at 15.67 MHz, IIfx 10 MIPS at 40 MHz).
    // The 68040 sits at 1.14 (22 MIPS at 25 MHz, 35 at 40 MHz), and Apple's
    // 68040 description states 1.3 cycles per instruction; 1 is the nearest
    // integer.  The 68040 machines build on this core too, and used to
    // inherit the 68030's 4, which ran a Quadra at a quarter of its speed.
    scheduler_set_cpi(cfg->scheduler, cfg->machine->cpu_model == CPU_MODEL_68040 ? 1 : 4);
    g_uncached_read_cycles = 0; // a board that prices uncached reads sets it after this
}

void mac030_map_mirrored(uint32_t start_page, uint32_t window_pages, uint8_t *host, uint32_t size_pages,
                         mac030_fill_fn fill, bool writable) {
    if (size_pages == 0)
        return; // a bank smaller than one page decodes nothing
    for (uint32_t i = 0; i < window_pages && start_page + i < g_page_count; i++)
        fill(start_page + i, host + ((i % size_pages) << MEM_PAGE_SHIFT), writable);
}

// Hardware RESET: ROM overlay back on, shared devices reset.
// The GLUE/MDU half of the board's /RESET net: VIA1 goes back to power-on and
// pulls Overlay high, so the memory controller uses the ROM overlay map
// again (Guide p.256), plus the devices every board shares.
//
// The 68030 PMMU used to be cleared here.  It is INSIDE THE CPU and not on
// the net, so an external chip reset must not touch it; that moved to
// cpu_hardware_reset.  The `mmu` parameter is gone
// with it.
void mac030_glue_bus_reset(config_t *cfg, mac030_rom_overlay_t *overlay) {
    mac030_rom_overlay_arm(overlay); // unconditionally: the pages may have changed since
    system_reset_common_devices(cfg);
}

// A VIA output the board does not observe.
void mac030_glue_via_output_ignored(void *context, uint8_t port, uint8_t output) {
    (void)context;
    (void)port;
    (void)output;
}

// A VIA shift-out the board does not observe.
void mac030_glue_via_shift_out_ignored(void *context, uint8_t byte) {
    (void)context;
    (void)byte;
}

// Shared IRQ callbacks — route a device's interrupt line to the CPU IPL.
void mac030_glue_scc_irq(void *context, bool active) {
    mac030_glue_update_ipl((config_t *)context, MAC030_GLUE_IRQ_SCC, active);
}
void mac030_glue_via1_irq(void *context, bool active) {
    mac030_glue_update_ipl((config_t *)context, MAC030_GLUE_IRQ_VIA1, active);
}
void mac030_glue_via2_irq(void *context, bool active) {
    mac030_glue_update_ipl((config_t *)context, MAC030_GLUE_IRQ_VIA2, active);
}

// Set/clear an IRQ source bit and re-derive the CPU IPL.  The routing itself
// is the data-driven glue_irq_routes table (mac030_glue_io.c) run by the
// shared engine (machine_irq.h).
void mac030_glue_update_ipl(config_t *cfg, int source, bool active) {
    int old_irq = cfg->rt.irq;
    int new_ipl = machine_irq_update(cfg, mac030_glue_irq_routes(), source, active);
    LOG(2, "mac030_glue_update_ipl: source=%d active=%d irq:%d->%d ipl->%d", source, active ? 1 : 0, old_irq,
        cfg->rt.irq, new_ipl);
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
void mac030_glue_slot_irq_source(config_t *cfg, int pa_bit, bool active) {
    mac030_glue_state_t *st = (mac030_glue_state_t *)cfg->machine_context;
    if (!st)
        return;
    slot_irq_source(cfg->via2, &st->slot_pa_mask, pa_bit, active);
}

void mac030_glue_nubus_slot_irq(config_t *cfg, int slot, bool active) {
    int pa_bit = slot - 0x9;
    if (pa_bit < 0 || pa_bit > 5)
        return;
    mac030_glue_slot_irq_source(cfg, pa_bit, active);
}

// ============================================================
// The shared GLUE-family substrate
// ============================================================
//
// SE/30, IIcx and IIx all bind this one substrate.  Each machine's deltas live
// in its mac030_glue_board_t (reached via cfg->machine->board) + its profile;
// the per-machine init/reset/teardown/checkpoint clones are gone.

static inline const mac030_glue_board_t *glue_board(config_t *cfg) {
    return (const mac030_glue_board_t *)cfg->machine->board;
}

static int glue_init(config_t *cfg, checkpoint_t *cp) {
    if (mac030_glue_init(cfg, cp, glue_board(cfg)) != 0)
        return -1;
    return 0;
}

static void glue_bus_reset(config_t *cfg) {
    mac030_glue_state_t *st = (mac030_glue_state_t *)cfg->machine_context;
    mac030_glue_bus_reset(cfg, &st->overlay);
}

static void glue_teardown(config_t *cfg) {
    mac030_glue_state_t *st = (mac030_glue_state_t *)cfg->machine_context;
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
    const mac030_glue_board_t *board = glue_board(cfg);
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
    .init = glue_init,
    .bus_reset = glue_bus_reset,
    .teardown = glue_teardown,
    .seed = mac_seed_rtc_pram,
    .trigger_vbl = glue_trigger_vbl,
    .nubus_slot_irq = mac030_glue_nubus_slot_irq,
    MAC_SUBSTRATE_HOST_IO,
    MAC_SUBSTRATE_MEDIA_STD,
};
