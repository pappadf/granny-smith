// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// q660av.c
// Macintosh Centris/Quadra 660AV ("Tempest", 25 MHz 68040, July 1993) — the
// pizza-box sibling of the Quadra 840AV (renamed "Quadra 660AV" late in
// life).  Same 2 MB $5BF10FD1 ROM and chipset; the deltas are pure data
// (the q950.c pattern):
//   * 25 MHz full 68040 (not LC)
//   * YMCA strap nibble $B (Tempest25), BoxFlag 54, Gestalt 60
//   * MUNI optional and absent by default — MUNI_Control bus-errors so the
//     ROM's TestForMUNI clears MUNIExists

#include "av.h"

#include "machine.h"
#include "nubus.h"
#include "pram_defaults.h"
#include "slot_tables.h"

#include <stdint.h>

// 4 MB on the logic board plus two 72-pin SIMM slots of 4, 8, 16 or 32 MB
// (developer note p. 13): every total those make, 4 MB to 68 MB.  Totals
// such as 32 MB are not among them -- 28 MB more than the soldered bank
// needs a 12 MB bank, which YMCA cannot decode (av_ram_banks).
static const uint32_t q660av_ram_options_kb[] = {4096,  8192,  12288, 16384, 20480, 24576, 28672,
                                                 36864, 40960, 45056, 53248, 69632, 0};

static const struct floppy_slot q660av_floppy_slots[] = {
    {0},
};

static const av_board_desc_t q660av_board_desc = {
    .common =
        {
                 .chipset = "YMCA+PSC",
                 .rom_base = 0x40800000u,
                 .rom_end = 0x40A00000u,
                 .io_ranges = av_io_ranges,
                 .io_mirror_mask = 0x0003FFFFu,
                 .io_unmapped_read = 0xFF,
                 .bus_err_lo = 0xA0000000u, // super-slots + slots; see nubus.h
            .bus_err_hi = NUBUS_BERR_HI,
                 },
    .strap_nibble = 0xB, // Tempest25 straps %1011
    .muni_present = false, // no NuBus adapter: MUNI_Control bus-errors
    .ram_onboard = 0x400000, // 4 MB soldered, YMCA bank 0
    .simm_slots = 2, // banks 2/3 and 4/5
    .simm_first_bank = 2,
};

static const av_board_t q660av_board = {
    .desc = &q660av_board_desc,
    .via1_output = av_via1_output,
    .via1_shift_out = av_via1_shift_out,
    .build_devices = av_build_devices,
};

// The DSP3210 aux core (55.5 MHz).
static const struct aux_cpu_slot q660av_aux_cpus[] = {
    {"dsp", "dsp3210", 55500000u},
    {NULL,  NULL,      0        },
};

const hw_profile_t machine_q660av = {
    .name = "Macintosh Centris 660AV / Quadra 660AV",
    .id = "q660av",

    .cpu_model = 68040,
    .freq = 25000000, // 25 MHz
    .mmu_kind = MMU_68040,

    .address_bits = 32,
    .ram_default = 0x1000000, // 16 MB
    .ram_max = 0x4400000, // 68 MB: 4 MB soldered + two 32 MB SIMMs
    .rom_size = 0x200000, // 2 MB ($5BF10FD1, shared with the 840AV)

    .ram_options = q660av_ram_options_kb,
    .floppy_slots = q660av_floppy_slots,
    .storage = mac_storage_scsi_cd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &av_builtin_video_q660av,
    .cdrom_drive = &mac_cdrom_drive_applecd,
    .has_video_in = true, // on-board DMSD/VDC digitizer
    .has_audio_in = true, // Singer codec microphone input (singer.md)
    .aux_cpus = q660av_aux_cpus, // the DSP3210 (machine.dsp)

    .nubus_slots = NULL,

    .pram = &pram_defaults_av,
    .substrate = &av_substrate,
    .board = &q660av_board,
};
