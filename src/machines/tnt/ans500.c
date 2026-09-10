// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ans500.c
// Apple Network Server 500/132 ("Shiner LE", February 1996) — Apple's only
// non-Macintosh computer, and the first machine in this repository whose
// primary operating system is a commercial Unix (AIX 4.1.5 for Apple
// Network Servers).  132 MHz MPC604 on a 44 MHz processor bus, 512 KB
// L2 cache DIMM, 32 MB of parity DRAM in eight slots, six PCI slots split
// 2/4 across two Bandits, seven hot-swap SCSI bays, single 325 W supply.
//
// The board is a Power Macintosh 9500 with the Macintosh removed.  Apple
// says so itself, in the first paragraph of its own developer note:
//
//   "This specification is unfortunately not one-stop shopping, owing to
//    the architectural origins of the Network Servers in the PowerMac 9500
//    family.  Therefore much of the hardware detail which is fully
//    documented in the PowerMac family is not repeated here.  Instead,
//    unique hardware interfaces are described."
//   (Apple, "Network Server Hardware Developer Notes", 1996, §1.)
//
// So this profile is a DELTA over pm9500.c: same Hammerhead, same two
// Bandits, same Grand Central and its entire internal subtree, same DBDMA,
// same 53C94 external bus, same Cuda/ADB/ESCC/AWACS.  What changes is
// enumerated in tnt.h's tnt_board_desc_t and, for the parts a profile
// carries, right here — the interrupt rewiring, the slot map, and the four
// board facts (no MESH, GBUS present, parity DRAM, L2 size).
//
// The ROM is Apple's production Open Firmware 1.1.22 ($962F6C13); it has
// no Mac OS Toolbox at all and refuses to boot Mac OS.  The 2.0 prototype
// ROM ($49B2BE8F) is the mirror image — Mac OS only, no AIX — and both
// identify against the same hardware model (rom.c).

#include "slot_tables.h"
#include "tnt.h"

// Eight DIMM slots in four interleaved bank pairs (against the 9500's
// twelve), and a hard 512 MB ceiling that is a ROM DECODE limit, not a
// Hammerhead one: "Production ROMs through (TBD) August 1996? provide
// decoding of up to 512 Mbytes" (ibid., §5), and the documented failure
// mode above it is a HANG during the RAM test rather than a clean error.
// Being less permissive than the silicon is the faithful choice here.
static const uint32_t ans500_ram_options_kb[] = {16384, 32768, 49152, 65536, 131072, 262144, 524288, 0};

// Bays 4-6 cable to the SECOND fast/wide controller, which the production
// ROM's own device aliases settle: `disk0`..`disk3` resolve through
// `/bandit/53c825@11`, `disk4` onward through `@12`.  That controller is
// live -- tnt_fwscsi_attach binds channel 1 to machine.scsi2, and
// ans-scsi drives a CD-ROM on it through Open Firmware's probe-scsi2 --
// so the bays are declared rather than described.
//
// Bay 0 is Apple's expected CD-ROM position and is deliberately NOT declared
// here: it is what hw_profile_t.cdrom_id addresses, which F-10 of the
// 2026-09-03 review owns.
static const struct scsi_slot ans500_scsi_slots_fw1[] = {
    {.label = "Bay 5 (fast/wide 1)", .id = 4},
    {.label = "Bay 6 (fast/wide 1)", .id = 5},
    {.label = "Bay 7 (fast/wide 1)", .id = 6},
    {0},
};

static const scsi_bus_decl_t ans500_scsi_buses[] = {
    {.object = "scsi", .label = "Front backplane (fast/wide 0)", .slots = ans_scsi_slots_fw0},
    {.object = "scsi2", .label = "Front backplane (fast/wide 1)", .slots = ans500_scsi_slots_fw1},
    {0},
};

static const tnt_board_desc_t ans500_board = {
    // BoxID straps.  The 9500 bits stand ("bits not mentioned are the same
    // as in 9500"), and the ANS adds a top byte whose only CONSTANT member
    // is the hardwired box identifier BoxId0 = 1 / BoxId1 = 0 (LE bits 11
    // and 12).  Bit 14 is NOT the Macintosh boards' "MESH present" strap
    // here — it is `Keyswitch LockedL` — and bit 15 is `TwoSuppliesH`, so
    // neither of the 9500's idle-high straps applies.  The keyswitch and
    // PSU bits are live state, contributed at read time by gbus.c.
    //
    // Bit 8 idles HIGH on this board.  POST reads Board Register 1
    // byte-reversed (`lwbrx`) on every boot whose store already carries
    // its "RobG" signature, and `andi. r6,r6,0x100` decides: bit 8 clear
    // sends it into the ROM's serial diagnostic monitor (`>` on ttya,
    // T/A/Q commands, no timeout) instead of Open Firmware.  A Network
    // Server that boots is a Network Server whose bit 8 reads set.
    .boxid = 0x0800u | 0x0100u, // BoxId0 = 1, BoxId1 = 0; bit 8 high
    .hh_id = 0x39000000u, // $39 first byte = the TNT identification path
    // The ANS ROM is built from the 9500 v2 codebase and takes the 9500
    // arm of Hammerhead's +$20 selector (bit 30 SET); machine identity
    // proper comes from the 53C825A probe that sets `?esb`, not from here.
    .hh_r20 = 0x40000000u,
    // "The system clock is provided by the CPU card" and 40-50 MHz is
    // supported (ibid., §6).  The 700/150's 50 MHz is ATTESTED — the
    // machine prints "150 MHz 604, 50 MHz Bus" on its own LCD.  The
    // 500/132's 44 MHz is DERIVED from the fast-L2 rule ("Network Server
    // ROM enables 'fast L2' mode for bus speeds of 44 MHz or less") and
    // the 3:1 ratio the 132 MHz card implies; treat it as a fitted
    // constant until the ladder shows which path the ROM takes.
    .bus_hz = 44000000u,
    .bandit_count = 2,
    .kind = TNT_BOARD_SHINER,
    .has_mesh = false, // delta #4: MESH is gone; two 53C825As replace it
    .has_gbus = true, // delta #6/#9: LCD, board registers, Ethernet PROM
    .has_parity = true, // delta #7: parity DRAM, and it selects 60 ns timing
    .l2_kb = 512u, // 512 KB cache DIMM (8500-compatible slot)
    .two_supplies = false, // one 325 W supply; TwoSuppliesH reads low
};

const hw_profile_t machine_ans500 = {
    .name = "Apple Network Server 500/132",
    .id = "ans500",

    .cpu_model = CPU_MODEL_PPC604,
    .freq = 132000000, // 132 MHz 604 processor card
    .mmu_kind = MMU_PPC_604,

    .address_bits = 32,
    .ram_default = 0x2000000, // 32 MB parity (the shipping configuration)
    .ram_max = 0x20000000, // 512 MB — the ROM's decode ceiling, not the chipset's
    .rom_size = 0x400000, // 4 MB ($962F6C13 production / $49B2BE8F prototype)

    .ram_options = ans500_ram_options_kb,
    .scsi_buses = ans500_scsi_buses,
    .floppy_slots = mac_floppy_slots_1hd,
    // Bay 0 is the CD-ROM bay Apple expects, and it is the documented
    // install path: with the front keyswitch in Service on a machine that
    // has never been booted, Open Firmware "will automatically attempt to
    // find a diagnostic floppy or Install CD to boot from."
    .has_cdrom = true,
    .cdrom_id = 0,

    .pci_slots = ans_pci_slots,

    .substrate = &tnt_substrate,
    .board = &ans500_board,
};
