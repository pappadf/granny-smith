// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iici.c
// Macintosh IIci ("Aurora", 25 MHz 68030, 1989) machine implementation.
// Architecturally the IIcx with VIA2 replaced by the RBV chip and built-in
// video reading from the slot-$B framebuffer aperture.  See
// proposal-machine-iici-iisi.md and docs/machines/mdu/iici.md.
//
// Diff vs iicx.c at a glance:
//   * I/O island: VIA2 ($2000) dropped; VDAC ($24000) and RBV ($26000)
//     added.  The mirror mask widens to $3FFFF so RBV/VDAC decode
//     distinctly from the SCSI windows.
//   * ROM base is $40800000 (MDUtable), not $40000000.
//   * Interrupts: RBV's combined IFR drives IPL 2 (replacing VIA2); SCSI
//     IRQ/DRQ route through the RBV via scsi_set_irq_callback (no VIA2).
//   * Soft power-off: RBV RvPowerOff (RvDataB bit 2) instead of VIA2 PB2.
//   * Built-in video: a NuBus pseudo-card in slot $0 whose framebuffer the
//     machine maps at $FBB00000; depth follows RvMonP via a mode callback.
//   * ADB / RTC use the classic VIA1 path (no Egret) — identical to IIcx.

#include "mac030_glue.h"
#include "mac_host_io.h"
#include "machine.h"
#include "mdu.h" // mdu_substrate + mac030_mdu_board_t
#include "mmu_checkpoint.h"
#include "system_config.h"

#include "adb.h"
#include "asc.h"
#include "builtin_rbv_video.h"
#include "checkpoint_images.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h" // for cpu->mmu field
#include "debug.h"
#include "floppy.h"
#include "iici_internal.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rbv.h"
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

LOG_USE_CATEGORY_NAME("iici");

// ============================================================
// I/O island offsets (private to the dispatcher)
// ============================================================

// ============================================================
// SoA page helper (same logic as the SE/30 / IIcx helper)
// ============================================================

// ============================================================
// ROM overlay
// ============================================================

static void iici_set_rom_overlay(config_t *cfg, bool overlay) {
    mac030_glue_set_rom_overlay(cfg, &iici_state(cfg)->rom_overlay, IICI_ROM_START, overlay);
}

// ============================================================
// I/O dispatcher (MDU island — IIcx map minus VIA2, plus VDAC + RBV)
// ============================================================

// The MDU I/O dispatcher is shared with the IIsi — see mdu_io.c.  This
// machine fills an mdu_io_t at init and registers it as the I/O region's
// device context.

// ============================================================
// Memory layout
// ============================================================

static void iici_memory_layout_init(config_t *cfg) {
    iici_state_t *st = iici_state(cfg);

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
    uint32_t rom_start_page = IICI_ROM_START >> PAGE_SHIFT;
    uint32_t rom_end_page = IICI_ROM_END >> PAGE_SHIFT;
    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
            uint32_t offset_in_rom = (p - rom_start_page) % rom_pages;
            mac030_fill_page(p, rom_data + (offset_in_rom << PAGE_SHIFT), false);
        }
    }

    mac030_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->mem_map, IICI_IO_BASE, IICI_IO_SIZE, "IIci I/O", &st->io_interface, &st->mdu_io);

    // Wire the built-in framebuffer (registered via mmu_register_vram) and
    // its Mode-24 slot-$B alias into the page table — same machinery as the
    // IIcx VRAM/Mode-24 handling so QuickDraw's 32-bit ($FBB08000) and
    // 24-bit ($00B08000) screen-base writes both land in the card buffer.
    if (st->mmu) {
        if (st->mmu->physical_vram && st->mmu->physical_vram_size > 0) {
            uint32_t pages = st->mmu->physical_vram_size >> PAGE_SHIFT;
            uint32_t start = st->mmu->vram_phys_base >> PAGE_SHIFT;
            for (uint32_t i = 0; i < pages && (int)(start + i) < g_page_count; i++)
                mac030_fill_page(start + i, st->mmu->physical_vram + (i << PAGE_SHIFT), /*writable*/ true);

            uint32_t base32 = st->mmu->vram_phys_base;
            uint32_t high = base32 & 0xFF000000u;
            if (high >= 0xF9000000u && high <= 0xFE000000u) {
                int slot = (int)((base32 >> 24) & 0xFu);
                uint32_t mode24_base = (uint32_t)slot << 20; // $00s00000
                uint32_t alias_bytes = st->mmu->physical_vram_size;
                if (alias_bytes > 0x100000u)
                    alias_bytes = 0x100000u; // 1 MB Mode-24 slot window
                uint32_t alias_pages = alias_bytes >> PAGE_SHIFT;
                uint32_t start24 = mode24_base >> PAGE_SHIFT;
                for (uint32_t i = 0; i < alias_pages && (int)(start24 + i) < g_page_count; i++)
                    mac030_fill_page(start24 + i, st->mmu->physical_vram + (i << PAGE_SHIFT), /*writable*/ true);
            }
        }
    }

    st->rom_overlay = false;
    iici_set_rom_overlay(cfg, true);
}

// ============================================================
// Interrupt routing
// ============================================================

// ============================================================
// RBV / SCC / SCSI callbacks
// ============================================================

// RBV combined interrupt → 68030 IPL 2 (replaces the IIcx VIA2 source).
static void iici_rbv_irq(void *context, bool active) {
    mac030_glue_update_ipl((config_t *)context, IICI_IRQ_RBV, active);
}

// RBV soft power-off (RvPowerOff falling edge) — stop the scheduler so the
// headless run exits cleanly, matching the IIcx VIA2 PB2 behaviour.
static void iici_power_off(void *context) {
    config_t *cfg = (config_t *)context;
    LOG(1, "IIci soft power-off (RBV RvPowerOff)");
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
}

// RBV depth change (RvMonP bits 0-2) → reshape the built-in video display.
static void iici_rbv_mode(void *context, int depth_code) {
    config_t *cfg = (config_t *)context;
    iici_state_t *st = iici_state(cfg);
    builtin_rbv_video_set_depth(st->video_card, depth_code);
}

// SCSI IRQ/DRQ → RBV flag bits (RvSCSIRQ / RvSCSIDRQ).  The IIci routes
// SCSI through the RBV, not a VIA2 — so we use scsi_set_irq_callback like
// the IIfx rather than scsi_set_via.
static void iici_scsi_irq(void *context, bool irq, bool drq) {
    config_t *cfg = (config_t *)context;
    iici_state_t *st = iici_state(cfg);
    if (!st->rbv)
        return;
    rbv_set_scsi_irq(st->rbv, irq);
    rbv_set_scsi_drq(st->rbv, drq);
}

// ASC interrupt output → RBV sound flag (RvSndIRQ, RvIFR bit 4).  The RBV
// emulates the VIA2-CB1 dispatch slot the shared OS interrupt code expects
// (jASCInt = Via2DT+4*ifCB1); level 2 like every VIA2/RBV sound source.
static void iici_asc_irq(void *context, bool active) {
    rbv_set_snd_irq((rbv_t *)context, active);
}

// VIA1 outputs.  Identical to the IIcx: floppy head-select + ROM overlay on
// port A; ADB shift-state filtering + classic RTC on port B.
static void iici_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    iici_state_t *st = iici_state(cfg);

    if (port == 0) {
        if (st->floppy)
            floppy_set_sel_signal(st->floppy, (output & 0x20) != 0);
        iici_set_rom_overlay(cfg, (output & 0x10) != 0);
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

static void iici_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    iici_state_t *st = iici_state(cfg);
    if (st->adb)
        adb_shift_byte(st->adb, byte);
}

// ============================================================
// Slot table
// ============================================================
//
// Built-in RBV video is "slot $0" to the Slot Manager, but the slot table
// is sentinel-terminated by slot == 0, so we seat the card at our internal
// NuBus slot $B — the slot whose address space actually holds the
// framebuffer ($FBxxxxxx / VideoInfoMDU's PRAM slot $0B).  The slot index
// is an implementation detail (the boot ROM drives built-in video from the
// baked-in VideoInfoMDU, not by scanning this bus; the RBV video VBL uses
// logical slot 0 = RvIRQ0 independently).  Slots $C/$D/$E are the
// user-visible NuBus expansion slots (empty in v1).
static const nubus_slot_decl_t iici_slots[] = {
    {.slot = 0xB, .kind = NUBUS_SLOT_BUILTIN, .builtin_card_id = "builtin_rbv_video"},
    {.slot = 0xC, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xD, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xE, .kind = NUBUS_SLOT_EMPTY},
    {0},
};

// The IIci board descriptor (proposal §4.2.2): MDU+RBV hardware data, consumed
// at init by the shared helpers.  ROM at $40800000; the 18-bit $40000 I/O
// mirror; the shared MDU window table.
static const mac030_board_desc_t iici_board = {
    .chipset = "MDU+RBV",
    .rom_base = IICI_ROM_START,
    .rom_end = IICI_ROM_END,
    .io_ranges = mdu_io_ranges_tbl,
    .io_mirror_mask = 0x0003FFFFUL,
    .io_unmapped_read = 0,
    .slots = iici_slots,
    .bus_err_lo = 0xF9000000,
    .bus_err_hi = 0xFEFFFFFF,
};

// ============================================================
// Init / Teardown
// ============================================================

// IIci device construction (mac030_mdu_board_t.build_devices): everything after
// the shared core/RTC/SCC/VIA1 prefix and before mac030_glue_finish.
static void iici_build_devices(config_t *cfg, checkpoint_t *checkpoint) {
    iici_state_t *st = iici_state(cfg);
    st->last_port_b = 0x30; // ADB ST1:ST0 idle = 11
    // The IIci bit-bangs the RTC on VIA1 (classic transceiver path).
    rtc_set_via(cfg->rtc, cfg->via1);

    // Machine-ID readback on VIA1 port A: PA6/PA4/PA2/PA1 = 1/0/1/1 for the
    // production (no Parity Generator Card) IIci — mask $56, value $46 per
    // InfoMacIIci.  (Verified at the assembler level during bring-up.)
    via_input(cfg->via1, 0, 6, 1);
    via_input(cfg->via1, 0, 4, 0);
    via_input(cfg->via1, 0, 2, 1);
    via_input(cfg->via1, 0, 1, 1);
    // CA1 / CB1 idle high (VBL heartbeat + ADB clock reference edges).
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);

    st->adb = adb_init(cfg->via1, cfg->scheduler, checkpoint);
    cfg->adb = st->adb;

    if (checkpoint)
        mac_checkpoint_restore_images(cfg, checkpoint);

    cfg->scsi = scsi_init(NULL, checkpoint);
    scsi_set_irq_callback(cfg->scsi, iici_scsi_irq, cfg);
    setup_images(cfg);

    st->asc = asc_init(NULL, cfg->scheduler, checkpoint);
    asc_set_mix(st->asc, ASC_MIX_CH_A); // internal speaker takes the left channel
    st->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, checkpoint);
    cfg->floppy = st->floppy;

    // RBV chip (VIA2 replacement + video control).  Default monitor sense 6
    // = 13" RGB.  IRQ → IPL 2; RvPowerOff → scheduler stop.
    st->rbv = rbv_init(RBV_VARIANT_IICI, checkpoint);
    assert(st->rbv != NULL);
    rbv_set_irq_callback(st->rbv, iici_rbv_irq, cfg);
    rbv_set_power_off_callback(st->rbv, iici_power_off, cfg);
    rbv_set_mode_callback(st->rbv, iici_rbv_mode, cfg);
    rbv_set_monitor_sense(st->rbv, 6);
    asc_set_irq_handler(st->asc, iici_asc_irq, st->rbv); // sound IRQ → RvIFR bit 4

    st->mmu = mac030_build_mmu(cfg, iici_board.rom_base, iici_board.rom_end);
    // TT1 identity-maps NuBus space $F0-$FF for supervisor FCs (same as SE/30).
    st->mmu->tt1 = 0xF00F8043;

    cfg->nubus = nubus_init(cfg, iici_board.slots, checkpoint);
    st->video_card = nubus_card(cfg->nubus, 0xB);
    assert(st->video_card != NULL);
    builtin_rbv_video_set_rbv(st->video_card, st->rbv);

    // Bind device handles + the board's I/O window table for the shared engine.
    mdu_io_bind(&st->mdu_io, cfg, &iici_board, st->asc, st->floppy, st->rbv, st->video_card);

    // Register the built-in framebuffer at the slot-$B aperture so the boot
    // ROM's VideoInfoMDU screen base ($FBB08000) and its Mode-24 alias land
    // in the card buffer.  Mirrors se30_init's VRAM registration.
    uint8_t *fb = builtin_rbv_video_framebuffer(st->video_card);
    assert(fb != NULL);
    memory_map_host_region(cfg->mem_map, "iici_vram", fb, BUILTIN_RBV_VRAM_BASE, BUILTIN_RBV_VRAM_SIZE,
                           /*writable*/ true);

    // NuBus expansion slots $9..$E bus-error on unmapped reads; the mapped
    // built-in video aperture at $FBxxxxxx resolves ahead of this range.
    memory_set_bus_error_range(cfg->mem_map, iici_board.bus_err_lo, iici_board.bus_err_hi);

    iici_memory_layout_init(cfg);

    if (checkpoint) {
        mmu_checkpoint_restore(st->mmu, checkpoint);
        mmu_invalidate_tlb(st->mmu);
        g_mmu = st->mmu;
        cpu_attach_mmu(cfg->cpu, st->mmu);
        via_redrive_outputs(cfg->via1);
    }
}

// ============================================================
// Machine descriptor
// ============================================================

static const uint32_t iici_ram_options_kb[] = {1024, 2048, 4096, 5120, 8192, 16384, 32768, 65536, 131072, 0};

static const struct floppy_slot iici_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {.label = "External FD1", .kind = FLOPPY_HD},
    {0},
};

static const struct scsi_slot iici_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

// IIci board: the shared mdu_substrate reads its data descriptor + VIA1 hooks
// + the device-construction body.
static const mac030_mdu_board_t iici_mdu_board = {
    .desc = &iici_board,
    .via1_output = iici_via1_output,
    .via1_shift_out = iici_via1_shift_out,
    .build_devices = iici_build_devices,
};

const hw_profile_t machine_iici = {
    .name = "Macintosh IIci",
    .id = "iici",

    .cpu_model = 68030,
    .freq = 25000000, // 25 MHz
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x8000000, // 128 MB
    .rom_size = 0x80000, // 512 KB

    .ram_options = iici_ram_options_kb,
    .floppy_slots = iici_floppy_slots,
    .scsi_slots = iici_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 3,
    // Built-in RBV video has no separate declaration ROM — the boot ROM
    // drives it from the hard-coded VideoInfoMDU record.

    .nubus_slots = iici_slots,

    .substrate = &mdu_substrate, // shared MDU+RBV-family substrate
    .board = &iici_mdu_board,
};
