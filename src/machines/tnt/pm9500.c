// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm9500.c
// Power Macintosh 9500/132 ("Tsunami", 132 MHz MPC604 daughtercard,
// May 1995) — the six-slot tower.  Two Bandits, MESH + 53C94 SCSI, 44 MHz
// processor bus at 3:1, twelve DIMM slots to 1.5 GB (Apple, "Power
// Macintosh 9500 Computer" Developer Note, 1995).
//
// The real 9500 has NO onboard video — it "requires a display card in a
// PCI slot" (ibid.), and a real machine's Open Firmware device tree has no
// /chaos node at all, only a dangling `vci0` devalias the shipping ROM is
// perfectly happy with (Apple Technote 1062).
//
// So slot 7's Control/Chaos entry is a BUILTIN_FALLBACK: it stands in only
// while no socket supplies a display card, purely so a cardless boot has
// somewhere to draw.  Seat an Apple Accelerated PCI Graphics Card in a
// socket and the fake retires, which is what the hardware looks like.
// Removing Chaos from the machine altogether is the remaining step and is
// deliberately separate — it changes what probe-pci walks on a
// boot-critical path.

#include "tnt.h"

// Twelve DIMM slots, interleaved in pairs; 1.5 GB architectural max.
static const uint32_t pm9500_ram_options_kb[] = {16384, 32768, 65536, 131072, 262144, 524288, 1048576, 1572864, 0};

// The internal fast-SCSI (MESH) bus carries the boot disks; the
// external 53C94 chain is present but empty until the CD-ROM phase.
static const struct scsi_slot pm9500_scsi_slots[] = {
    {.label = "Internal HD0", .id = 0},
    {.label = "Internal HD1", .id = 1},
    {0},
};

// PCI topology (proposal-pci-architecture §6.1).  Six sockets — three on
// each Bandit, all at IDSEL 13/14/15 on their own bus (the bandit node's
// FCode instantiates twice) — with their strapped INTA-D lines on Grand
// Central externals 23/24/25 (Bandit 1) and 27/28/29 (Bandit 2), which is
// Apple's own 9500 external-interrupt table verbatim.  Slot 7 carries the
// Control/Chaos video deviation documented above.
//
// The slot LABELS come from each bridge's own `slot-names` property, dumped
// live from a real 9500 under Open Firmware (Apple Technote 1062):
// Bandit 1 publishes `0000E000 "A1" "B1" "C1"` and Bandit 2 publishes
// `0000E000 "D2" "E2" "F2"`.  Phase 1 declared the second bank D1/E1/F1,
// having judged the strings "not decidable from the token stream"; the
// ROM's own property decides them.
static const pci_slot_decl_t pm9500_pci_slots[] = {
    {.slot = 1, .kind = PCI_SLOT_SOCKET, .label = "A1", .bus = TNT_PCI_BUS_1, .device = 13, .int_line = 23},
    {.slot = 2, .kind = PCI_SLOT_SOCKET, .label = "B1", .bus = TNT_PCI_BUS_1, .device = 14, .int_line = 24},
    {.slot = 3, .kind = PCI_SLOT_SOCKET, .label = "C1", .bus = TNT_PCI_BUS_1, .device = 15, .int_line = 25},
    {.slot = 4, .kind = PCI_SLOT_SOCKET, .label = "D2", .bus = TNT_PCI_BUS_2, .device = 13, .int_line = 27},
    {.slot = 5, .kind = PCI_SLOT_SOCKET, .label = "E2", .bus = TNT_PCI_BUS_2, .device = 14, .int_line = 28},
    {.slot = 6, .kind = PCI_SLOT_SOCKET, .label = "F2", .bus = TNT_PCI_BUS_2, .device = 15, .int_line = 29},
    {.slot = 7,
     .kind = PCI_SLOT_BUILTIN_FALLBACK,
     .label = "VCI",
     .bus = TNT_PCI_BUS_VCI,
     .device = 11,
     .int_line = TNT_INT_VBL,
     .builtin_card_id = "tnt_control"},
    {0},
};

static const tnt_board_desc_t pm9500_board = {
    // BoxID: bit 11 clear (the 9500 is flagged by Hammerhead +$20 bit 30
    // instead — the shipping ROM's identification routine at $FFC14844),
    // MESH present, idle-high straps.
    .boxid = 0x8000u | 0x4000u,
    .hh_id = 0x39000000u, // $39 first byte = the TNT identification path
    // +$20 bit 30 SET = 9500 (the 68k routine tests it directly; Open
    // Firmware's selector m = (b>>5)|((b>>1)&8) over the top byte reads
    // $40 as 2 -> "AAPL,9500").  Bit 31 must stay CLEAR — set it and OF
    // classifies the box as a 7500/8500 (see pm7500.c).
    .hh_r20 = 0x40000000u,
    .bus_hz = 44000000u, // 3:1 bus (132 MHz 604 card)
    .bandit_count = 2,
    .kind = TNT_BOARD_MAC,
    .has_mesh = true, // the internal fast-SCSI cell (absent on the Network Servers)
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
    .scsi_slots = pm9500_scsi_slots,

    .pci_slots = pm9500_pci_slots,

    .substrate = &tnt_substrate,
    .board = &pm9500_board,
};
