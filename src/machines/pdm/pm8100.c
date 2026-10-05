// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm8100.c
// Power Macintosh 8100/80 ("Cold Fusion", 80 MHz MPC601, March 1994) — the
// PDM flagship.  Machine-ID register $A55A3013; 40 MHz bus (2:1); eight
// fixed-window SIMM banks; three NuBus slots ($B/$C/$D) behind BART + the
// PDS video slot $E (no PDS card is modeled: that window reads empty); a
// second, discrete 53CF96 on a fast internal SCSI bus (modeled with no
// devices attached — media land on the standard Curio bus).

#include "pdm.h"
#include "slot_tables.h"

#include "nubus.h"
#include "pram_defaults.h"

// 8 MB soldered + four SIMM pairs, carved into fixed-window banks of
// {2,8,32} MB: every total the pairs make, up to 264 MB.
static const uint32_t pm8100_ram_options_kb[] = {
    8192,   10240,  12288,  14336,  16384,  18432,  20480,  22528,  24576,  26624,  28672,  30720,  32768,  34816,
    36864,  38912,  40960,  43008,  45056,  47104,  49152,  51200,  53248,  55296,  57344,  59392,  61440,  63488,
    65536,  67584,  69632,  73728,  75776,  77824,  79872,  81920,  83968,  86016,  88064,  90112,  92160,  94208,
    96256,  98304,  100352, 102400, 106496, 108544, 110592, 112640, 114688, 116736, 118784, 122880, 124928, 126976,
    131072, 139264, 141312, 143360, 145408, 147456, 149504, 151552, 155648, 157696, 159744, 163840, 172032, 174080,
    176128, 180224, 188416, 204800, 206848, 208896, 212992, 221184, 237568, 270336, 0};

// One internal manual-inject SuperDrive behind SWIM3, and no external
// port — the PDM family has no second bay (Apple, "Power Macintosh
// Computers" Developer Note, Table 3-7).

// Media attach to the standard Curio bus (SCSI Manager bus 1 on this
// model); the fast 53CF96 bus scans empty.

static const pdm_board_desc_t pm8100_board = {
    .machine_id = 0x3013,
    .bus_hz = 40000000u, // 2:1 bus
    .bank_layout = PDM_BANKS_FIXED,
    .bank_count = 8,
    .wait_state_penalty = 2, // pinned by the pdm-rom-ladder L7 bus-ratio row
    .has_fast_scsi = true, // discrete 53CF96, island +$11000, DMA channel B
};

const hw_profile_t machine_pm8100 = {
    .name = "Power Macintosh 8100",
    .id = "pm8100",

    .cpu_model = CPU_MODEL_PPC601,
    .freq = 80000000, // 80 MHz
    .mmu_kind = MMU_PPC_601,

    .address_bits = 32,
    .ram_default = 0x2000000, // 32 MB (a typical well-equipped machine)
    .ram_max = 0x10800000, // 264 MB
    .rom_size = 0x400000, // 4 MB ($9FEB69B3)

    .ram_options = pm8100_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_cd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .cdrom_drive = &mac_cdrom_drive_applecd,

    .builtin_video = &pdm_builtin_video,
    .nubus_slots = pdm_nubus_slots_cde,

    .pram = &pram_defaults_pdm,
    .substrate = &pdm_substrate,
    .board = &pm8100_board,
};
