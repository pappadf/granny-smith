// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platinum.c
// Platinum (343S1184) — the Power Macintosh 7200's ("Catalyst") memory and
// video controller, at $F8000000 where the 7500's Hammerhead sits, plus the
// DACula RAMDAC whose byte cells live in Grand Central at +$1B000.  One part
// controls the system bus, DRAM, ROM, L2 cache and the VRAM frame buffer
// (Apple, "Power Macintosh 7200 Computer" Developer Note, 1995, "Platinum
// Memory and Video Controller IC").
//
// Software shape:
//
//   * 128 x 32-bit registers on $10 centres, big-endian on the processor
//     bus (the Open Firmware node's `reg` is F8000000/800 — 128 x $10).
//     +$00 is the part identifier; the ROM's 68k identification picks the
//     Catalyst ProductInfo when its upper halfword reads $3001.
//   * The DRAM bank base registers, one per bank at +$60..+$D0 (8 banks:
//     four DIMM slots of up to two banks).  "Linear memory organization
//     only ... The base address for each bank is based on the sum of the
//     sizes of all the lower-numbered banks" (ibid., "Bank Base
//     Registers"); POST sizes each bank and writes those bases.
//   * The display: a timing set at reg 32..57 (horizontal in units of one
//     or two dot clocks, vertical in half-lines), the scan start as a
//     physical address (reg 16), the pitch (reg 18), the display-off
//     control (reg 24), the monitor sense lines (reg 23) and a status
//     register (reg 34) whose bit 2 is the vertical blank the video driver
//     spin-polls.
//   * VRAM: 1 MB soldered, 2 or 4 MB with expansion, linear at $F1000000
//     (a 16 MB aperture; the space past the fitted VRAM reads nothing back,
//     which is how drivers size it).
//   * DACula: addr (+$00), d1 (+$10), the indexed register port d2 (+$20,
//     index = addr) and the CLUT data port (+$30, R/G/B auto-advance).
//     Index $20 is the control register (bits 3:2 = 8/16/32 bpp, bit 1 the
//     hardware cursor, bit 4 the 64-bit VRAM organisation), $23 selects
//     which pixel-clock pair ($24/$25 or $27/$28) drives the dot clock,
//     $10/$11 the cursor X, $40 reads back the DACula type; d1 streams the
//     eight-entry cursor palette.
//   * The retrace interrupt is Grand Central line 30 (the node's
//     AAPL,interrupts), a level the driver acknowledges by reading reg 35.
//
// Register truth: the 7200 developer note; the OS driver corpus for this
// exact machine (Linux platinumfb, NetBSD platinumfb, OSF/Apple MkLinux
// video_platinum) for the register geometry, the DACula layout and the
// timing-set shape; the shipping ROM's own POST, Open Firmware and native
// video driver for the bank registers, the sense port and the status bit.

#include "regfile.h"
#include "tnt.h"

#include "display.h"
#include "log.h"
#include "page_fill.h"
#include "ppc.h"
#include "scheduler.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("platinum");

#define PLAT_VRAM_BASE     0xF1000000u // the frame-buffer aperture
#define PLAT_VRAM_APERTURE 0x01000000u // 16 MB decoded
#define PLAT_DECODE_TOP    0x40000000u // 1 GB of DRAM decode space

// Register indices (offset / $10).
#define PR_ID          0 // part identifier
#define PR_BANK0       6 // +$60: bank 0 base; banks 1..7 follow
#define PR_FB_BASE     16 // +$100: scan start (physical address)
#define PR_PITCH       18 // +$120: bytes per scan line
#define PR_MODE        19 // +$130
#define PR_VRAM_CFG    20 // +$140: VRAM bank configuration ($1000 = 64-bit banks)
#define PLAT_VRAM_WIDE 0x1000u
#define PR_SENSE       23 // +$170: monitor sense lines
#define PR_DISP_OFF    24 // +$180: nonzero = display off
#define PR_STATUS      34 // +$220: bit 2 = vertical blank
#define PR_VBL_ACK     35 // +$230: reading it acknowledges the retrace interrupt
#define PR_TIMING      32 // +$200: first of the 26-register timing set
#define PR_HTOTAL      44 // horizontal total - 1 (dot-clock units)
#define PR_HACT_ON     48 // horizontal display start
#define PR_HACT_OFF    49 // horizontal display end
#define PR_VTOTAL      51 // vertical total (half-lines)
#define PR_VACT_ON     55 // vertical display start (half-lines)
#define PR_VACT_OFF    56 // vertical display end (half-lines)

#define PLAT_STATUS_VBL 0x4u
#define PLAT_FB_OFF     16u

// The part identifier.  Upper halfword $3001: the 68k identification's
// Catalyst test (ROM $FFC14844 — "swap($F8000000) == $3001").
#define PLAT_ID 0x30010000u

// DACula
#define DAC_CTRL      0x20u // bits 3:2 depth, bit 1 hardware cursor
#define DAC_CURSOR_ON 0x02u
#define DAC_CLKSEL    0x23u // 2 = pair $24/$25 drives the clock, 3 = $27/$28
#define DAC_TYPE      0x40u // reads the DACula type
#define DAC_TYPE_NEW  0x84u // the "new" DACula (clock F = 15 MHz x c0 / (n+2) / 2^p)
#define DAC_CLOCK_REF 15000000u

#define PLAT_VBL_FALLBACK_NS (1000000000ull / 60u)
#define PLAT_VBL_MIN_NS      4000000ull
#define PLAT_VBL_MAX_NS      40000000ull

static tnt_platinum_t *plat(config_t *cfg) {
    return &tnt_st(cfg)->plat;
}

static uint32_t plat_vram_size(config_t *cfg) {
    uint32_t kb = tnt_board(cfg)->vram_kb;
    if (kb == 0 || kb * 1024u > TNT_VRAM_SIZE)
        kb = 1024u;
    return kb * 1024u;
}

// ============================================================
// DRAM
// ============================================================

// The DIMMs as banks: the largest power-of-two DIMM (4..128 MB per bank)
// that fits the remainder goes in the next slot, one bank per DIMM.
static void plat_bank_inventory(config_t *cfg) {
    tnt_platinum_t *p = plat(cfg);
    uint32_t rest = cfg->ram_size, off = 0;
    memset(p->bank_size, 0, sizeof(p->bank_size));
    memset(p->bank_host_off, 0, sizeof(p->bank_host_off));
    for (unsigned k = 0; k < TNT_PLAT_BANKS && rest >= 0x400000u; k++) {
        uint32_t size = 0x8000000u; // 128 MB, the largest bank
        while (size > rest)
            size >>= 1;
        p->bank_size[k] = size;
        p->bank_host_off[k] = off;
        off += size;
        rest -= size;
    }
    if (rest)
        LOG(0, "RAM size $%X does not decompose into banks; $%X unmapped", cfg->ram_size, rest);
}

// A bank base register holds the bank's base address itself, on a 4 MB
// grain: POST's sizing pass (ROM $FFF04540) writes k x $08000000 to bank k,
// probes each 128 MB window, then rewrites the bases as running sums of the
// sizes it found ($FFF045C0) — an empty bank takes the end of memory.
#define PLAT_BANK_BASE_MASK 0xFFC00000u
#define PLAT_BANK_WINDOW    0x08000000u // 128 MB: the largest bank, and POST's probe stride

static uint32_t plat_bank_base(const tnt_platinum_t *p, unsigned k) {
    return p->reg[PR_BANK0 + k] & PLAT_BANK_BASE_MASK;
}

static void plat_vram_map(config_t *cfg);

void tnt_plat_remap(config_t *cfg) {
    tnt_platinum_t *p = plat(cfg);
    plat_vram_map(cfg);
    uint8_t *ram = ram_native_pointer(cfg->memory_map, 0);
    for (uint32_t pg = 0; pg < (PLAT_DECODE_TOP >> MEM_PAGE_SHIFT); pg++)
        page_clear(pg);
    for (unsigned k = 0; k < TNT_PLAT_BANKS; k++) {
        uint32_t size = p->bank_size[k];
        if (!size)
            continue;
        uint32_t base = plat_bank_base(p, k);
        if (base >= PLAT_DECODE_TOP)
            continue;
        // A bank decodes its DIMM, aliased, from its base up to the next
        // bank's base or 128 MB — POST's probe walks the window for the
        // highest address that holds a pattern, so the DRAM must echo
        // through all of it and stop at the next bank.
        uint32_t window = PLAT_BANK_WINDOW;
        for (unsigned j = 0; j < TNT_PLAT_BANKS; j++) {
            uint32_t other = plat_bank_base(p, j);
            if (j != k && other > base && other - base < window)
                window = other - base;
        }
        if (window < size)
            window = size;
        if (base + window > PLAT_DECODE_TOP)
            window = PLAT_DECODE_TOP - base;
        uint8_t *host = ram + p->bank_host_off[k];
        for (uint32_t pg = 0; pg < (window >> MEM_PAGE_SHIFT); pg++)
            page_fill_identity_sv((base >> MEM_PAGE_SHIFT) + pg, host + ((pg << MEM_PAGE_SHIFT) % size), true);
        LOG(3, "bank %u: $%X bytes at $%08X", k, size, base);
    }
}

// ============================================================
// Display
// ============================================================

static uint32_t plat_bpp(const tnt_platinum_t *p) {
    switch ((p->dac_reg[DAC_CTRL] >> 2) & 3u) {
    case 1:
        return 16;
    case 2:
        return 32;
    default:
        return 8;
    }
}

static pixel_format_t plat_format(const tnt_platinum_t *p) {
    switch (plat_bpp(p)) {
    case 16:
        return PIXEL_16BPP_555;
    case 32:
        return PIXEL_32BPP_XRGB;
    default:
        return PIXEL_8BPP;
    }
}

// Dot clocks per horizontal timing unit: the largest of 1/2/4 whose active
// width still fits the programmed pitch (the 512-wide modes count single
// dots, the 640..1152 modes pairs, the 1280 modes quads).
static uint32_t plat_hunit(const tnt_platinum_t *p) {
    uint32_t on = p->reg[PR_HACT_ON], off = p->reg[PR_HACT_OFF];
    uint32_t bpx = plat_bpp(p) / 8u, pitch = p->reg[PR_PITCH];
    if (off <= on || !pitch)
        return 2;
    uint32_t units = off - on, u = 1;
    while (u < 4 && units * (u * 2) * bpx <= pitch)
        u *= 2;
    return u;
}

static void plat_refresh_clut(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_platinum_t *p = &st->plat;
    if (plat_bpp(p) > 8) {
        uint32_t shift = plat_bpp(p) == 16 ? 3u : 0u;
        for (uint32_t i = 0; i < 256; i++)
            for (int c = 0; c < 3; c++)
                st->dac_view[c][i] = p->clut[i >> shift][c];
        st->display.clut = NULL;
        st->display.clut_len = 0;
        st->display.dac_lut = st->dac_view;
        st->display.response_dirty = true;
        return;
    }
    if (st->display.dac_lut) {
        st->display.dac_lut = NULL;
        st->display.response_dirty = true;
    }
    for (uint32_t i = 0; i < 256; i++) {
        st->clut_view[i].r = p->clut[i][0];
        st->clut_view[i].g = p->clut[i][1];
        st->clut_view[i].b = p->clut[i][2];
        st->clut_view[i].a = 255;
    }
    st->display.clut = st->clut_view;
    st->display.clut_len = 256;
    st->display.clut_dirty = true;
}

// The DACula hardware cursor (control register bit 1): the same scheme as
// its RaDACal sibling on Control (control.c).  The image plane rides in the
// 16 bytes before each scan line's pixel 0 — the reason pixel 0 sits 16
// bytes past the programmed scan start — one row per line at the current
// pitch: 16 pixels at 4 bits, nibble bit 3 opaque, bits 2:0 an entry of the
// eight-colour cursor palette streamed through d1.  X is DACula registers
// $10 (high) / $11 (low), hotspot-biased by one; Y is implicit, the driver
// clears the old rows and draws the new ones.
static uint8_t plat_clut_match(const tnt_platinum_t *p, const uint8_t *rgb) {
    uint32_t best = 0, bestd = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < 256 && bestd; i++) {
        int dr = (int)p->clut[i][0] - rgb[0], dg = (int)p->clut[i][1] - rgb[1], db = (int)p->clut[i][2] - rgb[2];
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

static void plat_compose(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_platinum_t *p = &st->plat;
    if (!st->blank || !st->compose || !st->display.bits || st->display.bits == st->blank)
        return;
    if (!(p->dac_reg[DAC_CTRL] & DAC_CURSOR_ON))
        return; // update() already points at live VRAM
    uint32_t vsize = plat_vram_size(cfg);
    uint32_t bpx = plat_bpp(p) / 8u;
    uint32_t stride = st->display.stride, w = st->display.width, h = st->display.height;
    uint64_t base = (uint64_t)(st->display.bits - st->vram);
    if (st->display.bits == st->compose)
        base = (p->reg[PR_FB_BASE] + PLAT_FB_OFF - PLAT_VRAM_BASE) & (PLAT_VRAM_APERTURE - 1u);
    uint64_t span = (uint64_t)stride * h;
    if (base + span > vsize)
        return;
    memcpy(st->compose, st->vram + base, (size_t)span);
    uint32_t x = ((uint32_t)p->dac_reg[0x10] << 8 | p->dac_reg[0x11]) + 1u;
    for (uint32_t y = 0; y < h; y++) {
        uint64_t poff = base + (uint64_t)y * stride;
        if (poff < 16u || poff + 8u > vsize)
            continue;
        const uint8_t *row = st->vram + poff - 16u;
        uint8_t *line = st->compose + (size_t)y * stride;
        for (uint32_t i = 0; i < 16u; i++) {
            uint8_t nib = (i & 1u) ? (row[i >> 1] & 0x0Fu) : (row[i >> 1] >> 4);
            uint32_t px = x + i;
            if (!(nib & 0x8u) || px >= w || (uint64_t)(px + 1u) * bpx > stride)
                continue;
            const uint8_t *pal = p->crsr[nib & 7u];
            uint8_t *dst = line + (size_t)px * bpx;
            if (bpx == 1) {
                dst[0] = plat_clut_match(p, pal);
            } else if (bpx == 2) {
                uint16_t v = display_pack555(pal[0], pal[1], pal[2]);
                dst[0] = (uint8_t)(v >> 8);
                dst[1] = (uint8_t)v;
            } else {
                dst[0] = 0;
                dst[1] = pal[0];
                dst[2] = pal[1];
                dst[3] = pal[2];
            }
        }
    }
    st->display.bits = st->compose;
}

void tnt_plat_update(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_platinum_t *p = &st->plat;
    if (!st->blank)
        return;
    uint32_t bpp = plat_bpp(p);
    uint32_t pitch = p->reg[PR_PITCH];
    uint32_t hon = p->reg[PR_HACT_ON], hoff = p->reg[PR_HACT_OFF];
    uint32_t von = p->reg[PR_VACT_ON], voff = p->reg[PR_VACT_OFF];
    uint32_t width = (hoff > hon) ? (hoff - hon) * plat_hunit(p) : 640u;
    uint32_t height = (voff > von) ? (voff - von) / 2u : 480u;
    if (width == 0 || width > 2048u)
        width = 640u;
    if (height == 0 || height > 1536u)
        height = 480u;
    st->display.format = plat_format(p);
    st->display.par_w = 0;
    st->display.par_h = 0;
    st->display.crt_response = NULL;
    uint32_t stride = pitch ? pitch : width * (bpp / 8u);
    // Pixel 0 is 16 bytes past the programmed scan start (the drivers'
    // frame-buffer offset is reg 16 + $10).
    uint32_t base = (p->reg[PR_FB_BASE] + PLAT_FB_OFF - PLAT_VRAM_BASE) & (PLAT_VRAM_APERTURE - 1u);
    bool blanked = p->reg[PR_DISP_OFF] != 0 || pitch == 0;
    display_set_scanout(&st->display, blanked ? NULL : st->vram, plat_vram_size(cfg), base, stride, width, height,
                        st->blank, TNT_VRAM_SIZE);
    plat_refresh_clut(cfg);
    st->display.shape_dirty = true;
    st->display.fb_dirty = true;
    st->display.clut_dirty = true;
    plat_compose(cfg);
    LOG(2, "mode: %ux%u %ubpp stride=%u base=+$%X dac_ctrl=$%02X%s", width, height, bpp, stride, base,
        p->dac_reg[DAC_CTRL], st->display.bits == st->blank ? " BLANKED" : "");
}

struct display *tnt_plat_display(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st || !st->blank || !tnt_board(cfg)->has_platinum)
        return NULL;
    return &st->display;
}

void tnt_plat_host_vbl(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st || !st->blank || !tnt_board(cfg)->has_platinum)
        return;
    // Back to live VRAM, then the cursor composite over it (a frame's worth
    // of guest drawing bypasses the renderer either way).
    if (st->display.bits == st->compose)
        st->display.bits =
            st->vram + ((st->plat.reg[PR_FB_BASE] + PLAT_FB_OFF - PLAT_VRAM_BASE) & (PLAT_VRAM_APERTURE - 1u));
    plat_compose(cfg);
    st->display.fb_dirty = true;
}

// The programmed dot clock in Hz (the "new" DACula's formula), 0 while
// unprogrammed.
static uint64_t plat_dot_clock_hz(const tnt_platinum_t *p) {
    unsigned i = (p->dac_reg[DAC_CLKSEL] == 3u) ? 0x27u : 0x24u;
    uint32_t c0 = p->dac_reg[i], c1 = p->dac_reg[i + 1];
    if (!c0)
        return 0;
    return (uint64_t)DAC_CLOCK_REF * c0 / ((c1 & 0x1Fu) + 2u) / (1u << (c1 >> 5));
}

static uint64_t plat_frame_ns(config_t *cfg) {
    tnt_platinum_t *p = plat(cfg);
    uint64_t dot = plat_dot_clock_hz(p);
    uint64_t htotal = ((uint64_t)p->reg[PR_HTOTAL] + 1u) * plat_hunit(p);
    uint64_t vtotal = p->reg[PR_VTOTAL] / 2u;
    if (!dot || !p->reg[PR_HTOTAL] || !vtotal || p->reg[PR_DISP_OFF])
        return PLAT_VBL_FALLBACK_NS;
    uint64_t ns = htotal * vtotal * 1000000000ull / dot;
    if (ns < PLAT_VBL_MIN_NS || ns > PLAT_VBL_MAX_NS)
        return PLAT_VBL_FALLBACK_NS;
    return ns;
}

// The frame phase as a line number, against the programmed totals.
static bool plat_in_vblank(config_t *cfg) {
    tnt_platinum_t *p = plat(cfg);
    uint32_t vtotal = p->reg[PR_VTOTAL] / 2u;
    uint32_t von = p->reg[PR_VACT_ON] / 2u, voff = p->reg[PR_VACT_OFF] / 2u;
    if (!vtotal || vtotal > 4096u || voff <= von || voff > vtotal) {
        vtotal = 525u;
        von = 45u;
        voff = 525u;
    }
    uint64_t frame = cfg->machine->freq * plat_frame_ns(cfg) / 1000000000ull;
    if (!frame)
        return false;
    uint32_t line = (uint32_t)((scheduler_cpu_cycles(cfg->scheduler) % frame) * vtotal / frame);
    return line < von || line >= voff;
}

// ============================================================
// Retrace interrupt — Grand Central line 30
// ============================================================

#define PLAT_INT_VBL 30

// Every retrace raises line 30 as a LEVEL, held until the driver
// acknowledges it by reading reg 35 (+$230): the ROM's video driver's
// interrupt handler (the native routine the ndrv installs, decoded live at
// RAM $440F0 under System 7.5.3) is exactly two `lwz` of $F8000230 followed
// by its VBL service — which is where it applies the gamma and CLUT loads
// it defers.  The NanoKernel classifies a TNT interrupt from Levels & Mask,
// so an edge-only pulse never reaches the driver (System 7.6 then never
// loads its gamma table and the desktop shows as one flat grey).  The rate
// is the programmed raster's (dot clock over the totals), nominal 60 Hz
// until programmed.
static void plat_vbl_event(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    tnt_platinum_t *p = plat(cfg);
    if (!p->reg[PR_DISP_OFF]) {
        p->vbl_pending = 1;
        tnt_gc_set_source(cfg, PLAT_INT_VBL, true);
    }
    p->vbl_armed = 1;
    scheduler_new_cpu_event(cfg->scheduler, plat_vbl_event, cfg, 0, 0, plat_frame_ns(cfg));
}

// Start the retrace clock once; it runs for the machine's lifetime (a reset
// does not cancel a pending scheduler event, so the armed flag survives it).
static void plat_vbl_arm(config_t *cfg) {
    tnt_platinum_t *p = plat(cfg);
    if (p->vbl_armed)
        return;
    p->vbl_armed = 1;
    scheduler_new_cpu_event(cfg->scheduler, plat_vbl_event, cfg, 0, 0, plat_frame_ns(cfg));
}

// ============================================================
// Monitor sense (reg 23): bits 2/1/0 = lines A/B/C; writing 0 drives a line
// low, 1 releases it.  The read returns the lines INVERTED — a bit reads 1
// where the line is low (driven, or strapped to ground by the monitor):
// with every driver off, the ROM's video driver picks 640x870 portrait
// (Apple sense 1, A and B grounded) from a read of 6 — Linux's
// read_platinum_sense complements the read for the same reason.
// ============================================================

static uint32_t plat_sense_read(config_t *cfg) {
    tnt_platinum_t *p = plat(cfg);
    uint32_t levels = p->reg[PR_SENSE] & ~(uint32_t)p->mon_grounded & 7u;
    return (p->reg[PR_SENSE] & ~7u) | (~levels & 7u);
}

// ============================================================
// The register window
// ============================================================

static uint32_t plat_reg_read(config_t *cfg, unsigned idx) {
    tnt_platinum_t *p = plat(cfg);
    switch (idx) {
    case PR_SENSE:
        return plat_sense_read(cfg);
    case PR_STATUS:
        return (p->reg[idx] & ~PLAT_STATUS_VBL) | (plat_in_vblank(cfg) ? PLAT_STATUS_VBL : 0u);
    case PR_VBL_ACK:
        if (p->vbl_pending) {
            p->vbl_pending = 0;
            tnt_gc_set_source(cfg, PLAT_INT_VBL, false);
        }
        return p->reg[idx];
    default:
        return p->reg[idx];
    }
}

static void plat_reg_written(config_t *cfg, unsigned idx) {
    if (idx >= PR_BANK0 && idx < PR_BANK0 + TNT_PLAT_BANKS) {
        tnt_plat_remap(cfg);
        return;
    }
    switch (idx) {
    case PR_VRAM_CFG:
        plat_vram_map(cfg);
        break;
    case PR_FB_BASE:
    case PR_PITCH:
    case PR_DISP_OFF:
    case PR_HACT_ON:
    case PR_HACT_OFF:
    case PR_VACT_ON:
    case PR_VACT_OFF:
        tnt_plat_update(cfg);
        break;
    default:
        break;
    }
}

uint8_t tnt_plat_read(config_t *cfg, uint32_t offset) {
    if (offset >= TNT_PLAT_REGS * 0x10u) {
        LOG(2, "read beyond the register file: +$%03X", offset);
        return 0;
    }
    uint32_t lane = offset & 0xFu;
    uint32_t value = plat_reg_read(cfg, offset >> 4);
    uint8_t b = (lane < 4) ? be_lane8(value, lane) : 0;
    LOG(4, "read  +$%03X -> $%02X (reg $%08X) pc=$%08X", offset, b, value, ppc_get_pc(cfg->ppc));
    return b;
}

void tnt_plat_write(config_t *cfg, uint32_t offset, uint8_t value) {
    tnt_platinum_t *p = plat(cfg);
    if (offset >= TNT_PLAT_REGS * 0x10u || (offset & 0xFu) >= 4) {
        LOG(2, "write outside the register lanes: +$%03X = $%02X", offset, value);
        return;
    }
    unsigned idx = offset >> 4;
    be_lane8_set(&p->reg[idx], offset & 3u, value);
    if (idx == PR_ID)
        p->reg[PR_ID] = PLAT_ID;
    LOG(3, "write +$%03X = $%02X (reg now $%08X) pc=$%08X", offset, value, p->reg[idx], ppc_get_pc(cfg->ppc));
    // Act once the register's last lane lands (the bus writes longwords as
    // four byte lanes through the window's byte path).
    if ((offset & 3u) == 3u)
        plat_reg_written(cfg, idx);
}

// ============================================================
// DACula (Grand Central +$1B000)
// ============================================================

static uint8_t dac_read(config_t *cfg, uint32_t offset, bool peek) {
    tnt_platinum_t *p = plat(cfg);
    uint8_t comp, idx;
    switch (offset & 0x30u) {
    case 0x00:
        return ramdac_seq_addr(&p->dac);
    case 0x10: // the cursor palette, entry auto-advancing like the CLUT port
        p->crsr_seq.widx = p->crsr_seq.ridx = ramdac_seq_addr(&p->dac);
        idx = ramdac_seq_read(&p->crsr_seq, peek, &comp);
        p->dac.widx = p->dac.ridx = ramdac_seq_addr(&p->crsr_seq);
        return p->crsr[idx & 7u][comp];
    case 0x20: {
        uint8_t a = ramdac_seq_addr(&p->dac);
        if (a == DAC_TYPE)
            return DAC_TYPE_NEW;
        return p->dac_reg[a];
    }
    default:
        idx = ramdac_seq_read(&p->dac, peek, &comp);
        return p->clut[idx][comp];
    }
}

uint8_t tnt_plat_dac_read(config_t *cfg, uint32_t offset) {
    return dac_read(cfg, offset, false);
}

uint8_t tnt_plat_dac_peek(config_t *cfg, uint32_t offset) {
    return dac_read(cfg, offset, true);
}

void tnt_plat_dac_write(config_t *cfg, uint32_t offset, uint8_t value) {
    tnt_platinum_t *p = plat(cfg);
    uint8_t idx, a = ramdac_seq_addr(&p->dac);
    switch (offset & 0x30u) {
    case 0x00:
        ramdac_seq_set_waddr(&p->dac, value);
        ramdac_seq_set_waddr(&p->crsr_seq, value);
        break;
    case 0x10:
        p->crsr_seq.widx = p->crsr_seq.ridx = a;
        if (ramdac_seq_write(&p->crsr_seq, value, &idx))
            memcpy(p->crsr[idx & 7u], p->crsr_seq.pending, 3);
        p->dac.widx = p->dac.ridx = ramdac_seq_addr(&p->crsr_seq);
        break;
    case 0x20:
        LOG(2, "DACula reg[$%02X] = $%02X", a, value);
        p->dac_reg[a] = value;
        if (a == DAC_CTRL || a == 0x10u || a == 0x11u)
            tnt_plat_update(cfg);
        break;
    default:
        if (ramdac_seq_write(&p->dac, value, &idx)) {
            memcpy(p->clut[idx], p->dac.pending, 3);
            LOG(3, "CLUT[$%02X] = %02X %02X %02X pc=$%08X", idx, p->clut[idx][0], p->clut[idx][1], p->clut[idx][2],
                ppc_get_pc(cfg->ppc));
            plat_refresh_clut(cfg);
        }
        break;
    }
}

// ============================================================
// VRAM aperture past the fitted VRAM: reads nothing, drops writes
// ============================================================

static void vhole_log(void *ctx, uint32_t offset, bool write, uint32_t value) {
    config_t *cfg = (config_t *)ctx;
    LOG(2, "VRAM aperture +$%06X past the fitted VRAM: %s $%X pc=$%08X", offset + plat_vram_size(cfg),
        write ? "write" : "read", value, ppc_get_pc(cfg->ppc));
}
static uint8_t vhole_read8(void *ctx, uint32_t offset) {
    vhole_log(ctx, offset, false, 0);
    return 0;
}
static uint16_t vhole_read16(void *ctx, uint32_t offset) {
    vhole_log(ctx, offset, false, 0);
    return 0;
}
static uint32_t vhole_read32(void *ctx, uint32_t offset) {
    vhole_log(ctx, offset, false, 0);
    return 0;
}
static void vhole_write8(void *ctx, uint32_t offset, uint8_t value) {
    vhole_log(ctx, offset, true, value);
}
static void vhole_write16(void *ctx, uint32_t offset, uint16_t value) {
    vhole_log(ctx, offset, true, value);
}
static void vhole_write32(void *ctx, uint32_t offset, uint32_t value) {
    vhole_log(ctx, offset, true, value);
}

// ============================================================
// The half bank.  The soldered 1 MB is a 32-bit-wide half bank; under the
// 64-bit organisation (reg 20 bit 12, which every driver selects to size
// VRAM) it backs only the even longword of each doubleword.  The ROM's
// video driver writes two adjacent longwords at +0/+4 (and at +2 MB) and
// reads them back: +4 lost is "half bank, 1 MB" (driver state 984 = 0,
// 995 = 1), and it then programs the 32-bit organisation ($11) the 1 MB
// modes run in.  Without this the driver takes the soldered VRAM for a
// full 2 MB bank and sets 32 bpp modes the VRAM cannot hold.
// ============================================================

static bool plat_half_bank_wide(config_t *cfg) {
    return plat_vram_size(cfg) == 0x100000u && (plat(cfg)->reg[PR_VRAM_CFG] & PLAT_VRAM_WIDE);
}

static uint8_t vhalf_read8(void *ctx, uint32_t offset) {
    return (offset & 4u) ? 0 : tnt_st((config_t *)ctx)->vram[offset];
}
static uint16_t vhalf_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((vhalf_read8(ctx, offset) << 8) | vhalf_read8(ctx, offset + 1));
}
static uint32_t vhalf_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)vhalf_read16(ctx, offset) << 16) | vhalf_read16(ctx, offset + 2);
}
static void vhalf_write8(void *ctx, uint32_t offset, uint8_t value) {
    if (!(offset & 4u))
        tnt_st((config_t *)ctx)->vram[offset] = value;
}
static void vhalf_write16(void *ctx, uint32_t offset, uint16_t value) {
    vhalf_write8(ctx, offset, (uint8_t)(value >> 8));
    vhalf_write8(ctx, offset + 1, (uint8_t)value);
}
static void vhalf_write32(void *ctx, uint32_t offset, uint32_t value) {
    vhalf_write16(ctx, offset, (uint16_t)(value >> 16));
    vhalf_write16(ctx, offset + 2, (uint16_t)value);
}

// Put the fitted VRAM on the page table: straight, or through the half
// bank's even-longword view.
static void plat_vram_map(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st->vram)
        return;
    uint32_t vsize = plat_vram_size(cfg);
    bool half = plat_half_bank_wide(cfg);
    for (uint32_t pg = 0; pg < (vsize >> MEM_PAGE_SHIFT); pg++) {
        uint32_t p = (PLAT_VRAM_BASE >> MEM_PAGE_SHIFT) + pg;
        if (half)
            page_route_device(p, &st->plat_vhalf_if, cfg, PLAT_VRAM_BASE);
        else
            page_fill_identity_sv(p, st->vram + (pg << MEM_PAGE_SHIFT), true);
    }
    // The physical map moved under the MMU's caches.
    if (cfg->ppc)
        ppc_mmu_invalidate_all(cfg->ppc);
}

// ============================================================
// Lifecycle
// ============================================================

void tnt_plat_register_events(config_t *cfg) {
    scheduler_new_event_type(cfg->scheduler, "platinum", cfg, "vbl", plat_vbl_event);
}

void tnt_plat_reset(config_t *cfg) {
    tnt_platinum_t *p = plat(cfg);
    uint8_t mon = p->mon_grounded, armed = p->vbl_armed;
    uint32_t bsize[TNT_PLAT_BANKS], boff[TNT_PLAT_BANKS];
    memcpy(bsize, p->bank_size, sizeof bsize);
    memcpy(boff, p->bank_host_off, sizeof boff);
    memset(p, 0, sizeof(*p));
    p->mon_grounded = mon;
    p->vbl_armed = armed;
    memcpy(p->bank_size, bsize, sizeof bsize);
    memcpy(p->bank_host_off, boff, sizeof boff);
    p->reg[PR_ID] = PLAT_ID;
    p->reg[PR_SENSE] = 7u;
    p->reg[PR_DISP_OFF] = 7u;
    // Power-on: every bank at its sum-of-lower-banks base, the layout POST
    // re-establishes after sizing.
    uint32_t base = 0;
    for (unsigned k = 0; k < TNT_PLAT_BANKS; k++) {
        p->reg[PR_BANK0 + k] = base;
        base += p->bank_size[k];
    }
    ramdac_seq_init(&p->dac, 0);
    ramdac_seq_init(&p->crsr_seq, 0);
    plat_vbl_arm(cfg);
    tnt_plat_remap(cfg);
    tnt_plat_update(cfg);
}

static display_t *plat_fb_resolve(void *owner) {
    return tnt_plat_display((config_t *)owner);
}

static uint64_t plat_fb_base(void *owner) {
    config_t *cfg = (config_t *)owner;
    tnt_state_t *st = tnt_st(cfg);
    return display_guest_addr(&st->display, st->vram, plat_vram_size(cfg), PLAT_VRAM_BASE);
}

int tnt_plat_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    plat_bank_inventory(cfg);
    st->plat.mon_grounded = (uint8_t)(~cfg->build_opts.builtin_sense & 7u);
    st->vram = calloc(1, TNT_VRAM_SIZE);
    st->blank = calloc(1, TNT_VRAM_SIZE);
    st->compose = calloc(1, TNT_VRAM_SIZE);
    if (!st->vram || !st->blank || !st->compose) {
        LOG(0, "Error: out of memory allocating Platinum's frame buffers");
        free(st->vram);
        free(st->blank);
        free(st->compose);
        st->vram = st->blank = st->compose = NULL;
        return -1;
    }
    // The fitted VRAM on the page table (plat_vram_map, from the reset);
    // the rest of the aperture answers as empty sockets.
    uint32_t vsize = plat_vram_size(cfg);
    st->plat_vhalf_if.read_uint8 = vhalf_read8;
    st->plat_vhalf_if.read_uint16 = vhalf_read16;
    st->plat_vhalf_if.read_uint32 = vhalf_read32;
    st->plat_vhalf_if.write_uint8 = vhalf_write8;
    st->plat_vhalf_if.write_uint16 = vhalf_write16;
    st->plat_vhalf_if.write_uint32 = vhalf_write32;
    st->plat_vram_if.read_uint8 = vhole_read8;
    st->plat_vram_if.read_uint16 = vhole_read16;
    st->plat_vram_if.read_uint32 = vhole_read32;
    st->plat_vram_if.write_uint8 = vhole_write8;
    st->plat_vram_if.write_uint16 = vhole_write16;
    st->plat_vram_if.write_uint32 = vhole_write32;
    memory_map_add(cfg->memory_map, PLAT_VRAM_BASE + vsize, PLAT_VRAM_APERTURE - vsize, "Platinum VRAM (empty)",
                   &st->plat_vram_if, cfg);
    st->display.response_dirty = true;
    st->control_fb_node = (display_fb_node_t){.owner = cfg, .resolve = plat_fb_resolve, .base = plat_fb_base};
    st->control_video_node = display_attach_video_node(&st->control_fb_node, "Video (Platinum)");
    return 0;
}

void tnt_plat_teardown(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    display_detach_video_node(st->control_video_node);
    st->control_video_node = NULL;
    free(st->vram);
    st->vram = NULL;
    free(st->blank);
    st->blank = NULL;
    free(st->compose);
    st->compose = NULL;
}

// ============================================================
// The built-in port
// ============================================================

// The monitors the port can present, by passive sense code.
static const struct {
    const char *id, *monitor;
    uint8_t sense;
} plat_monitors[] = {
    {"hires",    "13in_rgb",      0x6u},
    {"twopage",  "21in_rgb",      0x0u},
    {"portrait", "15in_portrait", 0x1u},
    {"rubik",    "12in_rgb",      0x2u},
    {NULL,       NULL,            0   },
};

static bool plat_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i >= sizeof plat_monitors / sizeof plat_monitors[0] - 1)
        return false;
    *id = plat_monitors[i].id;
    *monitor = plat_monitors[i].monitor;
    return true;
}

static bool plat_monitor_sense(const char *id, uint8_t *out_sense) {
    for (size_t i = 0; id && plat_monitors[i].id; i++)
        if (strcmp(plat_monitors[i].id, id) == 0) {
            *out_sense = plat_monitors[i].sense;
            return true;
        }
    return false;
}

const builtin_video_desc_t tnt_platinum_video = {
    .detail = "Platinum",
    .monitor_at = plat_monitor_at,
    .monitor_sense = plat_monitor_sense,
    .default_monitor = "13in_rgb",
};
