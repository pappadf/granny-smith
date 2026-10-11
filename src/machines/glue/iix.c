// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iix.c
// Macintosh IIx machine implementation.  Sister of iicx.c — shares the
// GLUE-driven I/O map, dual-VIA / 68030 / Universal-ROM family, and the
// page table / ROM overlay helpers via iicx_internal.h.  See
// docs/reference/machines/glue/iicx.md, which covers the IIx differences.
//
// Diff vs iicx.c at a glance:
//   * Slot table: six NuBus slots ($9..$E) — slot $9 is VIDEO with
//     mdc_8_24 default, the rest are EMPTY in v1.
//   * Machine-ID bits: PA6 = 0, PB3 = 0 (vs IIcx PA6 = 1, PB3 = 1).
//   * No soft-power-off (PB2 is a free pin); no sound-jack-detect.
//   * Otherwise identical: same VIA1 callbacks, same I/O dispatcher,
//     same memory layout, same VBL trigger.

#include "mac030_glue.h"
#include "machine.h"
#include "slot_tables.h"
#include "system_internal.h"

#include "asc.h"
#include "iicx_internal.h"
#include "nubus.h"
#include "pram_defaults.h"
#include "via.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// VIA callbacks
// ============================================================
//
// VIA1 callbacks come from iicx_internal.h.  VIA2 has nothing to observe
// (no soft-power-off, no sound-jack-detect), so it takes the family's
// ignored-output callbacks.

// (VBL is the default GLUE NuBus VBL in glue_substrate; no IIx override.)

// ============================================================
// Slot table
// ============================================================

static const nubus_slot_decl_t iix_slots[] = {
    {.slot = 0x9, .kind = NUBUS_SLOT_SOCKET, .default_card = "mdc_8_24", .label = "NuBus slot 1", .fill_order = 1},
    {.slot = 0xA, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot 2", .fill_order = 2},
    {.slot = 0xB, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot 3", .fill_order = 3},
    {.slot = 0xC, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot 4", .fill_order = 4},
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot 5", .fill_order = 5},
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot 6", .fill_order = 6},
    {0},
};

// ============================================================
// Init / Teardown
// ============================================================

// Machine-ID straps: VIA1 PA6 = 0 and VIA2 PB3 = 0 identify the IIx.  Both are
// driven because both differ from the VIA's idle-high power-on state; the
// slot-IRQ PA lines and the CA1/CA2/CB2 control lines this used to park are
// that state already.
static void iix_setup_id(config_t *cfg) {
    via_input(cfg->via1, 0, 6, 0); // VIA1 PA6
    via_input(cfg->via2, 1, 3, 0); // PB3
}

// IIx board: GLUE family, six NuBus slots, no soft-power / sound-jack.
static const mac030_board_desc_t iix_board_desc = {
    .chipset = "GLUE",
    .rom_base = 0x40000000UL,
    .rom_end = 0x50000000UL,
    .io_ranges = glue_io_ranges,
    .io_mirror_mask = MAC030_GLUE_IO_MIRROR,
    .io_unmapped_read = 0xFF, // undecoded island reads float high (see mac030_glue.h)
    .bus_err_lo = NUBUS_BERR_LO,
    .bus_err_hi = NUBUS_BERR_HI,
    .asc_mix = ASC_MIX_CH_A, // internal speaker takes the left channel
};

static const mac030_glue_board_t iix_board = {
    .desc = &iix_board_desc,
    .via1_output = iicx_via1_output,
    // No VIA1 shift-out routing: adb.c reads the VIA's shift register
    // directly at each port-B ST transition, because in mode 7 the ADB
    // transceiver clocks the shift, not the VIA's internal timer, and the
    // ROM's SR writes during interrupt handling fire the callback
    // spuriously (BUG-004).  via.c tolerates a NULL here.
    .via1_shift_out = NULL,
    .via2_output = mac030_glue_via_output_ignored,
    .via2_shift_out = mac030_glue_via_shift_out_ignored,
    .setup_id = iix_setup_id,
    .memory_layout_tail = iicx_memory_layout_tail,
};

// ============================================================
// Machine descriptor
// ============================================================

const hw_profile_t machine_iix = {
    .name = "Macintosh IIx",
    .id = "iix",

    .cpu_model = 68030,
    .freq = 15667200,
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = MAC030_GLUE_RAM_DEFAULT,
    .ram_max = MAC030_GLUE_RAM_MAX,
    .rom_size = 0x040000, // 256 KB

    .ram_options = iicx_iix_ram_options_kb, // shared, 5120 included (iicx.c)
    .floppy_slots = mac_floppy_slots_2int,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .cdrom_drive = &mac_cdrom_drive_applecd,
    // No built-in video, as on the IIcx: the screen is the NuBus card in
    // slot $9.  The default is the Display Card 8•24 (1989), a later card
    // than the IIx (1988) but one it takes; the card kind requires its
    // declaration ROM.

    .nubus_slots = iix_slots,

    .pram = &pram_defaults_mac_ii,
    .substrate = &glue_substrate, // shared GLUE-family substrate
    .board = &iix_board,
};
