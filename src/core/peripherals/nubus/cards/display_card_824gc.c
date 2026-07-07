// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// display_card_824gc.c
// "Apple Macintosh Display Card 8•24 GC" ("Dolphin") — HLE ("option B").  See
// display_card_824gc.h, the proposal
// local/gs-docs/proposals/proposal-8-24gc-hle-acceleration.md, and the dossier
// under local/gs-docs/8-24GC/ (protocol §§3-9/14, driver §§2-6, kernel §§1/7).
//
// Stage 0/1 scope (this file): the card presents the genuine v1.1 declaration
// ROM (BoardId $2C), its JMFB-family display half boots a desktop, and the
// accelerator bring-up state machine makes the `.GraphAccel` driver believe a
// live Am29000 card booted — SRAM/DRAM windows, alive registers, the ACEFload
// byte sink, the boot handshake, CB arming, the RPC (doorbell) transport for
// the trivial/bring-up funcs, and the per-VBL heartbeat.  Drawing funcs and
// the DrawMultiObject queue *decline* (proposal §4 safety net): QuickDraw's own
// ROM path renders every primitive, so the desktop is pixel-correct while the
// accelerator's own rasterizers (stage 2, gcqd/) are still a follow-up.
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

LOG_USE_CATEGORY_NAME("gc824");

// === Bring-up state (for the object model + logging) ========================
typedef enum {
    GC_ST_RESET = 0, // power-on; no firmware
    GC_ST_BOOTED, // boot handshake done; CB published
    GC_ST_ARMED, // host armed the CB (reply mailbox + arm magic)
    GC_ST_ON, // acceleration kicked on (Control $0D / firmware kick)
    GC_ST_ERROR, // a hard bring-up gate failed
} gc_state_t;

// === Per-card private state =================================================

typedef struct display_card_824gc_priv display_card_824gc_priv_t;

// A device-region callback context: binds the card to the absolute physical
// base of the region it was registered for, so one memory_interface_t serves
// every accelerator/display region and dispatches on the full physical addr.
typedef struct {
    display_card_824gc_priv_t *p;
    uint32_t region_base; // absolute physical base of this region
} gc_reg_ctx_t;

struct display_card_824gc_priv {
    nubus_card_t *card; // back-pointer for IRQ helpers
    uint32_t slot_base; // standard slot space base ($Fs000000)
    uint32_t super_base; // super-slot space base ($s0000000)

    // --- Display half (standard slot space) ---
    uint8_t *vram; // GC824_VRAM_SIZE
    uint8_t *vrom; // GC824_DECLROM_BUS_SIZE
    char *vrom_path;
    uint32_t vrom_size;
    rgba8_t clut[256];
    display_t display;
    // JMFB-family register shadows (subset the video driver drives).
    uint16_t jmfb_csr;
    uint16_t jmfb_video_base;
    uint16_t jmfb_row_words;
    uint16_t sw_ic_reg; // Stopwatch interrupt/control (bit1 = VINT disable)
    uint16_t sw_status_reg;
    uint16_t clut_pbcr;
    uint8_t clut_idx;
    uint8_t clut_phase; // 0=R,1=G,2=B; reset on index write
    uint32_t clut_pending[3];
    uint16_t clut_long_hi;
    uint8_t sense_code; // 3-bit monitor sense the card reports

    // --- Accelerator (super-slot space) ---
    uint8_t *sram; // GC824_SRAM_SIZE (firmware code sink; plain RAM)
    uint8_t *dram; // GC824_DRAM_SIZE (firmware data + comm regions)
    uint8_t *regs; // GC824_REGS_SIZE — MFB / config / ACDC register space
                   // (card-local 0x04000000..0x07FFFFFF), backed as RAM so the
                   // video driver's write-then-read-back register probes and
                   // ACDC CLUT accesses behave; the few semantic registers
                   // (alive, MFB heartbeat, attach, kick) are intercepted.

    gc_state_t state;
    bool booted; // boot handshake completed (CB published)
    bool armed; // CB armed
    bool attached; // Control $05 attach observed ($04000028 = -1)
    bool gc_on; // Control $0D observed ($04000050 kick)
    uint32_t mailbox; // host reply-mailbox physical address (CB+0x0C)
    uint32_t cb_nubus; // published NuBus(CB) address
    uint32_t expected_seq; // next RPC sequence expected
    uint32_t last_func; // last dispatched func code
    uint64_t rpc_count; // total RPCs serviced
    uint32_t queue_ack; // Transport-B bytes consumed so far
    uint64_t queue_bytes; // total Transport-B bytes drained
    int32_t error; // last posted error (0 = none)
    uint32_t mfb_sync; // MFB 0x44001C0 bus-side-effect toggle (value discarded)
    uint32_t sync_hb; // read counter for the 0x4C00000 video heartbeat (bit 31)

    // Region contexts.
    gc_reg_ctx_t ctx_jmfb; // slot+$200000 display registers (standard slot)
    gc_reg_ctx_t ctx_super; // whole super-slot space (SRAM/DRAM/ctl/comm)
};

// === DRAM big-endian accessors ==============================================
// The comm protocol is big-endian longwords; the DRAM buffer holds them
// natively.  These raw accessors do NOT run the trigger check (the state
// machine uses them to write its own responses back).
static uint32_t dram_be32(display_card_824gc_priv_t *p, uint32_t off) {
    if (off + 4 > GC824_DRAM_SIZE)
        return 0;
    return LOAD_BE32(p->dram + off);
}
static void dram_set_be32(display_card_824gc_priv_t *p, uint32_t off, uint32_t val) {
    if (off + 4 > GC824_DRAM_SIZE)
        return;
    STORE_BE32(p->dram + off, val);
}

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
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_ARGSOFF, p->cb_nubus + GC824_CB_ARGSAREA);
    // Queue-buffer one-block free list (protocol §9.2): {size, next=0}.
    uint32_t free_base = p->cb_nubus + GC824_CB_FREEAREA;
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_FREEPTR, free_base);
    uint32_t cb_local = GC824_DRAM_OFFSET + GC824_DRAM_CB; // card-local 0x0C007000
    uint32_t free_size = (0x0C00FFFCu - (cb_local + GC824_CB_FREEAREA) - 8u);
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_FREEAREA + 0, free_size);
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_FREEAREA + 4, 0);

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
        dram_set_be32(p, GC824_DRAM_CB + GC824_CB_ARGSOFF, p->cb_nubus + GC824_CB_ARGSAREA);
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
        dram_set_be32(p, GC824_DRAM_CB + 0x604 + 4, p->cb_nubus); // ctx token (non-zero)
        LOG(2, "func $17 PQDInit: ScrnBase=$%08x -> ctx=$%08x", scrnbase, p->cb_nubus);
        result = 0;
        break;
    }
    case 0x26: // submit / checkpoint — the queue is already drained
    case 0x38:
        dram_set_be32(p, GC824_DRAM_CB + GC824_CB_QUEUE_ACK, dram_be32(p, GC824_DRAM_CB + GC824_CB_QUEUE_PUB));
        result = 0;
        break;
    // --- Drawing surface: decline to the ROM path (stage 1 safety net) ---
    case 0x15: // StretchBits — any status > 0 declines (proposal §3.8)
        result = 9; // firmware's own "decline" status
        LOG(3, "func $15 StretchBits declined (stage 1) -> ROM blit");
        break;
    case 0x2D: // SetPort / DrawMultiObject — decline (stage 2 interpreter)
        result = 9;
        LOG(3, "func $2D DrawMultiObject declined (stage 1) -> ROM");
        break;
    case 0x30: // FontDownload — returning 0 declines text cleanly (proposal §3.10)
        result = 0;
        LOG(3, "func $30 FontDownload declined (stage 1) -> ROM text");
        break;
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
        p->expected_seq = (func == 1) ? 1 : seq + 1;
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

// Transport B (CB+0x1C0 bytes published): drain the opcode stream.  Stage 1
// declines to the ROM path, so we mark it consumed (QDDone answers "done") and
// log — a live stream here means GCQD engaged the async queue, which stage 2's
// interpreter will service.
static void gc_drain_queue(display_card_824gc_priv_t *p) {
    uint32_t pub = dram_be32(p, GC824_DRAM_CB + GC824_CB_QUEUE_PUB);
    if (pub == p->queue_ack)
        return;
    uint32_t n = pub - p->queue_ack;
    p->queue_bytes += n;
    p->queue_ack = pub;
    dram_set_be32(p, GC824_DRAM_CB + GC824_CB_QUEUE_ACK, pub);
    LOG(2, "Transport B: %u bytes published (drained, not interpreted — stage 1)", n);
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

static uint32_t gc_read(display_card_824gc_priv_t *p, uint32_t phys, unsigned width) {
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
    if (cl >= GC824_SRAM_OFFSET && cl < GC824_SRAM_OFFSET + GC824_SRAM_SIZE)
        return buf_read(p->sram, cl - GC824_SRAM_OFFSET, GC824_SRAM_SIZE, width);
    if (cl >= GC824_SRAM_IALIAS_OFFSET && cl < GC824_SRAM_IALIAS_OFFSET + GC824_SRAM_SIZE)
        return buf_read(p->sram, cl - GC824_SRAM_IALIAS_OFFSET, GC824_SRAM_SIZE, width);
    if (cl >= GC824_REGS_OFFSET && cl < GC824_REGS_OFFSET + GC824_REGS_SIZE) {
        switch (cl) {
        case GC824_REG_ALIVE_A:
        case GC824_REG_ALIVE_B:
        case GC824_REG_ALIVE_C:
            return 0x80000000u; // bit 31 set = board alive (all three => alive)
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
            return 0x06000000u;
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
    if (cl >= GC824_SRAM_OFFSET && cl < GC824_SRAM_OFFSET + GC824_SRAM_SIZE) {
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
        } else if (cl == GC824_REG_KICK && val == 0xFFFFFFFFu) {
            p->gc_on = true;
            if (p->state < GC_ST_ON)
                p->state = GC_ST_ON;
            LOG(1, "firmware kick ($04000050 = -1) -> GC ON");
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
static bool load_vrom(display_card_824gc_priv_t *p) {
    char *path = NULL;
    if (!declrom_load_vrom(GC824_VROM_FILENAME, GC824_DECLROM_CHIP_SIZE, p->vrom, GC824_DECLROM_BUS_SIZE, &path))
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
    p->clut_long_hi = 0;

    // The GC decl-ROM video driver brings the screen up at 8 bpp in the card
    // DRAM aperture (ScrnBase = NuBus 0x9C010000 = DRAM + 0x10000), not the
    // standard-slot VRAM.  Surface that as the host framebuffer.
    p->display.format = PIXEL_8BPP;
    p->display.stride = (p->display.width ? p->display.width : 640u); // 8 bpp: 1 byte/px
    p->display.bits = p->dram + GC824_FB_DRAM_OFFSET;
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
    if (!p->vram || !p->vrom || !p->sram || !p->dram || !p->regs) {
        free(p->vram);
        free(p->vrom);
        free(p->sram);
        free(p->dram);
        free(p->regs);
        free(p);
        return -1;
    }

    if (!load_vrom(p))
        LOG(0, "%s not found; declaration ROM is zero-filled", GC824_VROM_FILENAME);

    card->declrom = p->vrom;
    card->declrom_size = p->vrom_size;

    // Default monitor: 640×480 (multisync), or a pending pick.
    p->sense_code = 6;
    p->display.width = 640;
    p->display.height = 480;
    if (seeded_monitor) {
        p->sense_code = seeded_monitor->sense_code;
        p->display.width = seeded_monitor->width;
        p->display.height = seeded_monitor->height;
    }
    set_poweron_defaults(p);
    if (seeded_monitor) {
        pixel_format_t f = format_for_bpp(seeded_depth_bpp);
        p->display.format = f;
        p->jmfb_row_words = (uint16_t)(p->display.width / 32u);
        recompute_stride(p);
    }

    card->priv = p;

    // --- Standard slot space: VRAM, framebuffer alias, declrom, display regs.
    memory_map_host_region(cfg->mem_map, "gc824_vram", p->vram, p->slot_base, GC824_VRAM_SIZE, /*writable*/ true);
    memory_map_host_region_alias(cfg->mem_map, p->slot_base + GC824_FB_ALIAS_OFFSET, p->slot_base);
    memory_map_host_region(cfg->mem_map, "gc824_declrom", p->vrom, p->slot_base + GC824_DECLROM_BUS_OFFSET,
                           GC824_DECLROM_BUS_SIZE, /*writable*/ false);
    p->ctx_jmfb = (gc_reg_ctx_t){.p = p, .region_base = p->slot_base + GC824_JMFB_BLOCK_OFFSET};
    memory_map_add(cfg->mem_map, p->slot_base + GC824_JMFB_BLOCK_OFFSET, GC824_REGISTER_SIZE, "gc824_jmfb_regs",
                   &s_gc824_mem_iface, &p->ctx_jmfb);

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
    if (!(p->sw_ic_reg & GC824_VINT_DISABLE))
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

// Advertised modes.  The GC's declaration ROM carries a rich monitor table;
// stage 0 exposes the common Apple RGB modes at 1/2/4/8 bpp (the accelerated
// depths mirror the shipped configs).  Sense codes follow the JMFB family.
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
