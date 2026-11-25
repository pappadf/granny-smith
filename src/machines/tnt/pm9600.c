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
    TNT_MAC_BOARD_COMMON,
    .boxid = TNT_BOXID_9500, // the 9500's straps (tnt.h)
    .hh_r20 = TNT_HH_R20_9500,
    .bus_hz = 50000000u, // 4:1 bus (200 MHz 604e card)
    .bandit_count = 2,
    .l2_kb = 512u, // "512 KB of fast static RAM on the main logic board" (ibid.)
};

const hw_profile_t machine_pm9600 = {
    .name = "Power Macintosh 9600",
    .id = "pm9600",

    TNT_MAC_PROFILE_COMMON, // the ROM: $960E4BE9 / $960FC647
    TNT_CPU_604, // the 604e card, run as a 604 (tnt.h)
    .freq = 200000000, // 200 MHz

    .ram_default = 0x4000000, // 64 MB
    .ram_max = 0x60000000, // 1.5 GB (twelve DIMM slots, as on the 9500)

    .ram_options = tnt_ram_options_kb,
    .storage = tnt_storage_8500,
    .default_storage = tnt_default_storage, // the CD-ROM drive on the 53C94: see pm7500.c
    .cdrom_drive = &scsi_cd_drive_applecd_1600i, // "a built-in 12X-speed CD-ROM drive, an AppleCD 1600i"

    .pci_slots = tnt_pci_slots_9500,

    .board = &pm9600_board,
};
