// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// q950.c
// Macintosh Quadra 950 ("Zydeco", 33 MHz 68040, March 1992) — the faster
// tower.  Sister of the Quadra 900: same Eclipse board architecture (Caboose,
// two PIC/IOPs, dual 53C96, five NuBus '90 slots), so every hook comes from
// q900_internal.h.  Deltas (UniversalTables.a InfoQuadra950):
//   * 33.33 MHz CPU clock; VIA2 PB5 speed sense reads 1 (33 MHz)
//   * model sense $90: PA & $56 == $10 (PA6 = 0, PA4 = 1)
//   * dedicated 3DC27823 ROM
//   * DAFB revision 3 ("DAFB 3": DAFB_Test version bits read 3) with the
//     AC842a RAMDAC — PCBR1 + x555 16-bit "Thousands" direct mode

#include "mcu.h"
#include "q900_internal.h"

#include "machine.h"
#include "nubus.h"
#include "pram_defaults.h"
#include "slot_tables.h"

#include <stdint.h>

static const mcu_board_desc_t q950_board_desc = {
    .common =
        {
                 .chipset = "MCU+DAFB",
                 .rom_base = 0x40000000u,
                 .rom_end = 0x50000000u,
                 .io_ranges = mcu_q900_io_ranges, // identical tower island decode
            .io_mirror_mask = 0x0003FFFFu,
                 .io_unmapped_read = 0xFF, // undecoded island reads float high (see mac030_glue.h)
            .bus_err_lo = 0xF1000000u, // slots $1-$E: this board decodes below $F9
            .bus_err_hi = NUBUS_BERR_HI,
                 },
    .ram_bank_count = 4, // sixteen SIMM sockets = four four-SIMM banks
    .via1_pa_model = 0x90, // Q950 model sense: PA & $56 == $10 (InfoQuadra950)
    .dafb_version = 3, // "DAFB 3" — the driver's 16bpp-always-allowed check
    .has_ac842a = true, // AC842a RAMDAC: PCBR1 + x555 16-bit mode
    .dafb_vram_size = 0x00200000u, // modelled maxed; ships 1 MiB (80 ns), expands to 2
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
    .ram_default = 0x1000000, // 16 MB (a typical well-equipped machine)
    .ram_max = 0x10000000, // 256 MB (same Eclipse board as the Q900)
    .rom_size = 0x100000, // 1 MB (3DC27823)

    .ram_options = q900_ram_options_kb, // same Eclipse board
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = q900_storage, // same Eclipse board (q900_internal.h)
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &mcu_builtin_video_q950,
    .cdrom_drive = &mac_cdrom_drive_applecd,

    .nubus_slots = q900_nubus_slots, // same Eclipse board (q900_internal.h)

    .pram = &pram_defaults_q950,
    .substrate = &mcu_substrate,
    .board = &q950_board,
};
