// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm9500.c
// Power Macintosh 9500/132 ("Tsunami", 132 MHz MPC604 daughtercard,
// May 1995) — the six-slot tower.  Two Bandits, MESH + 53C94 SCSI, 44 MHz
// processor bus at 3:1, twelve DIMM slots to 1.5 GB (Apple, "Power
// Macintosh 9500 Computer" Developer Note, 1995).
//
// Documented fidelity deviation: the real 9500 has NO onboard video — it
// requires a PCI display card ("The computer requires a display card in a
// PCI slot", ibid.).  The emulated pm9500 will populate Chaos/Control
// like its siblings when the video phase lands, because an FCode-carrying
// PCI framebuffer card is a substantial gated follow-up of its own; the
// About-box/Gestalt row still proves identity.

#include "tnt.h"

// Twelve DIMM slots, interleaved in pairs; 1.5 GB architectural max.
static const uint32_t pm9500_ram_options_kb[] = {16384, 32768, 65536, 131072, 262144, 524288, 1048576, 1572864, 0};

static const tnt_board_desc_t pm9500_board = {
    // BoxID: model code %00 at bits 12-11 (community-attested for the
    // 9500), MESH present, idle-high straps.
    .boxid = 0x8000u | 0x4000u | 0x0100u,
    .bus_hz = 44000000u, // 3:1 bus (132 MHz 604 card)
    .bandit_count = 2,
};

const hw_profile_t machine_pm9500 = {
    .name = "Power Macintosh 9500/132",
    .id = "pm9500",

    .cpu_model = CPU_MODEL_PPC604,
    .freq = 132000000, // 132 MHz
    .mmu_kind = MMU_PPC_604,

    .address_bits = 32,
    .ram_default = 0x2000000, // 32 MB
    .ram_max = 0x60000000, // 1.5 GB
    .rom_size = 0x400000, // 4 MB ($96CD923D / $9630C68B)

    .ram_options = pm9500_ram_options_kb,

    .substrate = &tnt_substrate,
    .board = &pm9500_board,
};
