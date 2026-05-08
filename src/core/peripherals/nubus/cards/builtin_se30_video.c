// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// builtin_se30_video.c
// SE/30 built-in video as a NuBus card living in slot $E.  Implements the
// nubus_card_ops_t vtable plus the SE/30-specific hooks declared in
// builtin_se30_video.h.  See proposal-machine-iicx-iix.md §3.2.5.
//
// What the card owns:
//   * 64 KB VRAM at $FEE00000
//   * 32 KB declaration ROM at $FEFF8000 (real SE30.vrom if available,
//     synthesised fallback otherwise; the byte-banged synth is the same
//     220-line sequence se30.c used to carry inline)
//   * the display_t exposed via system_display() — 512×342×1bpp, with
//     `bits` toggled between primary and alternate VRAM offsets when
//     se30_via1_output reports a VIA1 PA6 change

#include "builtin_se30_video.h"
#include "card.h"
#include "checkpoint.h"
#include "display.h"
#include "log.h"
#include "memory.h"
#include "rom.h"
#include "system.h"
#include "system_config.h"
#include "vrom.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("se30video");

#define SE30_VRAM_SIZE           0x00010000UL // 64 KB
#define SE30_VROM_SIZE           0x00008000UL // 32 KB
#define SE30_FB_PRIMARY_OFFSET   0x8040
#define SE30_FB_ALTERNATE_OFFSET 0x0040

// Per-card private state.  Hangs off card->priv.
typedef struct {
    uint8_t *vram;
    uint8_t *vrom;
    char *vrom_path; // path the VROM was loaded from; NULL if synthesised
    bool main_buf; // true: primary, false: alternate
    display_t display;
} se30_priv_t;

// === VROM loading / synth (moved verbatim from se30.c) ======================

// Build a minimal fallback NuBus declaration ROM (8 KB at the top of the
// 32 KB buffer) when the real SE30.vrom is not available.  The byte
// sequence below is taken verbatim from the legacy se30_build_vrom_fallback
// in src/machines/se30.c — see that history for rationale.
static void synthesise_vrom_fallback(uint8_t *rom) {
    memset(rom, 0, SE30_VROM_SIZE);
    uint8_t *top = rom + 0x6000;

    // ---- sResource Directory at 0x0000 ----
    top[0x00] = 0x01;
    top[0x03] = 0x14; // Board sResource (ID=1) at +0x14
    top[0x04] = 0x80;
    top[0x07] = 0x3C; // Video sResource (ID=0x80) at +0x3C
    top[0x08] = 0xFF; // end

    // ---- Board sResource at 0x0014 ----
    top[0x14] = 0x01;
    top[0x17] = 0xAC; // sRsrcType → 0x00C0
    top[0x18] = 0x02;
    top[0x1B] = 0xB8; // sRsrcName → 0x00D0
    top[0x1C] = 0x20;
    top[0x1F] = 0x0C; // BoardId = $0C
    top[0x20] = 0x22;
    top[0x22] = 0x02;
    top[0x23] = 0xE0; // PrimaryInit → 0x0300
    top[0x24] = 0xFF;

    // ---- Video sResource at 0x0040 ----
    top[0x40] = 0x01;
    top[0x43] = 0xA0; // sRsrcType → 0x00E0
    top[0x44] = 0x02;
    top[0x47] = 0xAC; // sRsrcName → 0x00F0
    top[0x48] = 0x04;
    top[0x4A] = 0x01;
    top[0x4B] = 0xB8; // sRsrcDrvrDir → 0x0200
    top[0x4C] = 0x08;
    top[0x4F] = 0x01; // sRsrcHWDevId = 1
    top[0x50] = 0x0A;
    top[0x52] = 0x01;
    top[0x53] = 0x2A; // minorBaseOS → 0x017A
    top[0x54] = 0x0B;
    top[0x56] = 0x01;
    top[0x57] = 0x2A; // minorLength → 0x017E
    top[0x58] = 0x80;
    top[0x5B] = 0xE8; // OneBitMode → 0x0140
    top[0x5C] = 0xFF;

    // ---- Type & name blocks ----
    top[0xC1] = 0x01; // catBoard
    memcpy(&top[0xD0], "Macintosh SE/30", 16);
    top[0xE1] = 0x03;
    top[0xE3] = 0x01;
    top[0xE5] = 0x01; // drSwApple
    top[0xE7] = 0x09;
    memcpy(&top[0xF0], "Built-in Video", 15);

    // ---- PrimaryInit sBlock at 0x0300 ----
    top[0x303] = 0x80; // sBlock size
    top[0x304] = 0x02;
    top[0x305] = 0x02; // rev=2, cpu=68020
    top[0x30B] = 0x04; // code offset
    static const uint8_t primaryinit[] = {0x31, 0x7C, 0x00, 0x01, 0x00, 0x02, // MOVE.W #1,spResult(A0)
                                          0x41, 0xFA, 0x00, 0x6A, // LEA data(PC),A0
                                          0x20, 0x3C, 0x00, 0x02, 0x00, 0x80, // MOVE.L #$00020080,D0
                                          0xA0, 0x52, // _WriteXPRam
                                          0x20, 0x78, 0x01, 0xD4, // MOVEA.L $01D4.W,A0 (VIA1 base)
                                          0x08, 0xE8, 0x00, 0x06, 0x06, 0x00, // BSET #6,$600(A0)
                                          0x08, 0xE8, 0x00, 0x06, 0x04, 0x00, // BSET #6,$400(A0)
                                          0x08, 0xE8, 0x00, 0x06, 0x1E, 0x00, // BSET #6,$1E00(A0)
                                          0x08, 0xD0, 0x00, 0x06, // BSET #6,(A0)
                                          0x22, 0x7C, 0xFE, 0xE0, 0x00, 0x00, // MOVEA.L #$FEE00000,A1
                                          0x24, 0x49, // MOVEA.L A1,A2
                                          0x2A, 0x3C, 0xAA, 0xAA, 0xAA, 0xAA, // MOVE.L #$AAAAAAAA,D5
                                          0xD3, 0xFC, 0x00, 0x00, 0x80, 0x40, // ADDA.L #$8040,A1
                                          0x36, 0x3C, 0x01, 0x55, // MOVE.W #$155,D3
                                          0x34, 0x3C, 0x00, 0x0F, // MOVE.W #$F,D2
                                          0x22, 0xC5, 0x51, 0xCA, 0xFF, 0xFC, 0x46, 0x85, 0x51, 0xCB, 0xFF,
                                          0xF2, 0x22, 0x4A, 0xD2, 0xFC, 0x00, 0x40, 0x36, 0x3C, 0x01, 0x55,
                                          0x34, 0x3C, 0x00, 0x0F, 0x22, 0xC5, 0x51, 0xCA, 0xFF, 0xFC, 0x46,
                                          0x85, 0x51, 0xCB, 0xFF, 0xF2, 0x70, 0x00, 0x4E, 0x75, 0x0E, 0x80};
    memcpy(&top[0x30C], primaryinit, sizeof(primaryinit));

    // ---- Mode sResource at 0x0140 ----
    top[0x140] = 0x01;
    top[0x143] = 0x10;
    top[0x144] = 0x03;
    top[0x147] = 0x02;
    top[0x148] = 0x04;
    top[0x14C] = 0xFF;

    // ---- VPBlock at 0x0150 ----
    top[0x153] = 0x2E;
    top[0x156] = 0x80;
    top[0x157] = 0x40;
    top[0x159] = 0x40;
    top[0x15E] = 0x01;
    top[0x15F] = 0x56;
    top[0x160] = 0x02;
    top[0x16A] = 0x00;
    top[0x16B] = 0x48;
    top[0x16E] = 0x00;
    top[0x16F] = 0x48;
    top[0x175] = 0x01;
    top[0x177] = 0x01;
    top[0x179] = 0x01;

    // ---- minorLength data at 0x017E ----
    top[0x180] = 0xD5;
    top[0x181] = 0xC0;

    // ---- Driver directory at 0x0200 ----
    top[0x200] = 0x02;
    top[0x203] = 0x20;
    top[0x204] = 0xFF;

    // ---- DRVR sBlock at 0x0220 ----
    static const uint8_t drvr_sblock[] = {
        0x00, 0x00, 0x00, 0x38, 0x4F, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00,
        0x00, 0x00, 0x30, 0x00, 0x34, 0x00, 0x2C, 0x19, 0x2E, 0x44, 0x69, 0x73, 0x70, 0x6C, 0x61,
        0x79, 0x5F, 0x56, 0x69, 0x64, 0x65, 0x6F, 0x5F, 0x41, 0x70, 0x70, 0x6C, 0x65, 0x5F, 0x53,
        0x45, 0x33, 0x30, 0x70, 0x00, 0x4E, 0x75, 0x70, 0x00, 0x4E, 0x75, 0x70, 0x00, 0x4E, 0x75,
    };
    memcpy(&top[0x220], drvr_sblock, sizeof(drvr_sblock));

    // ---- Format Header at top+0x1FEC ----
    top[0x1FEC] = 0x00;
    top[0x1FED] = 0xFF;
    top[0x1FEE] = 0xE0;
    top[0x1FEF] = 0x14;
    top[0x1FF2] = 0x20;
    top[0x1FF8] = 0x01;
    top[0x1FF9] = 0x01;
    top[0x1FFA] = 0x5A;
    top[0x1FFB] = 0x93;
    top[0x1FFC] = 0x2B;
    top[0x1FFD] = 0xC7;
    top[0x1FFF] = 0x0F;

    // CRC over the top 8 KB
    uint32_t crc = 0;
    for (int i = 0; i < 0x2000; i++) {
        crc = ((crc << 1) | (crc >> 31));
        if (i < 0x1FF4 || i >= 0x1FF8)
            crc += top[i];
    }
    top[0x1FF4] = (uint8_t)(crc >> 24);
    top[0x1FF5] = (uint8_t)(crc >> 16);
    top[0x1FF6] = (uint8_t)(crc >> 8);
    top[0x1FF7] = (uint8_t)(crc);
}

// Try to load the VROM bytes from a single path.  Returns true on success.
static bool try_load_vrom(const char *path, uint8_t *vrom_buf) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(vrom_buf, 1, SE30_VROM_SIZE, f);
    fclose(f);
    if (n == SE30_VROM_SIZE) {
        LOG(1, "Loaded real VROM from %s (%zu bytes)", path, n);
        return true;
    }
    return false;
}

// Try the explicit pending path, then well-known search paths, then the
// directory holding the loaded ROM.  Stores the path used in *out_path
// (caller takes ownership) on success.
static bool load_real_vrom(config_t *cfg, uint8_t *vrom_buf, char **out_path) {
    *out_path = NULL;
    const char *explicit_path = vrom_pending_path();
    if (explicit_path) {
        if (try_load_vrom(explicit_path, vrom_buf)) {
            *out_path = strdup(explicit_path);
            return true;
        }
        LOG(0, "VROM file %s not found or wrong size", explicit_path);
    }
    static const char *search_paths[] = {"/opfs/images/vrom/SE30.vrom", "tests/data/roms/SE30.vrom", "SE30.vrom", NULL};
    for (const char **p = search_paths; *p; p++) {
        if (try_load_vrom(*p, vrom_buf)) {
            *out_path = strdup(*p);
            return true;
        }
    }
    const char *rom_path = rom_pending_path();
    if (!rom_path)
        rom_path = memory_rom_filename(cfg->mem_map);
    if (rom_path) {
        const char *slash = strrchr(rom_path, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - rom_path + 1);
            char vrom_path[512];
            if (dir_len + sizeof("SE30.vrom") <= sizeof(vrom_path)) {
                memcpy(vrom_path, rom_path, dir_len);
                memcpy(vrom_path + dir_len, "SE30.vrom", sizeof("SE30.vrom"));
                if (try_load_vrom(vrom_path, vrom_buf)) {
                    *out_path = strdup(vrom_path);
                    return true;
                }
            }
        }
    }
    LOG(0, "FATAL: Real VROM (SE30.vrom) not found.");
    return false;
}

// === Card vtable ============================================================

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp) {
    (void)cp;
    se30_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->vram = calloc(1, SE30_VRAM_SIZE);
    p->vrom = calloc(1, SE30_VROM_SIZE);
    if (!p->vram || !p->vrom) {
        free(p->vram);
        free(p->vrom);
        free(p);
        return -1;
    }
    if (!load_real_vrom(cfg, p->vrom, &p->vrom_path)) {
        if (!cp) {
            // Real VROM was the long-standing requirement on cold boot;
            // fall back to the synthesised image so the SE/30 keeps booting
            // even when SE30.vrom is absent in CI environments.
            synthesise_vrom_fallback(p->vrom);
        }
        // Restoring from a checkpoint?  The VROM contents will be overwritten
        // from the checkpoint stream; an empty buffer here is fine.
    }

    // Populate the display descriptor.  Primary buffer at $8040 is the
    // boot-time selection — VIA1 PA6 will toggle it before the OS draws.
    p->main_buf = true;
    p->display.width = 512;
    p->display.height = 342;
    p->display.stride = 512 / 8;
    p->display.format = PIXEL_1BPP_MSB;
    p->display.bits = p->vram + SE30_FB_PRIMARY_OFFSET;
    p->display.clut = NULL;
    p->display.clut_len = 0;
    p->display.generation = 1;

    card->priv = p;
    return 0;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    se30_priv_t *p = card->priv;
    if (!p)
        return;
    free(p->vram);
    free(p->vrom);
    free(p->vrom_path);
    free(p);
    card->priv = NULL;
}

static const display_t *card_display(nubus_card_t *card) {
    se30_priv_t *p = card->priv;
    return p ? &p->display : NULL;
}

static const char *card_name(const nubus_card_t *card) {
    (void)card;
    return "Macintosh SE/30 Built-in Video";
}

static const nubus_card_ops_t builtin_se30_video_ops = {
    .init = card_init,
    .teardown = card_teardown,
    .on_vbl = NULL, // VBL slot-IRQ flow stays in se30_trigger_vbl for v1
    .display = card_display,
    .name = card_name,
};

// === Factory + kind descriptor ==============================================

static nubus_card_t *factory(int slot, config_t *cfg, checkpoint_t *cp) {
    nubus_card_t *card = calloc(1, sizeof(*card));
    if (!card)
        return NULL;
    card->ops = &builtin_se30_video_ops;
    card->slot = slot;
    if (card->ops->init(card, cfg, cp) != 0) {
        free(card);
        return NULL;
    }
    return card;
}

// One monitor entry — the SE/30 built-in is fixed at 512×342×1bpp; the
// list is included so the dialog's monitor dropdown has *something* to
// show even though the user can't change it.
static const int builtin_se30_depths[] = {1, 0};
static const nubus_monitor_t builtin_se30_monitors[] = {
    {.id = "se30_internal", .name = "Built-in 9\" CRT", .width = 512, .height = 342, .depths = builtin_se30_depths},
    {0},
};

const nubus_card_kind_t builtin_se30_video_kind = {
    .id = "builtin_se30_video",
    .display_name = "Macintosh SE/30 Built-in Video",
    .requires_vrom = false,
    .monitors = builtin_se30_monitors,
    .factory = factory,
};

// === SE/30-specific public hooks ============================================

void builtin_se30_video_select_buffer(nubus_card_t *card, bool main_buf) {
    if (!card)
        return;
    se30_priv_t *p = card->priv;
    if (!p || p->main_buf == main_buf)
        return;
    p->main_buf = main_buf;
    p->display.bits = p->vram + (main_buf ? SE30_FB_PRIMARY_OFFSET : SE30_FB_ALTERNATE_OFFSET);
    p->display.generation++;
}

uint8_t *builtin_se30_video_vram(nubus_card_t *card) {
    se30_priv_t *p = card ? card->priv : NULL;
    return p ? p->vram : NULL;
}

uint8_t *builtin_se30_video_vrom(nubus_card_t *card) {
    se30_priv_t *p = card ? card->priv : NULL;
    return p ? p->vrom : NULL;
}

const char *builtin_se30_video_vrom_path(nubus_card_t *card) {
    se30_priv_t *p = card ? card->priv : NULL;
    return p ? p->vrom_path : NULL;
}

void builtin_se30_video_checkpoint_save_vram(nubus_card_t *card, checkpoint_t *cp) {
    se30_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    system_write_checkpoint_data(cp, p->vram, SE30_VRAM_SIZE);
}

void builtin_se30_video_checkpoint_restore_vram(nubus_card_t *card, checkpoint_t *cp) {
    se30_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    system_read_checkpoint_data(cp, p->vram, SE30_VRAM_SIZE);
    // Re-derive display.bits in case the checkpoint stream restores
    // VIA1 PA6 to a value that toggles the buffer (via_redrive_outputs
    // will fire after we return).
    p->display.bits = p->vram + (p->main_buf ? SE30_FB_PRIMARY_OFFSET : SE30_FB_ALTERNATE_OFFSET);
    p->display.generation++;
}
