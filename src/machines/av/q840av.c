// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// q840av.c
// Macintosh Quadra 840AV ("Cyclone", 40 MHz 68040, July 1993) — the desktop
// flagship of the AV family and its first leaf.
// YMCA strap nibble $F, BoxFlag 72, Gestalt 78; MUNI present with three
// NuBus '90 slots C/D/E (declared but unpopulated — no AV declaration-ROM
// work in scope).  Shares the 2 MB $5BF10FD1 ROM with the Centris 660AV;
// the strap nibble is the only identity input the ROM reads.

#include "av.h"

#include "machine.h"
#include "nubus.h"
#include "pram_defaults.h"
#include "slot_tables.h"

#include <stdint.h>

// Four 72-pin SIMM slots of 4, 8, 16 or 32 MB, no RAM on the logic board:
// the shipping 8/16 MB configurations plus larger populations up to
// 128 MB (four 32 MB SIMMs, all eight YMCA banks).
static const uint32_t q840av_ram_options_kb[] = {8192, 16384, 32768, 65536, 131072, 0};

// New Age reports "no drive" (ST3 = $FF) — no floppy slots offered until a
// real New Age model lands.
static const struct floppy_slot q840av_floppy_slots[] = {
    {0},
};

static const av_board_desc_t q840av_board_desc = {
    .common =
        {
                 .chipset = "YMCA+PSC",
                 .rom_base = 0x40800000u,
                 .rom_end = 0x40A00000u,
                 .io_ranges = av_io_ranges,
                 .io_mirror_mask = 0x0003FFFFu, // 256 KiB island + the $50F40000 alias
            .io_unmapped_read = 0xFF, // undecoded island reads float high
            .bus_err_lo = 0xA0000000u, // super-slots + slots; see nubus.h
            .bus_err_hi = NUBUS_BERR_HI,
                 },
    .strap_nibble = 0xF, // Cyclone40 straps %1111
    .muni_present = true,
    .ram_onboard = 0, // all RAM on SIMMs
    .simm_slots = 4, // slot k = banks 2k/2k+1
    .simm_first_bank = 0,
};

static const av_board_t q840av_board = {
    .desc = &q840av_board_desc,
    .via1_output = av_via1_output,
    .via1_shift_out = av_via1_shift_out,
    .build_devices = av_build_devices,
};

// The DSP3210 aux core (66.6667 MHz).
static const struct aux_cpu_slot q840av_aux_cpus[] = {
    {"dsp", "dsp3210", 66666667u},
    {NULL,  NULL,      0        },
};

const hw_profile_t machine_q840av = {
    .name = "Macintosh Quadra 840AV",
    .id = "q840av",

    .cpu_model = 68040,
    .freq = 40000000, // 40 MHz
    .mmu_kind = MMU_68040,

    .address_bits = 32,
    .ram_default = 0x1000000, // 16 MB (typical shipping configuration)
    .ram_max = 0x8000000, // 128 MB (8 banks x 16 MB)
    .rom_size = 0x200000, // 2 MB ($5BF10FD1, shared with the 660AV)

    .ram_options = q840av_ram_options_kb,
    .floppy_slots = q840av_floppy_slots,
    .storage = mac_storage_scsi_cd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &av_builtin_video_q840av,
    .cdrom_drive = &mac_cdrom_drive_applecd,
    .has_video_in = true, // on-board DMSD/VDC digitizer
    .has_audio_in = true, // Singer codec microphone input (singer.md)
    .aux_cpus = q840av_aux_cpus, // the DSP3210 (machine.dsp)

    .nubus_slots = NULL,

    .pram = &pram_defaults_av,
    .substrate = &av_substrate,
    .board = &q840av_board,
};
