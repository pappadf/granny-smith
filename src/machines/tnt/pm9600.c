// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm9600.c
// Power Macintosh 9600/200 (February 1997) — the 9500's six-slot board in
// the new tower enclosure, with a 200 MHz PowerPC 604e card on a 50 MHz
// processor bus and 512 KB of L2 cache on the main logic board (Apple,
// "Power Macintosh 7300, 7600, 8600, and 9600 Computers" Developer Note,
// 1996, Tables 1-1, 1-2 and 1-4).  It keeps the 9500's machine ID (ibid.,
// "Machine Identification"), so the board straps are the 9500's.  Like the
// 9500 it has no onboard video: it shipped with an accelerated PCI display
// card (ibid., "Accelerated Display Card"), and the Control/Chaos stand-in
// in slot 7 retires as soon as a socket supplies one (pm9500.c).  Runs the
// 1997 ROMs: $960E4BE9 and the later $960FC647 ("Kansas").
//
// The 9600/200MP's dual-604e card is not modeled: the emulator runs one
// processor, so Hammerhead's ArbConfig reports TwoCPU clear (hammerhead.c).

#include "of_nvram.h"
#include "slot_tables.h"
#include "tnt.h"

static const tnt_board_desc_t pm9600_board = {
    // The 9500's straps (pm9500.c): BoxID bit 11 clear, MESH present,
    // idle-high bit 15, the factory-test strap HIGH; Hammerhead +$20 bit
    // 30 marks the 9500 class for both the 68k routine and Open Firmware.
    .boxid = 0x8000u | 0x4000u | 0x0100u,
    .hh_id = 0x39000000u, // $39 first byte = the TNT identification path
    .hh_r20 = 0x40000000u,
    .bus_hz = 50000000u, // 4:1 bus (200 MHz 604e card)
    .bandit_count = 2,
    .kind = TNT_BOARD_MAC,
    .has_mesh = true,
    .has_chaos = true, // the display-bus bridge
    .l2_kb = 512u, // "512 KB of fast static RAM on the main logic board" (ibid.)
};

const hw_profile_t machine_pm9600 = {
    .name = "Power Macintosh 9600",
    .id = "pm9600",

    // The 604 core with its default PVR ($0004), not the 604e ($0009): POST
    // times a 604e through HID1's PLL configuration, which the core does not
    // model on the 604 class; under a 604 PVR it takes the 1995 path.
    .cpu_model = CPU_MODEL_PPC604,
    .freq = 200000000, // 200 MHz
    .mmu_kind = MMU_PPC_604,

    .address_bits = 32,
    .ram_default = 0x4000000, // 64 MB
    .ram_max = 0x60000000, // 1.5 GB (twelve DIMM slots, as on the 9500)
    .rom_size = 0x400000, // 4 MB ($960E4BE9 / $960FC647)

    .ram_options = tnt_ram_options_kb,
    .storage = tnt_storage_8500,
    .default_storage = tnt_default_storage, // the CD-ROM drive on the 53C94: see pm7500.c
    .appletalk = true,
    .cdrom_drive = &scsi_cd_drive_applecd_1600i, // "a built-in 12X-speed CD-ROM drive, an AppleCD 1600i"
    .floppy_slots = mac_floppy_slots_1hd,

    .pci_slots = pm9500_pci_slots,

    .substrate = &tnt_substrate,
    .of_nvram_check = of_nvram_check_tnt,
    .board = &pm9600_board,
};
