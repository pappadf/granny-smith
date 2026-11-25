// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm7300.c
// Power Macintosh 7300 (February 1997) — the 7600's board without the AV
// video-input panel: one Bandit (three PCI slots), MESH + 53C94 SCSI,
// Control/Chaos onboard video, a PowerPC 604e card (166, 180 or 200 MHz)
// and a 256 KB L2 cache DIMM (Apple, "Power Macintosh 7300, 7600, 8600, and
// 9600 Computers" Developer Note, 1996, Tables 1-1, 1-2 and 1-4).  Modeled
// as the 7300/200: 200 MHz on a 50 MHz processor bus.
//
// Identification.  The developer note gives the 7300 "a new machine ID
// value: 109 (hexadecimal 6D)" (ibid., "Machine Identification") — but the
// 1997 ROM it ships with ($960E4BE9) carries the 1995 68k identification
// routine and Open Firmware byte for byte, and both read the board only as
// one of 7500/8500/9500 (or the 7200's Catalyst).  The 7500 straps below
// are therefore what the ROM can see; the System software that shipped
// with the machine is what tells a 7300 from a 7600.

#include "of_nvram.h"
#include "slot_tables.h"
#include "tnt.h"

static const tnt_board_desc_t pm7300_board = {
    // The 7500/7600 straps (pm7500.c): bit 15 pulled high, bit 14 MESH
    // present, bit 13 SET (Open Firmware's 7500 pick), bit 11 CLEAR, bit 8
    // the factory-test strap idling HIGH.
    .boxid = 0x8000u | 0x4000u | 0x2000u | 0x0100u,
    .hh_id = 0x39000000u, // $39 first byte = the TNT identification path
    .hh_r20 = 0x80000000u, // the 7500/8500 class (pm7500.c)
    .bus_hz = 50000000u, // 4:1 bus (200 MHz 604e card)
    .bandit_count = 1,
    .kind = TNT_BOARD_MAC,
    .has_mesh = true,
    .l2_kb = 256u, // the 256 KB cache DIMM (ibid., Table 1-4)
};

const hw_profile_t machine_pm7300 = {
    .name = "Power Macintosh 7300",
    .id = "pm7300",

    // The 604 core with its default PVR ($0004), not the 604e ($0009): POST
    // times a 604e through HID1's PLL configuration, which the core does not
    // model on the 604 class; under a 604 PVR it takes the 1995 path.
    .cpu_model = CPU_MODEL_PPC604,
    .freq = 200000000, // 200 MHz (the 7300/200)
    .mmu_kind = MMU_PPC_604,

    .address_bits = 32,
    .ram_default = 0x2000000, // 32 MB (the 7300/200's shipping configuration)
    .ram_max = 0x40000000, // 1 GB
    .rom_size = 0x400000, // 4 MB ($960E4BE9)

    .ram_options = tnt_ram_options_kb,
    .storage = tnt_storage_7500,
    .default_storage = tnt_default_storage, // the CD-ROM drive on the 53C94: see pm7500.c
    .appletalk = true,
    .cdrom_drive = &scsi_cd_drive_applecd_1600i, // "a built-in 12X-speed CD-ROM drive, an AppleCD 1600i"
    .floppy_slots = mac_floppy_slots_1hd,

    .pci_slots = tnt_pci_slots_1bandit,

    .builtin_video = &tnt_builtin_video,

    .substrate = &tnt_substrate,
    .of_nvram_check = of_nvram_check_tnt,
    .board = &pm7300_board,
};
