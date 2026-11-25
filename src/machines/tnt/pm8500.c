// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm8500.c
// Power Macintosh 8500/120 ("Nitro", 120 MHz MPC604 daughtercard, August
// 1995) — the AV tower.  Two Bandits, MESH + 53C94 SCSI, Control/Chaos
// onboard video plus the (unmodeled) Sixty6/Plan B AV path, 40 MHz
// processor bus at 3:1 (Apple, "Power Macintosh 7500 and 8500 Computers"
// Developer Note, 1995).

#include "of_nvram.h"
#include "slot_tables.h"
#include "tnt.h"

// The internal fast-SCSI (MESH) bus carries the boot disks; the
// 53C94 carries the CD-ROM drive and the external chain (machine.scsi2).

static const tnt_board_desc_t pm8500_board = {
    TNT_MAC_BOARD_COMMON,
    // BoxID: bit 11 SET (the 68k routine's 8500), bit 13 CLEAR (Open
    // Firmware's 8500), MESH, the idle-high straps (tnt.h).
    .boxid = TNT_BOXID_8500,
    .hh_r20 = TNT_HH_R20_7500, // the 7500/8500 class; bit 30 clear: not a 9500
    .bus_hz = 40000000u, // 3:1 bus (120 MHz 604 card)
    .bandit_count = 2,
};

const hw_profile_t machine_pm8500 = {
    .name = "Power Macintosh 8500",
    .id = "pm8500",

    TNT_MAC_PROFILE_COMMON, // the ROM: $96CD923D / $9630C68B
    TNT_CPU_604,
    .freq = 120000000, // 120 MHz

    .ram_default = 0x2000000, // 32 MB (a typical well-equipped machine)
    .ram_max = 0x40000000, // 1 GB

    // 168-pin DIMMs in 8 slots, interleaved in pairs (tnt.c)
    .ram_options = tnt_ram_options_kb,
    .storage = tnt_storage_8500,
    .default_storage = tnt_default_storage, // the CD-ROM drive on the 53C94: see pm7500.c
    .cdrom_drive = &scsi_cd_drive_applecd_600i,

    .pci_slots = tnt_pci_slots_1bandit,

    .builtin_video = &tnt_builtin_video,

    .board = &pm8500_board,
};
