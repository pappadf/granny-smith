// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sonora.c
// The Sonora chip kind of the V8 family substrate (see v8.h): the decoder of
// the Macintosh LC III and its successors.  Sonora "integrates the functions
// of the V8 and SWIM chips" (Apple, *Macintosh LC III* Developer Note (1993),
// p. 7); the Ardbeg of the LC 520 family is the same decoder with a power
// register and the front-panel buttons added (Apple, *Macintosh LC 520*
// Developer Note (1993), p. 21).  What differs from the V8 is all here:
//
// Address map (LC III Developer Note Table 1-1, p. 7).  The hardware decodes
// 32-bit addresses only; 24-bit mode is the PMMU's remapping.  RAM from 0,
// up to 36 MB; ROM at $40800000 (the image repeats through the 256 MB ROM
// space, which is also the overlay's trigger); the I/O island at $50F00000;
// VRAM at $60B00000; the CPU ID register at $5FFFFFFC.  No 24-bit windows.
//
// RAM.  The RAM configuration register ($50F26001, the V8's expansion
// register) carries a SIMM size code per socket in bits 0-2 and 3-5 (bank
// size 2^(n-1) MB, as the ROM's sizing code counts it; 0 = empty), the
// built-in bank's presence in bit 6 and the PDS clock in bit 7.  Code 7 is the
// sizing configuration: every bank decodes on a 16 MB boundary (built-in RAM
// at 0, the first SIMM's two banks at 16 and 32 MB, the second socket's at 48
// and 64 MB) so the ROM can size each one by its wrap.  Any other value
// stitches the banks end to end from 0: the built-in 4 MB, then the SIMM
// (LC III Developer Note Table 1-2, p. 9: expansion RAM starts at $400000).
// Host RAM is laid out [built-in][SIMM], so the stitched decode is
// host-contiguous and the PMMU's plain `phys < ram_size` path serves it.
//
// VRAM.  512 KB soldered, a second 512 KB bank for the VRAM SIMM (not
// fitted here); the VRAM size register ($50F26004) only tells the decoder
// what the video driver found.  The second bank floats.
//
// Video control ($50F28000-$50F28002): the monitor code and blanking bit, the
// frame-buffer depth (0..4 = 1..16 bpp), and the sense lines -- the low three
// bits drive them (a 0 pulls a line low), the next three read them back.
// The ROM reads the built-in display's sense through this register to tell
// the boards that share a CPU ID apart (sense 6: the 14" LC 520 family;
// sense 2: the Color Classic II).

#include "v8.h"
#include "v8_internal.h"

#include "board_io.h"
#include "cpu.h"
#include "log.h"
#include "memory.h"
#include "rbv.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

#define MB (1024u * 1024u)

// ============================================================
// Register rows
// ============================================================

// $50F26004-$50F2600F: the registers Sonora adds after the RBV-style ones
// (VRAM size, CPU speed, the Ardbeg's power register): plain latches.
static uint8_t sonora_misc_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    return v8_st(cfg)->sonora.misc[win_off % sizeof v8_st(cfg)->sonora.misc];
}

static void sonora_misc_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)addr;
    LOG(2, "Sonora register $%02X = $%02X", (unsigned)(4 + win_off), value);
    v8_st(cfg)->sonora.misc[win_off % sizeof v8_st(cfg)->sonora.misc] = value;
}

// The sense lines as the monitor presents them under the current drive.
static uint8_t sonora_sense_lines(config_t *cfg) {
    v8_state_t *st = v8_st(cfg);
    return (uint8_t)(v8_video_sense(cfg) & st->sonora.vd_sense & 7u);
}

// $50F28000-: the video control registers.
static uint8_t sonora_vctl_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    v8_state_t *st = v8_st(cfg);
    switch (win_off) {
    case 0:
        return st->sonora.vd_mode;
    case 1:
        return st->sonora.vd_colr;
    case 2:
        return (uint8_t)((sonora_sense_lines(cfg) << 4) | (st->sonora.vd_sense & 7u));
    default:
        return 0xFF;
    }
}

static void sonora_vctl_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)addr;
    v8_state_t *st = v8_st(cfg);
    switch (win_off) {
    case 0:
        LOG(2, "Sonora video mode = $%02X", value);
        st->sonora.vd_mode = value;
        v8_video_set_mode(cfg, value & 0x0Fu);
        break;
    case 1:
        LOG(2, "Sonora video depth = $%02X", value);
        st->sonora.vd_colr = value;
        v8_video_set_depth(cfg, value & 0x07u);
        break;
    case 2:
        st->sonora.vd_sense = value;
        break;
    default:
        break;
    }
}

BOARD_IO_ROW_READ(sonora_ariel_row_read, v8_ariel_read)
BOARD_IO_ROW_READ(sonora_ariel_row_peek, v8_ariel_peek)
BOARD_IO_ROW_WRITE(sonora_ariel_row_write, v8_ariel_write)

#define SONORA_VIA_IO_PENALTY 16
#define SONORA_DEV_IO_PENALTY 2

// The $50F00000 island: the V8's rows, plus Sonora's extra registers ahead of
// the RBV-style row they sit inside (first match wins) and the video control
// block at $28000.
//   base     end      device            penalty               xform               rd  wr     name
const board_io_range_t sonora_io_ranges_tbl[] = {
    {0x00000, 0x02000, BOARD_DEV_VIA1, SONORA_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x04000, 0x06000, BOARD_DEV_SCC, SONORA_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x06000, 0x08000, BOARD_DEV_SCSI, SONORA_DEV_IO_PENALTY, BOARD_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_drq"},
    {0x10000, 0x12000, BOARD_DEV_SCSI, SONORA_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scsi_reg"},
    {0x12000, 0x14000, BOARD_DEV_SCSI, SONORA_DEV_IO_PENALTY, BOARD_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_blind"},
    {0x14000, 0x16000, BOARD_DEV_ASC, SONORA_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "asc"},
    {0x16000, 0x18000, BOARD_DEV_FLOPPY, SONORA_DEV_IO_PENALTY, BOARD_IO_STRIDE_512, 0, 0, NULL, NULL, "swim2"},
    {0x24000, 0x26000, BOARD_DEV_VDAC, SONORA_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, sonora_ariel_row_read,
     sonora_ariel_row_write, "ariel", .peek_fn = sonora_ariel_row_peek},
    {0x26004, 0x26010, BOARD_DEV_RBV, SONORA_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, sonora_misc_read, sonora_misc_write,
     "sonora_regs"},
    {0x26000, 0x28000, BOARD_DEV_RBV, SONORA_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "sonora_via2"},
    {0x28000, 0x28004, BOARD_DEV_RBV, SONORA_DEV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, sonora_vctl_read, sonora_vctl_write,
     "sonora_video"},
    {0}, // sentinel
};

// ============================================================
// CPU ID register
// ============================================================

// The page at $5FFFF000 answers only in its last longword: the ID, read-only
// (the ROM checks a write does not change it).
static uint8_t cpuid_byte(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    uint32_t off = addr & 0xFFFu;
    if (off < 0xFFCu)
        return 0xFF;
    return (uint8_t)(v8_board(cfg)->cpu_id >> (8 * (3 - (off & 3))));
}
static uint8_t cpuid_rd8(void *ctx, uint32_t a, bool peek) {
    (void)peek;
    return cpuid_byte(ctx, a);
}
static uint16_t cpuid_rd16(void *ctx, uint32_t a, bool peek) {
    (void)peek;
    return (uint16_t)((cpuid_byte(ctx, a) << 8) | cpuid_byte(ctx, a + 1));
}
static uint32_t cpuid_rd32(void *ctx, uint32_t a, bool peek) {
    return ((uint32_t)cpuid_rd16(ctx, a, peek) << 16) | cpuid_rd16(ctx, a + 2, peek);
}
MEMORY_READ_FACE(cpuid)
static void cpuid_write8(void *ctx, uint32_t a, uint8_t v) {
    (void)ctx, (void)a, (void)v;
}
static void cpuid_write16(void *ctx, uint32_t a, uint16_t v) {
    (void)ctx, (void)a, (void)v;
}
static void cpuid_write32(void *ctx, uint32_t a, uint32_t v) {
    (void)ctx, (void)a, (void)v;
}
MEMORY_IFACE(cpuid);

// ============================================================
// RAM
// ============================================================

// The SIMM's banks: a 2, 8 or 32 MB SIMM is two banks of half its size, the
// others one bank (LC III Developer Note Table 1-2).
static uint32_t sonora_simm_bank(const v8_state_t *st) {
    uint32_t s = st->simm_size;
    return (s == 2 * MB || s == 8 * MB || s == 32 * MB) ? s / 2 : s;
}

// Map one bank's host bytes from `addr`, repeating `size` through `span`.
static void sonora_map_bank(uint32_t addr, uint32_t span, uint8_t *host, uint32_t size) {
    if (!size)
        return;
    for (uint32_t off = 0; off < span; off += MEM_PAGE_SIZE)
        page_fill_68k((addr + off) >> MEM_PAGE_SHIFT, host + (off % size), true);
}

// Map the RAM space as the configuration register `cfg_reg` decodes it.
static void sonora_map_ram_decode(config_t *cfg, uint8_t cfg_reg) {
    v8_state_t *st = v8_st(cfg);
    for (uint32_t a = 0; a < SONORA_RAM_SPACE; a += MEM_PAGE_SIZE)
        page_clear(a >> MEM_PAGE_SHIFT);
    uint32_t bank = sonora_simm_bank(st);
    if ((cfg_reg & 7u) == 7u) {
        // Sizing: a bank per 16 MB, each repeating through its span.
        sonora_map_bank(0, SONORA_RAM_BANK, st->pcb_host, st->pcb_size);
        sonora_map_bank(SONORA_RAM_BANK, SONORA_RAM_BANK, st->simm_host, bank);
        if (bank < st->simm_size)
            sonora_map_bank(2 * SONORA_RAM_BANK, SONORA_RAM_BANK, st->simm_host + bank, st->simm_size - bank);
    } else {
        // Stitched: the built-in RAM, then the first socket's SIMM.
        sonora_map_bank(0, st->pcb_size, st->pcb_host, st->pcb_size);
        uint32_t code = cfg_reg & 7u;
        uint32_t decode = code ? (1u << (code - 1)) * MB : 0;
        uint32_t simm = decode < st->simm_size ? decode : st->simm_size;
        sonora_map_bank(st->pcb_size, simm, st->simm_host, simm);
    }
    st->ram_exp = cfg_reg;
}

// Overlay drop: RAM at zero as the register decodes it.
static void sonora_map_ram(config_t *cfg) {
    sonora_map_ram_decode(cfg, v8_st(cfg)->ram_exp);
}

void sonora_ram_cfg_written(void *ctx, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    v8_state_t *st = v8_st(cfg);
    LOG(2, "Sonora RAM configuration = $%02X (pc=%08X)", value, cpu_get_pc(cfg->cpu));
    st->ram_exp = value;
    if (!st->overlay.armed)
        sonora_map_ram_decode(cfg, value);
}

int sonora_ram_layout(config_t *cfg) {
    const v8_board_t *b = v8_board(cfg);
    v8_state_t *st = v8_st(cfg);
    uint8_t *ram = ram_native_pointer(cfg->memory_map, 0);
    st->pcb_size = b->pcb_ram;
    st->simm_size = cfg->ram_size > b->pcb_ram ? cfg->ram_size - b->pcb_ram : 0;
    st->pcb_host = ram;
    st->simm_host = ram + st->pcb_size;
    st->sonora.vd_sense = 7; // the sense lines come up tri-stated
    LOG(1, "%s RAM: %u KB soldered + %u KB SIMM", b->chip, st->pcb_size / 1024, st->simm_size / 1024);
    return 0;
}

// ============================================================
// Memory layout
// ============================================================

void sonora_memory_layout_init(config_t *cfg) {
    const v8_board_t *b = v8_board(cfg);
    v8_state_t *st = v8_st(cfg);

    board_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->memory_map, V8_ISLAND_BASE, V8_ISLAND_SIZE, "I/O", &st->io_interface, &st->io);
    memory_map_add(cfg->memory_map, SONORA_CPUID_ADDR & ~0xFFFu, MEM_PAGE_SIZE, "CPU ID", &cpuid_iface, cfg);

    // VRAM: the soldered bank at $60B00000; the SIMM bank above it floats.
    if (st->video.vram) {
        sonora_map_bank(SONORA_VRAM_BASE, b->vram_size, st->video.vram, b->vram_size);
        memory_map_host_region(cfg->memory_map, "sonora_vram", st->video.vram, SONORA_VRAM_BASE, b->vram_size, true);
    }

    // The overlay: ROM at zero until the first access to the ROM space.
    m68k_board_overlay_init(&st->overlay, cfg, b->desc.rom_base, b->desc.rom_end, sonora_map_ram, b->chip);
    memory_map_add(cfg->memory_map, b->desc.rom_base, b->desc.rom_end - b->desc.rom_base, "ROM aperture",
                   &st->overlay.iface, &st->overlay);
    m68k_board_overlay_arm(&st->overlay);
}

// ============================================================
// Checkpoint and reset
// ============================================================

void sonora_checkpoint(config_t *cfg, checkpoint_t *cp) {
    v8_state_t *st = v8_st(cfg);
    system_write_checkpoint_data(cp, &st->sonora, sizeof st->sonora);
}

void sonora_restore(config_t *cfg, checkpoint_t *cp) {
    v8_state_t *st = v8_st(cfg);
    system_read_checkpoint_data(cp, &st->sonora, sizeof st->sonora);
    if (!st->overlay.armed)
        sonora_map_ram_decode(cfg, st->ram_exp);
}

// /RESET: the decoder's registers back to power-on (sense lines tri-stated).
void sonora_bus_reset(config_t *cfg) {
    v8_state_t *st = v8_st(cfg);
    memset(&st->sonora, 0, sizeof st->sonora);
    st->sonora.vd_sense = 7;
    st->ram_exp = 0;
}
