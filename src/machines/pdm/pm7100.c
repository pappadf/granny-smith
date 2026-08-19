// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm7100.c
// Power Macintosh 7100/66 ("Carl Sagan", 66 MHz MPC601, March 1994) — the
// mid-range PDM.  Machine-ID register $A55A3012; 33 MHz bus (2:1); four
// SIMM banks at the fixed window addresses (never relocated); three NuBus
// slots ($B/$C/$D) behind BART, plus the PDS video slot $E (no PDS card is
// modeled, so that window reads as an empty slot).

#include "pdm.h"

#include "nubus.h"

// 8 MB soldered + fixed-window banks of {2,8,32} MB: up to 136 MB.
static const uint32_t pm7100_ram_options_kb[] = {8192, 16384, 24576, 40960, 73728, 139264, 0};

static const struct floppy_slot pm7100_floppy_slots[] = {
    {0},
};

// One standard 5 MB/s bus (the Curio 53C94 cell), internal + external.
static const struct scsi_slot pm7100_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

// The three NuBus connectors behind BART.  They are $B/$C/$D, not the
// widely repeated "$C/$D/$E": slot $E is the PDS video pseudo-slot on this
// board (HPV / AV card), and the ROM disables BART's path to it on every
// boot.  Physical board order is B, D, C — the middle connector is $D.
// (Apple, Macintosh 8100 schematics, sheet 22 — the two boards share this
// topology and the ROM's machine table — where the three 96-pin connectors
// J11/J12/J13 are labelled NuBus Slot B, C and D.)  Each ships empty; the
// user stages a card per slot.
static const struct nubus_slot_decl pm7100_nubus_slots[] = {
    {.slot = 0xB, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xC, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET},
    {0},
};

static const pdm_board_desc_t pm7100_board = {
    .machine_id = 0x3012,
    .bus_hz = 33000000u, // 2:1 bus
    .bank_layout = PDM_BANKS_FIXED,
    .bank_count = 4,
    .wait_state_penalty = 2, // pinned by the rung-L7 bus-ratio row
};

const hw_profile_t machine_pm7100 = {
    .name = "Power Macintosh 7100/66",
    .id = "pm7100",

    .cpu_model = CPU_MODEL_PPC601,
    .freq = 66000000, // 66 MHz
    .mmu_kind = MMU_PPC_601,

    .address_bits = 32,
    .ram_default = 0x1800000, // 24 MB
    .ram_max = 0x8800000, // 136 MB
    .rom_size = 0x400000, // 4 MB ($9FEB69B3)

    .ram_options = pm7100_ram_options_kb,
    .floppy_slots = pm7100_floppy_slots,
    .scsi_slots = pm7100_scsi_slots,
    // The AppleCD 300i rides the same Curio 53C96 bus as the HD slots
    // (Phase G): no CD-specific hardware is involved, so the bay is
    // offered as soon as that bus exists.
    .has_cdrom = true,
    .cdrom_id = 3,

    .nubus_slots = pm7100_nubus_slots,

    .substrate = &pdm_substrate,
    .board = &pm7100_board,
};
