// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm7200.c
// Power Macintosh 7200/90 ("Catalyst", August 1995) — the entry TNT-era
// board.  Bandit (three PCI slots) and Grand Central as on the 7500, but
// the memory controller and the on-board video are one part, Platinum
// (343S1184), at $F8000000 where the 7500's Hammerhead sits, with the
// DACula RAMDAC; there is no Chaos, no Control and no MESH — the Curio's
// 53C94 is the machine's only SCSI bus, internal and external.  A PowerPC
// 601 at 75 or 90 MHz on a half-speed processor bus (37.5 / 45 MHz), four
// DIMM slots to 256 MB, 1 MB of soldered VRAM expandable to 2 or 4 MB
// (Apple, "Power Macintosh 7200 Computer" Developer Note, 1995, Tables 1-1
// and 1-2, "Bus Clock Rates", "VRAM and VRAM Expansion").  Modeled as the
// 7200/90.
//
// The ROM is the 7500/8500/9500's own ($96CD923D, $9630C68B): the board,
// not the image, tells them apart.  Platinum's part identifier ($3001 in
// its upper halfword, platinum.c) is what sends the 68k identification to
// the Catalyst ProductInfo (BoxFlag $66, Gestalt 108) and Open Firmware
// down its Catalyst arm.

#include "of_nvram.h"
#include "slot_tables.h"
#include "tnt.h"

// 168-pin DIMMs, four slots, "linear memory organization only" (ibid.,
// "Data Interleaving"); 4 MB the smallest bank, 256 MB the documented
// maximum.
static const uint32_t pm7200_ram_options_kb[] = {8192,  16384, 24576,  32768,  40960,  49152,  65536,
                                                 81920, 98304, 131072, 163840, 196608, 262144, 0};

// The one SCSI bus: the 53C94 in Curio.  The hard disk bay at ID 0, the
// CD-ROM drive at ID 3, the external DB-25 on the same chain.
static const storage_bay_decl_t pm7200_bays[] = {
    {.unit = 0, .label = "Internal hard disk bay"},
    {.unit = 3, .label = "CD-ROM bay"},
    {0},
};

static const storage_bus_decl_t pm7200_storage[] = {
    MAC_SCSI_BUS("scsi", "SCSI", MEDIA_BUS_SCSI, pm7200_bays, true),
    {0},
};

static const storage_device_decl_t pm7200_default_storage[] = {
    {.bus = "scsi", .unit = 0, .type = STORAGE_DEV_HD},
    {.bus = "scsi", .unit = 3, .type = STORAGE_DEV_CD},
    {0},
};

// Three PCI sockets on Bandit at IDSEL 13/14/15 with their INTA lines on
// Grand Central externals 23/24/25, as on the 7500 — and no built-in PCI
// device: the video is Platinum's.
static const pci_slot_decl_t pm7200_pci_slots[] = {
    {.slot = 1,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot A1",
     .detail = "A1",
     .fill_order = 1,
     .bus = TNT_PCI_BUS_1,
     .device = 13,
     .int_line = 23},
    {.slot = 2,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot B1",
     .detail = "B1",
     .fill_order = 2,
     .bus = TNT_PCI_BUS_1,
     .device = 14,
     .int_line = 24},
    {.slot = 3,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot C1",
     .detail = "C1",
     .fill_order = 3,
     .bus = TNT_PCI_BUS_1,
     .device = 15,
     .int_line = 25},
    {0},
};

static const tnt_board_desc_t pm7200_board = {
    // BoxID: bit 15 pulled high, bit 8 the factory-test strap idling HIGH
    // (pm7500.c), bit 14 CLEAR — no MESH.  Open Firmware's Catalyst arm
    // publishes BoxID >> 11 as AAPL,cpu-id and reads no model bits.
    .boxid = 0x8000u | 0x0100u,
    .bus_hz = 45000000u, // half the 90 MHz processor clock
    .bandit_count = 1,          .kind = TNT_BOARD_MAC,
    .has_mesh = false, // the 53C94 is the one bus (tnt.c)
    .has_chaos = false, // the video is Platinum's, on the processor bus
    .mc = &tnt_platinum_mc,     .video = &tnt_platinum_video_ops,
    .vram_kb = 1024u, // the soldered 1 MB
};

const hw_profile_t machine_pm7200 = {
    .name = "Power Macintosh 7200",
    .id = "pm7200",

    .cpu_model = CPU_MODEL_PPC601,
    .freq = 90000000, // 90 MHz (the 7200/90)
    .mmu_kind = MMU_PPC_601,

    .address_bits = 32,
    .ram_default = 0x1000000, // 16 MB (the 7200/90 16/500 CD)
    .ram_max = 0x10000000, // 256 MB
    .rom_size = 0x400000, // 4 MB ($96CD923D / $9630C68B)

    .ram_options = pm7200_ram_options_kb,
    .storage = pm7200_storage,
    .default_storage = pm7200_default_storage,
    .appletalk = true,
    .cdrom_drive = &scsi_cd_drive_applecd_600i,
    .floppy_slots = mac_floppy_slots_1hd,

    .pci_slots = pm7200_pci_slots,

    .builtin_video = &tnt_platinum_video,

    .substrate = &tnt_substrate,
    .of_nvram_check = of_nvram_check_tnt,
    .board = &pm7200_board,
};
