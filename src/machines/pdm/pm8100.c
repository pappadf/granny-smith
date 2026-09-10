// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm8100.c
// Power Macintosh 8100/80 ("Cold Fusion", 80 MHz MPC601, March 1994) — the
// PDM flagship.  Machine-ID register $A55A3013; 40 MHz bus (2:1); eight
// fixed-window SIMM banks; three NuBus slots ($B/$C/$D) behind BART + the
// PDS video slot $E (no PDS card is modeled: that window reads empty); a
// second, discrete 53CF96 on a fast internal SCSI bus (modeled with no
// devices attached — media land on the standard Curio bus).

#include "pdm.h"
#include "slot_tables.h"

#include "nubus.h"

// 8 MB soldered + fixed-window banks of {2,8,32} MB: up to 264 MB.
static const uint32_t pm8100_ram_options_kb[] = {8192, 16384, 40960, 73728, 139264, 270336, 0};

// One internal manual-inject SuperDrive behind SWIM3, and no external
// port — the PDM family has no second bay (Apple, "Power Macintosh
// Computers" Developer Note, Table 3-7).

// Media attach to the standard Curio bus (SCSI Manager bus 1 on this
// model); the fast 53CF96 bus scans empty.

static const scsi_bus_decl_t pm8100_scsi_buses[] = {
    {.object = "scsi", .label = "SCSI", .slots = mac_scsi_slots_hd01},
    {0},
};

static const pdm_board_desc_t pm8100_board = {
    .machine_id = 0x3013,
    .bus_hz = 40000000u, // 2:1 bus
    .bank_layout = PDM_BANKS_FIXED,
    .bank_count = 8,
    .wait_state_penalty = 2, // pinned by the rung-L7 bus-ratio row
    .has_fast_scsi = true, // discrete 53CF96, island +$11000, DMA channel B
};

const hw_profile_t machine_pm8100 = {
    .name = "Power Macintosh 8100/80",
    .id = "pm8100",

    .cpu_model = CPU_MODEL_PPC601,
    .freq = 80000000, // 80 MHz
    .mmu_kind = MMU_PPC_601,

    .address_bits = 32,
    .ram_default = 0x2800000, // 40 MB (8 soldered + one 32 MB bank)
    .ram_max = 0x10800000, // 264 MB
    .rom_size = 0x400000, // 4 MB ($9FEB69B3)

    .ram_options = pm8100_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .scsi_buses = pm8100_scsi_buses,
    // The AppleCD 300i rides the same Curio 53C96 bus as the HD slots
    // (Phase G): no CD-specific hardware is involved, so the bay is
    // offered as soon as that bus exists.
    .has_cdrom = true,
    .cdrom_id = 3,

    .builtin_video = &pdm_builtin_video,
    .nubus_slots = pdm_nubus_slots_cde,

    .substrate = &pdm_substrate,
    .board = &pm8100_board,
};
