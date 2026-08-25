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

// The internal fast-SCSI (MESH) bus carries the boot disks; the
// external 53C94 chain is present but empty until the CD-ROM phase.
static const struct scsi_slot pm7500_scsi_slots[] = {
    {.label = "Internal HD0", .id = 0},
    {.label = "Internal HD1", .id = 1},
    {0},
};

static const tnt_board_desc_t pm7500_board = {
    // BoxID (little-endian bit numbering): bit 15 pulled high, bit 14 MESH
    // present, bit 8 factory-test strap CLEAR (set sends the ROM into its
    // serial test monitor), bit 11 CLEAR (set = 8500 — the shipping ROM's
    // identification routine at $FFC14844, decoded during Phase D), and
    // bit 13 SET — Open Firmware's model decode (OpenFW image $10592,
    // decoded during Phase D part 2) reads BoxID as xw@>>11 into its
    // machine word and picks "AAPL,7500" over "AAPL,8500" on bit 13.
    .boxid = 0x8000u | 0x4000u | 0x2000u,
    // Hammerhead identity: first byte $39 selects the ROM's TNT path
    // (a $3001xxxx identifier is the 7200/Catalyst); +$20 bit 31 SET =
    // the 7500/8500 class in Open Firmware's selector (m = (b>>5) |
    // ((b>>1)&8) over the +$20 top byte: $80 -> 4 -> 7500/8500,
    // $40 -> 2 -> 9500), bit 30 clear = not a 9500 for the 68k routine.
    // Without bit 31 OF emits compatible "AAPL,????" and never
    // instantiates the chaos/control display nodes (the Phase-D video
    // wall's root cause).
    .hh_id = 0x39000000u,
    .hh_r20 = 0x80000000u,
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
    .scsi_slots = pm7500_scsi_slots,

    .substrate = &tnt_substrate,
    .board = &pm7500_board,
};
