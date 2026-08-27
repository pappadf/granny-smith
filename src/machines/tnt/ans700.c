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

#include "tnt.h"

// Eight DIMM slots in four interleaved bank pairs; 512 MB is the ROM's
// decode ceiling (see ans500.c).  48 MB is this model's shipping default.
static const uint32_t ans700_ram_options_kb[] = {16384, 32768, 49152, 65536, 131072, 262144, 524288, 0};

// The 700 adds two REAR drive bays, and they cable to fast/wide bus 1
// (IDs 0 and 1) rather than to the front backplane — so on a 700 bus 0
// carries four devices and bus 1 carries five.  Apple's own count of the
// three SCSI buses: "The buses accommodate four, five, and seven SCSI
// devices, respectively."
static const struct scsi_slot ans700_scsi_slots[] = {
    {.label = "Bay 1 (fast/wide 0)", .id = 1},
    {.label = "Bay 2 (fast/wide 0)", .id = 2},
    {.label = "Bay 3 (fast/wide 0)", .id = 3},
    {0},
};

// Identical to the 500's — same board, same backplane, same IDSELs, same
// interrupt map.  Duplicated rather than shared because that is how this
// family has always carried per-model topology (pm8500.c / pm9500.c), and
// because the `ans-pci-slots` row asserts every entry of BOTH profiles, so
// drift between the two copies is a test failure rather than a surprise.
// See ans500.c for the derivation and the two boot-critical traps.
static const pci_slot_decl_t ans700_pci_slots[] = {
    {.slot = 1,
     .kind = PCI_SLOT_SOCKET,
     .label = "SLOT1_PCI0",
     .bus = TNT_PCI_BUS_1,
     .device = 13,
     .int_line = ANS_INT_SLOT1},
    {.slot = 2,
     .kind = PCI_SLOT_SOCKET,
     .label = "SLOT2_PCI0",
     .bus = TNT_PCI_BUS_1,
     .device = 14,
     .int_line = ANS_INT_SLOT2},
    {.slot = 3,
     .kind = PCI_SLOT_SOCKET,
     .label = "SLOT3_PCI1",
     .bus = TNT_PCI_BUS_2,
     .device = 13,
     .int_line = ANS_INT_SLOT3},
    {.slot = 4,
     .kind = PCI_SLOT_SOCKET,
     .label = "SLOT4_PCI1",
     .bus = TNT_PCI_BUS_2,
     .device = 14,
     .int_line = ANS_INT_SLOT4},
    {.slot = 5,
     .kind = PCI_SLOT_SOCKET,
     .label = "SLOT5_PCI1",
     .bus = TNT_PCI_BUS_2,
     .device = 15,
     .int_line = ANS_INT_SLOT5},
    {.slot = 6,
     .kind = PCI_SLOT_SOCKET,
     .label = "SLOT6_PCI1",
     .bus = TNT_PCI_BUS_2,
     .device = 16,
     .int_line = ANS_INT_SLOT6},
    // The three soldered-down PCI devices, all on Bandit 1 (Apple, ibid.,
    // §4.6.2 — the six-device bus).  Grand Central's own config presence at
    // IDSEL 16 is attached by grand_central.c, not from this table, exactly
    // as on the Macintosh boards.
    //
    // The 54M30 takes NO interrupt line: "the 54M30 does not have an
    // interrupt" (ibid., §4.2), and allocating it a Grand Central external
    // would corrupt the map.  The two 53C825As take EXT2 and EXT6, the two
    // positions the Network Server freed by ganging both Bandits' error
    // interrupts onto EXT1.
    {.slot = 7,
     .kind = PCI_SLOT_BUILTIN,
     .label = "VIDEO",
     .bus = TNT_PCI_BUS_1,
     .device = 15,
     .int_line = 0,
     .builtin_card_id = "cirrus_54m30"},
    {.slot = 8,
     .kind = PCI_SLOT_BUILTIN,
     .label = "FWSCSI0",
     .bus = TNT_PCI_BUS_1,
     .device = 17,
     .int_line = ANS_INT_FW0,
     .builtin_card_id = "sym53c825_0"},
    {.slot = 9,
     .kind = PCI_SLOT_BUILTIN,
     .label = "FWSCSI1",
     .bus = TNT_PCI_BUS_1,
     .device = 18,
     .int_line = ANS_INT_FW1,
     .builtin_card_id = "sym53c825_1"},
    {0},
};

static const tnt_board_desc_t ans700_board = {
    .boxid = 0x0800u, // BoxId0 = 1, BoxId1 = 0 (see ans500.c)
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
    .scsi_slots = ans700_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 0,

    .pci_slots = ans700_pci_slots,

    .substrate = &tnt_substrate,
    .board = &ans700_board,
};
