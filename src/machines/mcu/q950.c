// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// q950.c
// Macintosh Quadra 950 ("Zydeco", 33 MHz 68040, March 1992) — the faster
// tower (proposal-machine-quadra-700-900-950.md Phase H).  Sister of the
// Quadra 900: same Eclipse board architecture (Caboose, two PIC/IOPs,
// dual 53C96, five NuBus '90 slots), so every hook comes from
// q900_internal.h.  Deltas (ref §18.3, UniversalTables.a InfoQuadra950):
//   * 33.33 MHz CPU clock; VIA2 PB5 speed sense reads 1 (33 MHz)
//   * model sense $90: PA & $56 == $10 (PA6 = 0, PA4 = 1)
//   * dedicated 3DC27823 ROM
//   * DAFB revision 3 ("DAFB 3": DAFB_Test version bits read 3) with the
//     AC842a RAMDAC — PCBR1 + x555 16-bit "Thousands" direct mode
//   * RAM to 256 MB (sixteen SIMM slots, four 4-SIMM banks)

#include "mcu.h"
#include "q900_internal.h"

#include "machine.h"
#include "nubus.h"

#include <stdint.h>

// Four banks of four equal SIMMs; geometrically valid totals up to the
// 256 MB later-system maximum (ref §18.3 [A]).
static const uint32_t q950_ram_options_kb[] = {8192, 16384, 20480, 32768, 65536, 131072, 262144, 0};

static const struct floppy_slot q950_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {0},
};

static const struct scsi_slot q950_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

// Same five NuBus '90 sockets A-E as the Q900 (ref §10.3).
static const nubus_slot_decl_t q950_nubus_slots[] = {
    {.slot = 0xA, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xB, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xC, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET},
    {0},
};

static const mcu_board_desc_t q950_board_desc = {
    .chipset = "MCU+DAFB",
    .rom_base = 0x40000000u,
    .rom_end = 0x50000000u,
    .io_ranges = mcu_q900_io_ranges, // identical tower island decode
    .ram_bank_count = 4, // sixteen SIMM sockets = four four-SIMM banks
    .io_mirror_mask = 0x0003FFFFu,
    .io_unmapped_read = 0xFF, // undecoded island reads float high (see mac030_glue.h)
    .slots = q950_nubus_slots,
    .bus_err_lo = 0xF1000000u,
    .bus_err_hi = 0xFEFFFFFFu,
    .via1_pa_model = 0x90, // Q950 model sense: PA & $56 == $10 (InfoQuadra950)
    .dafb_version = 3, // "DAFB 3" — the driver's 16bpp-always-allowed check
    .has_ac842a = true, // AC842a RAMDAC: PCBR1 + x555 16-bit mode
};

static const mcu_board_t q950_board = {
    .desc = &q950_board_desc,
    .via1_output = q900_via1_output,
    .via1_shift_out = q900_via1_shift_out,
    .via2_output = q900_via2_output,
    .build_devices = q900_build_devices,
    .scc_irq = q900_scc_irq,
};

const hw_profile_t machine_q950 = {
    .name = "Macintosh Quadra 950",
    .id = "q950",

    .cpu_model = 68040,
    .freq = 33333333, // 33.33 MHz
    .mmu_kind = MMU_68040,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x10000000, // 256 MB
    .rom_size = 0x100000, // 1 MB (3DC27823)

    .ram_options = q950_ram_options_kb,
    .floppy_slots = q950_floppy_slots,
    .scsi_slots = q950_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 3,

    .nubus_slots = q950_nubus_slots,

    .substrate = &mcu_substrate,
    .board = &q950_board,
};
