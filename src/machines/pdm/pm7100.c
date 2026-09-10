// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm7100.c
// Power Macintosh 7100/66 ("Carl Sagan", 66 MHz MPC601, March 1994) — the
// mid-range PDM.  Machine-ID register $A55A3012; 33 MHz bus (2:1); four
// SIMM banks at the fixed window addresses (never relocated); three NuBus
// slots ($B/$C/$D) behind BART, plus the PDS video slot $E (no PDS card is
// modeled, so that window reads as an empty slot).

#include "pdm.h"
#include "slot_tables.h"

#include "nubus.h"

// 8 MB soldered + fixed-window banks of {2,8,32} MB: up to 136 MB.
static const uint32_t pm7100_ram_options_kb[] = {8192, 16384, 24576, 40960, 73728, 139264, 0};

// One internal manual-inject SuperDrive behind SWIM3, and no external
// port — the PDM family has no second bay (Apple, "Power Macintosh
// Computers" Developer Note, Table 3-7).

// One standard 5 MB/s bus (the Curio 53C94 cell), internal + external.

static const scsi_bus_decl_t pm7100_scsi_buses[] = {
    {.object = "scsi", .label = "SCSI", .slots = mac_scsi_slots_hd01},
    {0},
};

static const pdm_board_desc_t pm7100_board = {
    .machine_id = 0x3012,
    .bus_hz = 33000000u, // 2:1 bus
    .bank_layout = PDM_BANKS_FIXED,
    .bank_count = 4,
    .wait_state_penalty = 2, // pinned by the rung-L7 bus-ratio row
};

const hw_profile_t machine_pm7100 = {
    .name = "Power Macintosh 7100/66",
    .id = "pm7100",

    .cpu_model = CPU_MODEL_PPC601,
    .freq = 66000000, // 66 MHz
    .mmu_kind = MMU_PPC_601,

    .address_bits = 32,
    .ram_default = 0x1800000, // 24 MB
    .ram_max = 0x8800000, // 136 MB
    .rom_size = 0x400000, // 4 MB ($9FEB69B3)

    .ram_options = pm7100_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .scsi_buses = pm7100_scsi_buses,
    // The AppleCD 300i rides the same Curio 53C96 bus as the HD slots
    // (Phase G): no CD-specific hardware is involved, so the bay is
    // offered as soon as that bus exists.
    .has_cdrom = true,
    .cdrom_id = 3,

    .builtin_video = &pdm_builtin_video,
    .nubus_slots = pdm_nubus_slots_cde,

    .substrate = &pdm_substrate,
    .board = &pm7100_board,
};
