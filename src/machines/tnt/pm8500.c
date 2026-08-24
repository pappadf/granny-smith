// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm8500.c
// Power Macintosh 8500/120 ("Nitro", 120 MHz MPC604 daughtercard, August
// 1995) — the AV tower.  Two Bandits, MESH + 53C94 SCSI, Control/Chaos
// onboard video plus the (unmodeled) Sixty6/Plan B AV path, 40 MHz
// processor bus at 3:1 (Apple, "Power Macintosh 7500 and 8500 Computers"
// Developer Note, 1995).

#include "tnt.h"

// 168-pin DIMMs in 8 slots, interleaved in pairs; 1 GB architectural max.
static const uint32_t pm8500_ram_options_kb[] = {16384, 32768, 65536, 131072, 262144, 524288, 1048576, 0};

static const tnt_board_desc_t pm8500_board = {
    // BoxID: model code %01 at bits 12-11 (community-attested for the
    // 8500), MESH present, idle-high straps.  Bit 13 (composite video /
    // Sixty6 present) stays CLEAR while the AV subsystem is unmodeled so
    // nothing probes for it; revisited with the gated AV phase.
    .boxid = 0x8000u | 0x4000u | 0x0800u,
    .bus_hz = 40000000u, // 3:1 bus (120 MHz 604 card)
    .bandit_count = 2,
};

const hw_profile_t machine_pm8500 = {
    .name = "Power Macintosh 8500/120",
    .id = "pm8500",

    .cpu_model = CPU_MODEL_PPC604,
    .freq = 120000000, // 120 MHz
    .mmu_kind = MMU_PPC_604,

    .address_bits = 32,
    .ram_default = 0x2000000, // 32 MB
    .ram_max = 0x40000000, // 1 GB
    .rom_size = 0x400000, // 4 MB ($96CD923D / $9630C68B)

    .ram_options = pm8500_ram_options_kb,

    .substrate = &tnt_substrate,
    .board = &pm8500_board,
};
