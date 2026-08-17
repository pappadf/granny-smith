// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pm6100.c
// Power Macintosh 6100/60 ("PDM"/"Piltdown Man", 60 MHz MPC601, March
// 1994) — the first PowerPC Macintosh and this family's base model.
// Machine-ID register $A55A3010 (the "$3011" PDM ProductInfo ID is a 68k
// software promotion, never read from hardware); 30 MHz bus (2:1); one
// SIMM-pair socket whose two banks the HMC relocates via the
// SIMM_BANK_SIZE config code; single PDS slot (no NuBus without the
// adapter — BART space bus-errors, which the base model's probe expects).

#include "pdm.h"

// 8 MB soldered plus the SIMM-bank splits the HMC accepts ({2,8,32} MB
// banks, at most two): 16 = 8+8x1, 24 = 8+8x2, 40 = 8+32, 72 = 8+32x2.
static const uint32_t pm6100_ram_options_kb[] = {8192, 16384, 24576, 40960, 73728, 0};

// No SWIM3 model yet (Phase H): no floppy slots offered.
static const struct floppy_slot pm6100_floppy_slots[] = {
    {0},
};

// SCSI arrives with Phase G; no slots offered until the 53C94 glue exists.
static const struct scsi_slot pm6100_scsi_slots[] = {
    {0},
};

static const pdm_board_desc_t pm6100_board = {
    .machine_id = 0x3010,
    .bus_hz = 30000000u, // 2:1 bus
    .bank_layout = PDM_BANKS_MOVABLE,
    .bank_count = 2,
    .wait_state_penalty = 2, // pinned by the rung-L7 bus-ratio row
};

const hw_profile_t machine_pm6100 = {
    .name = "Power Macintosh 6100/60",
    .id = "pm6100",

    .cpu_model = CPU_MODEL_PPC601,
    .freq = 60000000, // 60 MHz
    .mmu_kind = MMU_PPC_601,

    .address_bits = 32,
    .ram_default = 0x1800000, // 24 MB (8 soldered + 8+8 SIMM banks)
    .ram_max = 0x4800000, // 72 MB
    .rom_size = 0x400000, // 4 MB ($9FEB69B3, shared with 7100/8100)

    .ram_options = pm6100_ram_options_kb,
    .floppy_slots = pm6100_floppy_slots,
    .scsi_slots = pm6100_scsi_slots,
    .has_cdrom = false,
    .cdrom_id = 3,

    .nubus_slots = NULL,

    .substrate = &pdm_substrate,
    .board = &pm6100_board,
};
