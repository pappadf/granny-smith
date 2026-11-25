// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lc520.c
// Macintosh LC 520 ("Hook", 25 MHz 68030, 1993; Performa 520) and LC 550
// ("Hook33", 33 MHz; Performa 550/560): the LC III's architecture in an
// all-in-one case with a built-in 14" Trinitron display (640 x 480, cabled as
// the 13" RGB), stereo sound, a CD-ROM bay and Cuda in place of Egret, around
// the Ardbeg -- Sonora with the front-panel buttons and a power register
// added.  Apple, *Macintosh LC 520* Developer Note (1993).  Both run the
// 1 MB ROM $EDE66CBD, which finds them by the CPU ID register ($A55A0100 and
// $A55A0101) and the display's sense code (110).  See v8.h and sonora.c.

#include "v8.h"

#include "machine.h"
#include "pram_defaults.h"
#include "scsi.h"
#include "slot_tables.h"

// The built-in 14" colour display: one monitor, nothing to choose.
static bool lc520_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i != 0)
        return false;
    *id = *monitor = "builtin_14in_color";
    return true;
}

static const builtin_video_desc_t lc520_builtin_video = {
    .detail = "Ardbeg, 640 \xc3\x97 480",
    .monitor_at = lc520_monitor_at,
    .default_monitor = "builtin_14in_color",
};

#define LC520_DESC                                                                                                     \
    {                                                                                                                  \
        .chipset = "Ardbeg",                                                                                           \
        .rom_base = V8_ROM_START,                                                                                      \
        .rom_end = V8_ROM_END,                                                                                         \
        .io_ranges = sonora_io_ranges_tbl,                                                                             \
        .io_mirror_mask = V8_IO_MIRROR,                                                                                \
        .io_unmapped_read = 0xFF,                                                                                      \
        .bus_err_lo = 0xFE000000u,                                                                                     \
        .bus_err_hi = 0xFEFFFFFFu,                                                                                     \
    }

// The display is cabled as the 13" RGB: sense 110, 640 x 480 (LC 520
// Developer Note Table 2-1, p. 25: the AppleColor High-Resolution RGB timing).
#define LC520_BOARD(id)                                                                                                \
    {                                                                                                                  \
        .chip = "Ardbeg",                                                                                              \
        .kind = V8_KIND_SONORA,                                                                                        \
        .desc = LC520_DESC,                                                                                            \
        .pcb_ram = 4u * 1024 * 1024,                                                                                   \
        .fb_source = V8_FB_VRAM,                                                                                       \
        .vram_size = 512u * 1024,                                                                                      \
        .crt_width = 640,                                                                                              \
        .crt_height = 480,                                                                                             \
        .crt_sense = 6,                                                                                                \
        .asc_version = 0xE8,                                                                                           \
        .cuda = true,                                                                                                  \
        .cpu_id = (id),                                                                                                \
    }

static const v8_board_t lc520_board = LC520_BOARD(0xA55A0100u);
static const v8_board_t lc550_board = LC520_BOARD(0xA55A0101u);

// 4 MB soldered plus one 72-pin SIMM of 1 to 32 MB (Developer Note p. 22:
// "from 4 MB to 36 MB").
static const uint32_t lc520_ram_options_kb[] = {4096, 5120, 6144, 8192, 12288, 20480, 36864, 0};

const hw_profile_t machine_lc520 = {
    .name = "Macintosh LC 520",
    .id = "lc520",

    .cpu_model = 68030,
    .freq = 25000000,
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x2400000, // 36 MB
    .rom_size = 0x100000, // 1 MB

    .ram_options = lc520_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &lc520_builtin_video,
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus,

    .pram = &pram_defaults_iici,
    .substrate = &v8_substrate,
    .board = &lc520_board,
};

const hw_profile_t machine_lc550 = {
    .name = "Macintosh LC 550",
    .id = "lc550",

    .cpu_model = 68030,
    .freq = 33000000,
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x2400000, // 36 MB
    .rom_size = 0x100000, // 1 MB

    .ram_options = lc520_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &lc520_builtin_video,
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus,

    .pram = &pram_defaults_iici,
    .substrate = &v8_substrate,
    .board = &lc550_board,
};
