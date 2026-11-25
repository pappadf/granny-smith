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
    // BoxID: bit 11 SET = 8500 (the shipping ROM's 68k identification
    // routine at $FFC14844 — the one BoxID bit that dispatch reads), and
    // bit 13 CLEAR = 8500 in Open Firmware's decode (OpenFW $10592 picks
    // "AAPL,7500" over "AAPL,8500" on bit 13 of xw@>>11 — the earlier
    // "composite video" reading of bit 13 was wrong).  MESH present,
    // idle-high straps, and bit 8 — the factory-test strap — HIGH: POST
    // tests it on every boot after the first on a formatted store and a
    // clear bit is the Serial Test Manager instead of a boot (pm7500.c).
    .boxid = 0x8000u | 0x4000u | 0x0800u | 0x0100u,
    .hh_id = 0x39000000u, // $39 first byte = the TNT identification path
    // +$20 bit 31 SET = the 7500/8500 class in Open Firmware's selector
    // (see pm7500.c); bit 30 clear = not a 9500 for the 68k routine.
    .hh_r20 = 0x80000000u,
    .bus_hz = 40000000u, // 3:1 bus (120 MHz 604 card)
    .bandit_count = 2,
    .kind = TNT_BOARD_MAC,
    .has_mesh = true, // the internal fast-SCSI cell (absent on the Network Servers)
    .has_chaos = true, // the display-bus bridge
};

const hw_profile_t machine_pm8500 = {
    .name = "Power Macintosh 8500",
    .id = "pm8500",

    .cpu_model = CPU_MODEL_PPC604,
    .freq = 120000000, // 120 MHz
    .mmu_kind = MMU_PPC_604,

    .address_bits = 32,
    .ram_default = 0x2000000, // 32 MB (a typical well-equipped machine)
    .ram_max = 0x40000000, // 1 GB
    .rom_size = 0x400000, // 4 MB ($96CD923D / $9630C68B)

    // 168-pin DIMMs in 8 slots, interleaved in pairs (tnt.c)
    .ram_options = tnt_ram_options_kb,
    .storage = tnt_storage_8500,
    .default_storage = tnt_default_storage, // the CD-ROM drive on the 53C94: see pm7500.c
    .appletalk = true,
    .cdrom_drive = &scsi_cd_drive_applecd_600i,
    .floppy_slots = mac_floppy_slots_1hd,

    .pci_slots = tnt_pci_slots_1bandit,

    .builtin_video = &tnt_builtin_video,

    .substrate = &tnt_substrate,
    .of_nvram_check = of_nvram_check_tnt,
    .board = &pm8500_board,
};
