// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pmg3dt.c
// Power Macintosh G3 desktop (M3979) on the Rev C "Silk" logic board
// (820-0991): a 266 MHz PowerPC 750 module with a 512 KB back-side L2, the
// ATI Rage Pro Turbo soldered on as PCI device $12, three SDRAM DIMM
// slots, three PCI slots, a Whisper personality card, one MESH SCSI bus
// and the manual-inject SuperDrive (Apple, "Power Macintosh G3 Computers"
// Developer Note, 1998).  Boots the Rev C ROM ($78F57389, Open Firmware
// 2.4); the Rev A ROM ($79D68D63, 2.0f1) runs the same board program.

#include "gossamer.h"
#include "slot_tables.h"

// PC66 SDRAM, three 168-pin DIMM slots, 256 MB per slot at most.
static const uint32_t pmg3_ram_options_kb[] = {32768, 65536, 98304, 131072, 196608, 262144, 393216, 524288, 786432, 0};

static const scsi_bus_decl_t pmg3_scsi_buses[] = {
    {.object = "scsi", .label = "SCSI", .slots = gossamer_scsi_slots},
    {0},
};

// The Whisper card's ID EEPROM (I2C $53): a length/marker byte, the
// AA 55 AA signature, the NUL-terminated name, the version the tree
// surfaces as `version 00000002` (read back from a real Rev C machine with
// `8c 0 0a6 read-iic` at an Open Firmware prompt).
static const uint8_t whisper_eeprom[16] = {0x0F, 0xAA, 0x55, 0xAA, 'W',  'h',  'i',  's',
                                           'p',  'e',  'r',  0x00, 0x00, 0x00, 0x00, 0x02};

static const gossamer_board_desc_t pmg3dt_board = {
    // bit 15 SWIM3, bit 14 clear (burst ROM), bits 13-8 all ones (three
    // empty slots), PID 1 (the ROM's L2CR table entry $A9100000: 512 KB,
    // L2CLK /2, pipelined burst), bit 4 set (not an All-in-One), bus code
    // 6 (66.82 MHz), bit 0 set (no Bose module).
    .board_id = 0x8000u | 0x3F00u | (1u << 5) | 0x0010u | (6u << 1) | 0x0001u,
    .bus_hz = 66820000u,
    .pvr = 0x00080202u, // 750 rev 2.2, the stock part
    .hid1 = 0xA0000000u, // PLL_CFG 1010: 4x -> 267.28 MHz (the ROM's own table)
    .ati_device = 0x4750u, // Rage Pro Turbo (GP)
    .ati_revision = 0x7Cu,
    .perch_eeprom = whisper_eeprom,
};

const hw_profile_t machine_pmg3dt = {
    .name = "Power Macintosh G3 (Desktop)",
    .id = "pmg3dt",

    .cpu_model = CPU_MODEL_PPC750,
    .freq = 267280000, // bus x 4
    .mmu_kind = MMU_PPC_604, // the architected split-BAT MMU (MPC750UM §5)

    .address_bits = 32,
    .ram_default = 0x4000000, // 64 MB
    .ram_max = 0x30000000, // 768 MB: three 256 MB DIMMs
    .rom_size = 0x400000, // 4 MB ($78F57389 / $79D68D63)

    .ram_options = pmg3_ram_options_kb,
    .scsi_buses = pmg3_scsi_buses,
    // A SCSI CD-ROM on the MESH bus at the era's id 3 (the shipping drive
    // is ATAPI on ATA channel 1; this is the SCSI path the install CD can
    // boot through).
    .has_cdrom = true,
    .cdrom_id = 3,
    .cdrom_drive = &mac_cdrom_drive_applecd,
    .floppy_slots = mac_floppy_slots_1hd,

    .pci_slots = gossamer_pci_slots,

    .substrate = &gossamer_substrate,
    .board = &pmg3dt_board,
};
