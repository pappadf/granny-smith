// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// builtin_rbv_video.c
// Macintosh IIci / IIsi built-in video pseudo-card.  See builtin_rbv_video.h for
// the contract and docs/internals/machines/mdu/rbv.md for the RBV/video
// split.  Modelled on jmfb.c (CLUT + depth-switch video) but much smaller:
// the depth/monitor-sense register lives on the RBV chip, there is no slot
// register window, and the framebuffer is main RAM (the bottom of Bank A,
// handed over by the machine) rather than a buffer at nubus_slot_base(slot).

#include "builtin_rbv_video.h"

#include "card.h"
#include "checkpoint.h"
#include "display.h"
#include "display_timing.h" // the sense code -> raster table
#include "log.h"
#include "memory.h" // ram_native_pointer: the frame buffer in main RAM
#include "nubus.h"
#include "rbv.h"
#include "system_config.h"

#include <stddef.h> // offsetof — the checkpoint range
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

// The raster presented while video is halted (an unsupported or absent
// monitor): a black stub at the 13" RGB size, so consumers keep a canvas.
#define RBV_HALTED_SENSE 6

// === Per-card private state =================================================

// Field order IS the checkpoint format (the via_t / adb_t / asc_t idiom): the
// scalars go as ONE range ending at `display`.  A scalar added above that line
// is checkpointed automatically; a POINTER added above it restores a stale
// address, so the pointers and construction facts sit below the marker.
typedef struct {
    rgba8_t clut[256]; // 256-entry palette fed by the VDAC

    // VDAC (Bt450) write state: an address write resets the R/G/B counter;
    // three data writes load one entry, then the index auto-increments.
    uint8_t vdac_idx; // current CLUT write index
    uint8_t vdac_phase; // 0 = R, 1 = G, 2 = B
    uint8_t vdac_rgb[3]; // accumulated R/G/B for the in-progress entry
    uint8_t vdac_pix_mask; // pixel read mask (accept-and-log)
    bool video_off; // RvMonP RvVIDOff: the raster is blanked

    // --- Pointers and construction facts last; NOT in the range above ---
    // `display` leads them because it embeds `bits`/`clut` pointers of its own;
    // its scalar head is checkpointed separately as a display_head_t.
    display_t display;
    rbv_t *rbv; // RBV chip — set post-init by the machine (slot-0 IRQ)
    uint8_t *fb; // framebuffer: private until the machine hands over Bank A
    bool fb_external; // true if fb points at machine-owned memory (don't free)
    // RvMonP's RvVIDOff bit, and the black raster presented while it is set.
    // A separate buffer because the framebuffer is LIVE GUEST MEMORY on the
    // IIsi (the V8 DMAs main DRAM): blanking the screen must not write it.
    // Same reasoning as ariel.c's `blank`.
    uint8_t *blank;
    size_t blank_size; // bytes in `blank`: the raster at 8 bpp
    uint32_t screen_offset; // where the visible raster starts within `fb`
    // The raster the latched monitor sense selects (a power-up strap, so a
    // construction fact): its size, or `halted` when the chip decodes no
    // monitor from the code and drives no video at all.
    uint32_t width, height;
    bool halted;
} rbv_video_priv_t;

// The layout above is load-bearing.  If this fires, a member moved across the
// boundary: re-check what the checkpoint range now covers before updating it.
_Static_assert(offsetof(rbv_video_priv_t, display) < offsetof(rbv_video_priv_t, rbv),
               "RBV-video checkpoint range must end before the pointer block");

// VDAC register offsets (RBV's Bt450) — see HardwarePrivateEqu.a:927-931.
#define VDAC_WADDR 0x0 // vDACwAddReg — set CLUT index
#define VDAC_WDATA 0x4 // vDACwDataReg — R/G/B sequential
#define VDAC_PIXRD 0x8 // vDACPixRdMask
#define VDAC_RADDR 0xC // vDACrAddReg

// Map a depth code (RvMonP bits 0-2) to a packed pixel format.
static pixel_format_t depth_to_format(int depth_code) {
    switch (depth_code) {
    case 0:
        return PIXEL_1BPP_MSB;
    case 1:
        return PIXEL_2BPP_MSB;
    case 2:
        return PIXEL_4BPP_MSB;
    case 3:
    default:
        return PIXEL_8BPP;
    }
}

// Decode the latched sense code the way this RBV variant does: a code is live
// exactly when the card kind lists a monitor for it (the IIci decodes 001 and
// 110, the IIsi's V8 adds 010); every other code halts video.  The raster is
// the canonical one for the code.  NULL: halted.
static const display_timing_t *rbv_decode_sense(const nubus_card_kind_t *kind, uint8_t sense) {
    for (const nubus_monitor_t *m = kind ? kind->monitors : NULL; m && m->id; m++)
        if (m->sense_code == sense)
            return display_timing_for_sense(sense);
    return NULL;
}

// Re-derive the scanout from the current depth and the video-off bit.  One
// checked transition, the same helper every other producer uses: geometry and
// buffer are decided together, so a blanked screen cannot advertise a raster
// the stub cannot serve, and the framebuffer window is bounds-checked against
// the aperture rather than assumed to fit.
static void rbv_video_apply_scanout(rbv_video_priv_t *p) {
    uint32_t bpp = display_bpp(p->display.format);
    uint32_t stride = p->width * bpp / 8u;
    // A halted RBV drives no pixels, whatever RvVIDOff says.
    bool dark = p->video_off || p->halted;
    display_set_scanout(&p->display, dark ? NULL : p->fb, BUILTIN_RBV_VRAM_SIZE, p->screen_offset, stride, p->width,
                        p->height, p->blank, p->blank_size);
}

// Point display.clut at the slice of the 256-entry hardware CLUT the current
// depth actually addresses.
//
// The RBV feeds the VDAC eight index lines but a reduced-depth pixel only
// occupies the low `bpp` of them; the rest are driven HIGH.  So the active
// palette sits at the TOP of the table, and Mac OS programs it there —
// measured on a 7.0.1 boot of this machine:
//
//   2 bpp -> CLUT[252..255] = white / ltGray / dkGray / black
//   4 bpp -> CLUT[240..255] = the 16-colour palette (white, yellow $FCF610,
//                             purple $6800BC, brown $784B10, ... black)
//   8 bpp -> CLUT[0..255]
//
// i.e. base = 256 - (1 << bpp).  display.h defines `clut` as the "0/4/16/256-
// entry palette", so exposing just that window is the contract the renderer
// already expects (it indexes clut[pixel % clut_len]).  Without this the
// renderer read the untouched low entries, which after the boot ROM's
// gray-out all hold the same 50% gray — so a 2 or 4 bpp desktop scanned out
// as a uniform gray field even though the framebuffer and ScreenRow were
// correct.
static void rbv_video_apply_clut_window(rbv_video_priv_t *p) {
    uint32_t bpp = display_bpp(p->display.format);
    uint32_t len = 1u << bpp; // 2, 4, 16 or 256
    if (len > 256)
        len = 256;
    p->display.clut = p->clut + (256u - len);
    p->display.clut_len = len;
    p->display.clut_dirty = true;
}

// === Card vtable ============================================================

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp, const slot_opts_t *opts) {
    rbv_video_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;

    // The raster follows the monitor strapped on the sense lines -- the code
    // the RBV latches in RvMonP (the bus seats the same code the chip gets).
    uint8_t sense = opts ? opts->sense : MACHINE_SENSE_NONE;
    const display_timing_t *t = rbv_decode_sense(nubus_slot_kind(card->bus, card->slot), sense);
    p->halted = (t == NULL);
    if (!t)
        t = display_timing_for_sense(RBV_HALTED_SENSE);
    p->width = t->width;
    p->height = t->height;
    p->blank_size = (size_t)p->width * p->height; // the deepest depth, 8 bpp
    if (p->halted)
        LOG(1, "RBV video: sense %u decodes to no monitor; video halted", sense);
    else
        LOG(1, "RBV video: sense %u -> %s, %ux%u", sense, t->name, p->width, p->height);

    p->fb = calloc(1, BUILTIN_RBV_VRAM_SIZE);
    p->blank = calloc(1, p->blank_size);
    if (!p->fb || !p->blank) {
        free(p->fb);
        free(p->blank);
        free(p);
        return -1;
    }

    // Power-up state: 1 bpp, matching the RBV's RvMonP depth default.  The
    // boot ROM grays the screen at this depth before the OS picks 8 bpp.
    p->display.format = PIXEL_1BPP_MSB;
    p->screen_offset = BUILTIN_RBV_SCREEN_OFFSET;
    rbv_video_apply_scanout(p); // the sense's geometry, or the halted stub
    // Cold boot scans out black, not the white an all-zero 1 bpp buffer gives.
    display_blank_raster(&p->display);
    p->display.clut = p->clut; // narrowed to the active window below
    p->display.clut_len = 256;
    p->display.crt_response = NULL; // 13" RGB gamma is near-identity
    p->display.shape_dirty = true;
    p->display.clut_dirty = true;
    p->display.fb_dirty = true;
    p->display.response_dirty = true;

    // Seed a grayscale ramp so the canvas isn't blank before the OS programs
    // a palette via the VDAC (mirrors jmfb's init ramp).
    for (int i = 0; i < 256; i++) {
        p->clut[i].r = (uint8_t)i;
        p->clut[i].g = (uint8_t)i;
        p->clut[i].b = (uint8_t)i;
        p->clut[i].a = 255;
    }

    rbv_video_apply_clut_window(p);

    card->priv = p;

    // The IIci and IIsi scan out of main RAM: a board fact, so it is taken
    // here, before the card's checkpoint part is read -- a card that does not
    // own its buffer has no VRAM in its block.  A cold boot blanks the screen;
    // a restore's RAM image already holds it.
    const nubus_slot_decl_t *decl = nubus_slot_decl_get(card->bus, card->slot);
    if (decl && decl->fb_in_ram)
        builtin_rbv_video_set_framebuffer(card, ram_native_pointer(cfg->memory_map, 0), 0, /*blank*/ cp == NULL);
    return 0;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    rbv_video_priv_t *p = card->priv;
    if (!p)
        return;
    if (!p->fb_external)
        free(p->fb); // machine-owned (IIsi main-RAM) framebuffers are not ours to free
    free(p->blank);
    free(p);
    card->priv = NULL;
}

static void card_on_vbl(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    rbv_video_priv_t *p = card->priv;
    if (!p)
        return;
    // A halted RBV stops its sync outputs, so there is no vertical blanking.
    if (p->halted)
        return;
    // Assert the built-in-video vertical-blanking interrupt (RvIRQ0 = slot 0).
    // The boot ROM polls RvSInt bit 6 for this during video init, and the OS
    // VBL manager runs off it.  RBV clears the bit on RvSInt read, so each
    // VBL surfaces as a single pulse.
    if (p->rbv)
        rbv_assert_slot_irq(p->rbv, 0);
    // Mark the framebuffer dirty every VBL so the renderer re-uploads ongoing
    // Mac OS drawing (CPU writes to the framebuffer bypass the renderer).
    p->display.fb_dirty = true;
}

static display_t *card_display(nubus_card_t *card) {
    rbv_video_priv_t *p = card->priv;
    return p ? &p->display : NULL;
}

// Save/restore the card's own display state.
//
// The RBV *chip* registers ride along in rbv_checkpoint(), but nothing covered
// the card: VRAM contents, the active depth/format and stride, and the CLUT
// the VDAC has been fed.  Without them a restore re-ran card_init and came up
// on a freshly zeroed framebuffer at the 1 bpp power-up default, i.e. a blank
// white screen that never repainted.
//
// The dirty flags are forced on restore rather than saved: the frontend has
// just been handed a different buffer and must re-upload everything once.
static void card_checkpoint_save(nubus_card_t *card, checkpoint_t *cp) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    // A machine-owned framebuffer (the IIsi's, which lives in main RAM) is
    // already covered by the RAM image; only a card-owned buffer needs saving.
    if (!p->fb_external)
        system_write_checkpoint_data(cp, p->fb, BUILTIN_RBV_VRAM_SIZE);
    system_write_checkpoint_data(cp, p, offsetof(rbv_video_priv_t, display));
    {
        // Fixed widths, not a raw struct prefix: the prefix carried a bare
        // pixel_format_t, whose size is implementation-defined (see
        // display.h).
        display_head_t head = display_head_of(&p->display);
        system_write_checkpoint_data(cp, &head, sizeof head);
    }
}

static void card_checkpoint_restore(nubus_card_t *card, checkpoint_t *cp) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    if (!p->fb_external)
        system_read_checkpoint_data(cp, p->fb, BUILTIN_RBV_VRAM_SIZE);
    system_read_checkpoint_data(cp, p, offsetof(rbv_video_priv_t, display));
    {
        display_head_t head;
        system_read_checkpoint_data(cp, &head, sizeof head);
        display_head_apply(&p->display, &head);
    }

    // The CLUT window depends on the restored depth, so recompute it.
    rbv_video_apply_clut_window(p);

    // p->display.bits still points into p->fb (card_init set it up and the
    // buffer address has not moved), but everything the frontend caches about
    // this display is now stale.
    p->display.shape_dirty = true;
    p->display.clut_dirty = true;
    p->display.fb_dirty = true;
    p->display.response_dirty = true;
}

static const nubus_card_ops_t builtin_rbv_video_ops = {
    .init = card_init,
    .teardown = card_teardown,
    .on_vbl = card_on_vbl,
    .display = card_display,
    .checkpoint_save = card_checkpoint_save,
    .checkpoint_restore = card_checkpoint_restore,
};

// === Machine-facing hooks ===================================================

void builtin_rbv_video_set_framebuffer(nubus_card_t *card, uint8_t *aperture, uint32_t screen_offset, bool blank) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p || !aperture)
        return;
    // The IIci and IIsi read their framebuffer directly out of main DRAM (the
    // RBV / V8 DMA Bank A starting at physical 0).  Point the card's framebuffer at the
    // machine-supplied aperture (a window into main RAM) instead of the private
    // buffer, so the renderer and the guest's screen writes share the same
    // storage.  `screen_offset` locates the active screen within the aperture.
    if (!p->fb_external)
        free(p->fb);
    p->fb = aperture;
    p->fb_external = true;
    p->screen_offset = screen_offset;
    rbv_video_apply_scanout(p);
    // The private buffer card_init blanked has just been thrown away, so blank
    // the visible window of the aperture too — otherwise the IIsi cold-boots to
    // a white screen (zeroed DRAM is white at 1 bpp) while every other machine
    // comes up black.  Only the screen the renderer scans out is touched, and
    // only before the guest has run; the ROM's RAM test writes and reads back
    // its own patterns over this either way.  A checkpoint restore skips it:
    // the RAM image it has loaded (or is about to) holds the live screen.
    if (blank)
        display_blank_raster(&p->display);
    p->display.fb_dirty = true;
}

// RvMonP bit 6 (RvVIDOff).  The RBV decoded this bit, logged it and threw it
// away, so the descriptor kept scanning out the framebuffer while the guest
// believed video was off -- every sibling chip honours its blanking bit
// (ariel.c `vid_mode & 0x80`, control.c `CR_CTRL & 0x400`, mach64gx.c
// `CRTC_EN`/`CRTC_DISPLAY_DIS`, civic.c `SLOT_ENABLE`) and RBV was the only
// one that did not.  The visible effect is the mode-change
// flicker a real IIci shows during a depth switch: guest code that
// blanks-then-reprograms was visible mid-transition.
void builtin_rbv_video_set_blank(nubus_card_t *card, bool video_off) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p || p->video_off == video_off)
        return;
    p->video_off = video_off;
    rbv_video_apply_scanout(p);
    p->display.shape_dirty = true;
    p->display.fb_dirty = true;
    LOG(2, "RBV video: video %s", video_off ? "off (blanked)" : "on");
}

void builtin_rbv_video_set_rbv(nubus_card_t *card, rbv_t *rbv) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (p)
        p->rbv = rbv;
}

void builtin_rbv_video_set_depth(nubus_card_t *card, int depth_code) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    pixel_format_t f = depth_to_format(depth_code);
    if (p->display.format == f)
        return;
    // A depth change before anything has been drawn (the IIsi picks 8 bpp
    // during machine init) would leave the power-on blank showing as white.
    bool pristine = display_raster_is_pristine(&p->display);
    p->display.format = f;
    rbv_video_apply_scanout(p); // stride follows the depth; re-fills the stub if blanked
    if (pristine && !p->video_off && !p->halted)
        display_blank_raster(&p->display);
    rbv_video_apply_clut_window(p);
    p->display.shape_dirty = true;
    p->display.fb_dirty = true;
    LOG(2, "RBV video: depth -> %u bpp (stride %u)", display_bpp(f), p->display.stride);
}

void builtin_rbv_video_vdac_write(nubus_card_t *card, uint32_t off, uint8_t val) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    switch (off & 0xF) {
    case VDAC_WADDR:
        // Set the CLUT write index; reset the R/G/B sub-counter.
        p->vdac_idx = val;
        p->vdac_phase = 0;
        return;
    case VDAC_WDATA:
        // Accumulate R, then G, then B; commit the entry on the third write
        // and auto-increment the index (Bt450 run-write protocol).
        if (p->vdac_phase < 3)
            p->vdac_rgb[p->vdac_phase] = val;
        p->vdac_phase++;
        if (p->vdac_phase >= 3) {
            p->clut[p->vdac_idx].r = p->vdac_rgb[0];
            p->clut[p->vdac_idx].g = p->vdac_rgb[1];
            p->clut[p->vdac_idx].b = p->vdac_rgb[2];
            p->clut[p->vdac_idx].a = 255;
            p->vdac_idx++;
            p->vdac_phase = 0;
            p->display.clut_dirty = true;
        }
        return;
    case VDAC_PIXRD:
        p->vdac_pix_mask = val;
        return;
    case VDAC_RADDR:
        // Read-address register write: resets the read sub-counter (we don't
        // model CLUT readback beyond the address latch).
        p->vdac_idx = val;
        p->vdac_phase = 0;
        return;
    default:
        LOG(2, "RBV video: VDAC write at +%X = $%02X (unmodeled)", off, val);
        return;
    }
}

// A VDAC register read; a data read steps the R/G/B phase unless it is an
// inspection (`peek`), which reports the component the next read returns.
static uint8_t vdac_read(nubus_card_t *card, uint32_t off, bool peek) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return 0xFF;
    switch (off & 0xF) {
    case VDAC_PIXRD:
        return p->vdac_pix_mask;
    case VDAC_WADDR:
    case VDAC_RADDR:
        return p->vdac_idx;
    case VDAC_WDATA: {
        // Return the current entry's components in R/G/B sequence.
        uint8_t v = (p->vdac_phase < 3) ? ((const uint8_t *)&p->clut[p->vdac_idx])[p->vdac_phase] : 0;
        if (!peek)
            p->vdac_phase = (uint8_t)((p->vdac_phase + 1) % 3);
        return v;
    }
    default:
        return 0;
    }
}

uint8_t builtin_rbv_video_vdac_read(nubus_card_t *card, uint32_t off) {
    return vdac_read(card, off, false);
}

uint8_t builtin_rbv_video_vdac_peek(nubus_card_t *card, uint32_t off) {
    return vdac_read(card, off, true);
}

// === Factory + kind descriptor ==============================================

// The monitors each variant decodes from its sense lines (docs/reference/
// machines/mdu/rbv.md §3.5) -- for catalog.profile, and the decode table
// rbv_decode_sense reads: a code with no row here halts video.  The sizes
// are display_timings[]' for each code.  1/2/4/8 bpp, except that the 15"
// Portrait has no 8 bpp mode (Guide to the Macintosh Family Hardware 2e,
// Table 12-3).
static const int builtin_rbv_depths[] = {1, 2, 4, 8, 0};
static const int builtin_rbv_portrait_depths[] = {1, 2, 4, 0};

// The 13" RGB (110; the default, so listed first) and the 15" B&W Portrait
// (001), which both variants decode.
#define RBV_MONITOR_13IN_RGB                                                                                           \
    {.id = "13in_rgb",                                                                                                 \
     .monitor = "13in_rgb",                                                                                            \
     .width = 640,                                                                                                     \
     .height = 480,                                                                                                    \
     .depths = builtin_rbv_depths,                                                                                     \
     .sense_code = 6}
#define RBV_MONITOR_15IN_PORTRAIT                                                                                      \
    {.id = "15in_portrait",                                                                                            \
     .monitor = "15in_portrait",                                                                                       \
     .width = 640,                                                                                                     \
     .height = 870,                                                                                                    \
     .depths = builtin_rbv_portrait_depths,                                                                            \
     .sense_code = 1}

// IIci RBV: 001 and 110; 010 and 101 are reserved, the rest unsupported.
static const nubus_monitor_t builtin_rbv_monitors[] = {
    RBV_MONITOR_13IN_RGB,
    RBV_MONITOR_15IN_PORTRAIT,
    {0},
};

// IIsi: adds 010, the 12" RGB at 512 x 384 (IIsi Developer Note, Table 4-2).
static const nubus_monitor_t builtin_rbv_iisi_monitors[] = {
    RBV_MONITOR_13IN_RGB,
    RBV_MONITOR_15IN_PORTRAIT,
    {.id = "12in_rgb",
      .monitor = "12in_rgb",
      .width = 512,
      .height = 384,
      .depths = builtin_rbv_depths,
      .sense_code = 2},
    {0},
};

const nubus_card_kind_t builtin_rbv_video_kind = {
    .id = "builtin_rbv_video",
    .display_name = "Built-in video",
    .attach = CARD_ATTACH_BUILTIN, // motherboard circuitry — never socketed
    .requires_vrom = false,
    .monitors = builtin_rbv_monitors,
    .ops = &builtin_rbv_video_ops,
};

// The IIsi's: the same card, decoding one more monitor.
const nubus_card_kind_t builtin_rbv_iisi_video_kind = {
    .id = "builtin_rbv_iisi_video",
    .display_name = "Built-in video",
    .attach = CARD_ATTACH_BUILTIN, // motherboard circuitry — never socketed
    .requires_vrom = false,
    .monitors = builtin_rbv_iisi_monitors,
    .ops = &builtin_rbv_video_ops,
};
