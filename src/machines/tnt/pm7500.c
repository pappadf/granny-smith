// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm7500.c
// Power Macintosh 7500/100 ("TNT", 100 MHz MPC601 daughtercard, August
// 1995) — the 601 member of the family.  One Bandit (three PCI slots),
// MESH + 53C94 SCSI, Control/Chaos onboard video, 50 MHz processor bus
// (Apple, "Power Macintosh 7500 and 8500 Computers" Developer Note, 1995).

#include "tnt.h"

// 168-pin DIMMs in 8 slots, interleaved in pairs; 1 GB architectural max.
static const uint32_t pm7500_ram_options_kb[] = {16384, 32768, 65536, 131072, 262144, 524288, 1048576, 0};

static const tnt_board_desc_t pm7500_board = {
    // BoxID (little-endian bit numbering): bit 15 pulled high, bit 14 MESH
    // present, bit 8 factory-test strap idle-high, model code %10 at bits
    // 12-11 (STARTING GUESS — the 9500/8500 codes %00/%01 are community-
    // attested, the 7500's is pinned at ladder rung T4 from the
    // `compatible` string the ROM's Open Firmware emits).
    .boxid = 0x8000u | 0x4000u | 0x1000u | 0x0100u,
    .bus_hz = 50000000u, // 2:1 bus (100 MHz 601 card)
    .bandit_count = 1,
};

const hw_profile_t machine_pm7500 = {
    .name = "Power Macintosh 7500/100",
    .id = "pm7500",

    .cpu_model = CPU_MODEL_PPC601,
    .freq = 100000000, // 100 MHz
    .mmu_kind = MMU_PPC_601,

    .address_bits = 32,
    .ram_default = 0x2000000, // 32 MB
    .ram_max = 0x40000000, // 1 GB
    .rom_size = 0x400000, // 4 MB ($96CD923D / $9630C68B)

    .ram_options = pm7500_ram_options_kb,

    .substrate = &tnt_substrate,
    .board = &pm7500_board,
};
