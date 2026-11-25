// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// wgs9150.c
// Apple Workgroup Server 9150/80 (April 1994) and 9150/120 (April 1995) —
// the Power Macintosh 8100's board in a tower server case.  What differs
// from the 8100 (Apple, "Workgroup Server 9150" Developer Note, 1995):
//
//   * its own CPU identification: the 3 low bits of the machine-ID
//     register at $5FFFFFFC read %100 (the 8100's read %011), so the
//     register is $A55A3014 — and both 9150 ROMs carry a ProductInfo
//     record for $3014 that the 6100/7100/8100 ROM lacks.  Gestalt
//     reports $27 (9150/80) or $39 (9150/120) (ibid., Table 1);
//   * a fourth NuBus slot at slot address $A, its /NMRQ on the FAT AMIC's
//     pseudo-VIA2 slot bank bit 1 ("SLT4"; ibid., Figure 1);
//   * 80 MHz on a 40 MHz bus, or 120 MHz on a 40 MHz bus (3:1; Table 2);
//   * BART 4 (9150/80) or BART 21 (9150/120) — register compatible.
//
// Each speed has its own ROM ($9C7C98F7 / $9B037F6F), so each is its own
// profile.

#include "pdm.h"
#include "slot_tables.h"

#include "nubus.h"
#include "pram_defaults.h"

// The 8100's SIMM complement: 8 MB soldered + four SIMM pairs, carved into
// fixed-window banks of {2,8,32} MB.
static const uint32_t wgs9150_ram_options_kb[] = {
    8192,   10240,  12288,  14336,  16384,  18432,  20480,  22528,  24576,  26624,  28672,  30720,  32768,  34816,
    36864,  38912,  40960,  43008,  45056,  47104,  49152,  51200,  53248,  55296,  57344,  59392,  61440,  63488,
    65536,  67584,  69632,  73728,  75776,  77824,  79872,  81920,  83968,  86016,  88064,  90112,  92160,  94208,
    96256,  98304,  100352, 102400, 106496, 108544, 110592, 112640, 114688, 116736, 118784, 122880, 124928, 126976,
    131072, 139264, 141312, 143360, 145408, 147456, 149504, 151552, 155648, 157696, 159744, 163840, 172032, 174080,
    176128, 180224, 188416, 204800, 206848, 208896, 212992, 221184, 237568, 270336, 0};

// The 8100's three NuBus connectors plus the 9150's fourth at slot $A.
static const struct nubus_slot_decl wgs9150_nubus_slots[] = {
    {.slot = 0xA, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot A", .fill_order = 4},
    {.slot = 0xC, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot B", .fill_order = 1},
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot C", .fill_order = 2},
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot D", .fill_order = 3},
    {0},
};

// The 9150/80: 80 MHz 601 on a 40 MHz bus, as the 8100/80.
static const pdm_board_desc_t wgs9150_80_board = {
    .machine_id = 0x3014, // the 9150's own CPU identification (%100)
    .bus_hz = 40000000u, // 2:1 bus
    .bank_layout = PDM_BANKS_FIXED,
    .bank_count = 8,
    .wait_state_penalty = 2, // the 8100's (pdm-rom-ladder L7)
    .has_fast_scsi = true, // the 8100's discrete 53CF96 internal bus
    .has_slot_a = true, // the fourth NuBus connector
};

// The 9150/120: 120 MHz 601 on a 40 MHz bus (3:1).
static const pdm_board_desc_t wgs9150_120_board = {
    .machine_id = 0x3014,
    .bus_hz = 40000000u, // 3:1 bus
    .bank_layout = PDM_BANKS_FIXED,
    .bank_count = 8,
    // Sized so HWInit's bus-ratio measurement lands on 3:1 (a 40 MHz bus;
    // 2 measured 60 MHz, the 2:1 boards' answer).
    .wait_state_penalty = 3,
    .has_fast_scsi = true,
    .has_slot_a = true,
};

const hw_profile_t machine_wgs9150 = {
    .name = "Workgroup Server 9150/80",
    .id = "wgs9150",

    .cpu_model = CPU_MODEL_PPC601,
    .freq = 80000000, // 80 MHz
    .mmu_kind = MMU_PPC_601,

    .address_bits = 32,
    .ram_default = 0x2000000, // 32 MB
    .ram_max = 0x10800000, // 264 MB
    .rom_size = 0x400000, // 4 MB ($9C7C98F7)

    .ram_options = wgs9150_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_cd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .cdrom_drive = &scsi_cd_drive_applecd_300i_plus,

    .builtin_video = &pdm_builtin_video,
    .nubus_slots = wgs9150_nubus_slots,

    .pram = &pram_defaults_pdm,
    .substrate = &pdm_substrate,
    .board = &wgs9150_80_board,
};

const hw_profile_t machine_wgs9150_120 = {
    .name = "Workgroup Server 9150/120",
    .id = "wgs9150_120",

    .cpu_model = CPU_MODEL_PPC601,
    .freq = 120000000, // 120 MHz
    .mmu_kind = MMU_PPC_601,

    .address_bits = 32,
    .ram_default = 0x2000000, // 32 MB
    .ram_max = 0x10800000, // 264 MB
    .rom_size = 0x400000, // 4 MB ($9B037F6F)

    .ram_options = wgs9150_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_cd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .cdrom_drive = &scsi_cd_drive_applecd_300i_plus,

    .builtin_video = &pdm_builtin_video,
    .nubus_slots = wgs9150_nubus_slots,

    .pram = &pram_defaults_pdm,
    .substrate = &pdm_substrate,
    .board = &wgs9150_120_board,
};
