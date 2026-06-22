// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue.c
// Shared GLUE-family lifecycle leaves — see mac030_glue.h.

#include "mac030_glue.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint_images.h"
#include "cpu.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "via.h"

#include <assert.h>
#include <stdlib.h>

LOG_USE_CATEGORY_NAME("setup");

// Construct the GLUE peripheral set in canonical order — see header.
void mac030_glue_build_peripherals(config_t *cfg, checkpoint_t *cp, mac030_glue_state_t *st) {
    st->adb = adb_init(cfg->via1, cfg->scheduler, cp);
    cfg->adb = st->adb;

    // Restore the image list before devices that reference it.
    if (cp)
        mac_checkpoint_restore_images(cfg, cp);

    cfg->scsi = scsi_init(NULL, cp);
    scsi_set_via(cfg->scsi, cfg->via2);
    setup_images(cfg);

    st->asc = asc_init(NULL, cfg->scheduler, cp);
    asc_set_via(st->asc, cfg->via2);

    st->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, cp);
    cfg->floppy = st->floppy;

    mac030_glue_io_bind(&st->glue_io, cfg, st->asc, st->floppy);
}

// Create + attach the 68030 PMMU over the GLUE map — see header.
struct mmu_state *mac030_glue_build_mmu(config_t *cfg) {
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint32_t ram_size = cfg->ram_size;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    mmu_state_t *mmu =
        mmu_init(ram_base, ram_size, cfg->machine->ram_max, rom_data, rom_size, 0x40000000UL, 0x50000000UL);
    assert(mmu != NULL);
    g_mmu = mmu;
    cpu_attach_mmu(cfg->cpu, mmu);
    return mmu;
}

// Finish init: debugger, scheduler start, cold-boot IRQ/IPL reset.
void mac030_glue_finish(config_t *cfg, checkpoint_t *cp) {
    cfg->debugger = debug_init();
    scheduler_start(cfg->scheduler);
    if (!cp) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
}

// The shared GLUE init — board-driven (see header).  Order is the canonical
// se30/iicx/iix init spine; per-machine deltas come from the board's data and
// hooks.  TT1 is uniform across the GLUE family ($F0..$FF supervisor identity);
// it is set right after the PMMU is built (no MMU walk happens before
// scheduler_start, so the exact moment is immaterial).
void mac030_glue_init(config_t *cfg, checkpoint_t *cp, const mac030_glue_board_t *board) {
    mac030_glue_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;
    st->last_port_b = 0x30; // ADB ST1:ST0 idle = 11
    st->last_via2_port_b = 0xFF; // PB2 starts high (IIcx soft-power; unused elsewhere)

    mac030_build_core(cfg, cp);
    if (board->pre_devices)
        board->pre_devices(cfg);
    if (cp)
        system_read_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));

    cfg->rtc = rtc_init(cfg->scheduler, cp, true);
    cfg->scc = scc_init(NULL, cfg->scheduler, mac030_glue_scc_irq, cfg, cp);
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    cfg->via1 = via_init(NULL, cfg->scheduler, 20, "via1", board->via1_output, board->via1_shift_out,
                         mac030_glue_via1_irq, cfg, cp);
    cfg->via2 = via_init(NULL, cfg->scheduler, 20, "via2", board->via2_output, board->via2_shift_out,
                         mac030_glue_via2_irq, cfg, cp);
    rtc_set_via(cfg->rtc, cfg->via1);

    board->setup_id(cfg);

    mac030_glue_build_peripherals(cfg, cp, st);

    st->mmu = mac030_glue_build_mmu(cfg);
    st->mmu->tt1 = 0xF00F8043; // supervisor-only identity map for NuBus $F0..$FF

    cfg->nubus = nubus_init(cfg, board->slots, cp);
    if (board->post_nubus)
        board->post_nubus(cfg);

    memory_set_bus_error_range(cfg->mem_map, board->bus_err_lo, board->bus_err_hi);
    board->memory_layout(cfg);

    if (cp) {
        if (board->ckpt_restore_extra)
            board->ckpt_restore_extra(cfg, cp);
        mmu_checkpoint_restore(st->mmu, cp);
        mmu_invalidate_tlb(st->mmu);
        g_mmu = st->mmu;
        cpu_attach_mmu(cfg->cpu, st->mmu);
        via_redrive_outputs(cfg->via1);
        via_redrive_outputs(cfg->via2);
    }

    mac030_glue_finish(cfg, cp);
}

// Build the shared II-family construction prefix.  Reads the CPU model from
// the profile (single source of truth — §1.3), not a hardcoded constant.
void mac030_build_core(config_t *cfg, checkpoint_t *cp) {
    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, cp);
    cfg->cpu = cpu_init(cfg->machine->cpu_model, cp);
    cfg->scheduler = scheduler_init(cfg->cpu, cp);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    scheduler_set_cpi(cfg->scheduler, 4, 4);
}

// Populate one page in the AoS table + SoA fast-path arrays.  Read-only pages
// leave the write SoA entries at their zero-initialised value (slow path).
void mac030_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    if ((int)page_index >= g_page_count)
        return;
    g_page_table[page_index].host_base = host_ptr;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = writable;
    uint32_t guest_base = page_index << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;
    if (g_user_read)
        g_user_read[page_index] = adjusted;
    if (writable) {
        if (g_supervisor_write)
            g_supervisor_write[page_index] = adjusted;
        if (g_user_write)
            g_user_write[page_index] = adjusted;
    }
}

// Toggle the ROM overlay at $00000000.  overlay=true maps the ROM image
// (read-only); overlay=false restores RAM (writable).  No-op if unchanged.
void mac030_glue_set_rom_overlay(config_t *cfg, bool *overlay_flag, uint32_t rom_start, bool on) {
    if (*overlay_flag == on)
        return;
    *overlay_flag = on;
    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = rom_start >> PAGE_SHIFT;
    if (on) {
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++)
            mac030_fill_page(p, g_page_table[rom_start_page + p].host_base, false);
    } else {
        uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++)
            mac030_fill_page(p, ram_base + (p << PAGE_SHIFT), true);
    }
}

// Hardware RESET: ROM overlay back on, MMU disabled.
void mac030_glue_reset(config_t *cfg, bool *overlay_flag, uint32_t rom_start, struct mmu_state *mmu) {
    *overlay_flag = false; // force the set_rom_overlay toggle below
    mac030_glue_set_rom_overlay(cfg, overlay_flag, rom_start, true);
    if (mmu) {
        mmu->enabled = false;
        mmu->tc = 0;
        mmu_invalidate_tlb(mmu);
    }
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

// Set/clear an IRQ source bit and re-derive the CPU IPL.  Verbatim from the
// (identical) se30_update_ipl / iicx_update_ipl.
void mac030_glue_update_ipl(config_t *cfg, int source, bool active) {
    int old_irq = cfg->irq;
    if (active)
        cfg->irq |= source;
    else
        cfg->irq &= ~source;

    // Highest active source wins.
    uint32_t new_ipl;
    if (cfg->irq & MAC030_GLUE_IRQ_NMI)
        new_ipl = 7;
    else if (cfg->irq & MAC030_GLUE_IRQ_SCC)
        new_ipl = 4;
    else if (cfg->irq & MAC030_GLUE_IRQ_VIA2)
        new_ipl = 2;
    else if (cfg->irq & MAC030_GLUE_IRQ_VIA1)
        new_ipl = 1;
    else
        new_ipl = 0;

    cpu_set_ipl(cfg->cpu, new_ipl);
    LOG(2, "mac030_glue_update_ipl: source=%d active=%d irq:%d->%d ipl->%d", source, active ? 1 : 0, old_irq, cfg->irq,
        new_ipl);
    cpu_reschedule();
}

// Family-shared teardown delete-chain.  Order matches the (identical)
// per-machine teardowns; NuBus cards are already gone (system_destroy calls
// nubus_delete before machine teardown — §6.2 ownership invariant).
void mac030_glue_teardown(config_t *cfg, struct adb *adb, struct asc *asc, struct floppy *floppy,
                          struct mmu_state *mmu) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);

    // Machine-owned devices (in the machine's private state).
    if (mmu)
        mmu_delete(mmu); // also clears g_mmu if it matches
    if (floppy) {
        floppy_delete(floppy);
        cfg->floppy = NULL;
    }
    if (asc)
        asc_delete(asc);
    if (adb) {
        adb_delete(adb);
        cfg->adb = NULL;
    }

    // config_t-owned devices.
    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
    }
    if (cfg->via2) {
        via_delete(cfg->via2);
        cfg->via2 = NULL;
    }
    if (cfg->via1) {
        via_delete(cfg->via1);
        cfg->via1 = NULL;
    }
    if (cfg->scc) {
        scc_delete(cfg->scc);
        cfg->scc = NULL;
    }
    if (cfg->rtc) {
        rtc_delete(cfg->rtc);
        cfg->rtc = NULL;
    }
    if (cfg->scheduler) {
        scheduler_delete(cfg->scheduler);
        cfg->scheduler = NULL;
    }
    if (cfg->cpu) {
        cpu_delete(cfg->cpu);
        cfg->cpu = NULL;
    }
    if (cfg->mem_map) {
        memory_map_delete(cfg->mem_map);
        cfg->mem_map = NULL;
    }
    if (cfg->debugger) {
        debug_cleanup(cfg->debugger);
        cfg->debugger = NULL;
    }
}
