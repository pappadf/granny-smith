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
    // The 7500's straps (pm7500.c): bit 15 pulled high, bit 14 MESH
    // present, bit 13 SET (Open Firmware's 7500-over-8500 pick), bit 11
    // CLEAR (the 68k routine's 8500 bit), bit 8 the factory-test strap
    // idling HIGH.
    .boxid = 0x8000u | 0x4000u | 0x2000u | 0x0100u,
    .hh_id = 0x39000000u, // $39 first byte = the TNT identification path
    .hh_r20 = 0x80000000u, // the 7500/8500 class (pm7500.c)
    .bus_hz = 50000000u, // 4:1 bus (200 MHz 604e card)
    .bandit_count = 1,
    .kind = TNT_BOARD_MAC,
    .has_mesh = true,
    .has_chaos = true, // the display-bus bridge
    .l2_kb = 256u, // the 256 KB cache DIMM (ibid., Table 1-4)
};

const hw_profile_t machine_pm7600 = {
    .name = "Power Macintosh 7600",
    .id = "pm7600",

    // The 604 core with its default PVR ($0004), not the 604e ($0009): POST
    // times a 604e through HID1's PLL configuration, which the core does not
    // model on the 604 class; under a 604 PVR it takes the 1995 path.
    .cpu_model = CPU_MODEL_PPC604,
    .freq = 200000000, // 200 MHz
    .mmu_kind = MMU_PPC_604,

    .address_bits = 32,
    .ram_default = 0x2000000, // 32 MB (the shipping configuration, ibid. Table 1-4)
    .ram_max = 0x40000000, // 1 GB
    .rom_size = 0x400000, // 4 MB ($960E4BE9; the 1995 $96CD923D / $9630C68B)

    // 168-pin DIMMs in 8 slots, interleaved in pairs, as on the 7500.
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
    .board = &pm7600_board,
};
