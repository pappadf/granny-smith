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

#include "mac030_glue.h"
#include "mac_host_io.h"
#include "machine.h"
#include "mmu_checkpoint.h"
#include "system_config.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint_images.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "debug.h"
#include "floppy.h"
#include "iicx_internal.h"
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

LOG_USE_CATEGORY_NAME("iix");

// ============================================================
// Forward declarations
// ============================================================

static void iix_via2_output(void *context, uint8_t port, uint8_t output);
static void iix_via2_shift_out(void *context, uint8_t byte);

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

// (VBL is the default GLUE NuBus VBL in glue_substrate; no IIx override.)

// ============================================================
// Slot table
// ============================================================

static const nubus_slot_decl_t iix_slots[] = {
    {.slot = 0x9, .kind = NUBUS_SLOT_VIDEO, .default_card = "mdc_8_24"},
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

// Machine-ID straps: PA6 = 0, PB3 = 0 (IIx); VIA2 slot-IRQ PA lines idle high.
static void iix_setup_id(config_t *cfg) {
    via_input(cfg->via1, 0, 6, 0);
    via_input(cfg->via2, 1, 3, 0);
    via_input(cfg->via2, 0, 0, 1);
    via_input(cfg->via2, 0, 1, 1);
    via_input(cfg->via2, 0, 2, 1);
    via_input(cfg->via2, 0, 3, 1);
    via_input(cfg->via2, 0, 4, 1);
    via_input(cfg->via2, 0, 5, 1);
    via_input_c(cfg->via2, 0, 0, 1);
    via_input_c(cfg->via2, 0, 1, 1);
    via_input_c(cfg->via2, 1, 1, 1);
}

// IIx board: GLUE family, six NuBus slots, no soft-power / sound-jack.
static const mac030_board_desc_t iix_desc = {
    .chipset = "GLUE",
    .rom_base = 0x40000000UL,
    .rom_end = 0x50000000UL,
    .io_ranges = glue_io_ranges,
    .io_mirror_mask = MAC030_GLUE_IO_MIRROR,
    .io_unmapped_read = 0,
    .slots = iix_slots,
    .bus_err_lo = 0xF9000000,
    .bus_err_hi = 0xFEFFFFFF,
    .asc_mix = ASC_MIX_CH_A, // internal speaker takes the left channel
};

static const mac030_glue_board_t iix_board = {
    .desc = &iix_desc,
    .via1_output = iicx_via1_output,
    .via1_shift_out = iicx_via1_shift_out,
    .via2_output = iix_via2_output,
    .via2_shift_out = iix_via2_shift_out,
    .setup_id = iix_setup_id,
    .memory_layout = iicx_memory_layout_init,
};

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
    .mmu_kind = MMU_68030_PMMU,

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

    .nubus_slots = iix_slots,

    .substrate = &glue_substrate, // shared GLUE-family substrate
    .board = &iix_board,
};
