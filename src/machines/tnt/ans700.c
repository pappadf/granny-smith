// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ans700.c
// Apple Network Server 700/150 ("Shiner HE", February 1996) — the larger of
// the two Network Servers.  150 MHz MPC604 on a 50 MHz processor bus, 1 MB
// L2 cache DIMM, 48 MB of parity DRAM, six PCI slots, seven front SCSI bays
// plus two rear ones, and one or two hot-swap 425 W supplies.
//
// See ans500.c for the board itself; everything structural is shared.
// Apple's own framing of the split (Apple, "Network Server Hardware
// Developer Notes", 1996, §1.1.2): the two models are "distinguished by:
// More drive Bays; Redundant Power Supplies; Higher Clock frequency; More
// Cache memory."  Its service documentation calls them LE (= 500) and HE
// (= 700) and budgets "180 watts for the LE, 260 watts for the HE."
//
// Almost nothing here is visible to an emulator: same board, same ASICs,
// same slots, same two 53C825A controllers.  The register-level deltas are
// exactly three — the CPU card's clock (which the family turns into the
// decrementer tick rate AND the fast-L2 decision), the L2 DIMM size
// Hammerhead +$E0 reports, and `TwoSuppliesH` in Board Register 1.  Both
// profiles exist because AIX and the service documentation distinguish
// them, and every ladder row runs BOTH, because two machines that barely
// differ are exactly where a bug hides in the one nobody tested.
//
// The 700/150's bus speed is the one clock figure Apple ever prints, and it
// prints it on the machine's own front-panel LCD: `150 MHz 604, 50 MHz Bus`
// ("Setting Up the Network Server", p. 86).  At 50 MHz the ROM does NOT
// enable fast L2 — that mode is for "bus speeds of 44 MHz or less" — which
// is the one place the 500 is architecturally ahead of the 700.

#include "slot_tables.h"
#include "tnt.h"

// Eight DIMM slots in four interleaved bank pairs; 512 MB is the ROM's
// decode ceiling (see ans500.c).  48 MB is this model's shipping default.
static const uint32_t ans700_ram_options_kb[] = {16384, 32768, 49152, 65536, 131072, 262144, 524288, 0};

// Bus 1 carries the same front bays 4-6 as the 500, PLUS the two rear bays
// this model adds -- which is how Apple's count works out: "The buses
// accommodate four, five, and seven SCSI devices, respectively."  Four on
// bus 0 (the CD bay plus bays 1-3), five on bus 1 (front 4-6 plus the two
// rear).  Until this the table was byte-identical to the 500's while the
// comment above described a topology it did not express.
static const struct scsi_slot ans700_scsi_slots_fw1[] = {
    {.label = "Bay 5 (fast/wide 1)", .id = 4},
    {.label = "Bay 6 (fast/wide 1)", .id = 5},
    {.label = "Bay 7 (fast/wide 1)", .id = 6},
    {.label = "Rear bay 1 (fast/wide 1)", .id = 0},
    {.label = "Rear bay 2 (fast/wide 1)", .id = 1},
    {0},
};

static const scsi_bus_decl_t ans700_scsi_buses[] = {
    {.object = "scsi", .label = "Front backplane (fast/wide 0)", .slots = ans_scsi_slots_fw0},
    {.object = "scsi2", .label = "Front + rear bays (fast/wide 1)", .slots = ans700_scsi_slots_fw1},
    {0},
};

static const tnt_board_desc_t ans700_board = {
    .boxid = 0x0800u | 0x0100u, // BoxId0 = 1, BoxId1 = 0; bit 8 high (see ans500.c)
    .hh_id = 0x39000000u,
    .hh_r20 = 0x40000000u,
    .bus_hz = 50000000u, // ATTESTED: the machine prints "50 MHz Bus" itself
    .bandit_count = 2,
    .kind = TNT_BOARD_SHINER,
    .has_mesh = false,
    .has_gbus = true,
    .has_parity = true,
    .l2_kb = 1024u, // 1 MB cache DIMM
    .two_supplies = true, // hot-swap redundant supplies: TwoSuppliesH reads high
};

const hw_profile_t machine_ans700 = {
    .name = "Apple Network Server 700/150",
    .id = "ans700",

    .cpu_model = CPU_MODEL_PPC604,
    .freq = 150000000, // 150 MHz 604 processor card
    .mmu_kind = MMU_PPC_604,

    .address_bits = 32,
    .ram_default = 0x3000000, // 48 MB parity (the shipping configuration)
    .ram_max = 0x20000000, // 512 MB — the ROM's decode ceiling
    .rom_size = 0x400000, // 4 MB ($962F6C13 production / $49B2BE8F prototype)

    .ram_options = ans700_ram_options_kb,
    .scsi_buses = ans700_scsi_buses,
    .floppy_slots = mac_floppy_slots_1hd,
    .has_cdrom = true,
    .cdrom_id = 0,

    .pci_slots = ans_pci_slots,

    .substrate = &tnt_substrate,
    .board = &ans700_board,
};
