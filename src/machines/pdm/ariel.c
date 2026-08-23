// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ariel.c
// PDM onboard video: the Sonora-model control registers AMIC implements at
// $50F28000 (mode/depth/sense), the Ariel II CLUT/DAC at $50F24000, and the
// scanout presentation over the framebuffer in ordinary DRAM.
//
// There is no VRAM and no scan-base register on this board: HMCMerge forces
// the framebuffer allocation to physical 0 ("HMC Requirement") and the
// HMC/AMIC fetch engine reads the low bank directly, so the display
// descriptor's `bits` points at host RAM offset 0 — always inside the
// soldered bank, which the HMC never relocates, so the window is
// host-contiguous on every model.  Geometry derives entirely from the mode
// and depth registers; the CLUT is the Ariel register file amic.c already
// dispatches here.
//
// Register truth: Apple, "Power Macintosh Computers" Developer Note (1994)
// §2 "Data Path Chips"/Table 3-10 and the shipping ROM's Sonora driver
// (`.Display_Video_Apple_Sonora`), whose access idioms (blank-during-
// mode-change, WaitVSync-gated CLUT streams, the V8-convention reduced-depth
// palette window) this model serves.

#include "pdm.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("ariel");

// Monitor sense lines (the HDI-45 carries three open-collector lines A/B/C
// with 10k pull-ups; a dumb monitor hard-wires a subset to ground and the
// readback nibble reflects wired-AND(drive, strap)).  The default monitor
// is the 14" AppleColor Hi-Res: sense code 6 = A,B floating high, C
// grounded.  SonoraVdSenseRg drive nibble: bit n = 0 drives line n low, 1
// releases it ($07 = tristate); readback bits 6:4 = lines A,B,C.
//
// The strap is per-machine (pdm.h), so the built-in port can also be left
// UNCONNECTED — code 7 grounds nothing, and the ROM's extended-sense walk
// then reads all-ones and turns built-in video off entirely.
#define PDM_MONITOR_SENSE_DEFAULT 0x6u // A=1, B=1, C=0: Hi-Res 13"/14"

// The straps this model can present.  Restricted to the eight 3-bit codes
// on purpose: the monitors Apple distinguished with the EXTENDED sense walk
// (VGA, GoldFish) need per-line strapping this model does not carry.
const pdm_monitor_kind_t pdm_monitors[] = {
    {"hires",    "AppleColor Hi-Res RGB 13\"/14\" (640x480)", 0x6u          },
    {"portrait", "Macintosh Portrait Display (640x870)",      0x1u          },
    {"rubik",    "Macintosh 12\" RGB (512x384)",              0x2u          },
    {"none",     "No monitor connected",                      PDM_SENSE_NONE},
    {NULL,       NULL,                                        0             },
};

// Strap staged by machine.boot's `monitor=` and consumed by the next
// pdm_video_init (the jmfb_pending_sense_set shape).  Reset to the default
// on consumption so a forgotten setting cannot leak into the next machine.
static uint8_t s_pending_sense = PDM_MONITOR_SENSE_DEFAULT;

void pdm_pending_monitor_set(uint8_t sense) {
    s_pending_sense = (uint8_t)(sense & 0x07u);
}

// Look a strap up by config token; NULL when the name is not one of ours.
const pdm_monitor_kind_t *pdm_monitor_lookup(const char *id) {
    if (!id || !*id)
        return NULL;
    for (const pdm_monitor_kind_t *m = pdm_monitors; m->id; m++)
        if (strcmp(m->id, id) == 0)
            return m;
    return NULL;
}

// Largest raster any reachable mode scans out (Hi-Res/VGA at 16 bpp:
// 640 x 480 x 2 bytes); sizes the blank buffer.
#define PDM_VIDEO_MAX_BYTES (640u * 480u * 2u)

// Timing-set geometry per monitor code (Developer Note Table 3-10).  Codes
// the PDM sense walk can never select (Vail-only 10/13) are still decoded —
// the register accepts any code and the raster geometry is what the code
// means everywhere the Sonora model appears.
static bool pdm_mode_geometry(uint8_t code, uint32_t *w, uint32_t *h) {
    switch (code & 0x1Fu) {
    case 1: // Portrait 640x870 75 Hz
        *w = 640;
        *h = 870;
        return true;
    case 2: // Rubik 12" 512x384 60.15 Hz
        *w = 512;
        *h = 384;
        return true;
    case 6: // Hi-Res 13"/14" 640x480 66.67 Hz
        *w = 640;
        *h = 480;
        return true;
    case 9: // GoldFish 16" 832x624 74.55 Hz
        *w = 832;
        *h = 624;
        return true;
    case 11: // VGA 640x480 59.94 Hz
        *w = 640;
        *h = 480;
        return true;
    case 13: // Hi-Res-400 640x400 (Vail timing set)
        *w = 640;
        *h = 400;
        return true;
    default: // $1F "none" and unobserved codes: no timing set selected
        return false;
    }
}

// Sonora depth codes 0..4 (the zero-based mode index the driver writes).
static pixel_format_t pdm_depth_format(uint8_t code) {
    switch (code & 0x07u) {
    case 0:
        return PIXEL_1BPP_MSB;
    case 1:
        return PIXEL_2BPP_MSB;
    case 2:
        return PIXEL_4BPP_MSB;
    case 4:
        return PIXEL_16BPP_555;
    case 3:
    default:
        return PIXEL_8BPP;
    }
}

static uint32_t format_bpp(pixel_format_t f) {
    switch (f) {
    case PIXEL_1BPP_MSB:
        return 1;
    case PIXEL_2BPP_MSB:
        return 2;
    case PIXEL_4BPP_MSB:
        return 4;
    case PIXEL_16BPP_555:
        return 16;
    case PIXEL_8BPP:
    default:
        return 8;
    }
}

// Rebuild the depth-windowed palette view.  At reduced depth the hardware
// feeds the DAC eight index lines with the unused low lines driven HIGH, so
// a pixel value i reads CLUT entry (skip-1) + i*skip with skip =
// 256/2^depth — the V8/Sonora convention the driver programs ($7F white /
// $FF black at 1 bpp).  display.h's renderer indexes clut[pixel], so the
// view materializes that window; 8 bpp is the identity.
static void pdm_video_refresh_clut(pdm_state_t *st) {
    pdm_amic_t *a = &st->amic;
    pdm_video_t *v = &st->video;
    uint32_t bpp = format_bpp(v->display.format);
    if (bpp > 8)
        return; // direct format: CLUT bypassed
    uint32_t len = 1u << bpp;
    uint32_t skip = 256u >> bpp;
    for (uint32_t i = 0; i < len; i++) {
        const uint8_t *e = a->clut[(skip - 1) + i * skip];
        v->clut_view[i].r = e[0];
        v->clut_view[i].g = e[1];
        v->clut_view[i].b = e[2];
        v->clut_view[i].a = 255;
    }
    v->display.clut = v->clut_view;
    v->display.clut_len = len;
    v->display.clut_dirty = true;
}

// Re-derive the whole descriptor from the register file.  Called on init,
// reset, checkpoint restore and every mode/depth register write — all rare,
// so this recomputes coarsely and marks everything dirty.
void pdm_video_update(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    pdm_video_t *v = &st->video;
    if (!v->blank)
        return; // registers poked before pdm_video_init (machine bring-up)
    pdm_amic_t *a = &st->amic;

    uint32_t w = 640, h = 480;
    bool timed = pdm_mode_geometry(a->vid_mode, &w, &h);
    pixel_format_t f = pdm_depth_format(a->vid_depth);
    v->display.width = w;
    v->display.height = h;
    v->display.format = f;
    v->display.stride = w * format_bpp(f) / 8u;
    v->display.par_w = 0;
    v->display.par_h = 0;
    v->display.crt_response = NULL;

    // The blank bit stops syncs; the monitor shows black.  Presenting a
    // black stub keeps guest RAM untouched (the real framebuffer bytes are
    // live guest memory — blanking must never write them).  A mode code
    // with no timing set behaves the same: nothing is being scanned.
    if ((a->vid_mode & 0x80u) || !timed) {
        size_t n = (size_t)v->display.stride * h;
        if (n > PDM_VIDEO_MAX_BYTES)
            n = PDM_VIDEO_MAX_BYTES;
        memset(v->blank, display_black_fill(f), n);
        v->display.bits = v->blank;
    } else {
        // The scan base is selected by HMC serial-config bit 33 (there is
        // no framebuffer-base register): set = physical 0, the ROM's and
        // Mac OS's constant state; clear = $100000, the base MkLinux
        // (VPDM_PHYSADDR) and Copland program, both of which keep their
        // vector page at physical 0.  Both windows sit inside the 8 MB
        // soldered bank, which the HMC never relocates.
        uint32_t base = (st->hmc.cfg_hi & 0x2u) ? 0u : 0x100000u;
        v->display.bits = ram_native_pointer(cfg->mem_map, base);
    }

    if (f == PIXEL_16BPP_555) {
        v->display.clut = NULL;
        v->display.clut_len = 0;
    } else {
        pdm_video_refresh_clut(st);
    }
    v->display.shape_dirty = true;
    v->display.fb_dirty = true;
    v->display.clut_dirty = true;
}

void pdm_video_init(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    st->video.sense = s_pending_sense; // what machine.boot's monitor= staged
    s_pending_sense = PDM_MONITOR_SENSE_DEFAULT;
    st->video.blank = calloc(1, PDM_VIDEO_MAX_BYTES);
    pdm_video_update(cfg);
    st->video.display.response_dirty = true;
}

void pdm_video_teardown(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    free(st->video.blank);
    st->video.blank = NULL;
}

display_t *pdm_video_display(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    if (!st || !st->video.blank)
        return NULL;
    // Nothing plugged into the HDI-45: the machine has no built-in screen
    // to present.  Returning NULL is what lets system_display() fall
    // through to the NuBus primary display, so a seated card becomes the
    // only screen — matching the ROM, which on this strap prunes its own
    // video sResources and never allocates the DRAM framebuffer.
    if (st->video.sense == PDM_SENSE_NONE)
        return NULL;
    return &st->video.display;
}

// Strap a monitor (or nothing) onto the built-in port.  Called by the
// machine builder before the first pdm_video_update; changing it while the
// guest runs would not match hardware, where the ROM samples the lines once
// at startup.
void pdm_video_set_sense(config_t *cfg, uint8_t sense) {
    pdm_state_t *st = pdm_st(cfg);
    if (!st)
        return;
    st->video.sense = (uint8_t)(sense & 0x07u);
    if (st->video.blank)
        pdm_video_update(cfg);
}

uint8_t pdm_video_sense(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    return st ? st->video.sense : 0u;
}

// Every VBL the framebuffer may have been drawn into by the guest (CPU
// writes bypass the renderer), so mark it for re-upload.
void pdm_video_vbl(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    if (st && st->video.blank)
        st->video.display.fb_dirty = true;
}

// ============================================================
// Video control ($50F28000)
// ============================================================

uint8_t pdm_video_ctl_read(config_t *cfg, uint32_t off) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    switch (off) {
    case 0:
        return a->vid_mode;
    case 1:
        return a->vid_depth;
    case 2: {
        // Open-collector wired-AND: a line reads low when the host drives
        // it low OR the monitor straps it to ground; high otherwise.
        uint8_t lines = (uint8_t)(a->vid_sense & 0x07u & pdm_st(cfg)->video.sense);
        return (uint8_t)((a->vid_sense & 0x0Fu) | (lines << 4));
    }
    case 3:
        return a->vid_test;
    case 4:
    case 5:
    case 6:
    case 7:
        return 0; // beam counters: static (nothing reads them on PDM)
    default:
        return 0;
    }
}

void pdm_video_ctl_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    switch (off) {
    case 0:
        a->vid_mode = value;
        LOG(2, "video mode = $%02X", value);
        pdm_video_update(cfg);
        break;
    case 1:
        a->vid_depth = value;
        LOG(2, "video depth = $%02X", value);
        pdm_video_update(cfg);
        break;
    case 2:
        a->vid_sense = value;
        break;
    case 3:
        a->vid_test = value;
        break;
    default:
        break;
    }
}

// ============================================================
// Ariel II CLUT/DAC ($50F24000)
// ============================================================

uint8_t pdm_ariel_read(config_t *cfg, uint32_t off) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    switch (off & 3) {
    case 0:
        return a->clut_addr;
    case 1: { // data reads auto-advance the RGB phase / address
        uint8_t v = a->clut[a->clut_addr][a->clut_phase];
        if (++a->clut_phase == 3) {
            a->clut_phase = 0;
            a->clut_addr++;
        }
        return v;
    }
    case 2:
        return (uint8_t)(a->clut_ctrl & 0x7Fu);
    default:
        return a->clut_key;
    }
}

void pdm_ariel_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_state_t *st = pdm_st(cfg);
    pdm_amic_t *a = &st->amic;
    switch (off & 3) {
    case 0:
        a->clut_addr = value;
        a->clut_phase = 0; // address write resets the RGB byte index
        break;
    case 1:
        a->clut[a->clut_addr][a->clut_phase] = value;
        if (++a->clut_phase == 3) {
            a->clut_phase = 0;
            a->clut_addr++;
            if (st->video.blank)
                pdm_video_refresh_clut(st);
        }
        break;
    case 2:
        // Low 3 bits mirror the depth code; the driver always programs
        // VdColrReg in the same blanking window, which is where the
        // presentation follows the depth from.  Bit 3 = "master mode".
        a->clut_ctrl = value;
        break;
    default:
        a->clut_key = value;
        break;
    }
}
