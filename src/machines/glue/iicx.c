// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iicx.c
// Macintosh IIcx machine implementation.  Shares the GLUE-driven I/O map
// and the dual-VIA / 68030 / Universal-ROM family with the SE/30, but
// replaces the slot-$E built-in video with a real NuBus slot at $9
// (defaulting to the Apple Macintosh Display Card 8•24).  See
// proposal-machine-iicx-iix.md §3.4.
//
// Diff vs se30.c at a glance (per proposal §3.4):
//   * Slot table: $9 = VIDEO with mdc_8_24 default, $A/$B = EMPTY,
//     no slot $E.
//   * Machine-ID bits: PA6 = 1, PB3 = 1 (vs SE/30 PA6 = 1, PB3 = 0).
//   * Soft-power-off via VIA2 PB2 (`v2PowerOff`); the OS writes 0
//     and we stop the scheduler so the headless run exits cleanly.
//   * Sound-jack-detect via VIA2 PB6 (`v2SndJck`) — input only.
//   * No buffer-select handling on VIA1 PA6 (no built-in framebuffer).
//
// The bulk of this file mirrors se30.c — the proper §3.1 glue030
// extraction (which would slim iicx down to ~150 LOC) is deliberately
// deferred per the proposal's "ratchet refactor" stance: ship working
// IIcx now, factor out the duplication when iix.c lands and the third
// caller arrives.

#include "mac030_glue.h"
#include "machine.h"
#include "mmu_checkpoint.h"
#include "system_config.h" // full config_t

#include "adb.h"
#include "asc.h"
#include "checkpoint_images.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h" // for cpu->mmu field
#include "debug.h"
#include "floppy.h"
#include "iicx_internal.h" // shared IIcx/IIx internals
#include "image.h"
#include "log.h"
#include "memory.h"
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

LOG_USE_CATEGORY_NAME("iicx");

// ============================================================
// Constants
// ============================================================
//
// Most address-space and IRQ constants live in iicx_internal.h so iix.c
// reuses them.  The I/O window offsets + dispatch now live with the shared
// GLUE dispatcher (mac030_glue_io.c).

// ============================================================
// Forward declarations
// ============================================================

static void iicx_via2_output(void *context, uint8_t port, uint8_t output);
static void iicx_via2_shift_out(void *context, uint8_t byte);

// ============================================================
// SoA page helper (mirrors the SE/30 helper — same logic)
// ============================================================

// ============================================================
// ROM overlay
// ============================================================

void iicx_set_rom_overlay(config_t *cfg, bool overlay) {
    mac030_glue_set_rom_overlay(cfg, &iicx_state(cfg)->rom_overlay, IICX_ROM_START, overlay);
}

// ============================================================
// Hardware reset
// ============================================================

static void iicx_reset(config_t *cfg) {
    iicx_state_t *st = iicx_state(cfg);
    mac030_glue_reset(cfg, &st->rom_overlay, IICX_ROM_START, st->mmu);
}

// ============================================================
// I/O dispatcher
// ============================================================
// The GLUE I/O dispatcher is shared with SE/30 and IIx — see
// mac030_glue_io.c.  This machine fills a mac030_glue_io_t at init and
// registers it as the I/O region's device context.

// ============================================================
// Memory layout
// ============================================================

void iicx_memory_layout_init(config_t *cfg) {
    iicx_state_t *st = iicx_state(cfg);

    uint32_t ram_size = cfg->ram_size;
    uint32_t rom_size = cfg->machine->rom_size;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);

    uint32_t ram_pages = ram_size >> PAGE_SHIFT;
    bool standard_bank = (ram_size == 1 * 1024 * 1024 || ram_size == 4 * 1024 * 1024 || ram_size == 16 * 1024 * 1024);
    uint32_t map_end_page = standard_bank ? ram_pages : (ram_pages * 2);
    for (uint32_t p = 0; p < map_end_page && (int)p < g_page_count; p++)
        mac030_fill_page(p, ram_base + ((p % ram_pages) << PAGE_SHIFT), true);

    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = IICX_ROM_START >> PAGE_SHIFT;
    uint32_t rom_end_page = IICX_ROM_END >> PAGE_SHIFT;
    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
            uint32_t offset_in_rom = (p - rom_start_page) % rom_pages;
            mac030_fill_page(p, rom_data + (offset_in_rom << PAGE_SHIFT), false);
        }
    }

    mac030_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->mem_map, IICX_IO_BASE, IICX_IO_SIZE, "IIcx I/O", &st->io_interface, &st->glue_io);

    // Populate page-table entries for any host-backed slot regions
    // registered by NuBus cards (matches the SE/30 pattern of explicit
    // se30_fill_page calls).  Without this, slot-space reads fall to the
    // unmapped-$FF path even when phys_to_host would resolve, because
    // memory_read's slow path doesn't probe phys_to_host directly.
    if (st->mmu) {
        if (st->mmu->physical_vram && st->mmu->physical_vram_size > 0) {
            uint32_t pages = st->mmu->physical_vram_size >> PAGE_SHIFT;
            uint32_t start = st->mmu->vram_phys_base >> PAGE_SHIFT;
            for (uint32_t i = 0; i < pages && (int)(start + i) < g_page_count; i++)
                mac030_fill_page(start + i, st->mmu->physical_vram + (i << PAGE_SHIFT), /*writable*/ true);
        }
        if (st->mmu->physical_vrom && st->mmu->physical_vrom_size > 0) {
            uint32_t pages = st->mmu->physical_vrom_size >> PAGE_SHIFT;
            uint32_t start = st->mmu->vrom_phys_base >> PAGE_SHIFT;
            for (uint32_t i = 0; i < pages && (int)(start + i) < g_page_count; i++)
                mac030_fill_page(start + i, st->mmu->physical_vrom + (i << PAGE_SHIFT), /*writable*/ false);
        }

        // Mode-24 (24-bit Memory Manager) NuBus slot windows: each slot s
        // ($9..$E) has a 1 MB region at $00s00000 that mirrors the start of
        // its 32-bit slot space at $Fs000000. On real Mac II family hardware
        // the GLUE / BBU chip decodes both ranges to the same NuBus slot;
        // the IIcx/IIx/SE/30 ROM is 24-bit-only, so the OS routinely uses
        // these low aliases. Without this mirror, a card's framebuffer that
        // QuickDraw reaches via a Mode-24 ScrnBase ($00900080) lands in
        // unmapped low memory.
        if (st->mmu->physical_vram) {
            uint32_t base32 = st->mmu->vram_phys_base;
            uint32_t high = base32 & 0xFF000000u;
            if (high >= 0xF9000000u && high <= 0xFE000000u) {
                int slot = (int)((base32 >> 24) & 0xFu);
                uint32_t mode24_base = (uint32_t)slot << 20; // $00s00000
                uint32_t alias_bytes = 0x100000u; // 1 MB Mode-24 slot window
                if (alias_bytes > st->mmu->physical_vram_size)
                    alias_bytes = st->mmu->physical_vram_size;
                uint32_t alias_pages = alias_bytes >> PAGE_SHIFT;
                uint32_t start = mode24_base >> PAGE_SHIFT;
                for (uint32_t i = 0; i < alias_pages && (int)(start + i) < g_page_count; i++)
                    mac030_fill_page(start + i, st->mmu->physical_vram + (i << PAGE_SHIFT), /*writable*/ true);
            }
        }
    }

    st->rom_overlay = false;
    iicx_set_rom_overlay(cfg, true);
}

// ============================================================
// Interrupt routing
// ============================================================

// ============================================================
// VIA / SCC callbacks
// ============================================================

// VIA1 outputs.  Mostly identical to SE/30 except for PA6 — IIcx has no
// built-in framebuffer, so PA6 is a free port pin we ignore.
void iicx_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    iicx_state_t *st = iicx_state(cfg);

    if (port == 0) {
        if (st->floppy)
            floppy_set_sel_signal(st->floppy, (output & 0x20) != 0);
        iicx_set_rom_overlay(cfg, (output & 0x10) != 0);
    } else {
        if (st->adb) {
            uint8_t st_mask = 0x30;
            uint8_t old_st = st->last_port_b & st_mask;
            uint8_t new_st = output & st_mask;
            if (new_st != old_st)
                adb_port_b_output(st->adb, output);
        }
        st->last_port_b = output;
        if (cfg->rtc)
            rtc_input(cfg->rtc, (output >> 2) & 1, (output >> 1) & 1, output & 1);
    }
}

void iicx_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    iicx_state_t *st = iicx_state(cfg);
    if (st->adb)
        adb_shift_byte(st->adb, byte);
}

// VIA2 outputs.  Adds the IIcx-specific soft-power-off on PB2.  When the
// OS pulls v2PowerOff low the PSU shuts down — v1 emulator equivalent:
// stop the scheduler so the headless run exits cleanly.
static void iicx_via2_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    iicx_state_t *st = iicx_state(cfg);
    if (port != 1)
        return;

    bool new_pb2 = (output & 0x04) != 0;
    if (!st->soft_power_armed && new_pb2) {
        st->soft_power_armed = true;
    } else if (st->soft_power_armed && !new_pb2) {
        LOG(1, "IIcx soft power-off (VIA2 PB2 = 0)");
        if (cfg->scheduler)
            scheduler_stop(cfg->scheduler);
    }
    st->last_via2_port_b = output;
}

static void iicx_via2_shift_out(void *context, uint8_t byte) {
    (void)context;
    (void)byte;
}

// ============================================================
// VBL trigger
// ============================================================

// Unlike SE/30 we do not directly poke a slot-IRQ bit on VIA2 PA — the
// per-card on_vbl handler does that via nubus_assert_irq.  The family
// CA1 heartbeat still fires on every VBL.
static void iicx_trigger_vbl(config_t *cfg) {
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

static const char *const iicx_video_cards[] = {"mdc_8_24", NULL};

static const nubus_slot_decl_t iicx_slots[] = {
    {.slot = 0x9, .kind = NUBUS_SLOT_VIDEO, .available_cards = iicx_video_cards, .default_card = "mdc_8_24"},
    {.slot = 0xA, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xB, .kind = NUBUS_SLOT_EMPTY},
    {0},
};

// ============================================================
// Init / Teardown
// ============================================================

// Machine-ID straps: PA6 = 1, PB3 = 1 (IIcx); VIA2 slot-IRQ PA lines idle
// high; PB6 reports the sound jack inserted (active-low).
static void iicx_setup_id(config_t *cfg) {
    via_input(cfg->via2, 1, 3, 1);
    via_input(cfg->via2, 0, 0, 1);
    via_input(cfg->via2, 0, 1, 1);
    via_input(cfg->via2, 0, 2, 1);
    via_input(cfg->via2, 0, 3, 1);
    via_input(cfg->via2, 0, 4, 1);
    via_input(cfg->via2, 0, 5, 1);
    via_input(cfg->via2, 1, 6, 0);
    via_input_c(cfg->via2, 0, 0, 1);
    via_input_c(cfg->via2, 0, 1, 1);
    via_input_c(cfg->via2, 1, 1, 1);
}

// IIcx board: GLUE family, three NuBus slots, VIA2 PB2 soft-power.
static const mac030_glue_board_t iicx_board = {
    .via1_output = iicx_via1_output,
    .via1_shift_out = iicx_via1_shift_out,
    .via2_output = iicx_via2_output,
    .via2_shift_out = iicx_via2_shift_out,
    .setup_id = iicx_setup_id,
    .slots = iicx_slots,
    .bus_err_lo = 0xF9000000,
    .bus_err_hi = 0xFEFFFFFF,
    .memory_layout = iicx_memory_layout_init,
};

static void iicx_init(config_t *cfg, checkpoint_t *checkpoint) {
    mac030_glue_init(cfg, checkpoint, &iicx_board);
}

static void iicx_teardown(config_t *cfg) {
    iicx_state_t *st = iicx_state(cfg);
    // Shared GLUE delete-chain (scheduler_stop → mmu → floppy → asc → adb →
    // scsi → via2 → via1 → scc → rtc → scheduler → cpu → mem_map → debugger).
    mac030_glue_teardown(cfg, st ? st->adb : NULL, st ? st->asc : NULL, st ? st->floppy : NULL, st ? st->mmu : NULL);
    if (st) {
        free(st);
        cfg->machine_context = NULL;
    }
}

// ============================================================
// Checkpoint
// ============================================================

static void iicx_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
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
    mac_checkpoint_save_images(cfg, cp);
    scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(st->asc, cp);
    floppy_checkpoint(st->floppy, cp);
    mmu_checkpoint_save(st->mmu, cp);
}

// ============================================================
// Machine descriptor
// ============================================================

static const uint32_t iicx_ram_options_kb[] = {1024, 2048, 4096, 5120, 8192, 16384, 32768, 65536, 131072, 0};

static const struct floppy_slot iicx_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {.label = "External FD1", .kind = FLOPPY_HD},
    {0},
};

static const struct scsi_slot iicx_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

static const machine_substrate_t iicx_substrate = {
    .init = iicx_init,
    .reset = iicx_reset,
    .teardown = iicx_teardown,
    .checkpoint_save = iicx_checkpoint_save,
    .update_ipl = mac030_glue_update_ipl,
    .trigger_vbl = iicx_trigger_vbl,
};

const hw_profile_t machine_iicx = {
    .name = "Macintosh IIcx",
    .id = "iicx",

    .cpu_model = 68030,
    .freq = 15667200,
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x8000000, // 128 MB
    .rom_size = 0x040000, // 256 KB

    .ram_options = iicx_ram_options_kb,
    .floppy_slots = iicx_floppy_slots,
    .scsi_slots = iicx_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 3,
    // The IIcx has no built-in video — its primary display comes from
    // a NuBus video card seated in slot $9 (Apple Display Card 8•24 by
    // default).  That card needs Apple-341-0868.vrom to declare itself
    // to the Slot Manager (without it, slot scan finds an empty slot
    // and the boot ROM can't bring up a framebuffer).  We mark
    // needs_vrom = true so the config dialog asks the user for a VROM
    // file; the JMFB card driver picks it up from /opfs/images/vrom/
    // by its canonical name during card init.

    .nubus_slots = iicx_slots,

    .substrate = &iicx_substrate,
};
