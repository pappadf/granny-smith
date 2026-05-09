// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iix.c
// Macintosh IIx machine implementation.  Sister of iicx.c — shares the
// GLUE-driven I/O map, dual-VIA / 68030 / Universal-ROM family, and the
// page table / ROM overlay helpers via iicx_internal.h.  See
// proposal-machine-iicx-iix.md §3.4.
//
// Diff vs iicx.c at a glance:
//   * Slot table: six NuBus slots ($9..$E) — slot $9 is VIDEO with
//     mdc_8_24 default, the rest are EMPTY in v1.
//   * Machine-ID bits: PA6 = 0, PB3 = 0 (vs IIcx PA6 = 1, PB3 = 1).
//   * No soft-power-off (PB2 is a free pin); no sound-jack-detect.
//   * Otherwise identical: same VIA1 callbacks, same I/O dispatcher,
//     same memory layout, same VBL trigger.

#include "machine.h"
#include "system_config.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "debug.h"
#include "floppy.h"
#include "glue030.h"
#include "iicx_internal.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "memory_internal.h"
#include "mmu.h"
#include "nubus.h"
#include "rom.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "via.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("iix");

// ============================================================
// Forward declarations
// ============================================================

static void iix_via2_output(void *context, uint8_t port, uint8_t output);
static void iix_via2_shift_out(void *context, uint8_t byte);
static void iix_via2_irq(void *context, bool active);
static void iix_init(config_t *cfg, checkpoint_t *checkpoint);
static void iix_teardown(config_t *cfg);
static void iix_reset(config_t *cfg);
static void iix_checkpoint_save(config_t *cfg, checkpoint_t *cp);
static void iix_trigger_vbl(config_t *cfg);

// ============================================================
// Hardware reset
// ============================================================

static void iix_reset(config_t *cfg) {
    iicx_state_t *st = iicx_state(cfg);
    st->rom_overlay = false;
    iicx_set_rom_overlay(cfg, true);
    if (st->mmu) {
        st->mmu->enabled = false;
        st->mmu->tc = 0;
        mmu_invalidate_tlb(st->mmu);
    }
}

// ============================================================
// VIA2 / SCC callbacks
// ============================================================
//
// VIA1 callbacks come from iicx_internal.h.  VIA2 differs because the
// IIx has no soft-power-off and no sound-jack-detect.

static void iix_via2_output(void *context, uint8_t port, uint8_t output) {
    (void)context;
    (void)port;
    (void)output;
    // No machine-specific outputs on IIx VIA2.
}

static void iix_via2_shift_out(void *context, uint8_t byte) {
    (void)context;
    (void)byte;
}

static void iix_via2_irq(void *context, bool active) {
    iicx_update_ipl((config_t *)context, IICX_IRQ_VIA2, active);
}

// ============================================================
// VBL trigger
// ============================================================

static void iix_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via2, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via2, 0, 0, 1);
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

// ============================================================
// Slot table
// ============================================================

static const char *const iix_video_cards[] = {"mdc_8_24", NULL};

static const nubus_slot_decl_t iix_slots[] = {
    {.slot = 0x9, .kind = NUBUS_SLOT_VIDEO, .available_cards = iix_video_cards, .default_card = "mdc_8_24"},
    {.slot = 0xA, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xB, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xC, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xD, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xE, .kind = NUBUS_SLOT_EMPTY},
    {0},
};

// ============================================================
// Init / Teardown
// ============================================================

static void iix_init(config_t *cfg, checkpoint_t *checkpoint) {
    iicx_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;
    st->last_port_b = 0x30;
    st->last_via2_port_b = 0xFF;

    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, checkpoint);
    cfg->cpu = cpu_init(CPU_MODEL_68030, checkpoint);
    cfg->scheduler = scheduler_init(cfg->cpu, checkpoint);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    scheduler_set_cpi(cfg->scheduler, 4, 4);
    if (checkpoint)
        system_read_checkpoint_data(checkpoint, &cfg->irq, sizeof(cfg->irq));

    cfg->rtc = rtc_init(cfg->scheduler, checkpoint);
    cfg->scc = scc_init(NULL, cfg->scheduler, iicx_scc_irq, cfg, checkpoint);
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    cfg->via1 = via_init(NULL, cfg->scheduler, 20, "via1", iicx_via1_output, iicx_via1_shift_out, iicx_via1_irq, cfg,
                         checkpoint);
    cfg->via2 =
        via_init(NULL, cfg->scheduler, 20, "via2", iix_via2_output, iix_via2_shift_out, iix_via2_irq, cfg, checkpoint);
    rtc_set_via(cfg->rtc, cfg->via1);

    // Machine ID: PA6 = 0, PB3 = 0 (IIx).  Default port-A input is
    // 0xF7 (bit 6 = 1); pull bit 6 low via via_input.
    via_input(cfg->via1, 0, 6, 0);
    via_input(cfg->via2, 1, 3, 0);
    // VIA2 PA bits idle high (no slot IRQ).
    via_input(cfg->via2, 0, 0, 1);
    via_input(cfg->via2, 0, 1, 1);
    via_input(cfg->via2, 0, 2, 1);
    via_input(cfg->via2, 0, 3, 1);
    via_input(cfg->via2, 0, 4, 1);
    via_input(cfg->via2, 0, 5, 1);
    via_input_c(cfg->via2, 0, 0, 1);
    via_input_c(cfg->via2, 0, 1, 1);
    via_input_c(cfg->via2, 1, 1, 1);

    st->adb = adb_init(cfg->via1, cfg->scheduler, checkpoint);
    cfg->adb = st->adb;

    if (checkpoint)
        glue030_checkpoint_restore_images(cfg, checkpoint);

    cfg->scsi = scsi_init(NULL, checkpoint);
    scsi_set_via(cfg->scsi, cfg->via2);
    setup_images(cfg);

    st->asc = asc_init(NULL, cfg->scheduler, checkpoint);
    asc_set_via(st->asc, cfg->via2);
    st->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, checkpoint);
    cfg->floppy = st->floppy;

    st->via1_iface = via_get_memory_interface(cfg->via1);
    st->via2_iface = via_get_memory_interface(cfg->via2);
    st->scc_iface = scc_get_memory_interface(cfg->scc);
    st->scsi_iface = scsi_get_memory_interface(cfg->scsi);
    st->asc_iface = asc_get_memory_interface(st->asc);
    st->floppy_iface = floppy_get_memory_interface(st->floppy);

    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint32_t ram_size = cfg->ram_size;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    st->mmu = mmu_init(ram_base, ram_size, cfg->machine->ram_max, rom_data, rom_size, IICX_ROM_START, IICX_ROM_END);
    assert(st->mmu != NULL);
    g_mmu = st->mmu;
    cfg->cpu->mmu = st->mmu;
    st->mmu->tt1 = 0xF00F8043;

    cfg->nubus = nubus_init(cfg, iix_slots, checkpoint);

    // Bus error window covers all six slots.
    memory_set_bus_error_range(cfg->mem_map, 0xF9000000, 0xFEFFFFFF);

    iicx_memory_layout_init(cfg);

    if (checkpoint) {
        system_read_checkpoint_data(checkpoint, &st->mmu->tc, sizeof(st->mmu->tc));
        system_read_checkpoint_data(checkpoint, &st->mmu->crp, sizeof(st->mmu->crp));
        system_read_checkpoint_data(checkpoint, &st->mmu->srp, sizeof(st->mmu->srp));
        system_read_checkpoint_data(checkpoint, &st->mmu->tt0, sizeof(st->mmu->tt0));
        system_read_checkpoint_data(checkpoint, &st->mmu->tt1, sizeof(st->mmu->tt1));
        system_read_checkpoint_data(checkpoint, &st->mmu->mmusr, sizeof(st->mmu->mmusr));
        system_read_checkpoint_data(checkpoint, &st->mmu->enabled, sizeof(st->mmu->enabled));
        mmu_invalidate_tlb(st->mmu);
        g_mmu = st->mmu;
        cfg->cpu->mmu = st->mmu;
        via_redrive_outputs(cfg->via1);
        via_redrive_outputs(cfg->via2);
    }

    cfg->debugger = debug_init();
    scheduler_register_vbl_type(cfg->scheduler, cfg);
    scheduler_start(cfg->scheduler);
    if (!checkpoint) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
}

static void iix_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    iicx_state_t *st = iicx_state(cfg);
    if (st) {
        if (st->mmu) {
            mmu_delete(st->mmu);
            st->mmu = NULL;
        }
        if (st->floppy) {
            floppy_delete(st->floppy);
            st->floppy = NULL;
            cfg->floppy = NULL;
        }
        if (st->asc) {
            asc_delete(st->asc);
            st->asc = NULL;
        }
        if (st->adb) {
            adb_delete(st->adb);
            st->adb = NULL;
            cfg->adb = NULL;
        }
    }
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
    if (st) {
        free(st);
        cfg->machine_context = NULL;
    }
}

// ============================================================
// Checkpoint
// ============================================================

static void iix_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    iicx_state_t *st = iicx_state(cfg);
    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp);
    scheduler_checkpoint(cfg->scheduler, cp);
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    via_checkpoint(cfg->via1, cp);
    via_checkpoint(cfg->via2, cp);
    adb_checkpoint(st->adb, cp);
    glue030_checkpoint_save_images(cfg, cp);
    scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(st->asc, cp);
    floppy_checkpoint(st->floppy, cp);
    system_write_checkpoint_data(cp, &st->mmu->tc, sizeof(st->mmu->tc));
    system_write_checkpoint_data(cp, &st->mmu->crp, sizeof(st->mmu->crp));
    system_write_checkpoint_data(cp, &st->mmu->srp, sizeof(st->mmu->srp));
    system_write_checkpoint_data(cp, &st->mmu->tt0, sizeof(st->mmu->tt0));
    system_write_checkpoint_data(cp, &st->mmu->tt1, sizeof(st->mmu->tt1));
    system_write_checkpoint_data(cp, &st->mmu->mmusr, sizeof(st->mmu->mmusr));
    system_write_checkpoint_data(cp, &st->mmu->enabled, sizeof(st->mmu->enabled));
}

// ============================================================
// Machine descriptor
// ============================================================

static const uint32_t iix_ram_options_kb[] = {1024, 2048, 4096, 5120, 8192, 16384, 32768, 65536, 131072, 0};

static const struct floppy_slot iix_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {.label = "External FD1", .kind = FLOPPY_HD},
    {0},
};

static const struct scsi_slot iix_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

const hw_profile_t machine_iix = {
    .name = "Macintosh IIx",
    .id = "iix",

    .cpu_model = 68030,
    .freq = 15667200,
    .mmu_present = true,
    .fpu_present = true,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x8000000, // 128 MB
    .rom_size = 0x040000, // 256 KB

    .ram_options = iix_ram_options_kb,
    .floppy_slots = iix_floppy_slots,
    .scsi_slots = iix_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 3,
    // Same reasoning as IIcx: no built-in video, so the slot card's
    // VROM (Apple-341-0868.vrom for the default JMFB card) must be
    // present.  See iicx.c for the full comment.
    .needs_vrom = true,

    .via_count = 2,
    .has_adb = true,
    .has_nubus = true,
    .nubus_slot_count = 6,

    .init = iix_init,
    .reset = iix_reset,
    .teardown = iix_teardown,
    .checkpoint_save = iix_checkpoint_save,
    .checkpoint_restore = NULL,
    .memory_layout_init = iicx_memory_layout_init,
    .update_ipl = iicx_update_ipl,
    .trigger_vbl = iix_trigger_vbl,
    .display = NULL,
};
