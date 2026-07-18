// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// display_card_824gc.c
// "Apple Macintosh Display Card 8•24 GC" ("Dolphin") — HLE ("option B").  See
// display_card_824gc.h, the proposal
// local/gs-docs/proposals/proposal-8-24gc-hle-acceleration.md, and the dossier
// under local/gs-docs/8-24GC/ (protocol §§3-9/14, driver §§2-6, kernel §§1/7).
//
// Scope (this file): the CARD SHELL — the card presents the genuine v1.1
// declaration ROM (BoardId $2C), its JMFB-family display half boots a desktop,
// and the accelerator bring-up state machine makes the `.GraphAccel` driver
// believe a live Am29000 card booted (SRAM/DRAM windows, alive registers, the
// ACEFload byte sink, the boot handshake, CB arming, the RPC doorbell
// transport, the per-VBL heartbeat).  The drawing work itself — the
// DrawMultiObject interpreter, the rasterizers, the func $15 blit engine and
// the text machinery — is the "GC QuickDraw" engine in
// display_card_824gc_qd.c, reached through the gc824_* entry points in
// display_card_824gc_priv.h.  Anything outside the engine's accept envelope
// *declines* (proposal §4 safety net): QuickDraw's own ROM path renders it,
// so the desktop stays pixel-correct.  gc.force_decline forces every drawing
// func down the ROM path — the differential test oracle.
//
// The firmware bytes are *stored, not executed*: SRAM/DRAM simply accept the
// driver's ACEFload writes into their backing buffers, which automatically
// satisfies the driver's own read-backs (the PublicOu signature, version
// words) — the HLE never runs the Am29000 code.

#include "display_card_824gc.h"

#include "card.h"
#include "checkpoint.h"
#include "declrom.h"
#include "display.h"
#include "log.h"
#include "memory.h"
#include "nubus.h"
#include "rtc.h"
#include "system.h"
#include "system_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display_card_824gc_priv.h"

LOG_USE_CATEGORY_NAME("gc824");

static pixel_format_t format_for_bpp(int bpp); // fwd (video-mode section)

// === Display-format helpers (ported from jmfb.c) ============================
static uint32_t format_bpp(pixel_format_t f) {
    switch (f) {
    case PIXEL_1BPP_MSB:
        return 1;
    case PIXEL_2BPP_MSB:
        return 2;
    case PIXEL_4BPP_MSB:
        return 4;
    case PIXEL_8BPP:
        return 8;
    case PIXEL_16BPP_555:
        return 16;
    case PIXEL_32BPP_XRGB:
        return 32;
    default:
        return 8;
    }
}

// Map the ≤8 bpp depth field in CLUTPBCR (bits 3-4) to a pixel_format_t.
static pixel_format_t depth_to_format(uint16_t pbcr) {
    switch ((pbcr >> 3) & 0x3) {
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

// Recompute stride + width from RowWords + current format (jmfb convention).
static void recompute_stride(display_card_824gc_priv_t *p) {
    if (p->jmfb_row_words == 0)
        return; // chip-reset sentinel; preserve last good stride+width
    if (p->display.format == PIXEL_32BPP_XRGB) {
        p->display.stride = (uint32_t)p->jmfb_row_words * 32u / 3u;
        p->display.width = p->display.stride / 4u;
    } else {
        p->display.stride = (uint32_t)p->jmfb_row_words * 4u;
        uint32_t bpp = format_bpp(p->display.format);
        if (bpp > 0)
            p->display.width = (uint32_t)p->jmfb_row_words * 32u / bpp;
    }
}

// === Display half: JMFB-family register I/O (ported from jmfb.c) ============
// Four 256-byte blocks (0=JMFB, 1=Stopwatch, 2=CLUT, 3=Endeavor); 16-bit
// registers live at the LOW half (+2) of a 32-bit-aligned slot.

static void clut_finalize_entry(display_card_824gc_priv_t *p) {
    uint32_t w0 = p->clut_pending[0], w1 = p->clut_pending[1], w2 = p->clut_pending[2];
    rgba8_t e;
    if (w0 == 0 && w1 == 0 && (w2 & 0xFFFFFF00u) != 0) {
        e.r = (uint8_t)(w2 & 0xFFu);
        e.g = (uint8_t)((w2 >> 8) & 0xFFu);
        e.b = (uint8_t)((w2 >> 16) & 0xFFu);
    } else {
        e.r = (uint8_t)(w0 & 0xFFu);
        e.g = (uint8_t)(w1 & 0xFFu);
        e.b = (uint8_t)(w2 & 0xFFu);
    }
    e.a = 255;
    p->clut[p->clut_idx] = e;
    p->clut_idx++;
    p->clut_phase = 0;
    p->display.clut_dirty = true;
}

static void jmfb_write16(display_card_824gc_priv_t *p, int blk, uint32_t off, uint16_t val) {
    if (blk == 0) { // JMFB block
        switch (off) {
        case GC824_JMFBCSR + 2:
            p->jmfb_csr = (uint16_t)((val & ~GC824_MASK_SENSE) | (p->jmfb_csr & GC824_MASK_SENSE));
            if (val & GC824_VRSTB) {
                p->jmfb_csr &= GC824_MASK_SENSE;
                p->clut_phase = 0;
            }
            return;
        case GC824_JMFBVIDEOBASE + 2:
            p->jmfb_video_base = val;
            if (p->display.format == PIXEL_32BPP_XRGB)
                p->display.bits = p->vram + ((size_t)val * 32u * 8u / 3u);
            else
                p->display.bits = p->vram + ((size_t)val * 32u);
            p->display.fb_dirty = true;
            return;
        case GC824_JMFBROWWORDS + 2:
            p->jmfb_row_words = val;
            recompute_stride(p);
            p->display.shape_dirty = true;
            return;
        default:
            return; // high-half / unmodeled — accept-and-ignore
        }
    } else if (blk == 1) { // Stopwatch block
        switch (off) {
        case GC824_SWICREG + 2:
            p->sw_ic_reg = val;
            return;
        case GC824_SWCLRVINT + 2:
            nubus_deassert_irq(p->card);
            return;
        case GC824_SWSTATUSREG + 2:
            p->sw_status_reg = val;
            return;
        default:
            return;
        }
    } else if (blk == 2) { // CLUT block
        switch (off) {
        case GC824_CLUTDATA: // high half of a CLUTDataReg long write
            p->clut_long_hi = val;
            return;
        case GC824_CLUTADDR + 2:
            p->clut_idx = (uint8_t)(val & 0xFFu);
            p->clut_phase = 0;
            return;
        case GC824_CLUTDATA + 2: {
            uint32_t full = ((uint32_t)p->clut_long_hi << 16) | val;
            p->clut_long_hi = 0;
            if (p->clut_phase < 3)
                p->clut_pending[p->clut_phase++] = full;
            if (p->clut_phase == 3)
                clut_finalize_entry(p);
            return;
        }
        case GC824_CLUTPBCR + 2: {
            p->clut_pbcr = val;
            pixel_format_t f = depth_to_format(val);
            if ((val & 0x0002u) && f == PIXEL_8BPP)
                f = PIXEL_32BPP_XRGB;
            if (p->display.format != f) {
                p->display.format = f;
                recompute_stride(p);
                p->display.shape_dirty = true;
            }
            return;
        }
        default:
            return;
        }
    }
    // blk == 3 Endeavor PLL — accept-and-ignore (PLL program has no effect).
}

static uint16_t jmfb_read16(display_card_824gc_priv_t *p, int blk, uint32_t off) {
    if (blk == 0) {
        switch (off) {
        case GC824_JMFBCSR + 2:
            // Sense lines live in bits 9-11 (outside MaskSenseLine).
            return (uint16_t)((p->jmfb_csr & GC824_MASK_SENSE) | ((p->sense_code & 7) << 9));
        case GC824_JMFBVIDEOBASE + 2:
            return p->jmfb_video_base;
        case GC824_JMFBROWWORDS + 2:
            return p->jmfb_row_words;
        default:
            return 0;
        }
    } else if (blk == 1) {
        switch (off) {
        case GC824_SWSTATUSREG + 2:
            // VBL toggle bit 2 — flip each read so the poll sees both edges.
            p->sw_status_reg ^= 0x0004u;
            return p->sw_status_reg & 0x0004u;
        case GC824_SWICREG + 2:
            return p->sw_ic_reg;
        default:
            return 0;
        }
    } else if (blk == 2) {
        switch (off) {
        case GC824_CLUTADDR + 2:
            return p->clut_idx;
        case GC824_CLUTPBCR + 2:
            return p->clut_pbcr;
        default:
            return 0;
        }
    }
    return 0; // Endeavor / high-half reads drive zero
}

// === Accelerator: bring-up state machine ====================================

static const char *state_name(gc_state_t s) {
    switch (s) {
    case GC_ST_RESET:
        return "reset";
    case GC_ST_BOOTED:
        return "booted";
    case GC_ST_ARMED:
        return "armed";
    case GC_ST_ON:
        return "gc-on";
    case GC_ST_ERROR:
        return "error";
    default:
        return "?";
    }
}

// The card's Boot/kernel initializes the comm regions when it sees the boot
// magic (protocol §5.3; ambiguity #1 — the driver reads PublicIn+0x40C at the
// end of Control 2, so publish the CB address here, at magic-clear time, not
// on the later entry write).
static void gc_boot(display_card_824gc_priv_t *p) {
    // Clear the boot magic, exactly as the Am29000 Boot code does.
    dram_set_be32(p, GC824_DRAM_PUBLICIN + GC824_PI_MAGIC, 0);

    // Initialize the CB image.
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_STATUS, GC824_CB_INIT);
    p->cb_nubus = p->super_base | (GC824_DRAM_OFFSET + GC824_DRAM_CB); // NuBus(0x4C007000)
    // The args area and free-list heap are addressed by GCQD relative to the
    // gcp window (standard slot): it reconstructs card pointers as
    // (gcp & $FFF00000) | (ptr & $FFFFF), so those pointers must be gcp-relative
    // (not super-slot) to land inside GC824_GCP_WINDOW and reach the DRAM CB.
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_ARGSOFF, p->gcp_base + GC824_CB_ARGSAREA);
    // Queue-buffer one-block free list (protocol §9.2): {size, next=0}.
    uint32_t free_base = p->gcp_base + GC824_CB_FREEAREA;
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_FREEPTR, free_base);
    // GCQD carves the drawing queue from this block, returning free_base + 8
    // (past the {size,next} header) — the base the Transport-B stream lives at.
    p->queue_base = free_base + 8;
    uint32_t cb_local = GC824_DRAM_OFFSET + GC824_DRAM_CB; // card-local 0x0C007000
    uint32_t free_size = (0x0C00FFFCu - (cb_local + GC824_CB_FREEAREA) - 8u);
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_FREEAREA + 0, free_size);
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_FREEAREA + 4, 0);

    // Clear GCQD's private context region so its offscreen-list lock starts
    // unlocked (myflag/peer 0, list head 0) — the Am29000 firmware would zero
    // this shared scratch at GC-OS bring-up; the HLE must do the same.  A
    // fresh GC-OS boot also starts with empty caches.
    memset(p->dram + GC824_DRAM_GCTX, 0, GC824_GCTX_SIZE);
    gc824_font_caches_flush(p);
    gc824_pixpats_flush(p, 0);

    // Fill the runtime PublicOu fields the kernel would (sig/version already
    // arrived as stored ACEF bytes; synthesize the signature too so the check
    // passes regardless of which section carried it).
    dram_set_be32(p, GC824_DRAM_PUBLICOU + GC824_PO_SIG, GC824_PUBLICOU_SIG);
    dram_set_be32(p, GC824_DRAM_PUBLICOU + GC824_PO_MSTICKS, 0);
    dram_set_be32(p, GC824_DRAM_PUBLICOU + GC824_PO_SENSE, p->sense_code);
    dram_set_be32(p, GC824_DRAM_PUBLICOU + GC824_PO_DEPTH, format_bpp(p->display.format));
    dram_set_be32(p, GC824_DRAM_PUBLICOU + GC824_PO_ROWBYTES, p->display.stride);
    dram_set_be32(p, GC824_DRAM_PUBLICOU + GC824_PO_VSIZE, p->display.height);
    dram_set_be32(p, GC824_DRAM_PUBLICOU + GC824_PO_FBCFG, 0);

    // Stamp the VidComm firmware tag: from now on the decl-ROM video driver
    // routes every cscSetMode through the VidComm block (programMode $3568
    // tests this magic), handing the card the new geometry explicitly — how
    // runtime depth switches (the Monitors panel) reach the model.  Real
    // hardware behaves the same way: the tag only exists once the GC-OS has
    // booted, so the driver-Open boot mode always takes the direct path.
    dram_set_be32(p, GC824_DRAM_VIDCOMM + GC824_VC_MAGIC_OFF, GC824_VC_MAGIC);

    // Publish NuBus(CB) where the driver polls for it.
    dram_set_be32(p, GC824_DRAM_PUBLICIN + GC824_PI_CBADDR, p->cb_nubus);

    p->booted = true;
    p->expected_seq = 0;
    p->state = GC_ST_BOOTED;
    LOG(1, "boot handshake: CB published at NuBus $%08x (free %uB)", p->cb_nubus, free_size);
}

// RPC (Transport A) dispatch — executed synchronously inside the doorbell
// write handler (proposal §3.5: the HLE is infinitely fast).  Returns the
// completion status word (3 = OK / 0xB = error).
static uint32_t gc_dispatch_func(display_card_824gc_priv_t *p, uint32_t func, uint32_t *out_result) {
    uint32_t result = 0;
    uint32_t statusw = GC824_STATUSW_OK;
    switch (func) {
    case 0x00: // ping
        result = 0;
        break;
    case 0x01: // HELLO — reset the sequence, return args-area offset
        p->expected_seq = 0;
        result = 1;
        dram_set_be32(p, GC824_DRAM_CB + GC824_CB_ARGSOFF, p->gcp_base + GC824_CB_ARGSAREA);
        // Seed the comm-mode word from the HELLO param (CB+0x04 aliases arg0).
        break;
    case 0x02: // liveness pass — Status $0A returns this (must be 1)
        result = 1;
        break;
    case 0x11: // goodbye — state reset
        result = 0;
        break;
    case 0x12: // memory config: {logical, physical, size} bank bookkeeping
    case 0x13:
    case 0x14: // slot-mask no-op
    case 0x16: // main-screen pair
        result = 0;
        break;
    case 0x17: { // PQDInit / register screen (CB+0x604 = {ScrnBase in, ctx out})
        uint32_t scrnbase = dram_be32(p, GC824_DRAM_CB + 0x604);
        // ctx token GCQD stores as comm+$182 (its cache-descriptor handle) and
        // threads through its bottlenecks, building per-context structures off
        // it (the offscreen-GWorld list lock at ctx+$C0, walked in 32-bit mode).
        // It must be a private card region — NOT the CB window (gcp_base), whose
        // blit scratch at CB+$58.. would overwrite the lock's peer word and spin
        // GCQD forever.  Hand out the dedicated super-slot GCTX region.
        uint32_t ctx = p->super_base | (GC824_DRAM_OFFSET + GC824_DRAM_GCTX);
        dram_set_be32(p, GC824_DRAM_CB + 0x604 + 4, ctx);
        // PQDInit is also the fault-recovery re-init: the real handler
        // flushes all 11 caches (protocol §9.2) — drop the font + PixPat caches.
        gc824_font_caches_flush(p);
        gc824_pixpats_flush(p, 0);
        LOG(2, "func $17 PQDInit: ScrnBase=$%08x -> ctx=$%08x", scrnbase, ctx);
        // Result 1 = registered OK — sub_61B0 checks this (== 1) and $884A posts
        // error 4 ("having difficulty") otherwise.  Protocol §9.2.
        result = 1;
        break;
    }
    case 0x26: // submit + sync (resets the queue) / $38 checkpoint (no reset)
    case 0x38: {
        // Args {bufferBase, byteCount}.  Most bytes are already drained
        // incrementally (gc_drain_queue on CB+$1C0); catch up to the
        // authoritative byteCount here in case the last append wasn't published.
        uint32_t cnt = dram_be32(p, GC824_DRAM_CB + GC824_CB_ARGSAREA + 4);
        if (p->gc_accel && cnt > p->queue_ack) {
            if (p->queue_ack == 0)
                gc824_reset_draw_state(p);
            gc824_interp(p, p->queue_base + p->queue_ack, cnt - p->queue_ack);
            p->queue_bytes += cnt - p->queue_ack;
            p->queue_ack = cnt;
        }
        dram_set_be32(p, GC824_DRAM_CB + GC824_CB_QUEUE_ACK, dram_be32(p, GC824_DRAM_CB + GC824_CB_QUEUE_PUB));
        if (func == 0x26) {
            // sub_7C56 resets the host's write pointers after every $26
            // submit: the next cycle re-marshals from the buffer start.
            p->queue_ack = 0;
            dram_set_be32(p, GC824_DRAM_CB + GC824_CB_QUEUE_PUB, 0);
        }
        // func $26 (submit+sync) ENDS the port epoch: on the real card the
        // DrawMultiObject activation — and the origin/clip it captured in the
        // func-$2D prologue — dies with the batch (pump status $C9), and the
        // host clears its last-synced-port cache (sub_7C56 clears C+$C8), so
        // the next drawing record ALWAYS re-stages CB+$170 from the live
        // GrafPort and re-sends func $2D ($6684/$66AC/$6702).  Mirroring that
        // here is what keeps a mid-session SetOrigin (the cdev list LDEF)
        // coherent: never interpret another buffer on a stale origin.
        // func $38 is the mid-batch checkpoint — the batch (and port) live on.
        if (func == 0x26)
            p->gc_accel = false;
        result = 0;
        break;
    }
    // --- Drawing surface: decline to the ROM path (stage 1 safety net) ---
    // The decline convention is result == 0 (protocol §9): for func $15 the host
    // reads "0 = declined → run the ROM blit" (1 = card drew it); for func $2D
    // (SetPort) the result's bit 0 is "accept" (else fall back to ROM for the
    // whole port).  Returning 0 for both makes GCQD render every primitive via
    // QuickDraw's own ROM bottlenecks — the stage-1 safety net (proposal §4).
    case 0x15: // StretchBits (CopyBits): accelerate the accept-envelope, else
        // decline (result 0 → host runs the ROM blit).  Runs synchronously here,
        // so it lands in the framebuffer in RPC order relative to the queued
        // geometry (drawn at the preceding func $26 submits).  NOT gated on
        // gc_accel: GCQD flushes with func $26 (which ends the port epoch)
        // immediately BEFORE each blit, and the next $2D only accompanies the
        // next queued batch — a gc_accel gate here auto-declines every blit.
        // The request block is self-contained (its own dst base/depth/regions),
        // and gc_stretchbits accepts only screen-destination requests.
        result = (!p->force_decline && gc824_stretchbits(p)) ? 1 : 0;
        break;
    case 0x2D: { // SetPort — accept the port so its geometry queue is interpreted.
        // The staged record at CB+$170 (built by GCQD $4774): +0 thePort,
        // +$E the port pixmap's baseAddr, +$14 its BOUNDS — the port's
        // local→global mapping (window content is drawn in port-LOCAL
        // coordinates; global = local − bounds.topLeft; the WMgr port's
        // bounds are (0,0) so the desktop never exposed this).
        // Accept only ports that target the SCREEN at a supported depth;
        // gc.force_decline (the differential test oracle, proposal §4.1)
        // declines everything → the ROM path renders the same scene.
        //
        // The screen test reads the LIVE thePort's baseAddr, NOT the staged
        // +$E copy: GCQD does not refresh +$E per SetPort — it leaves the
        // screen base there even when thePort is an OFFSCREEN GrafPort (e.g.
        // Norton's installer draws its caution text into an off-screen bitmap
        // then CopyBits it into the dialog).  Trusting the stale +$E made the
        // card accept that offscreen port as the screen and paint the
        // off-screen-bound text onto the real screen at origin (0,0).  The
        // real card resolves the port synchronously in the func-$2D prologue
        // (text_13264 bus-master-maps the live GrafPort) — do the same:
        // old-style GrafPort → baseAddr at thePort+2 (portBits.rowBytes at +6
        // never has the top two bits set); CGrafPort → portVersion at +6 has
        // both top bits set → portPixMap handle at +2, then PixMap.baseAddr.
        uint32_t tport = dram_be32(p, GC824_DRAM_CB + 0x170) & 0x00FFFFFFu;
        uint32_t pbase;
        if ((memory_debug_read_uint16(tport + 6) & 0xC000u) == 0xC000u) {
            uint32_t pmh = memory_debug_read_uint32(tport + 2) & 0x00FFFFFFu;
            uint32_t pm = memory_debug_read_uint32(pmh) & 0x00FFFFFFu;
            pbase = memory_debug_read_uint32(pm);
        } else {
            pbase = memory_debug_read_uint32(tport + 2);
        }
        uint32_t screen = p->super_base | (GC824_DRAM_OFFSET + GC824_FB_OFFSET);
        bool depth_ok = p->display.format == PIXEL_1BPP_MSB || p->display.format == PIXEL_8BPP ||
                        p->display.format == PIXEL_16BPP_555 || p->display.format == PIXEL_32BPP_XRGB;
        result = (!p->force_decline && depth_ok && pbase == screen) ? 1 : 0;
        if (result) {
            p->gc_org_y = (int16_t)(0 - (int16_t)dram_be16(p, GC824_DRAM_CB + 0x170 + 0x14));
            p->gc_org_x = (int16_t)(0 - (int16_t)dram_be16(p, GC824_DRAM_CB + 0x170 + 0x16));
            p->gc_port_ptr = dram_be32(p, GC824_DRAM_CB + 0x170);
        }
        p->gc_accel = (result != 0); // gate the interpreter on port acceptance
        LOG(3, "func $2D SetPort %s (org %d,%d)", result ? "accepted" : "declined", p->gc_org_x, p->gc_org_y);
        break;
    }
    case 0x0C: // CachePixPat — expand + cache a patType-1 PixPat (type 5).
        // Declining makes the host fall back to the ROM original for the
        // whole draw (sub_1530 returns -1), so the oracle switch and any
        // out-of-envelope pattern degrade safely.
        result = !p->force_decline ? (uint32_t)gc824_cache_pixpat(p) : 0;
        break;
    case 0x2C: // FlushPixPat (DisposePixPat) — one long arg: the handle.
        gc824_pixpats_flush(p, dram_be32(p, GC824_DRAM_CB + GC824_CB_ARGSAREA));
        result = 1;
        break;
    case 0x30: { // FontDownload — cache the strikes/width tables so ops $67/$06
        // draw text on-card; any failure (or the oracle switch, or an
        // unsupported depth) declines: the host rolls back its checksum and
        // draws text unaccelerated (proposal §3.10 safety net).  Accepted at
        // every port-accepted depth (1/8/16/32 — same set as func $2D): the
        // glyph cores render through the depth-generic gc_px, and the real
        // Rev B card accelerates text at the direct depths too.
        bool text_depth_ok = p->display.format == PIXEL_1BPP_MSB || p->display.format == PIXEL_8BPP ||
                             p->display.format == PIXEL_16BPP_555 || p->display.format == PIXEL_32BPP_XRGB;
        result = (!p->force_decline && text_depth_ok) ? (uint32_t)gc824_font_download(p) : 0;
        break;
    }
    default:
        if (func > 0x3B) {
            // Unknown function — flag the sequence/error bits (protocol §9).
            dram_set_be32(p, GC824_DRAM_CB + GC824_CB_STATUS, dram_be32(p, GC824_DRAM_CB + GC824_CB_STATUS) | 0x11u);
            statusw = GC824_STATUSW_ERR;
            LOG(1, "RPC unknown func $%02x", func);
        } else {
            // A known-but-unimplemented func: succeed benignly (bookkeeping).
            result = 0;
            LOG(2, "func $%02x accepted (no-op, stage 1)", func);
        }
        break;
    }
    *out_result = result;
    return statusw;
}

// Doorbell (CB+0x14 = -1): run the pending RPC to completion, in place.
static void gc_rpc(display_card_824gc_priv_t *p) {
    p->rpc_count++;
    // Processing marker — func $02 "returns 1 by construction".
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_RESULT, 1);
    uint32_t func = dram_be32(p, GC824_DRAM_CB + GC824_CB_FUNC);
    uint32_t seq = dram_be32(p, GC824_DRAM_CB + GC824_CB_SEQ);
    p->last_func = func;

    uint32_t result = 0, statusw;
    if (func != 1 && seq != p->expected_seq) {
        // Sequence desync (GCQD fault recovery relies on the seq-error bits).
        LOG(2, "RPC seq desync: got %u expected %u (func $%02x)", seq, p->expected_seq, func);
        dram_set_be32(p, GC824_DRAM_CB + GC824_CB_STATUS, dram_be32(p, GC824_DRAM_CB + GC824_CB_STATUS) | 3u);
        statusw = GC824_STATUSW_ERR;
    } else {
        statusw = gc_dispatch_func(p, func, &result);
        // HELLO resets the sequence: the host zeroes its counter (comm+$20) and
        // the *first* post-HELLO RPC therefore carries seq 0 (post-increment),
        // so the card must expect 0 next — not 1.  (An off-by-one here desyncs
        // that first ping → error bit → GCQD re-HELLOs forever.)
        p->expected_seq = (func == 1) ? 0 : seq + 1;
    }

    // Complete: result, status word, host-side status mirror, clear doorbell.
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_RESULT, result);
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_STATUSW, statusw);
    uint32_t mirror = dram_be32(p, GC824_DRAM_CB + GC824_CB_STATUSMIR);
    if (mirror)
        memory_debug_write_uint32(mirror, statusw); // bus-master into host RAM
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_DOORBELL, 0);
    LOG(3, "RPC func $%02x seq %u -> result $%08x status $%x", func, seq, result, statusw);
}

// Transport B (CB+0x1C0 bytes published): drain the opcode stream.  Draining
// happens INCREMENTALLY as the host publishes bytes (proposal §3.5) — not
// batched at the func-$26 submit — so accelerated geometry lands in the FB in
// the same temporal order as the ROM-declined ops (text, CopyBits) the Finder
// draws between publishes; a later batch would paint over that ROM output.
// The host resets the buffer (CB+$1C0 drops) after a func-$26 submit; we detect
// that as the start of a new drawing cycle and reset the interpreter state.
static void gc_drain_queue(display_card_824gc_priv_t *p) {
    uint32_t pub = dram_be32(p, GC824_DRAM_CB + GC824_CB_QUEUE_PUB);
    // A publish value BELOW the high-water mark is NOT a buffer reset: GCQD's
    // $1C0 counter lags one batch behind the func-$38 checkpoint (whose args
    // carry the authoritative count that already advanced queue_ack).
    // Re-interpreting from 0 here replayed the whole cycle after every
    // checkpoint — each ZoomRects/DragGrayRgn XOR frame re-XORed once per
    // step, leaving un-erased outline residue (window-open zoom, window
    // drag).  The REAL cycle reset is the func-$26 submit (sub_7C56 resets
    // the write pointers) — handled there; a lagging counter is ignored.
    if (pub < p->queue_ack)
        return;
    if (pub == p->queue_ack)
        return;
    uint32_t n = pub - p->queue_ack;
    if (p->gc_accel) {
        if (p->queue_ack == 0)
            gc824_reset_draw_state(p); // start of a cycle: QuickDraw defaults
        gc824_interp(p, p->queue_base + p->queue_ack, n);
    }
    p->queue_bytes += n;
    p->queue_ack = pub;
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_QUEUE_ACK, pub);
}

// VidComm mode change: the video driver published new geometry (see the
// header block).  Apply it to the display and clear the ack byte the host
// polls.  The FB base stays inside the DRAM aperture at every shipped mode
// (0x11400 at both 1 and 8 bpp — guest-probed); reject anything that doesn't
// decode rather than tearing the display.
static void gc_vidcomm(display_card_824gc_priv_t *p) {
    uint32_t vc = GC824_DRAM_VIDCOMM;
    uint32_t fbbase = dram_be32(p, vc + GC824_VC_FBBASE);
    uint32_t rowbytes = dram_be32(p, vc + GC824_VC_ROWBYTES);
    uint32_t bpp = dram_be32(p, vc + GC824_VC_BPP);
    uint32_t scanlines = dram_be32(p, vc + GC824_VC_SCANLINES);
    dram_set_be32(p, vc + GC824_VC_GO, 0); // consume the request
    uint32_t fboff = fbbase & 0x0FFFFFFFu; // card-local
    if (fboff >= GC824_DRAM_OFFSET && fboff - GC824_DRAM_OFFSET < GC824_DRAM_SIZE && rowbytes >= 8 &&
        rowbytes <= 8192 && scanlines >= 1 && scanlines <= 2048) {
        int b = (bpp == 24) ? 32 : (int)bpp; // programMode folds 32 -> 24
        p->display.format = format_for_bpp(b);
        p->display.bits = p->dram + (fboff - GC824_DRAM_OFFSET);
        p->display.stride = rowbytes;
        // `scanlines` is programMode's ADJUSTED scan count (the direct modes
        // add +5/+60 blank/overscan lines — 545 for 640×480×32) — a CRT
        // total, NOT the visible height.  The visible geometry is the
        // monitor's; only narrow the width if rowBytes can't hold it.
        if (b > 0 && rowbytes * 8u / (uint32_t)b < p->display.width)
            p->display.width = rowbytes * 8u / (uint32_t)b;
        p->display.shape_dirty = true;
        p->display.fb_dirty = true;
        LOG(1, "VidComm mode change: fb=$%08x rowBytes=%u bpp=%u scanlines=%u", fbbase, rowbytes, bpp, scanlines);
    } else {
        LOG(0, "VidComm mode change rejected: fb=$%08x rowBytes=%u bpp=%u scanlines=%u", fbbase, rowbytes, bpp,
            scanlines);
    }
    dram_set_be32(p, vc + GC824_VC_ACK, 0); // ack: host polls byte 0 == 0
}

// After any guest write into DRAM, check the watched comm-region longwords and
// react (protocol §5.3/§6).  Writes may be byte/word/long; the driver issues
// these as MOVE.L, so reading the full longword back is robust.
static void gc_check_triggers(display_card_824gc_priv_t *p) {
    if (!p->booted) {
        if (dram_be32(p, GC824_DRAM_PUBLICIN + GC824_PI_MAGIC) == GC824_BOOT_MAGIC)
            gc_boot(p);
        return;
    }
    // VidComm mode-change request (geometry published + go raised).
    if (dram_be32(p, GC824_DRAM_VIDCOMM + GC824_VC_GO) == 0x80000000u)
        gc_vidcomm(p);
    // Arm: reply-mailbox address + arm magic.
    if (!p->armed && dram_be32(p, GC824_DRAM_CB + GC824_CB_STATUS) == GC824_ARM_MAGIC) {
        p->armed = true;
        p->mailbox = dram_be32(p, GC824_DRAM_CB + GC824_CB_MAILBOX);
        if (p->state < GC_ST_ARMED)
            p->state = GC_ST_ARMED;
        LOG(1, "CB armed: reply-mailbox phys $%08x", p->mailbox);
    }
    // Doorbell RPC.
    if (dram_be32(p, GC824_DRAM_CB + GC824_CB_DOORBELL) == 0xFFFFFFFFu)
        gc_rpc(p);
    // Transport B queue.
    if (dram_be32(p, GC824_DRAM_CB + GC824_CB_QUEUE_PUB) != p->queue_ack)
        gc_drain_queue(p);
}

// === Unified device dispatcher over every region ===========================
// `phys` is the absolute physical address (region_base + region-relative addr).

// Read `width` bytes big-endian from a card RAM buffer at `off`.
static uint32_t buf_read(const uint8_t *buf, uint32_t off, uint32_t size, unsigned width) {
    if (width == 1)
        return off < size ? buf[off] : 0xFFu;
    if (width == 2)
        return off + 2 <= size ? LOAD_BE16(buf + off) : 0xFFFFu;
    return off + 4 <= size ? LOAD_BE32(buf + off) : 0xFFFFFFFFu;
}
static void buf_write(uint8_t *buf, uint32_t off, uint32_t size, uint32_t val, unsigned width) {
    if (width == 1) {
        if (off < size)
            buf[off] = (uint8_t)val;
    } else if (width == 2) {
        if (off + 2 <= size)
            STORE_BE16(buf + off, (uint16_t)val);
    } else if (off + 4 <= size) {
        STORE_BE32(buf + off, val);
    }
}

// A longword MFB/ACDC register whose meaningful state lives in the high bits
// (bit 31, or the ID nibble in bits 24-27) must present the big-endian MSB
// byte/word to a narrow read.  gc_read hands the aligned register value up and
// io_read8/16 slice its low bits, so without this a byte read of a longword
// register (e.g. the strap probe's `BFEXTU {#0:#1}` = a byte read of bit 31)
// would return the low byte (0) instead of the high byte.
static inline uint32_t reg_narrow(uint32_t v, unsigned width) {
    if (width == 1)
        return v >> 24;
    if (width == 2)
        return v >> 16;
    return v;
}

static uint32_t gc_read(display_card_824gc_priv_t *p, uint32_t phys, unsigned width) {
    // GCQD command-block window (standard slot): alias onto the DRAM CB so the
    // marshaller's doorbell/status/heartbeat/args polls hit the live engine.
    if (phys >= p->gcp_base && phys < p->gcp_base + GC824_GCP_WINDOW)
        return buf_read(p->dram, GC824_DRAM_CB + (phys - p->gcp_base), GC824_DRAM_SIZE, width);
    // Display registers (standard slot space).
    uint32_t jmfb_base = p->slot_base + GC824_JMFB_BLOCK_OFFSET;
    if (phys >= jmfb_base && phys < jmfb_base + GC824_REGISTER_SIZE) {
        uint32_t rel = phys - jmfb_base;
        int blk = (int)(rel >> 8);
        uint32_t off = rel & 0xFFu;
        if (width == 1) {
            uint16_t v = jmfb_read16(p, blk, off & ~1u);
            return (off & 1) ? (v & 0xFFu) : (v >> 8);
        }
        if (width == 2)
            return jmfb_read16(p, blk, off);
        return ((uint32_t)jmfb_read16(p, blk, off) << 16) | jmfb_read16(p, blk, off + 2);
    }
    // --- Super-slot space (card-local = phys - super_base) ---
    uint32_t cl = phys - p->super_base; // card-local offset
    if (cl - GC824_SRAM_OFFSET < GC824_SRAM_SIZE)
        return buf_read(p->sram, cl - GC824_SRAM_OFFSET, GC824_SRAM_SIZE, width);
    if (cl >= GC824_SRAM_IALIAS_OFFSET && cl < GC824_SRAM_IALIAS_OFFSET + GC824_SRAM_SIZE)
        return buf_read(p->sram, cl - GC824_SRAM_IALIAS_OFFSET, GC824_SRAM_SIZE, width);
    if (cl >= GC824_REGS_OFFSET && cl < GC824_REGS_OFFSET + GC824_REGS_SIZE) {
        switch (cl) {
        case GC824_REG_ALIVE_A:
        case GC824_REG_ALIVE_B:
        case GC824_REG_ALIVE_C: {
            // These MFB sense registers ($44/$48/$4C) serve triple duty:
            //   (1) .GraphAccel "board alive" flags — bit 31 = 1 when idle;
            //   (2) the video driver's 3-bit config strap — PrimaryInit reads
            //       bit 31 of each ($1CFE-$1D24) → a config code 0..7;
            //   (3) the VRAM-interconnect sense — vramProbe ($243C) drives
            //       walking-ones into the bank-drive lines $2C/$30/$34 one at a
            //       time and reads which sense bits respond, assembling a 6-bit
            //       signature: {p1.s48, p1.s4C, p2.s44, p2.s4C, p3.s44, p3.s48}
            //       (pass N = drive line $2C/$30/$34).  PrimaryInit maps
            //       $14→config 4, $00/$30→config 0, $2D→config 5, else 9.
            // Idle (no bank line driven) → bit 31 = 1: satisfies the alive
            // check AND makes the strap code 7 (→ the deep vramProbe path).
            // While vramProbe is driving a bank line → read 0, so the assembled
            // signature is 0, which the caller maps to **config 0** — the
            // 640×480 configuration this model is verified against.  (Probed
            // alternatives, for the record: signature $14 → config 4 = the
            // 512×384 monitor; $2D → config 5 = 640×480 with a 128-byte 1-bpp
            // pitch and no 32-bpp mode.  Config 0's post-SecondaryInit
            // sResource $A0 carries all six depths; 16/32 bpp are reachable at
            // RUNTIME via Monitors/cscSetMode → VidComm — never at boot: the
            // ROM Slot Manager keeps the 24-bit $B0 family, capped at 8 bpp,
            // until SecondaryInit swaps it, exactly like real hardware.)
            uint32_t bank_drive = buf_read(p->regs, 0x2C, GC824_REGS_SIZE, 4) |
                                  buf_read(p->regs, 0x30, GC824_REGS_SIZE, 4) |
                                  buf_read(p->regs, 0x34, GC824_REGS_SIZE, 4);
            uint32_t v = bank_drive ? 0u : 0x80000000u;
            // The meaningful state is bit 31, so a narrow read must return the
            // big-endian MSB byte/word (the strap probe BFEXTUs bit 31 via a
            // BYTE read at offset 0 — returning the low byte read the strap as 0
            // and mis-selected the config).
            return reg_narrow(v, width);
        }
        case GC824_REG_SYNC_HB: {
            // Video "heartbeat" the card-sync primitive actually polls.  Its
            // delayTouch helper (decl ROM $3420) reads this cell 10× and leaves
            // the LAST read in D0; cardSync ($33C8) then waits for bit 31 to
            // cycle set→clear→set (a full frame).  (The $44001C0 read in that
            // loop is a discarded bus side-effect — see decl-rom-disasm and
            // open-issues.md §0.)  So synthesize a heartbeat here: flip bit 31
            // on a read counter, period > the 10-read delay loop so consecutive
            // delayTouch results alternate and all three passes advance.
            // Synthesized on read (not stored): delayTouch writes the value back
            // each iteration, so a RAM cell would be frozen.
            if (width != 4)
                return buf_read(p->regs, cl - GC824_REGS_OFFSET, GC824_REGS_SIZE, width);
            uint32_t v = buf_read(p->regs, cl - GC824_REGS_OFFSET, GC824_REGS_SIZE, 4) & ~0x80000000u;
            if (((p->sync_hb++ >> 4) & 1u) != 0)
                v |= 0x80000000u;
            return v;
        }
        case GC824_REG_MFB_SYNC:
            // MFB register the card-sync loop reads for its bus side-effect;
            // its value is discarded (clobbered by delayTouch).  Toggle bit 31
            // anyway in case other code polls it directly.
            p->mfb_sync ^= 0x80000000u;
            return p->mfb_sync;
        case GC824_REG_ACDC_ID:
            // ACDC ID register: read-only, returns the ACDC identifier
            // (bits 24-27 = 6).  The video driver's ACDC probe writes then
            // reads this and requires (read & 0x0F000000) == 0x06000000 to
            // detect the ACDC (decl-ROM $4488); a RAM read-back would fail it.
            return reg_narrow(0x06000000u, width);
        default:
            // Every other MFB/config/ACDC register is backed as RAM so the
            // driver's write-then-read-back hardware probes behave.
            return buf_read(p->regs, cl - GC824_REGS_OFFSET, GC824_REGS_SIZE, width);
        }
    }
    if (cl >= GC824_DRAM_OFFSET && cl < GC824_DRAM_OFFSET + GC824_DRAM_SIZE)
        return buf_read(p->dram, cl - GC824_DRAM_OFFSET, GC824_DRAM_SIZE, width);
    if (cl >= GC824_DRAM_MIRROR_OFFSET && cl < GC824_DRAM_MIRROR_OFFSET + GC824_DRAM_SIZE)
        return buf_read(p->dram, cl - GC824_DRAM_MIRROR_OFFSET, GC824_DRAM_SIZE, width);
    LOG(3, "read card-local $%07x (unmapped) w%u", cl, width);
    return 0xFFFFFFFFu >> ((4 - width) * 8); // NuBus unmapped reads float high
}

static void gc_write(display_card_824gc_priv_t *p, uint32_t phys, uint32_t val, unsigned width) {
    // GCQD command-block window (standard slot): write into the DRAM CB and run
    // the comm-region trigger check, exactly as a super-slot CB write would.
    if (phys >= p->gcp_base && phys < p->gcp_base + GC824_GCP_WINDOW) {
        buf_write(p->dram, GC824_DRAM_CB + (phys - p->gcp_base), GC824_DRAM_SIZE, val, width);
        gc_check_triggers(p);
        return;
    }
    uint32_t jmfb_base = p->slot_base + GC824_JMFB_BLOCK_OFFSET;
    if (phys >= jmfb_base && phys < jmfb_base + GC824_REGISTER_SIZE) {
        uint32_t rel = phys - jmfb_base;
        int blk = (int)(rel >> 8);
        uint32_t off = rel & 0xFFu;
        if (width == 1)
            jmfb_write16(p, blk, off & ~1u, (uint16_t)(val | (val << 8)));
        else if (width == 2)
            jmfb_write16(p, blk, off, (uint16_t)val);
        else {
            jmfb_write16(p, blk, off, (uint16_t)(val >> 16));
            jmfb_write16(p, blk, off + 2, (uint16_t)val);
        }
        return;
    }
    // --- Super-slot space ---
    uint32_t cl = phys - p->super_base;
    if (cl - GC824_SRAM_OFFSET < GC824_SRAM_SIZE) {
        buf_write(p->sram, cl - GC824_SRAM_OFFSET, GC824_SRAM_SIZE, val, width);
        return;
    }
    if (cl >= GC824_SRAM_IALIAS_OFFSET && cl < GC824_SRAM_IALIAS_OFFSET + GC824_SRAM_SIZE) {
        buf_write(p->sram, cl - GC824_SRAM_IALIAS_OFFSET, GC824_SRAM_SIZE, val, width);
        return;
    }
    if (cl >= GC824_REGS_OFFSET && cl < GC824_REGS_OFFSET + GC824_REGS_SIZE) {
        // Store into the register-space RAM (so read-backs see it), then act
        // on the two semantic control registers.
        buf_write(p->regs, cl - GC824_REGS_OFFSET, GC824_REGS_SIZE, val, width);
        if (cl == GC824_REG_ATTACH) {
            if (val == 0xFFFFFFFFu) {
                p->attached = true;
                LOG(1, "attach ($04000028 = -1)");
            } else if (val == 0) {
                p->attached = false;
            }
        } else if (cl == GC824_REG_SYNC_HB) {
            // Serial command latch (see priv fields): shift bit 31 in;
            // every 12th bit completes a command frame.
            p->risc_cmd_shift = (uint16_t)((p->risc_cmd_shift << 1) | ((val >> 31) & 1u));
            if (++p->risc_cmd_bits == 12) {
                uint16_t cmd = p->risc_cmd_shift & 0xFFFu;
                p->risc_cmd_bits = 0;
                p->risc_cmd_shift = 0;
                if (cmd == 1) {
                    p->vbl_enabled = true;
                    LOG(1, "RISC serial command 1 (run) -> slot VBL on");
                } else if (cmd == 3) {
                    p->vbl_enabled = false;
                    nubus_deassert_irq(p->card);
                    LOG(1, "RISC serial command 3 (stop) -> slot VBL off");
                } else {
                    LOG(2, "RISC serial command $%03x (ignored)", cmd);
                }
            }
        } else if (cl == GC824_REG_VBL_ACK) {
            // The slot VBL ISR ($39E2) acks with CLR.L; PrimaryInit and the
            // mode loaders also clear it as part of their latch dance.
            nubus_deassert_irq(p->card);
        } else if (cl == GC824_REG_KICK && val == 0xFFFFFFFFu) {
            p->gc_on = true;
            if (p->state < GC_ST_ON)
                p->state = GC_ST_ON;
            LOG(1, "firmware kick ($04000050 = -1) -> GC ON");
        } else if (cl == GC824_REG_ACDC_ADDR) {
            // ACDC RAMDAC address port: a write sets the palette index and
            // resets the R/G/B phase.  (The ACDC probe / loadCRTCandCLUT write
            // this too — harmless: they never follow with DATA-port writes.)
            p->acdc_addr = (uint8_t)(val & 0xFFu);
            p->acdc_phase = 0;
        } else if (cl == GC824_REG_ACDC_DATA) {
            // ACDC RAMDAC data port: three successive byte writes are R, G, B
            // (8 bpc); after B, commit clut[index] and auto-increment the index.
            if (p->acdc_phase < 3)
                p->acdc_rgb[p->acdc_phase] = (uint8_t)(val & 0xFFu);
            if (++p->acdc_phase >= 3) {
                p->clut[p->acdc_addr] = (rgba8_t){p->acdc_rgb[0], p->acdc_rgb[1], p->acdc_rgb[2], 255};
                p->acdc_addr++;
                p->acdc_phase = 0;
                p->display.clut_dirty = true;
            }
        }
        return;
    }
    if (cl >= GC824_DRAM_OFFSET && cl < GC824_DRAM_OFFSET + GC824_DRAM_SIZE) {
        buf_write(p->dram, cl - GC824_DRAM_OFFSET, GC824_DRAM_SIZE, val, width);
        gc_check_triggers(p);
        return;
    }
    if (cl >= GC824_DRAM_MIRROR_OFFSET && cl < GC824_DRAM_MIRROR_OFFSET + GC824_DRAM_SIZE) {
        buf_write(p->dram, cl - GC824_DRAM_MIRROR_OFFSET, GC824_DRAM_SIZE, val, width);
        gc_check_triggers(p);
        return;
    }
    LOG(3, "write card-local $%07x = $%08x (unmapped) w%u", cl, val, width);
}

// === Memory interface (single dispatcher over every region) =================

static uint8_t io_read8(void *dev, uint32_t addr) {
    gc_reg_ctx_t *c = dev;
    return (uint8_t)gc_read(c->p, c->region_base + addr, 1);
}
static uint16_t io_read16(void *dev, uint32_t addr) {
    gc_reg_ctx_t *c = dev;
    return (uint16_t)gc_read(c->p, c->region_base + addr, 2);
}
static uint32_t io_read32(void *dev, uint32_t addr) {
    gc_reg_ctx_t *c = dev;
    return gc_read(c->p, c->region_base + addr, 4);
}
static void io_write8(void *dev, uint32_t addr, uint8_t val) {
    gc_reg_ctx_t *c = dev;
    gc_write(c->p, c->region_base + addr, val, 1);
}
static void io_write16(void *dev, uint32_t addr, uint16_t val) {
    gc_reg_ctx_t *c = dev;
    gc_write(c->p, c->region_base + addr, val, 2);
}
static void io_write32(void *dev, uint32_t addr, uint32_t val) {
    gc_reg_ctx_t *c = dev;
    gc_write(c->p, c->region_base + addr, val, 4);
}

static memory_interface_t s_gc824_mem_iface = {
    .read_uint8 = io_read8,
    .read_uint16 = io_read16,
    .read_uint32 = io_read32,
    .write_uint8 = io_write8,
    .write_uint16 = io_write16,
    .write_uint32 = io_write32,
};

// === VROM load ==============================================================
// Content-driven (Format-Block CRC): loads whichever of the three known GC
// declaration ROMs (v1.1 64 KB / v1.0 / alpha 32 KB) is actually present —
// the explicit machine.vrom.load path first (any filename), then the catalog
// names in the standard locations.  A 32 KB revision lands in the top half of
// the 256 KB bus window (the Format Block always ends at the slot top).
static bool load_vrom(display_card_824gc_priv_t *p) {
    char *path = NULL;
    if (!declrom_load_vrom_card(display_card_824gc_kind.id, p->vrom, GC824_DECLROM_BUS_SIZE, &path))
        return false;
    free(p->vrom_path);
    p->vrom_path = path;
    p->vrom_size = GC824_DECLROM_BUS_SIZE;
    return true;
}

// === Video-mode selection (machine.nubus.video_mode) ========================
static char s_pending_video_mode_id[40] = "";
static const nubus_monitor_t display_card_824gc_monitors[]; // fwd

static pixel_format_t format_for_bpp(int bpp) {
    switch (bpp) {
    case 1:
        return PIXEL_1BPP_MSB;
    case 2:
        return PIXEL_2BPP_MSB;
    case 4:
        return PIXEL_4BPP_MSB;
    case 8:
        return PIXEL_8BPP;
    case 16:
        return PIXEL_16BPP_555;
    case 32:
        return PIXEL_32BPP_XRGB;
    default:
        return PIXEL_8BPP;
    }
}
static uint8_t spdepth_for_bpp(int bpp) {
    switch (bpp) {
    case 1:
        return 0x80;
    case 2:
        return 0x81;
    case 4:
        return 0x82;
    case 8:
        return 0x83;
    case 16:
        return 0x84;
    case 32:
        return 0x85;
    default:
        return 0x83;
    }
}

// === Card vtable ============================================================

// Power-on register/display/accelerator state (shared by card_init and the
// /RESET hook).  VRAM, the declaration ROM, SRAM/DRAM buffers, and the host
// memory-map regions persist across a warm reset — only the externally visible
// state resets, as the /RESET pin does in silicon.
static void set_poweron_defaults(display_card_824gc_priv_t *p) {
    // JMFB display defaults: power up at 1 bpp (the video driver switches
    // depth via CLUTPBCR at boot; jmfb convention).
    p->sw_ic_reg = GC824_VINT_DISABLE; // VBL IRQ masked until installed
    p->jmfb_csr = 0;
    p->jmfb_video_base = 0xA00 / 32; // $A00 byte offset (driver convention)
    p->jmfb_row_words = p->display.width ? (uint16_t)(p->display.width / 32u) : (640u / 32u);
    p->clut_idx = 0;
    p->clut_phase = 0;
    p->clut_pbcr = 0;
    p->acdc_addr = 0;
    p->acdc_phase = 0;
    p->clut_long_hi = 0;

    // The GC decl-ROM video driver (config 0 → 640×480) brings the screen up
    // at the depth the slot PRAM selects (the video_mode seed wrote it; 1 bpp
    // is the virgin default).  With 32-bit QuickDraw present, its
    // SecondaryInit puts the framebuffer in the super-slot DRAM aperture:
    // ScrnBase = NuBus 0x9C011400 = p->dram + GC824_FB_OFFSET, stride 1024 at
    // every indexed depth (verified by guest probe at 1 and 8 bpp).  QuickDraw
    // draws the entire Finder desktop there, so that is what the host display
    // surfaces (NOT the standard-slot VRAM, which only holds the early
    // "Welcome to Macintosh" startup screen).  The boot mode must match what
    // the driver will program at Open (the direct MFB/ACDC path isn't
    // decoded); RUNTIME depth switches arrive via VidComm (gc_vidcomm).
    p->display.format = format_for_bpp(p->seeded_bpp ? p->seeded_bpp : 1);
    p->display.width = 640u;
    p->display.height = 480u;
    // Row pitch: 1024 bytes at every indexed depth (guest-probed at 1/8 bpp).
    // The direct modes use packed pitches (32 bpp = 2560, VidComm-probed) but
    // can never be the BOOT depth, so the power-on pitch is always 1024.
    p->display.stride = 1024u;
    p->display.bits = p->dram + GC824_FB_OFFSET;
    p->display.clut = p->clut;
    p->display.clut_len = 256;
    p->display.crt_response = NULL;
    p->display.shape_dirty = true;
    p->display.clut_dirty = true;
    p->display.fb_dirty = true;
    p->display.response_dirty = true;

    // Accelerator: cold, no firmware.  DRAM/SRAM buffers keep their contents
    // (the ROM re-downloads the firmware on reboot).
    p->state = GC_ST_RESET;
    p->booted = false;
    p->armed = false;
    p->attached = false;
    p->gc_on = false;
    p->mailbox = 0;
    p->cb_nubus = 0;
    p->expected_seq = 0;
    p->last_func = 0;
    p->queue_ack = 0;
    p->error = 0;
    p->gc_accel = false;
    p->draw_count = 0;

    for (int i = 0; i < 256; i++) {
        p->clut[i].r = p->clut[i].g = p->clut[i].b = (uint8_t)i;
        p->clut[i].a = 255;
    }
    // 1-bpp mono palette (Mac convention: pixel 0 = white, 1 = black).  The GC
    // OS programs the real palette into the ACDC (0x6C00000), not this clut[],
    // so seed the mono pair here so a B&W desktop reads correctly.  (Colour-
    // depth CLUT capture from the ACDC is a follow-up.)
    p->clut[0] = (rgba8_t){255, 255, 255, 255};
    p->clut[1] = (rgba8_t){0, 0, 0, 255};
}

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp) {
    (void)cp;
    display_card_824gc_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->card = card;
    p->slot_base = nubus_slot_base(card->slot);
    p->super_base = nubus_super_slot_base(card->slot);
    // GCQD command-block window: card+$16C = std base | (slot<<20); gcp = +$8C00.
    p->gcp_base = (p->slot_base | ((uint32_t)card->slot << 20)) + GC824_GCP_OFFSET;

    // Consume a pending video-mode pick (machine.nubus.video_mode = "...").
    const nubus_monitor_t *seeded_monitor = NULL;
    int seeded_depth_bpp = 0;
    if (s_pending_video_mode_id[0] &&
        display_card_824gc_video_mode_lookup(s_pending_video_mode_id, &seeded_monitor, &seeded_depth_bpp))
        s_pending_video_mode_id[0] = '\0';
    else
        seeded_monitor = NULL;

    p->vram = calloc(1, GC824_VRAM_SIZE);
    p->vrom = calloc(1, GC824_DECLROM_BUS_SIZE);
    p->sram = calloc(1, GC824_SRAM_SIZE);
    p->dram = calloc(1, GC824_DRAM_SIZE);
    p->regs = calloc(1, GC824_REGS_SIZE);
    p->gc_clipmask = calloc(1, (size_t)GC824_CLIP_STRIDE * GC824_CLIP_ROWS);
    p->gc_blitmask = calloc(1, (size_t)GC824_CLIP_STRIDE * GC824_CLIP_ROWS);
    p->gc_cliprgn = calloc(1, GC824_RGN_MAX);
    p->gc_visrgn = calloc(1, GC824_RGN_MAX);
    if (!p->vram || !p->vrom || !p->sram || !p->dram || !p->regs || !p->gc_clipmask || !p->gc_blitmask ||
        !p->gc_cliprgn || !p->gc_visrgn) {
        free(p->vram);
        free(p->vrom);
        free(p->sram);
        free(p->dram);
        free(p->regs);
        free(p->gc_clipmask);
        free(p->gc_blitmask);
        free(p->gc_cliprgn);
        free(p->gc_visrgn);
        free(p);
        return -1;
    }

    if (!load_vrom(p))
        LOG(0, "no 8•24 GC declaration ROM found (machine.vrom.load a GC vROM, "
               "or place one under /opfs/images/vrom/ or tests/data/roms/); "
               "declaration ROM is zero-filled");

    card->declrom = p->vrom;
    card->declrom_size = p->vrom_size;

    // Default monitor: 640×480 (multisync), or a pending pick.  The seeded
    // depth persists in priv (not just PRAM) so set_poweron_defaults restores
    // it across every /RESET — the guest's boot-time mode programming (the
    // direct MFB/ACDC path) isn't decoded, and the PRAM seed tells the driver
    // to bring the screen up at exactly this depth.
    p->sense_code = 6;
    p->display.width = 640;
    p->display.height = 480;
    p->seeded_bpp = 1;
    if (seeded_monitor) {
        p->sense_code = seeded_monitor->sense_code;
        p->display.width = seeded_monitor->width;
        p->display.height = seeded_monitor->height;
        p->seeded_bpp = seeded_depth_bpp;
    }
    set_poweron_defaults(p);

    card->priv = p;

    // --- Standard slot space: VRAM, framebuffer alias, declrom, display regs.
    memory_map_host_region(cfg->mem_map, "gc824_vram", p->vram, p->slot_base, GC824_VRAM_SIZE, /*writable*/ true);
    memory_map_host_region_alias(cfg->mem_map, p->slot_base + GC824_FB_ALIAS_OFFSET, p->slot_base);
    memory_map_host_region(cfg->mem_map, "gc824_declrom", p->vrom, p->slot_base + GC824_DECLROM_BUS_OFFSET,
                           GC824_DECLROM_BUS_SIZE, /*writable*/ false);
    p->ctx_jmfb = (gc_reg_ctx_t){.p = p, .region_base = p->slot_base + GC824_JMFB_BLOCK_OFFSET};
    memory_map_add(cfg->mem_map, p->slot_base + GC824_JMFB_BLOCK_OFFSET, GC824_REGISTER_SIZE, "gc824_jmfb_regs",
                   &s_gc824_mem_iface, &p->ctx_jmfb);
    // GCQD command-block window (standard slot, card+$16C+$8C00) — a device
    // region so CB writes fire the trigger engine; added after the FB alias so
    // its page-table entries win over the alias for the (unused) pages it spans.
    p->ctx_gcp = (gc_reg_ctx_t){.p = p, .region_base = p->gcp_base};
    memory_map_add(cfg->mem_map, p->gcp_base, GC824_GCP_WINDOW, "gc824_gcp", &s_gc824_mem_iface, &p->ctx_gcp);

    // --- Super-slot space: one catch-all device region over the whole
    // 256 MB slot super-space.  The driver reaches the accelerator (SRAM,
    // DRAM+comm regions, control registers) here in 32-bit mode; the single
    // dispatcher (gc_read/gc_write) decodes SRAM (+ instruction alias), the
    // control-register page, the config register, and DRAM (bank 0 + mirror)
    // from the card-local offset, and drives the bring-up/RPC state machine
    // on comm-region writes.  A device region (not host) is required so those
    // writes are observed; SRAM/DRAM are backed by real buffers so the guest
    // dereferences card addresses faithfully (proposal §3.2).
    p->ctx_super = (gc_reg_ctx_t){.p = p, .region_base = p->super_base};
    memory_map_add(cfg->mem_map, p->super_base, 0x10000000u, "gc824_super", &s_gc824_mem_iface, &p->ctx_super);

    // Seed PRAM for the picked video mode (mirrors jmfb.c / 24AC).
    if (seeded_monitor && seeded_depth_bpp > 0) {
        rtc_t *rtc = system_rtc();
        if (rtc) {
            uint8_t spDepth = spdepth_for_bpp(seeded_depth_bpp);
            rtc_pram_write(rtc, 0x0C, 0x4E); // 'NuMc' XPRAM validity signature
            rtc_pram_write(rtc, 0x0D, 0x75);
            rtc_pram_write(rtc, 0x0E, 0x4D);
            rtc_pram_write(rtc, 0x0F, 0x63);
            static const uint8_t pram_init_tbl[] = {
                0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xDF, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            for (size_t i = 0; i < sizeof(pram_init_tbl); i++)
                rtc_pram_write(rtc, (uint8_t)(0x76 + i), pram_init_tbl[i]);
            uint8_t off = (uint8_t)(0x46 + (card->slot - 9) * 8);
            rtc_pram_write(rtc, off + 0, 0x00);
            rtc_pram_write(rtc, off + 1, 0x2C); // BoardID = $2C (8•24 GC)
            rtc_pram_write(rtc, off + 2, spDepth);
            rtc_pram_write(rtc, off + 3, seeded_monitor->srsrc_sister);
            rtc_pram_write(rtc, off + 4, seeded_monitor->srsrc_sister);
            rtc_pram_write(rtc, off + 5, 0x00);
            rtc_pram_write(rtc, off + 6, 0x00);
            rtc_pram_write(rtc, off + 7, 0x00);
            LOG(1, "seeded slot-%d PRAM for '%s' (spDepth=$%02x sister=$%02x)", card->slot, seeded_monitor->id, spDepth,
                seeded_monitor->srsrc_sister);
        }
    }

    return 0;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    display_card_824gc_priv_t *p = card->priv;
    if (!p)
        return;
    free(p->vram);
    // p->vrom is published as card->declrom; nubus_delete owns and frees it.
    free(p->sram);
    free(p->dram);
    free(p->regs);
    free(p->gc_clipmask);
    free(p->gc_blitmask);
    free(p->gc_cliprgn);
    free(p->gc_visrgn);
    gc824_font_caches_flush(p);
    gc824_pixpats_flush(p, 0);
    free(p->vrom_path);
    free(p);
    card->priv = NULL;
}

static void card_reset(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    display_card_824gc_priv_t *p = card->priv;
    if (!p)
        return;
    nubus_deassert_irq(card);
    set_poweron_defaults(p);
    LOG(2, "/RESET -> power-on state");
}

static void card_on_vbl(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    display_card_824gc_priv_t *p = card->priv;
    if (!p)
        return;
    if (!(p->sw_ic_reg & GC824_VINT_DISABLE) || p->vbl_enabled)
        nubus_assert_irq(card);
    // Heartbeat + ms-tick counter (the driver watchdogs these when it spins).
    if (p->booted) {
        dram_set_be32(p, GC824_DRAM_CB + GC824_CB_HEARTBEAT, dram_be32(p, GC824_DRAM_CB + GC824_CB_HEARTBEAT) + 1);
        dram_set_be32(p, GC824_DRAM_PUBLICOU + GC824_PO_MSTICKS,
                      dram_be32(p, GC824_DRAM_PUBLICOU + GC824_PO_MSTICKS) + 16);
    }
    p->display.fb_dirty = true;
}

static display_t *card_display(nubus_card_t *card) {
    display_card_824gc_priv_t *p = card->priv;
    return p ? &p->display : NULL;
}

static const char *card_name(const nubus_card_t *card) {
    (void)card;
    return "Apple Macintosh Display Card 8\xe2\x80\xa2"
           "24 GC";
}

static const nubus_card_ops_t display_card_824gc_ops = {
    .init = card_init,
    .teardown = card_teardown,
    .reset = card_reset,
    .on_vbl = card_on_vbl,
    .display = card_display,
    .name = card_name,
};

// === Factory + kind descriptor ==============================================

static nubus_card_t *factory(int slot, config_t *cfg, checkpoint_t *cp) {
    nubus_card_t *card = calloc(1, sizeof(*card));
    if (!card)
        return NULL;
    card->ops = &display_card_824gc_ops;
    card->slot = slot;
    if (card->ops->init(card, cfg, cp) != 0) {
        free(card);
        return NULL;
    }
    return card;
}

// Advertised modes.  Seedable BOOT depths only: the ROM Slot Manager keeps
// the 24-bit sResource family (capped at 8 bpp) until SecondaryInit swaps in
// the six-depth $A0 family, so 16/32 bpp can never be the boot depth on
// System 6 — they are reached at runtime via Monitors (cscSetMode → VidComm),
// exactly like real hardware.  Sense codes follow the JMFB family.
static const int display_card_824gc_depths[] = {1, 2, 4, 8, 0};
// Monitor ids are prefixed "gc_" so they don't collide with the 24AC's
// "rgb_*" ids when nubus.video_mode routes a pick to the matching card.
static const nubus_monitor_t display_card_824gc_monitors[] = {
    {.id = "gc_640x480",
     .name = "13\" AppleColor (640×480)",
     .width = 640,
     .height = 480,
     .depths = display_card_824gc_depths,
     .sense_code = 6,
     // The GC's slot-PRAM bytes 3/4 hold the CONFIG CODE (low 3 bits), not an
     // sRsrc id: PrimaryInit ($1DB2-$1E1E) compares the saved low 3 bits
     // against the detected config and RESETS spDepth to $80 on mismatch.
     // $80's low bits are 0 = config 0 ✓, so the seeded depth survives.
     .srsrc_sister = 0x80},
    {.id = "gc_832x624",
     .name = "16\" (832×624)",
     .width = 832,
     .height = 624,
     .depths = display_card_824gc_depths,
     .sense_code = 6,
     .srsrc_sister = 0x81},
    {0},
};

const nubus_card_kind_t display_card_824gc_kind = {
    .id = "824gc",
    .display_name = "Apple Macintosh Display Card 8\xe2\x80\xa2"
                    "24 GC",
    .attach = CARD_ATTACH_NUBUS,
    .requires_vrom = true,
    .monitors = display_card_824gc_monitors,
    .factory = factory,
};

// === Video-mode selection ===================================================

void display_card_824gc_pending_video_mode_set(const char *id) {
    if (!id || !*id) {
        s_pending_video_mode_id[0] = '\0';
        return;
    }
    snprintf(s_pending_video_mode_id, sizeof s_pending_video_mode_id, "%s", id);
}

const char *display_card_824gc_pending_video_mode_get(void) {
    return s_pending_video_mode_id[0] ? s_pending_video_mode_id : NULL;
}

bool display_card_824gc_video_mode_lookup(const char *id, const nubus_monitor_t **out_monitor, int *out_depth_bpp) {
    if (!id || !*id)
        return false;
    const char *underscore_bpp = strrchr(id, '_');
    if (!underscore_bpp)
        return false;
    size_t mon_len = (size_t)(underscore_bpp - id);
    if (mon_len == 0 || mon_len >= 32)
        return false;
    char mon_id[32];
    memcpy(mon_id, id, mon_len);
    mon_id[mon_len] = '\0';
    const char *bpp_str = underscore_bpp + 1;
    char *end = NULL;
    long bpp = strtol(bpp_str, &end, 10);
    if (!end || end == bpp_str || strcmp(end, "bpp") != 0)
        return false;
    if (bpp < 1 || bpp > 32)
        return false;
    for (const nubus_monitor_t *m = display_card_824gc_monitors; m->id; m++) {
        if (strcmp(m->id, mon_id) != 0)
            continue;
        if (!m->depths)
            return false;
        for (const int *d = m->depths; *d; d++) {
            if ((int)bpp == *d) {
                if (out_monitor)
                    *out_monitor = m;
                if (out_depth_bpp)
                    *out_depth_bpp = (int)bpp;
                return true;
            }
        }
        return false;
    }
    return false;
}

// === Accelerator introspection (object model) ===============================

bool display_card_824gc_is_card(const nubus_card_t *card) {
    return card && card->ops == &display_card_824gc_ops;
}
const char *display_card_824gc_state(const nubus_card_t *card) {
    if (!display_card_824gc_is_card(card))
        return "";
    const display_card_824gc_priv_t *p = card->priv;
    return p ? state_name(p->state) : "";
}
uint32_t display_card_824gc_cb_addr(const nubus_card_t *card) {
    if (!display_card_824gc_is_card(card))
        return 0;
    const display_card_824gc_priv_t *p = card->priv;
    return p ? p->cb_nubus : 0;
}
uint32_t display_card_824gc_seq(const nubus_card_t *card) {
    if (!display_card_824gc_is_card(card))
        return 0;
    const display_card_824gc_priv_t *p = card->priv;
    return p ? p->expected_seq : 0;
}
uint32_t display_card_824gc_lastfunc(const nubus_card_t *card) {
    if (!display_card_824gc_is_card(card))
        return 0;
    const display_card_824gc_priv_t *p = card->priv;
    return p ? p->last_func : 0;
}
uint64_t display_card_824gc_rpc_count(const nubus_card_t *card) {
    if (!display_card_824gc_is_card(card))
        return 0;
    const display_card_824gc_priv_t *p = card->priv;
    return p ? p->rpc_count : 0;
}
uint64_t display_card_824gc_queue_bytes(const nubus_card_t *card) {
    if (!display_card_824gc_is_card(card))
        return 0;
    const display_card_824gc_priv_t *p = card->priv;
    return p ? p->queue_bytes : 0;
}
bool display_card_824gc_gc_on(const nubus_card_t *card) {
    if (!display_card_824gc_is_card(card))
        return false;
    const display_card_824gc_priv_t *p = card->priv;
    return p ? p->gc_on : false;
}
int32_t display_card_824gc_error(const nubus_card_t *card) {
    if (!display_card_824gc_is_card(card))
        return 0;
    const display_card_824gc_priv_t *p = card->priv;
    return p ? p->error : 0;
}
bool display_card_824gc_force_decline(const nubus_card_t *card) {
    if (!display_card_824gc_is_card(card))
        return false;
    const display_card_824gc_priv_t *p = card->priv;
    return p ? p->force_decline : false;
}
void display_card_824gc_set_force_decline(nubus_card_t *card, bool v) {
    if (!display_card_824gc_is_card(card))
        return;
    display_card_824gc_priv_t *p = card->priv;
    if (p)
        p->force_decline = v;
}
