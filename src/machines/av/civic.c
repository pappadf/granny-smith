// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// civic.c
// CIVIC + Sebastian + clock-synthesizer model — see civic.h.
//
// Model shape:
//   * every CIVIC longword slot ($000..$6FF, stride 4) stores ONE bit; the
//     serial protocol (LSB ascending on write, MSB descending on read) and
//     the five direct longword pokes both reduce to slot reads/writes
//   * computed slots: VBLInt (live flag), VDCInt (constant 1 = idle,
//     active-low), SyncClr (reads inverted), ReadSense (monitor sense),
//     CntTest (side-effect-free settle delay, reads 0), CurLine (0)
//   * monitor sense: a static Hi-Res 640x480 monitor (indexed code 6) — a
//     line reads low when the monitor ties it low (code bit 0) or the host
//     drives it (Sense0-2 = 1); the same static answer serves the ROM's
//     extended tie-matrix probe (civic.md §4)
//   * VBL: a 60.15 Hz scheduler event; when the timing generator and
//     VBLEnb are on it latches VBLInt and asserts PSC-VIA2 SInt bit 6
//     (active low) — the ack is the driver's VBLClr 0-then-1 dance
//   * display: geometry fixed by the Hi-Res mode (640x480); depth from
//     Sebastian's PCBR depth code; sub-8bpp CLUT entries live at the
//     documented start/skip positions inside the 256-entry bank

#include "civic.h"

#include "av.h"
#include "psc.h"

#include "cpu.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "scheduler.h"
#include "system.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("civic");

// CIVIC longword-slot indices (hardware byte offset >> 2; civic.md §3).
#define SLOT_VBLINT    (0x000u >> 2)
#define SLOT_ENABLE    (0x004u >> 2)
#define SLOT_VDCINT    (0x008u >> 2)
#define SLOT_SYNCCLR   (0x06Cu >> 2)
#define SLOT_SENSE0    (0x05Cu >> 2) // drives line C
#define SLOT_SENSE1    (0x060u >> 2) // drives line B
#define SLOT_SENSE2    (0x064u >> 2) // drives line A
#define SLOT_RDSENSE0  (0x080u >> 2) // ReadSense bits 0-2 (LSB first)
#define SLOT_RDSENSE2  (0x088u >> 2)
#define SLOT_ROWWORDS  (0x08Cu >> 2) // 8 bits
#define SLOT_BASEADDR  (0x0C0u >> 2) // 9 bits
#define SLOT_VBLENB    (0x110u >> 2)
#define SLOT_VBLCLR    (0x120u >> 2)
#define SLOT_CNTTEST   (0x140u >> 2) // 12 bits, settle-delay reads
#define SLOT_CURLINE   (0x6C0u >> 2) // 12 bits, R/O
#define AV_CIVIC_SLOTS (0x700u >> 2)

// The attached monitor: Hi-Res 640x480, indexed sense code 6 (%110).
#define AV_CIVIC_SENSE_CODE 6u

// 60.15 Hz frame cadence.
#define AV_CIVIC_FRAME_NS 16625103.0

struct av_civic {
    // --- plain data (checkpointed up to the first pointer field) ---
    uint8_t slot[AV_CIVIC_SLOTS]; // one stored bit per longword slot
    bool vbl_flag; // VBLInt latch
    bool vbl_armed; // VBLClr wrote 1 after 0 (re-armed)
    rgba8_t clut[2][256]; // Sebastian CLUT banks (graphics, video-in)
    uint8_t seb_addr; // CLUT address register
    uint8_t seb_phase; // 0-3 within the R,G,B,A quad
    uint8_t seb_pcbr; // Pixel Bus Control Register
    uint8_t clk_reg[3]; // Endeavor M/N/Clk latches

    // --- pointers (not checkpointed) ---
    config_t *cfg;
    uint8_t *vram; // 2 MB, host-owned
    display_t display;
    rgba8_t disp_clut[256]; // derived CLUT the display consumes
    memory_interface_t lo_iface; // the $50036000 register alias
};

static inline av_civic_t *civic_of(config_t *cfg) {
    return ((av_state_t *)cfg->machine_context)->civic;
}

// ============================================================
// Monitor sense (civic.md §4)
// ============================================================
// Line i (0 = C, 1 = B, 2 = A) reads LOW when the static monitor code has
// the bit clear or the host drives the line (SenseN = 1).  ReadSense
// returns the three line states as bits 0-2 — the reset-state read of an
// idle Hi-Res monitor yields the indexed code 6 directly.
static uint8_t civic_sense_lines(av_civic_t *cv) {
    uint8_t drive = (uint8_t)((cv->slot[SLOT_SENSE0] ? 1u : 0) | (cv->slot[SLOT_SENSE1] ? 2u : 0) |
                              (cv->slot[SLOT_SENSE2] ? 4u : 0));
    uint8_t lines = 0;
    for (int i = 0; i < 3; i++) {
        bool monitor_low = !(AV_CIVIC_SENSE_CODE & (1u << i));
        bool host_low = (drive & (1u << i)) != 0;
        lines |= (uint8_t)((monitor_low || host_low ? 0u : 1u) << i);
    }
    return lines;
}

// ============================================================
// Display derivation
// ============================================================

// Assemble a multi-bit logical register from its slots (LSB at lowest).
static uint32_t civic_get(av_civic_t *cv, uint32_t slot0, int width) {
    uint32_t v = 0;
    for (int i = width - 1; i >= 0; i--)
        v = (v << 1) | (cv->slot[slot0 + (uint32_t)i] & 1u);
    return v;
}

// Rebuild the derived display CLUT for the current depth: sub-8bpp modes
// keep their entries at start + i*skip inside the graphics bank
// (sebastian.md §4); 8 bpp is identity.
static void civic_update_disp_clut(av_civic_t *cv) {
    int code = cv->seb_pcbr & 7;
    int depth = 1 << code; // 1,2,4,8 bpp use the CLUT
    if (depth > 8)
        return;
    int entries = 1 << depth;
    int skip = 256 / entries;
    int start = skip - 1;
    for (int i = 0; i < entries; i++) {
        rgba8_t e = cv->clut[0][(start + i * skip) & 0xFF];
        e.a = 255;
        cv->disp_clut[i] = e;
    }
    cv->display.clut_dirty = true;
}

// Re-derive the display descriptor from the live registers.
//
// stride comes from RowWords, NOT from width*bpp/8: the VRAM row pitch is
// quantised well above the visible row.  PrimaryInit paints
// `cvpRowWords << 3` LONGWORDS per row (civic.md §4 step 15), so the pitch
// is RowWords * 32 bytes — 1024 at the Hi-Res 8 bpp setting, which is
// exactly what the booted System reports in ScreenRow.  Deriving it from
// the visible width instead renders 1024-byte rows at a 640-byte pitch and
// shears the picture into bands.
//
// width/height stay the mode constants.  They are not free parameters: the
// sense model answers "Hi-Res" (indexed code 6), which is what makes the
// ROM select this 640x480 mode and program these timings.  The vertical
// timing independently confirms the height — (VFP - VAL) / 2 half-lines =
// (1042 - 82) / 2 = 480 — but the horizontal count's units change with the
// pixel clock between depths, so the dossier's advice to treat the timing
// values as opaque per-mode signatures applies (civic.md §8).
static void civic_update_display(av_civic_t *cv) {
    static const pixel_format_t fmt_by_code[6] = {
        PIXEL_1BPP_MSB, PIXEL_2BPP_MSB, PIXEL_4BPP_MSB, PIXEL_8BPP, PIXEL_16BPP_555, PIXEL_32BPP_XRGB,
    };
    int code = cv->seb_pcbr & 7;
    if (code > 5)
        code = 5;
    uint32_t bpp = 1u << code;
    uint32_t width = 640, height = 480;
    uint32_t row_words = civic_get(cv, SLOT_ROWWORDS, 8);
    uint32_t stride = row_words * 32u;
    // Before the ROM programs RowWords there is no row pitch at all; fall
    // back to the packed width so the descriptor stays self-consistent.
    if (stride < width * bpp / 8u)
        stride = width * bpp / 8u;
    uint32_t base = (civic_get(cv, SLOT_BASEADDR, 9) & 0xFFu) << 5;

    display_t *d = &cv->display;
    bool shape_changed = d->format != fmt_by_code[code] || d->stride != stride || d->width != width;
    d->width = width;
    d->height = height;
    d->stride = stride;
    d->format = fmt_by_code[code];
    d->bits = cv->vram + (base % AV_CIVIC_VRAM_SIZE);
    if (bpp <= 8) {
        d->clut = cv->disp_clut;
        d->clut_len = 1u << bpp;
    } else {
        d->clut = NULL;
        d->clut_len = 0;
    }
    if (shape_changed)
        d->shape_dirty = true;
    civic_update_disp_clut(cv);
}

// ============================================================
// VBL (civic.md §4; the ack is VBLClr 0-then-1)
// ============================================================

static void civic_set_vbl(av_civic_t *cv, bool active) {
    cv->vbl_flag = active;
    av_state_t *st = (av_state_t *)cv->cfg->machine_context;
    if (st && st->psc)
        av_psc_slot_source(st->psc, AV_PSC_SINT_VBL, active);
}

static void civic_frame_event(void *source, uint64_t data) {
    (void)data;
    av_civic_t *cv = (av_civic_t *)source;
    if (cv->slot[SLOT_ENABLE] && cv->slot[SLOT_VBLENB] && cv->vbl_armed)
        civic_set_vbl(cv, true);
    // The framebuffer may have changed; nudge the renderer each frame.
    cv->display.fb_dirty = true;
    scheduler_new_cpu_event(cv->cfg->scheduler, &civic_frame_event, cv, 0, 0, (uint64_t)AV_CIVIC_FRAME_NS);
}

// ============================================================
// CIVIC slot access (serial protocol + direct pokes)
// ============================================================

// A slot read: only byte lane 3 carries D[0].
static uint8_t civic_slot_read(av_civic_t *cv, uint32_t off) {
    if ((off & 3) != 3)
        return 0;
    uint32_t slot = off >> 2;
    if (slot >= AV_CIVIC_SLOTS)
        return 0;
    switch (slot) {
    case SLOT_VBLINT:
        return cv->vbl_flag ? 1 : 0; // active high
    case SLOT_VDCINT:
        return 1; // active LOW — 1 means "no video-in interrupt pending"
    case SLOT_SYNCCLR:
        return cv->slot[SLOT_SYNCCLR] ? 0 : 1; // reads inverted
    default:
        break;
    }
    if (slot >= SLOT_RDSENSE0 && slot <= SLOT_RDSENSE2)
        return (uint8_t)((civic_sense_lines(cv) >> (slot - SLOT_RDSENSE0)) & 1);
    if (slot >= SLOT_CNTTEST && slot < SLOT_CNTTEST + 12)
        return 0; // pure settle delay — side-effect-free
    if (slot >= SLOT_CURLINE && slot < SLOT_CURLINE + 12)
        return 0;
    return (uint8_t)(cv->slot[slot] & 1);
}

static void civic_slot_write(av_civic_t *cv, uint32_t off, uint8_t value) {
    if ((off & 3) != 3)
        return;
    uint32_t slot = off >> 2;
    if (slot >= AV_CIVIC_SLOTS)
        return;
    uint8_t bit = (uint8_t)(value & 1);
    cv->slot[slot] = bit;
    switch (slot) {
    case SLOT_VBLCLR:
        // Ack dance: write 0 clears + disarms, write 1 re-arms.
        if (bit == 0) {
            civic_set_vbl(cv, false);
            cv->vbl_armed = false;
        } else {
            cv->vbl_armed = true;
        }
        break;
    case SLOT_ENABLE:
        if (bit)
            civic_update_display(cv);
        break;
    default:
        // BaseAddr moves the scanout pointer; RowWords changes the pitch.
        if (slot >= SLOT_BASEADDR && slot < SLOT_BASEADDR + 9)
            civic_update_display(cv);
        else if (slot >= SLOT_ROWWORDS && slot < SLOT_ROWWORDS + 8)
            civic_update_display(cv);
        break;
    }
}

uint8_t av_civic_read(config_t *cfg, uint32_t addr) {
    return civic_slot_read(civic_of(cfg), addr & 0x1FFFu);
}

void av_civic_write(config_t *cfg, uint32_t addr, uint8_t value) {
    civic_slot_write(civic_of(cfg), addr & 0x1FFFu, value);
}

// --- The $50036000 register alias (own region, byte-decomposed here) ---

static uint8_t civic_lo_read8(void *ctx, uint32_t off) {
    return civic_slot_read((av_civic_t *)ctx, off & 0x1FFFu);
}

static uint16_t civic_lo_read16(void *ctx, uint32_t off) {
    return (uint16_t)((civic_lo_read8(ctx, off) << 8) | civic_lo_read8(ctx, off + 1));
}

static uint32_t civic_lo_read32(void *ctx, uint32_t off) {
    return ((uint32_t)civic_lo_read16(ctx, off) << 16) | civic_lo_read16(ctx, off + 2);
}

static void civic_lo_write8(void *ctx, uint32_t off, uint8_t value) {
    civic_slot_write((av_civic_t *)ctx, off & 0x1FFFu, value);
}

static void civic_lo_write16(void *ctx, uint32_t off, uint16_t value) {
    civic_lo_write8(ctx, off, (uint8_t)(value >> 8));
    civic_lo_write8(ctx, off + 1, (uint8_t)value);
}

static void civic_lo_write32(void *ctx, uint32_t off, uint32_t value) {
    civic_lo_write16(ctx, off, (uint16_t)(value >> 16));
    civic_lo_write16(ctx, off + 2, (uint16_t)value);
}

// ============================================================
// Sebastian RAMDAC (sebastian.md)
// ============================================================

uint8_t av_civic_seb_read(config_t *cfg, uint32_t addr) {
    av_civic_t *cv = civic_of(cfg);
    uint32_t reg = (addr & 0xFFu) >> 4;
    int bank = (cv->seb_pcbr >> 6) & 1;
    switch (reg) {
    case 0: // address register
        return cv->seb_addr;
    case 1: { // data register: R,G,B,A with auto-increment after the quad
        rgba8_t *e = &cv->clut[bank][cv->seb_addr];
        uint8_t v = 0;
        switch (cv->seb_phase) {
        case 0:
            v = e->r;
            break;
        case 1:
            v = e->g;
            break;
        case 2:
            v = e->b;
            break;
        default:
            v = e->a;
            break;
        }
        if (++cv->seb_phase == 4) {
            cv->seb_phase = 0;
            cv->seb_addr++;
        }
        return v;
    }
    case 2:
        return cv->seb_pcbr;
    default:
        return 0;
    }
}

void av_civic_seb_write(config_t *cfg, uint32_t addr, uint8_t value) {
    av_civic_t *cv = civic_of(cfg);
    uint32_t reg = (addr & 0xFFu) >> 4;
    int bank = (cv->seb_pcbr >> 6) & 1;
    switch (reg) {
    case 0:
        cv->seb_addr = value;
        cv->seb_phase = 0;
        break;
    case 1: {
        rgba8_t *e = &cv->clut[bank][cv->seb_addr];
        switch (cv->seb_phase) {
        case 0:
            e->r = value;
            break;
        case 1:
            e->g = value;
            break;
        case 2:
            e->b = value;
            break;
        default:
            e->a = value;
            break;
        }
        if (++cv->seb_phase == 4) {
            cv->seb_phase = 0;
            cv->seb_addr++;
        }
        civic_update_disp_clut(cv);
        break;
    }
    case 2:
        cv->seb_pcbr = value;
        LOG(2, "Sebastian PCBR = $%02X (depth code %d)", value, value & 7);
        civic_update_display(cv);
        break;
    default:
        break;
    }
}

// ============================================================
// Endeavor / Clifton / PUMA clock latches (endeavor-clifton-puma.md)
// ============================================================
// Pure write-latches; the PUMA ID probe reads EndeavorM bit-serially and a
// Clifton answers all ones — return $FF so a 660AV detects Clifton.

uint8_t av_civic_clk_read(config_t *cfg, uint32_t addr) {
    (void)cfg;
    (void)addr;
    return 0xFF;
}

void av_civic_clk_write(config_t *cfg, uint32_t addr, uint8_t value) {
    av_civic_t *cv = civic_of(cfg);
    uint32_t reg = (addr & 0xFFu) >> 4;
    if (reg < 3)
        cv->clk_reg[reg] = value;
}

// ============================================================
// Memory installation
// ============================================================

void av_civic_install_memory(config_t *cfg, av_civic_t *cv) {
    // VRAM: direct writable pages + a bus-resolver host region so the 040
    // walker and DMA reach it by physical address.
    uint32_t pages = AV_CIVIC_VRAM_SIZE >> PAGE_SHIFT;
    uint32_t start = AV_CIVIC_VRAM_BASE >> PAGE_SHIFT;
    for (uint32_t i = 0; i < pages && (int)(start + i) < g_page_count; i++)
        mac030_fill_page(start + i, cv->vram + (i << PAGE_SHIFT), true);
    memory_map_host_region(cfg->mem_map, "civic_vram", cv->vram, AV_CIVIC_VRAM_BASE, AV_CIVIC_VRAM_SIZE,
                           /*writable*/ true);

    // The low CIVIC register alias at $50036000 (the island row at $36000
    // serves the $50F36000 face).
    cv->lo_iface.read_uint8 = civic_lo_read8;
    cv->lo_iface.read_uint16 = civic_lo_read16;
    cv->lo_iface.read_uint32 = civic_lo_read32;
    cv->lo_iface.write_uint8 = civic_lo_write8;
    cv->lo_iface.write_uint16 = civic_lo_write16;
    cv->lo_iface.write_uint32 = civic_lo_write32;
    memory_map_add(cfg->mem_map, 0x50036000u, 0x00002000u, "CIVIC", &cv->lo_iface, cv);
}

// ============================================================
// Lifecycle
// ============================================================

av_civic_t *av_civic_init(config_t *cfg, checkpoint_t *cp) {
    av_civic_t *cv = calloc(1, sizeof(*cv));
    if (!cv)
        return NULL;
    cv->cfg = cfg;
    cv->vram = calloc(1, AV_CIVIC_VRAM_SIZE);
    if (!cv->vram) {
        free(cv);
        return NULL;
    }

    if (cp) {
        size_t data_size = offsetof(av_civic_t, cfg);
        system_read_checkpoint_data(cp, cv, data_size);
        system_read_checkpoint_data(cp, cv->vram, AV_CIVIC_VRAM_SIZE);
    }

    civic_update_display(cv);

    scheduler_new_event_type(cfg->scheduler, "civic", cv, "frame", &civic_frame_event);
    scheduler_new_cpu_event(cfg->scheduler, &civic_frame_event, cv, 0, 0, (uint64_t)AV_CIVIC_FRAME_NS);

    LOG(1, "CIVIC init (Hi-Res 640x480 monitor, 2 MB VRAM)");
    return cv;
}

void av_civic_delete(av_civic_t *cv) {
    if (!cv)
        return;
    if (cv->cfg && cv->cfg->scheduler)
        remove_event(cv->cfg->scheduler, &civic_frame_event, cv);
    free(cv->vram);
    free(cv);
}

void av_civic_checkpoint(av_civic_t *cv, checkpoint_t *cp) {
    if (!cv || !cp)
        return;
    size_t data_size = offsetof(av_civic_t, cfg);
    system_write_checkpoint_data(cp, cv, data_size);
    system_write_checkpoint_data(cp, cv->vram, AV_CIVIC_VRAM_SIZE);
}

display_t *av_civic_display(av_civic_t *cv) {
    return cv ? &cv->display : NULL;
}
