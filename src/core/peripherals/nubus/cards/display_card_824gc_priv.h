// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// display_card_824gc_priv.h
// Internal (card-private) header shared by the two halves of the 8•24 GC
// model: display_card_824gc.c (the card shell — decl ROM, display registers,
// NuBus dispatch, CB bring-up and RPC transport) and display_card_824gc_qd.c
// (the "GC QuickDraw" drawing engine — the C reimplementation of the drawing
// work the card's Am29000 firmware performed).  Nothing outside the card
// model includes this; the public surface stays in display_card_824gc.h.

#ifndef DISPLAY_CARD_824GC_PRIV_H
#define DISPLAY_CARD_824GC_PRIV_H

#include "display_card_824gc.h"

#include "display.h"
#include "memory.h"
#include "nubus.h"

#include <stdbool.h>
#include <stdint.h>

// Clip/blit mask geometry (1 bit per pixel over the largest screen mode) and
// the cap on a stored/fetched QuickDraw region — shared by the engine's
// rasterizers and the shell's allocations.
#define GC824_CLIP_STRIDE 80
#define GC824_CLIP_ROWS   480
#define GC824_RGN_MAX     4096

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
    uint32_t gcp_base; // GCQD "gcp" CB window (standard slot; = card+$16C+$8C00)

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
    int seeded_bpp; // boot depth from machine.nubus.video_mode (default 1).
                    // The decl-ROM driver programs the boot mode at Open from
                    // the slot PRAM (which the same seed wrote), so the model's
                    // power-on format must match it across /RESET — the direct
                    // (no-VidComm) programming path isn't decoded.  Runtime
                    // depth switches arrive via VidComm (see gc_vidcomm).
    // ACDC RAMDAC palette load state (GC decl-ROM SetEntries path).
    uint8_t acdc_addr; // current palette index (auto-increments per entry)
    uint8_t acdc_phase; // 0=R,1=G,2=B; reset on ADDR-port write
    uint8_t acdc_rgb[3]; // R,G,B bytes accumulated for the current entry

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
    uint32_t queue_base; // gcp address of the queue buffer (carved from free list)
    uint64_t queue_bytes; // total Transport-B bytes drained
    int32_t error; // last posted error (0 = none)
    uint32_t mfb_sync; // MFB 0x44001C0 bus-side-effect toggle (value discarded)
    uint32_t sync_hb; // read counter for the 0x4C00000 video heartbeat (bit 31)
    // Am29000 serial command latch ($04C00000): the decl-ROM driver bit-bangs
    // 12-bit frames into it (serialWrite $39C6: one long write per bit,
    // bit 31, MSB first).  Command 1 = run -> slot VBL generation on
    // (enableCardVBL $304A); 3 = idle/stop -> off (disableCardVBL $2FFA,
    // also PrimaryInit's icache echo-fill terminator).
    uint16_t risc_cmd_shift;
    int risc_cmd_bits;
    bool vbl_enabled; // slot VBL armed by RISC command 1

    // --- Stage 2: DrawMultiObject interpreter state (proposal §3.7) ---
    // The GCQD marshaller stages drawing as a queue of opcode records (§10.1);
    // when func $2D (SetPort) is accepted the card interprets them.  State
    // opcodes set these fields; the next primitive resolves them.
    uint8_t gc_pat[4][8]; // pattern slots 1=pnPat 2=bkPat 3=fillPat (opPattern $71)
    int16_t gc_align_x, gc_align_y; // patAlign (op $75 which=1): pattern phase
    uint8_t gc_pat_kind[4]; // 0 = classic 1-bit; 2 = RGB 2x2 dither (op $74);
                            // 3 = cached PixPat tile (op $72)
    uint32_t gc_pat_cell[4][4]; // RGBPat resolved device pixels, cell 0..3
    // Type-5 cache: PixPats downloaded via func $0C (CachePixPat), keyed by
    // the HOST PixPat Handle (the card keys its cache the same way — the
    // host passes no card address and consumes none back).  The tile is
    // expanded at cache time (PATCONVERT): every pixel resolved through the
    // pattern's OWN ColorTable to a device pixel at the CURRENT screen depth.
    struct gc_pixpat {
        uint32_t key; // PixPat host Handle; 0 = free
        uint16_t w, h; // tile bounds (powers of two — enforced at cache time)
        uint8_t fmt; // pixel_format_t the tile was resolved at
        uint32_t *pix; // w*h device pixels, row-major
    } gc_pixpats[4];
    int gc_pixpat_rr;
    uint8_t gc_pat_pp[4]; // active gc_pixpats index per slot (kind 3)
    uint8_t gc_pat_slot; // active pattern slot selected by opWhichPat ($73)
    uint16_t gc_mode; // current transfer mode (opMode $69); 8 = patCopy
    uint32_t gc_fg, gc_bg; // fg/bg PIXEL VALUES resolved from the RGBs of ops
                           // $64/$66 and $6B/$6D — bit 0 meaningful at 1 bpp,
                           // the low byte at 8 bpp
    uint32_t gc_hilite; // hilite PIXEL (op $70 rgbHiliteColor; default low-mem
                        // HiliteRGB $0DA0) — the $32/$3A bk↔hilite swap
    uint32_t gc_op_color; // opColor PIXEL (op $70 rgbOpColor, resolved)
    uint16_t gc_op_rgb[3]; // opColor raw RGB — the blend weight / pin value
                           // for the arithmetic modes (pin EQU weight)
    int16_t gc_pen_x, gc_pen_y; // pen location (opPenLoc $68), GLOBAL coords
    uint16_t gc_pen_hfrac; // pnLocHFrac (op $68 +2; default $8000) — the pen's
                           // 16.16 fraction, threaded through text runs
    int16_t gc_org_x, gc_org_y; // the accepted port's local→global origin:
                                // global = local − portBits.bounds.topLeft
                                // (staged at CB+$170+$14; 0 for the WMgr port)
    uint32_t gc_port_ptr; // thePort (staged at CB+$170+0) — the card re-reads
                          // the LIVE port bounds at each drain: GCQD flushes
                          // (func $26) BEFORE SetOrigin changes them, so a
                          // drained batch is single-origin, but it never
                          // re-stages the port record ($23AC)
    int16_t gc_pen_w, gc_pen_h; // pen size (opPenSize $6E)

    // --- Text (func $30 FontDownload + ops $67/$06; proposal §3.10) ---
    // The host downloads font data into card caches: type 8 = strikes (raw
    // FontRec/NFNT bytes, keyed by the host handle), type 10 = Font-Manager
    // width tables (0x434 bytes: 256 Fixed advances + a trailer holding the
    // strike handle at +$410 and the gc24 checksum at +$432).  op $67 selects
    // the current font from the caches; op $06 renders with it.
    struct gc_cache_ent {
        uint32_t key; // host handle (the cache key); 0 = empty
        uint8_t *data;
        uint32_t size;
    } gc_fonts[8], gc_wtabs[4]; // type 8 / type 10
    int gc_font_rr, gc_wtab_rr; // round-robin replacement cursors
    uint8_t *gc_cur_wt; // current width table (points into gc_wtabs)
    uint8_t *gc_cur_strike; // current strike (points into gc_fonts)
    uint32_t gc_cur_strike_size;
    uint8_t gc_font_info[26]; // op $67 font-info block (style/scale fields)

    int16_t gc_clip_t, gc_clip_l, gc_clip_b, gc_clip_r; // clip ∩ vis bounding box
    uint8_t *gc_clipmask; // 1 bit/pixel drawable mask (clipRgn ∩ visRgn), stride 80
    // The CURRENT clip and vis region records (raw QD region bytes) — each
    // op $6A/$6C REPLACES its region (QuickDraw semantics), so the effective
    // mask is rebuilt as clip ∩ vis, not intersected cumulatively.
    uint8_t *gc_cliprgn, *gc_visrgn; // 4 KB each; length 0 = wide open
    uint32_t gc_cliprgn_len, gc_visrgn_len;
    int16_t gc_rgn_ox, gc_rgn_oy; // port origin the stored regions carry
    uint8_t *gc_blitmask; // 1 bit/pixel per-blit mask (func $15 rgnA∩rgnB∩rgnC)
    uint64_t draw_count; // primitives rasterized (introspection)
    bool gc_accel; // func $2D accepted the port → interpret its queue (stage 2)
    bool force_decline; // harness switch (gc.force_decline): decline the drawing
                        // funcs ($2D/$15/$30) so the ROM path renders everything —
                        // the differential test oracle (proposal §4.1).  Not guest
                        // state: survives /RESET, cleared only at card_init.

    // Region contexts.
    gc_reg_ctx_t ctx_jmfb; // slot+$200000 display registers (standard slot)
    gc_reg_ctx_t ctx_super; // whole super-slot space (SRAM/DRAM/ctl/comm)
    gc_reg_ctx_t ctx_gcp; // GCQD CB window (standard slot; aliases DRAM CB)
};

// === DRAM big-endian accessors ==============================================
// The comm protocol is big-endian longwords; the DRAM buffer holds them
// natively.  These raw accessors do NOT run the trigger check (the state
// machine uses them to write its own responses back).
static inline uint32_t dram_be32(display_card_824gc_priv_t *p, uint32_t off) {
    if (off + 4 > GC824_DRAM_SIZE)
        return 0;
    return LOAD_BE32(p->dram + off);
}
static inline uint16_t dram_be16(display_card_824gc_priv_t *p, uint32_t off) {
    if (off + 2 > GC824_DRAM_SIZE)
        return 0;
    return LOAD_BE16(p->dram + off);
}
static inline void dram_set_be32(display_card_824gc_priv_t *p, uint32_t off, uint32_t val) {
    if (off + 4 > GC824_DRAM_SIZE)
        return;
    STORE_BE32(p->dram + off, val);
}

// === Engine API (display_card_824gc_qd.c) ===================================
// The card shell calls into the drawing engine through these entry points;
// everything else in the engine is file-local.

// Drain entry: interpret `count` bytes of DrawMultiObject opcode records
// starting at DRAM offset `base` (proposal §3.7 / protocol §10.1).
void gc824_interp(display_card_824gc_priv_t *p, uint32_t base, uint32_t count);
// func $15 StretchBits/CopyBits: rasterize the request block at CB+$58 if it
// is inside the accept envelope.  Returns 1 if drawn, 0 to decline (ROM path).
int gc824_stretchbits(display_card_824gc_priv_t *p);
// func $0C CachePixPat: expand + cache a PixPat (type-5 cache).  Returns the
// RPC result (1 = cached, 0 = decline the whole draw to the ROM original).
int gc824_cache_pixpat(display_card_824gc_priv_t *p);
// func $2C FlushPixPat (key = the host PixPat Handle; 0 = flush all).
void gc824_pixpats_flush(display_card_824gc_priv_t *p, uint32_t key);
// func $30 FontDownload.  Returns the RPC result (1 = cached, 0 = decline).
int gc824_font_download(display_card_824gc_priv_t *p);
// Flush the font caches (PQDInit, boot, teardown).
void gc824_font_caches_flush(display_card_824gc_priv_t *p);
// Reset the interpreter's per-cycle QuickDraw drawing state (queue reset).
void gc824_reset_draw_state(display_card_824gc_priv_t *p);

#endif // DISPLAY_CARD_824GC_PRIV_H
