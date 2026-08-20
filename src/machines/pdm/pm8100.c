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

#include "nubus.h"

// 8 MB soldered + fixed-window banks of {2,8,32} MB: up to 264 MB.
static const uint32_t pm8100_ram_options_kb[] = {8192, 16384, 40960, 73728, 139264, 270336, 0};

// One internal manual-inject SuperDrive behind SWIM3, and no external
// port — the PDM family has no second bay (Apple, "Power Macintosh
// Computers" Developer Note, Table 3-7).
static const struct floppy_slot pm8100_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {0},
};

// Media attach to the standard Curio bus (SCSI Manager bus 1 on this
// model); the fast 53CF96 bus scans empty.
static const struct scsi_slot pm8100_scsi_slots[] = {
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
static const struct nubus_slot_decl pm8100_nubus_slots[] = {
    {.slot = 0xB, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xC, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET},
    {0},
};

static const pdm_board_desc_t pm8100_board = {
    .machine_id = 0x3013,
    .bus_hz = 40000000u, // 2:1 bus
    .bank_layout = PDM_BANKS_FIXED,
    .bank_count = 8,
    .wait_state_penalty = 2, // pinned by the rung-L7 bus-ratio row
    .has_fast_scsi = true, // discrete 53CF96, island +$11000, DMA channel B
};

const hw_profile_t machine_pm8100 = {
    .name = "Power Macintosh 8100/80",
    .id = "pm8100",

    .cpu_model = CPU_MODEL_PPC601,
    .freq = 80000000, // 80 MHz
    .mmu_kind = MMU_PPC_601,

    .address_bits = 32,
    .ram_default = 0x2800000, // 40 MB (8 soldered + one 32 MB bank)
    .ram_max = 0x10800000, // 264 MB
    .rom_size = 0x400000, // 4 MB ($9FEB69B3)

    .ram_options = pm8100_ram_options_kb,
    .floppy_slots = pm8100_floppy_slots,
    .scsi_slots = pm8100_scsi_slots,
    // The AppleCD 300i rides the same Curio 53C96 bus as the HD slots
    // (Phase G): no CD-specific hardware is involved, so the bay is
    // offered as soon as that bus exists.
    .has_cdrom = true,
    .cdrom_id = 3,

    .nubus_slots = pm8100_nubus_slots,

    .substrate = &pdm_substrate,
    .board = &pm8100_board,
};
