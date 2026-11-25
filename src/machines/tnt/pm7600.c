// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm7600.c
// Power Macintosh 7600/200 (February 1997) — the 7500's board (one Bandit,
// MESH + 53C94 SCSI, Control/Chaos onboard video, the AV video-input
// panel) with a 200 MHz PowerPC 604e card on a 50 MHz processor bus and a
// 256 KB L2 cache DIMM (Apple, "Power Macintosh 7300, 7600, 8600, and 9600
// Computers" Developer Note, 1996, Tables 1-1, 1-2 and 1-4).  "The new
// Power Macintosh 7600, 8600, and 9600 computers have the same machine ID
// values as their earlier counterparts" (ibid., "Machine Identification"):
// the board identifies exactly as a 7500, so the Mac OS reports Gestalt 68
// for it.  The earlier 7600/120 and /132 (1996) ran the 1995 ROM; this
// model also boots the 1997 $960E4BE9 ROM.

#include "of_nvram.h"
#include "slot_tables.h"
#include "tnt.h"

static const tnt_board_desc_t pm7600_board = {
    TNT_MAC_BOARD_COMMON,
    .boxid = TNT_BOXID_7500, // the 7500's straps (tnt.h)
    .hh_r20 = TNT_HH_R20_7500,
    .bus_hz = 50000000u, // 4:1 bus (200 MHz 604e card)
    .bandit_count = 1,
    .l2_kb = 256u, // the 256 KB cache DIMM (ibid., Table 1-4)
};

const hw_profile_t machine_pm7600 = {
    .name = "Power Macintosh 7600",
    .id = "pm7600",

    TNT_MAC_PROFILE_COMMON, // the ROM: $960E4BE9; the 1995 $96CD923D / $9630C68B
    TNT_CPU_604, // the 604e card, run as a 604 (tnt.h)
    .freq = 200000000, // 200 MHz

    .ram_default = 0x2000000, // 32 MB (the shipping configuration, ibid. Table 1-4)
    .ram_max = 0x40000000, // 1 GB

    // 168-pin DIMMs in 8 slots, interleaved in pairs, as on the 7500.
    .ram_options = tnt_ram_options_kb,
    .storage = tnt_storage_7500,
    .default_storage = tnt_default_storage, // the CD-ROM drive on the 53C94: see pm7500.c
    .cdrom_drive = &scsi_cd_drive_applecd_1600i, // "a built-in 12X-speed CD-ROM drive, an AppleCD 1600i"

    .pci_slots = tnt_pci_slots_1bandit,

    .builtin_video = &tnt_builtin_video,

    .board = &pm7600_board,
};
