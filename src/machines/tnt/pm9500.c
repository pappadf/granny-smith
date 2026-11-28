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

#include "of_nvram.h"
#include "slot_tables.h"
#include "tnt.h"

// The internal fast-SCSI (MESH) bus carries the boot disks; the
// 53C94 carries the CD-ROM drive and the external chain (machine.scsi2).
// The six-slot PCI topology, slot 7's Control stand-in included, is
// tnt_pci_slots_9500 (tnt.c), which the 9600 shares.

static const tnt_board_desc_t pm9500_board = {
    TNT_MAC_BOARD_COMMON,
    // BoxID: bit 11 clear (the 9500 is flagged by Hammerhead +$20 bit 30
    // instead), MESH, the idle-high straps (tnt.h).
    .boxid = TNT_BOXID_9500,
    .hh_r20 = TNT_HH_R20_9500, // bit 30 SET, bit 31 CLEAR: the 9500 class
    .bus_hz = 44000000u, // 3:1 bus (132 MHz 604 card)
    .bandit_count = 2,
};

const hw_profile_t machine_pm9500 = {
    .name = "Power Macintosh 9500",
    .id = "pm9500",

    TNT_MAC_PROFILE_COMMON, // the ROM: $96CD923D / $9630C68B
    TNT_CPU_604,
    .freq = 132000000, // 132 MHz

    .ram_default = 0x4000000, // 64 MB (a typical well-equipped machine)
    .ram_max = 0x60000000, // 1.5 GB

    // Twelve DIMM slots, interleaved in pairs; 1.5 GB architectural max.
    // Offered: the totals Hammerhead's four-pair carve maps today (DIMMs of
    // at most 64 MB), the 8500's list; the six-pair and larger totals wait
    // on the carve.
    .ram_options = tnt_ram_options_kb,
    .storage = tnt_storage_8500,
    .default_storage = tnt_default_storage, // the CD-ROM drive on the 53C94: see pm7500.c
    .cdrom_drive = &scsi_cd_drive_applecd_600i,

    .pci_slots = tnt_pci_slots_9500,

    .board = &pm9500_board,
};

// ---- Power Macintosh 9500/180MP ---------------------------------------------
// The 9500 board with Apple's two-way processor card: two 180 MHz 604-class
// processors on a 45 MHz bus (4:1; the real card's 604e identity is a
// follow-up, see .pvr below).  Identity-identical to a uniprocessor 9500 (Gestalt,
// the OF root, BoxID — Apple, "Power Macintosh 9500 Computers" Developer
// Note); system software finds the second processor only by probing
// Hammerhead's ArbConfig TwoCPU strap, and the ROM boots CPU 0 alone (it has
// no MP code).  Everything the card adds lives in mp.c.
static const tnt_board_desc_t pm9500mp_board = {
    TNT_MAC_BOARD_COMMON,
    .boxid = TNT_BOXID_9500, // the 9500's straps: the card changes none of them
    .hh_r20 = TNT_HH_R20_9500,
    .bus_hz = 45000000u, // 4:1 bus (180 MHz 604e card)
    .bandit_count = 2,
    .mp_cpus = 2,
    // The processors identify as the 604 (the core's default PVR), not the
    // 604e the 180MP card carried: with PVR $0009xxxx Open Firmware in both
    // TNT ROMs ($96CD923D, $9630C68B) stops before building the device tree
    // (its CPU-property code predates the 604e), and nothing in the MP path
    // keys on the PVR.  A follow-up when the 604e model lands.
    .pvr = 0,
};

const hw_profile_t machine_pm9500mp = {
    .name = "Power Macintosh 9500/180MP",
    .id = "pm9500mp",

    TNT_MAC_PROFILE_COMMON, // the ROM: $96CD923D / $9630C68B
    TNT_CPU_604,
    .freq = 180000000, // 180 MHz

    .ram_default = 0x4000000, // 64 MB
    .ram_max = 0x60000000, // 1.5 GB

    .ram_options = tnt_ram_options_kb,
    .storage = tnt_storage_8500,
    .default_storage = tnt_default_storage,
    .cdrom_drive = &scsi_cd_drive_applecd_600i,

    .pci_slots = tnt_pci_slots_9500,

    .board = &pm9500mp_board,
};
