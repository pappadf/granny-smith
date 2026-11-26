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
// value: 109 (hexadecimal 6D)" (ibid., "Machine Identification").  The ROM
// does not produce it: the 1997 ROM ($960E4BE9) carries the 1995 68k
// identification routine and Open Firmware byte for byte, and its
// ProductInfo directory still holds only the 9500/7500/8500/7200 records
// (BoxFlag $3D/$3E/$3F/$66).  The System does.  System 7.6 and Mac OS 8.1
// both install a startup patch that, on the TNT ROM (ROM version $077D,
// release $28 or $34) with a Hammerhead identifier byte of $39 and BoxFlag
// $3E, reads BoxID with a big-endian `AND.L #...,$F301A000` and rewrites
// the identity: little-endian bit 13 CLEAR -> BoxFlag $68 (Gestalt 110);
// then little-endian bit 15 CLEAR -> BoxFlag $67 (Gestalt 109, the 7300).
// (System 7.6 image, its patch code at disk offset $36E168.)  So the 7300
// board is a 7600 whose BoxID bit 15 is strapped LOW — the bit every other
// TNT board idles high — and bit 13 high (Open Firmware's 7500 pick).

#include "of_nvram.h"
#include "slot_tables.h"
#include "tnt.h"

static const tnt_board_desc_t pm7300_board = {
    TNT_MAC_BOARD_COMMON,
    // The 7600's straps (tnt.h) with bit 15 LOW -- the 7300 strap the
    // System's identification patch reads (above).
    .boxid = TNT_BOXID_7500 & ~TNT_BOXID_IDLE, .hh_r20 = TNT_HH_R20_7500,
    .bus_hz = 50000000u, // 4:1 bus (200 MHz 604e card)
    .bandit_count = 1,
    .l2_kb = 256u, // the 256 KB cache DIMM (ibid., Table 1-4)
};

const hw_profile_t machine_pm7300 = {
    .name = "Power Macintosh 7300",
    .id = "pm7300",

    TNT_MAC_PROFILE_COMMON, // the ROM: $960E4BE9
    TNT_CPU_604, // the 604e card, run as a 604 (tnt.h)
    .freq = 200000000, // 200 MHz (the 7300/200)

    .ram_default = 0x2000000, // 32 MB (the 7300/200's shipping configuration)
    .ram_max = 0x40000000, // 1 GB

    .ram_options = tnt_ram_options_kb,
    .storage = tnt_storage_7500,
    .default_storage = tnt_default_storage, // the CD-ROM drive on the 53C94: see pm7500.c
    .cdrom_drive = &scsi_cd_drive_applecd_1600i, // "a built-in 12X-speed CD-ROM drive, an AppleCD 1600i"

    .pci_slots = tnt_pci_slots_1bandit,

    .builtin_video = &tnt_builtin_video,

    .board = &pm7300_board,
};
