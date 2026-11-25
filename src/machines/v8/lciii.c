// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lciii.c
// Macintosh LC III ("Vail", 25 MHz 68030, 1993; also sold as the Performa
// 450) and LC III+ (33 MHz; Performa 460/466/467): the LC II's slim-line
// case around the Sonora decoder, with 4 MB soldered, one 72-pin SIMM socket,
// 512 KB of VRAM and an external monitor port.  Apple, *Macintosh LC III*
// Developer Note (1993).  The two speeds are one board; the ROM tells them
// apart by the CPU ID register.  See v8.h and sonora.c.

#include "v8.h"

#include "machine.h"
#include "pram_defaults.h"
#include "slot_tables.h"

#include <string.h>

// The external monitor port: the 13" RGB (sense 110, 640 x 480) and the 12"
// RGB (sense 010, 512 x 384) -- LC III Developer Note Table 1-4, p. 15.
static const struct {
    const char *id;
    uint8_t sense;
} lciii_monitors[] = {
    {"13in_rgb", 6},
    {"12in_rgb", 2},
};

// builtin_video_desc_t.monitor_at: the monitors, by index.
static bool lciii_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i >= sizeof lciii_monitors / sizeof lciii_monitors[0])
        return false;
    *id = *monitor = lciii_monitors[i].id;
    return true;
}

// builtin_video_desc_t.monitor_sense: a monitor's sense code.
static bool lciii_monitor_sense(const char *id, uint8_t *out_sense) {
    for (size_t i = 0; i < sizeof lciii_monitors / sizeof lciii_monitors[0]; i++) {
        if (strcmp(id, lciii_monitors[i].id) == 0) {
            *out_sense = lciii_monitors[i].sense;
            return true;
        }
    }
    return false;
}

static const builtin_video_desc_t lciii_builtin_video = {
    .detail = "Sonora",
    .monitor_at = lciii_monitor_at,
    .monitor_sense = lciii_monitor_sense,
    .default_monitor = "13in_rgb",
};

// The decoder descriptor both speeds share.
#define LCIII_DESC                                                                                                     \
    {                                                                                                                  \
        .chipset = "Sonora",                                                                                           \
        .rom_base = V8_ROM_START,                                                                                      \
        .rom_end = V8_ROM_END,                                                                                         \
        .io_ranges = sonora_io_ranges_tbl,                                                                             \
        .io_mirror_mask = V8_IO_MIRROR,                                                                                \
        .io_unmapped_read = 0xFF,                                                                                      \
        .bus_err_lo = 0xFE000000u,                                                                                     \
        .bus_err_hi = 0xFEFFFFFFu,                                                                                     \
    }

// CPU ID $A55A0001: the high-volume design centre, board ID 1 (25 MHz).
static const v8_board_t lciii_board = {
    .chip = "Sonora",
    .kind = V8_KIND_SONORA,
    .desc = LCIII_DESC,
    .pcb_ram = 4u * 1024 * 1024,
    .fb_source = V8_FB_VRAM,
    .vram_size = 512u * 1024,
    .asc_version = 0xE8, // the V8's sound block
    .cpu_id = 0xA55A0001u,
};

// CPU ID $A55A0003: board ID 3 (33 MHz).
static const v8_board_t lciiip_board = {
    .chip = "Sonora",
    .kind = V8_KIND_SONORA,
    .desc = LCIII_DESC,
    .pcb_ram = 4u * 1024 * 1024,
    .fb_source = V8_FB_VRAM,
    .vram_size = 512u * 1024,
    .asc_version = 0xE8, // the V8's sound block
    .cpu_id = 0xA55A0003u,
};

// 4 MB soldered plus one 72-pin SIMM of 1, 2, 4, 8, 16 or 32 MB (Developer
// Note Table 1-2, p. 9).
static const uint32_t lciii_ram_options_kb[] = {4096, 5120, 6144, 8192, 12288, 20480, 36864, 0};

const hw_profile_t machine_lciii = {
    .name = "Macintosh LC III",
    .id = "lciii",

    .cpu_model = 68030,
    .freq = 25000000,
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x2400000, // 36 MB
    .rom_size = 0x100000, // 1 MB

    .ram_options = lciii_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &lciii_builtin_video,
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus,

    .pram = &pram_defaults_iici,
    .substrate = &v8_substrate,
    .board = &lciii_board,
};

const hw_profile_t machine_lciiip = {
    .name = "Macintosh LC III+",
    .id = "lciiip",

    .cpu_model = 68030,
    .freq = 33000000,
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x2400000, // 36 MB
    .rom_size = 0x100000, // 1 MB

    .ram_options = lciii_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &lciii_builtin_video,
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus,

    .pram = &pram_defaults_iici,
    .substrate = &v8_substrate,
    .board = &lciiip_board,
};
