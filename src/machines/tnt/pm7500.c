// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm7500.c
// Power Macintosh 7500/100 ("TNT", 100 MHz MPC601 daughtercard, August
// 1995) — the 601 member of the family.  One Bandit (three PCI slots),
// MESH + 53C94 SCSI, Control/Chaos onboard video, 50 MHz processor bus
// (Apple, "Power Macintosh 7500 and 8500 Computers" Developer Note, 1995).

#include "of_nvram.h"
#include "slot_tables.h"
#include "tnt.h"

// 168-pin DIMMs in 8 slots, interleaved in pairs; 1 GB architectural max.
// Offered: the totals Hammerhead's four-pair carve maps today (DIMMs of at
// most 64 MB); larger and odd totals wait on the carve.
static const uint32_t pm7500_ram_options_kb[] = {
    8192,   16384,  24576,  32768,  40960,  49152,  57344,  65536,  73728,  81920,  90112,  98304,
    106496, 114688, 122880, 131072, 139264, 147456, 155648, 163840, 172032, 180224, 188416, 196608,
    204800, 212992, 221184, 229376, 237568, 245760, 262144, 270336, 278528, 286720, 294912, 303104,
    311296, 327680, 335872, 344064, 360448, 393216, 401408, 409600, 425984, 458752, 524288, 0};

// The internal fast-SCSI (MESH) bus carries the boot disks; the
// 53C94 carries the CD-ROM drive and the external chain (machine.scsi2).

static const tnt_board_desc_t pm7500_board = {
    TNT_MAC_BOARD_COMMON,
    // BoxID: MESH, bit 13 SET (Open Firmware's 7500 pick), bit 11 CLEAR
    // (not the 68k routine's 8500), the idle-high straps (tnt.h).
    .boxid = TNT_BOXID_7500,
    .hh_r20 = TNT_HH_R20_7500, // bit 30 clear: not a 9500 for the 68k routine
    .bus_hz = 50000000u, // 2:1 bus (100 MHz 601 card)
    .bandit_count = 1,
};

const hw_profile_t machine_pm7500 = {
    .name = "Power Macintosh 7500",
    .id = "pm7500",

    TNT_MAC_PROFILE_COMMON, // the ROM: $96CD923D / $9630C68B
    TNT_CPU_601,
    .freq = 100000000, // 100 MHz

    .ram_default = 0x2000000, // 32 MB (a typical well-equipped machine)
    .ram_max = 0x40000000, // 1 GB

    .ram_options = pm7500_ram_options_kb,
    // Factory configuration: an internal CD-ROM drive at SCSI ID 3, "Most
    // configurations also include a built-in CD-ROM drive" (Power Macintosh
    // 7500/8500 Developer Note, S1).  It sits on the SLOW 5 MB/s Curio 53C94
    // bus, the one also brought out to the external DB-25, not on the 10 MB/s
    // MESH bus that carries the internal hard disk ("a SCSI bus for external
    // SCSI devices and for the internal CD-ROM drive", ibid. S3) -- tnt.c
    // builds that bus as `machine.scsi2`.  Measured: seating it on MESH
    // instead breaks tnt-voodoo2-glide's Mac OS 8.1 startup.
    .storage = tnt_storage_7500,
    .default_storage = tnt_default_storage,
    .cdrom_drive = &scsi_cd_drive_applecd_600i,

    .pci_slots = tnt_pci_slots_1bandit,

    .builtin_video = &tnt_builtin_video,

    .board = &pm7500_board,
};
