// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lcii.c
// Macintosh LC II ("Foster Farms", 16 MHz 68030, 1992; also sold as the
// Performa 400/405/410/430): the Macintosh LC's V8 board with a 68030 and
// 4 MB soldered.  Apple, *Macintosh LC II* Developer Note (1992), a delta to
// the *Macintosh LC Computer* Developer Note (1990).  See v8.h.

#include "v8.h"

#include "machine.h"
#include "mdu_io.h"
#include "pram_defaults.h"
#include "slot_tables.h"

#include <string.h>

// The external monitor port: the two monitors the V8 decodes from its sense
// lines (LC Developer Note p. 14).
static const struct {
    const char *id;
    uint8_t sense;
} lcii_monitors[] = {
    {"13in_rgb", 6},
    {"12in_rgb", 2},
};

// builtin_video_desc_t.monitor_at: the monitors, by index.
static bool lcii_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i >= sizeof lcii_monitors / sizeof lcii_monitors[0])
        return false;
    *id = *monitor = lcii_monitors[i].id;
    return true;
}

// builtin_video_desc_t.monitor_sense: a monitor's sense code.
static bool lcii_monitor_sense(const char *id, uint8_t *out_sense) {
    for (size_t i = 0; i < sizeof lcii_monitors / sizeof lcii_monitors[0]; i++) {
        if (strcmp(id, lcii_monitors[i].id) == 0) {
            *out_sense = lcii_monitors[i].sense;
            return true;
        }
    }
    return false;
}

static const builtin_video_desc_t lcii_builtin_video = {
    .detail = "V8",
    .monitor_at = lcii_monitor_at,
    .monitor_sense = lcii_monitor_sense,
    .default_monitor = "13in_rgb",
};

// The LC II board.  VIA1 PA6/PA4/PA2/PA1 = 1/1/1/0, the LC's code: the two
// share a logic-board ID (LC II Developer Note, "Identifying the LC II").
static const v8_board_t lcii_board = {
    .chip = "V8",
    .desc =
        {
               .chipset = "V8",
               .rom_base = V8_ROM_START,
               .rom_end = V8_ROM_END,
               .io_ranges = mdu_io_ranges_tbl,
               .io_mirror_mask = V8_IO_MIRROR,
               .io_unmapped_read = 0xFF,
               .bus_err_lo = 0xFE000000u, // the PDS slot's space, empty
            .bus_err_hi = 0xFEFFFFFFu,
               },
    .id_strap = 0x54,
    .pcb_ram = 4u * 1024 * 1024,
    .fb_source = V8_FB_VRAM,
    .vram_size = 256u * 1024, // the standard VRAM SIMM
    .asc_version = 0xE8,
};

// 4 MB soldered plus a pair of 1, 2 or 4 MB SIMMs; with 4 MB SIMMs the V8's
// 10 MB space hides 2 MB of the soldered RAM.
static const uint32_t lcii_ram_options_kb[] = {4096, 6144, 8192, 10240, 0};

const hw_profile_t machine_lcii = {
    .name = "Macintosh LC II",
    .id = "lcii",

    .cpu_model = 68030,
    .freq = 15667200, // 15.6672 MHz
    .mmu_kind = MMU_68030_PMMU,
    .fpu_absent = true, // no FPU on board; the /FPU select reaches only the expansion connector

    .address_bits = 32,
    .ram_default = 0xA00000, // 10 MB
    .ram_max = 0xA00000,
    .rom_size = 0x80000, // 512 KB

    .ram_options = lcii_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &lcii_builtin_video,
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus,

    .pram = &pram_defaults_iici,
    .substrate = &v8_substrate,
    .board = &lcii_board,
};
