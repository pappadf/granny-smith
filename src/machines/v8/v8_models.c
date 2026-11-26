// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// v8_models.c
// The V8 family's model profiles, each a hw_profile_t over v8_substrate and a
// v8_board_t of its hardware deltas (see v8.h).  Every member is a 68030 with
// no FPU fitted (none on board, or an empty 68882 socket), a SCSI hard-disk
// bay, LocalTalk and the IIci's PRAM defaults; the decoder kind picks the
// descriptor (v8.c, sonora.c).
//
//   LC II          ("Foster Farms", 16 MHz, 1992; Performa 400/405/410/430):
//                  the Macintosh LC's V8 board with a 68030 and 4 MB soldered.
//                  Apple, *Macintosh LC II* Developer Note (1992), a delta to
//                  the *Macintosh LC Computer* Developer Note (1990).
//   Classic II     ("Apollo", 16 MHz, 1991; Performa 200): the LC's
//                  architecture in the compact case, around the Eagle -- a V8
//                  without VRAM, scanning a built-in 512 x 342 1-bit CRT out
//                  of main RAM at $9F9A80.  *Macintosh Classic II* Developer
//                  Note (1991), pp. 5-9.
//   Color Classic  ("Slice"/"Columbia", 16 MHz, 1993; Performa 250): the LC
//                  II's architecture in the compact case, around the Spice --
//                  a V8 with SWIM2 and the front-panel buttons -- driving a
//                  built-in 10" 512 x 384 colour CRT from 256 KB of VRAM.
//                  *Macintosh Color Classic* Developer Note (1993), ch. 1-2.
//   LC III / III+  ("Vail", 25 / 33 MHz, 1993; Performa 450, 460/466/467):
//                  the LC II's case around the Sonora decoder, 4 MB soldered,
//                  one 72-pin SIMM socket, 512 KB of VRAM, an external
//                  monitor port.  *Macintosh LC III* Developer Note (1993).
//                  One board; the ROM tells the speeds apart by the CPU ID.
//   LC 520 / 550   ("Hook" / "Hook33", 25 / 33 MHz, 1993; Performa 520,
//                  550/560): the LC III's architecture in an all-in-one case
//                  with a built-in 14" Trinitron (640 x 480, cabled as the 13"
//                  RGB), stereo sound, a CD-ROM bay and Cuda, around the
//                  Ardbeg -- Sonora with the front-panel buttons and a power
//                  register.  *Macintosh LC 520* Developer Note (1993).
//   Color Classic II ("Slice33", 33 MHz, 1993; Performa 275): the Color
//                  Classic's case and CRT on the LC 550's Sonora logic.
//
// The 1 MB ROM $EDE66CBD runs the LC 520, LC 550 and Color Classic II and
// finds each by the CPU ID register and the built-in display's sense code
// (110 the 14" "Hook" boards, 010 the "Slice" one).

#include "v8.h"

#include "machine.h"
#include "pram_defaults.h"
#include "scsi.h"
#include "slot_tables.h"

#include <string.h>

#define MB (1024u * 1024u)

// ============================================================
// Monitors
// ============================================================

// The external monitor port of the LC II and LC III: the 13" RGB (sense 110,
// 640 x 480) and the 12" RGB (sense 010, 512 x 384), the two the decoder
// times (LC Developer Note p. 14; LC III Developer Note Table 1-4, p. 15).
static const struct {
    const char *id;
    uint8_t sense;
} v8_monitors[] = {
    {"13in_rgb", 6},
    {"12in_rgb", 2},
};

// builtin_video_desc_t.monitor_at: the external monitors, by index.
static bool v8_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i >= sizeof v8_monitors / sizeof v8_monitors[0])
        return false;
    *id = *monitor = v8_monitors[i].id;
    return true;
}

// builtin_video_desc_t.monitor_sense: an external monitor's sense code.
static bool v8_monitor_sense(const char *id, uint8_t *out_sense) {
    for (size_t i = 0; i < sizeof v8_monitors / sizeof v8_monitors[0]; i++) {
        if (strcmp(id, v8_monitors[i].id) == 0) {
            *out_sense = v8_monitors[i].sense;
            return true;
        }
    }
    return false;
}

// An external monitor port: `detail` names the decoder driving it.
#define V8_EXTERNAL_PORT(detail_)                                                                                      \
    {.detail = (detail_), .monitor_at = v8_monitor_at, .monitor_sense = v8_monitor_sense, .default_monitor = "13in_rgb"}

// A built-in CRT: one monitor, nothing to choose.  Defines `name`_at and the
// builtin_video_desc_t `name`.
#define V8_BUILTIN_CRT(name, detail_, monitor_)                                                                        \
    static bool name##_at(size_t i, const char **id, const char **monitor) {                                           \
        if (i != 0)                                                                                                    \
            return false;                                                                                              \
        *id = *monitor = (monitor_);                                                                                   \
        return true;                                                                                                   \
    }                                                                                                                  \
    static const builtin_video_desc_t name = {                                                                         \
        .detail = (detail_), .monitor_at = name##_at, .default_monitor = (monitor_)}

static const builtin_video_desc_t lcii_builtin_video = V8_EXTERNAL_PORT("V8");
static const builtin_video_desc_t lciii_builtin_video = V8_EXTERNAL_PORT("Sonora");
V8_BUILTIN_CRT(classicii_builtin_video, "512 \xc3\x97 342, 1-bit", "compact_9in");
V8_BUILTIN_CRT(colorclassic_builtin_video, "Spice, 512 \xc3\x97 384", "compact_10in_color");
V8_BUILTIN_CRT(colorclassicii_builtin_video, "Sonora, 512 \xc3\x97 384", "compact_10in_color");
V8_BUILTIN_CRT(lc520_builtin_video, "Ardbeg, 640 \xc3\x97 480", "builtin_14in_color");

// ============================================================
// Boards
// ============================================================

// The LC II: VIA1 PA6/PA4/PA2/PA1 = 1/1/1/0, the LC's code -- the two share a
// logic-board ID (LC II Developer Note, "Identifying the LC II").
static const v8_board_t lcii_board = {
    .id_strap = 0x54,
    .pcb_ram = 4 * MB,
    .fb_source = V8_FB_VRAM,
    .vram_size = 256u * 1024, // the standard VRAM SIMM
    .asc_version = 0xE8,
};

// The Classic II: VIA1 PA6/PA4/PA2/PA1 = 0/1/0/1; 2 MB soldered; the screen
// is the 21,888 bytes below the sound buffer at the top of the RAM space
// ($9F9A80-$9FEFFF, Developer Note Table 1-1).
static const v8_board_t classicii_board = {
    .id_strap = 0x12,
    .pcb_ram = 2 * MB,
    .fb_source = V8_FB_DRAM,
    .dram_fb = 0x009F9A80u,
    .crt_width = 512,
    .crt_height = 342,
    .crt_sense = 7,
    .crt_mono = true,
    .asc_version = 0xE8,
};

// The Color Classic: VIA1 PA6/PA4/PA2/PA1 = 0/0/0/1; 4 MB soldered; the CRT
// is cabled as the 12" RGB (sense 010) -- the 512 x 384 timing of the LC
// II's video code, which its ROM reuses (Developer Note p. 47).
static const v8_board_t colorclassic_board = {
    .id_strap = 0x02,
    .pcb_ram = 4 * MB,
    .fb_source = V8_FB_VRAM,
    .vram_size = 256u * 1024, // soldered; the SIMM socket for 256 KB more stays empty
    .crt_width = 512,
    .crt_height = 384,
    .crt_sense = 2,
    .asc_version = 0xE8,
    .cuda = true,
};

// A Sonora board: 4 MB soldered, 512 KB of VRAM, the V8's sound block; the
// CPU ID register's low half is the board ID.
#define SONORA_BOARD(cpu_id_, ...)                                                                                     \
    {.kind = V8_KIND_SONORA,                                                                                           \
     .pcb_ram = 4 * MB,                                                                                                \
     .fb_source = V8_FB_VRAM,                                                                                          \
     .vram_size = 512u * 1024,                                                                                         \
     .asc_version = 0xE8,                                                                                              \
     .cpu_id = (cpu_id_),                                                                                              \
     __VA_ARGS__}

// The LC III and III+: CPU ID $A55A0001 (board ID 1, 25 MHz) and $A55A0003
// (board ID 3, 33 MHz); Egret.
static const v8_board_t lciii_board = SONORA_BOARD(0xA55A0001u);
static const v8_board_t lciiip_board = SONORA_BOARD(0xA55A0003u);

// The LC 520 and 550: CPU ID $A55A0100 / $A55A0101; the display cabled as
// the 13" RGB, sense 110, 640 x 480 (LC 520 Developer Note Table 2-1, p. 25:
// the AppleColor High-Resolution RGB timing); Cuda.
#define LC520_CRT .crt_width = 640, .crt_height = 480, .crt_sense = 6, .cuda = true
static const v8_board_t lc520_board = SONORA_BOARD(0xA55A0100u, LC520_CRT);
static const v8_board_t lc550_board = SONORA_BOARD(0xA55A0101u, LC520_CRT);

// The Color Classic II: the LC 550's CPU ID; the CRT cabled as the 12" RGB
// (sense 010); Cuda.
static const v8_board_t colorclassicii_board =
    SONORA_BOARD(0xA55A0101u, .crt_width = 512, .crt_height = 384, .crt_sense = 2, .cuda = true);

// ============================================================
// Profiles
// ============================================================

// What every member's profile shares.
#define V8_PROFILE_COMMON                                                                                              \
    .cpu_model = 68030, .mmu_kind = MMU_68030_PMMU, .fpu_absent = true, .address_bits = 32,                            \
    .storage = mac_storage_scsi_hd_bay, .default_storage = mac_default_storage_hd0_cd3, .appletalk = true,             \
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus, .pram = &pram_defaults_iici, .substrate = &v8_substrate

// The V8 decoders: a 10 MB RAM space (soldered RAM plus a pair of 1, 2 or 4 MB
// SIMMs; with 4 MB SIMMs the space hides 2 MB of a 4 MB soldered bank).
#define V8_RAM .ram_default = 0xA00000, .ram_max = 0xA00000

// The Sonora decoders: 4 MB soldered plus one 72-pin SIMM of 1 to 32 MB
// (LC III Developer Note Table 1-2, p. 9; LC 520 Developer Note p. 22: "from
// 4 MB to 36 MB"), 8 MB by default; the 1 MB ROM.
#define SONORA_RAM                                                                                                     \
    .ram_default = 0x800000, .ram_max = 0x2400000, .ram_options = mac_ram_options_4mb_72pin_kb, .rom_size = 0x100000,  \
    .floppy_slots = mac_floppy_slots_1hd

static const uint32_t lcii_ram_options_kb[] = {4096, 6144, 8192, 10240, 0};
static const uint32_t classicii_ram_options_kb[] = {2048, 4096, 6144, 10240, 0}; // Developer Note p. 7
static const uint32_t colorclassic_ram_options_kb[] = {4096, 6144, 8192, 10240, 0}; // Developer Note Figure 1-3

const hw_profile_t machine_lcii = {
    .name = "Macintosh LC II",
    .id = "lcii",
    .freq = 15667200, // 15.6672 MHz
    .rom_size = 0x80000, // 512 KB
    V8_RAM,
    .ram_options = lcii_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .builtin_video = &lcii_builtin_video,
    .board = &lcii_board,
    V8_PROFILE_COMMON,
};

const hw_profile_t machine_classicii = {
    .name = "Macintosh Classic II",
    .id = "classicii",
    .freq = 15667200,
    .rom_size = 0x80000,
    V8_RAM,
    .ram_options = classicii_ram_options_kb,
    .floppy_slots = mac_floppy_slots_ext,
    .builtin_video = &classicii_builtin_video,
    .board = &classicii_board,
    V8_PROFILE_COMMON,
};

const hw_profile_t machine_colorclassic = {
    .name = "Macintosh Color Classic",
    .id = "colorclassic",
    .freq = 15667200,
    .rom_size = 0x100000, // 1 MB
    V8_RAM,
    .ram_options = colorclassic_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .builtin_video = &colorclassic_builtin_video,
    .board = &colorclassic_board,
    V8_PROFILE_COMMON,
};

const hw_profile_t machine_lciii = {
    .name = "Macintosh LC III",
    .id = "lciii",
    .freq = 25000000,
    SONORA_RAM,
    .builtin_video = &lciii_builtin_video,
    .board = &lciii_board,
    V8_PROFILE_COMMON,
};

const hw_profile_t machine_lciiip = {
    .name = "Macintosh LC III+",
    .id = "lciiip",
    .freq = 33000000,
    SONORA_RAM,
    .builtin_video = &lciii_builtin_video,
    .board = &lciiip_board,
    V8_PROFILE_COMMON,
};

const hw_profile_t machine_lc520 = {
    .name = "Macintosh LC 520",
    .id = "lc520",
    .freq = 25000000,
    SONORA_RAM,
    .builtin_video = &lc520_builtin_video,
    .board = &lc520_board,
    V8_PROFILE_COMMON,
};

const hw_profile_t machine_lc550 = {
    .name = "Macintosh LC 550",
    .id = "lc550",
    .freq = 33000000,
    SONORA_RAM,
    .builtin_video = &lc520_builtin_video,
    .board = &lc550_board,
    V8_PROFILE_COMMON,
};

const hw_profile_t machine_colorclassicii = {
    .name = "Macintosh Color Classic II",
    .id = "colorclassicii",
    .freq = 33000000,
    SONORA_RAM,
    .builtin_video = &colorclassicii_builtin_video,
    .board = &colorclassicii_board,
    V8_PROFILE_COMMON,
};
