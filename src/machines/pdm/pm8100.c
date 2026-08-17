// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm8100.c
// Power Macintosh 8100/80 ("Cold Fusion", 80 MHz MPC601, March 1994) — the
// PDM flagship.  Machine-ID register $A55A3013; 40 MHz bus (2:1); eight
// fixed-window SIMM banks; three NuBus slots + PDS slot $E (Phase H); the
// second, discrete 53CF96 fast-SCSI bus arrives with Phase G.

#include "pdm.h"

// 8 MB soldered + fixed-window banks of {2,8,32} MB: up to 264 MB.
static const uint32_t pm8100_ram_options_kb[] = {8192, 16384, 40960, 73728, 139264, 270336, 0};

static const struct floppy_slot pm8100_floppy_slots[] = {
    {0},
};

static const struct scsi_slot pm8100_scsi_slots[] = {
    {0},
};

static const pdm_board_desc_t pm8100_board = {
    .machine_id = 0x3013,
    .bus_hz = 40000000u, // 2:1 bus
    .bank_layout = PDM_BANKS_FIXED,
    .bank_count = 8,
    .wait_state_penalty = 2, // pinned by the rung-L7 bus-ratio row
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
    .floppy_slots = pm8100_floppy_slots,
    .scsi_slots = pm8100_scsi_slots,
    .has_cdrom = false,
    .cdrom_id = 3,

    .nubus_slots = NULL,

    .substrate = &pdm_substrate,
    .board = &pm8100_board,
};
