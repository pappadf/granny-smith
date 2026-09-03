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

// The internal fast-SCSI (MESH) bus carries the boot disks; the
// external 53C94 chain is present but empty until the CD-ROM phase.
static const struct scsi_slot pm8500_scsi_slots[] = {
    {.label = "Internal HD0", .id = 0},
    {.label = "Internal HD1", .id = 1},
    {0},
};

// PCI topology (proposal-pci-architecture §6.1).  Three sockets on Bandit
// 1 at IDSEL 13/14/15 — the ROM's own `slot-names` bitmask ($0000E000) on
// the bandit node, corroborated by Apple's Network Server developer note
// IDSEL table — with their strapped INTA-D lines on Grand Central
// externals 23/24/25 (Apple's 9500 external-interrupt table, §1.4).
// Control is the soldered-down video device the machine names, on the
// Chaos display bus.
static const pci_slot_decl_t pm8500_pci_slots[] = {
    {.slot = 1, .kind = PCI_SLOT_SOCKET, .label = "A1", .bus = TNT_PCI_BUS_1, .device = 13, .int_line = 23},
    {.slot = 2, .kind = PCI_SLOT_SOCKET, .label = "B1", .bus = TNT_PCI_BUS_1, .device = 14, .int_line = 24},
    {.slot = 3, .kind = PCI_SLOT_SOCKET, .label = "C1", .bus = TNT_PCI_BUS_1, .device = 15, .int_line = 25},
    {.slot = 4,
     .kind = PCI_SLOT_BUILTIN,
     .label = "VCI",
     .bus = TNT_PCI_BUS_VCI,
     .device = 11,
     .int_line = TNT_INT_VBL,
     .builtin_card_id = "tnt_control"},
    {0},
};

static const tnt_board_desc_t pm8500_board = {
    // BoxID: bit 11 SET = 8500 (the shipping ROM's 68k identification
    // routine at $FFC14844 — the one BoxID bit that dispatch reads), and
    // bit 13 CLEAR = 8500 in Open Firmware's decode (OpenFW $10592 picks
    // "AAPL,7500" over "AAPL,8500" on bit 13 of xw@>>11 — the earlier
    // "composite video" reading of bit 13 was wrong).  MESH present,
    // idle-high straps.
    .boxid = 0x8000u | 0x4000u | 0x0800u,
    .hh_id = 0x39000000u, // $39 first byte = the TNT identification path
    // +$20 bit 31 SET = the 7500/8500 class in Open Firmware's selector
    // (see pm7500.c); bit 30 clear = not a 9500 for the 68k routine.
    .hh_r20 = 0x80000000u,
    .bus_hz = 40000000u, // 3:1 bus (120 MHz 604 card)
    .bandit_count = 2,
    .kind = TNT_BOARD_MAC,
    .has_mesh = true, // the internal fast-SCSI cell (absent on the Network Servers)
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
    .scsi_slots = pm8500_scsi_slots,
    .floppy_slots = tnt_floppy_slots,

    .pci_slots = pm8500_pci_slots,

    .substrate = &tnt_substrate,
    .board = &pm8500_board,
};
