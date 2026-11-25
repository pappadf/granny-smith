// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm8600.c
// Power Macintosh 8600/200 (February 1997) — the 8500's board (two
// Bandits, MESH + 53C94 SCSI, Control/Chaos onboard video plus the
// unmodeled AV video in/out) in the new tower enclosure, with a 200 MHz
// PowerPC 604e card on a 50 MHz processor bus and a 256 KB L2 cache DIMM
// (Apple, "Power Macintosh 7300, 7600, 8600, and 9600 Computers" Developer
// Note, 1996, Tables 1-1, 1-2 and 1-4).  It keeps the 8500's machine ID
// (ibid., "Machine Identification"), so the board straps are the 8500's.
// Runs the 1997 ROMs: $960E4BE9 and the later $960FC647 ("Kansas") that
// came with the 250/300 MHz cards (Apple, "Enhanced Power Macintosh 8600
// and 9600" Developer Note, 1997).

#include "of_nvram.h"
#include "slot_tables.h"
#include "tnt.h"

static const tnt_board_desc_t pm8600_board = {
    TNT_MAC_BOARD_COMMON,
    .boxid = TNT_BOXID_8500, // the 8500's straps (tnt.h)
    .hh_r20 = TNT_HH_R20_7500,
    .bus_hz = 50000000u, // 4:1 bus (200 MHz 604e card)
    .bandit_count = 2,
    .l2_kb = 256u, // the 256 KB cache DIMM (ibid., Table 1-4)
};

const hw_profile_t machine_pm8600 = {
    .name = "Power Macintosh 8600",
    .id = "pm8600",

    TNT_MAC_PROFILE_COMMON, // the ROM: $960E4BE9 / $960FC647
    TNT_CPU_604, // the 604e card, run as a 604 (tnt.h)
    .freq = 200000000, // 200 MHz

    .ram_default = 0x2000000, // 32 MB (the shipping configuration, ibid. Table 1-4)
    .ram_max = 0x40000000, // 1 GB

    // 168-pin DIMMs in 8 slots, interleaved in pairs, as on the 8500.
    .ram_options = tnt_ram_options_kb,
    .storage = tnt_storage_8500,
    .default_storage = tnt_default_storage, // the CD-ROM drive on the 53C94: see pm7500.c
    .cdrom_drive = &scsi_cd_drive_applecd_1600i, // "a built-in 12X-speed CD-ROM drive, an AppleCD 1600i"

    .pci_slots = tnt_pci_slots_1bandit,

    .builtin_video = &tnt_builtin_video,

    .board = &pm8600_board,
};
