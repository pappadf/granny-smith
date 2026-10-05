// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pmg3mt.c
// Power Macintosh G3 mini tower (M4405), and the Macintosh Server G3 that
// ships in the same enclosure: the desktop's logic board and ROM, here
// with the 300 MHz PowerPC 750 module and its 1 MB back-side L2.  The
// enclosure changes nothing the board register or the ROM can see
// (Apple, "Power Macintosh G3 Computers" Developer Note, 1998; "Macintosh
// Server G3" Developer Note, 1998).

#include "gossamer.h"
#include "slot_tables.h"

static const uint32_t pmg3_ram_options_kb[] = {32768,  65536,  98304,  131072, 163840, 196608, 229376,
                                               262144, 294912, 327680, 360448, 393216, 425984, 458752,
                                               524288, 557056, 589824, 655360, 786432, 0};

static const uint8_t whisper_eeprom[16] = {0x0F, 0xAA, 0x55, 0xAA, 'W',  'h',  'i',  's',
                                           'p',  'e',  'r',  0x00, 0x00, 0x00, 0x00, 0x02};

static const gossamer_board_desc_t pmg3mt_board = {
    // As the desktop, with PID 2: the ROM's L2CR entry $B9100000 (1 MB).
    .board_id = 0x8000u | 0x3F00u | (2u << 5) | 0x0010u | (6u << 1) | 0x0001u,
    .bus_hz = 66820000u,
    .pvr = 0x00080202u,
    .hid1 = 0x70000000u, // PLL_CFG 0111: 4.5x -> 300.69 MHz
    .ati_device = 0x4750u,
    .ati_revision = 0x7Cu,
    .perch_eeprom = whisper_eeprom,
};

const hw_profile_t machine_pmg3mt = {
    .name = "Power Macintosh G3 Minitower",
    .id = "pmg3mt",

    .cpu_model = CPU_MODEL_PPC750,
    .freq = 300690000, // bus x 4.5
    .mmu_kind = MMU_PPC_604,

    .address_bits = 32,
    .ram_default = 0x4000000, // 64 MB (a typical well-equipped machine)
    .ram_max = 0x30000000,
    .rom_size = 0x400000,

    .ram_options = pmg3_ram_options_kb,
    .storage = gossamer_storage,
    .default_storage = gossamer_default_storage,
    .appletalk = true,
    .builtin_video = &gossamer_builtin_video,
    .cdrom_drive = &mac_cdrom_drive_applecd,
    .floppy_slots = mac_floppy_slots_1hd,

    .pci_slots = gossamer_pci_slots,

    .substrate = &gossamer_substrate,
    .board = &pmg3mt_board,
};
