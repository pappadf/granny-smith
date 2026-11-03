// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// qd3d.c
// The Apple QuickDraw 3D Accelerator Card (M4333LL/A, 1995): a PCI
// bus-master rendering co-processor for QuickDraw 3D's RAVE layer.  One
// 343S1147 PCI bridge, one 343S1167 scanline renderer, a 128 KB object
// cache and a 512 KB texture SRAM -- and no frame buffer.  The card reads
// a frame's geometry out of host RAM, renders it a scanline at a time, and
// writes the finished pixels back over PCI into the screen's VRAM or a
// host back buffer.  Apple's driver and RAVE engine ("Apple QD3D HW Driver",
// an ndrv matching `pci106b,9`, and "Apple QD3D HW Plug-In") ship as
// extensions; the card carries no expansion ROM.
//
// The model is behavioural: the guest software never reads back any
// intermediate state, so what has to be right is the interface -- the
// three host-visible registers, the packet parser, the control program the
// renderer walks, the pixel-output descriptor list and the interrupt --
// and the pixels that come out.  How the ASIC gets there (16-pixel
// segments, its object cache, layer overflow and re-rendering) is not
// modelled.
//
// === The host interface ======================================================
//
// One 32-bit memory BAR; the driver touches three words, always as 32-bit
// big-endian accesses with no byte swapping:
//
//   +$00  PORT        write-only packet stream into the renderer's internal
//                     address space (see "Packets")
//   +$40  DMACONTROL  $8000 RUN, $4000 interrupt enable, $2000 interrupt
//                     acknowledge (write 1), $1000 reset, $0C00 16-bpp output
//   +$4C  DMACMDPTR   host-physical address of the pixel-output descriptor list
//
// Config register $40 is a vendor register the driver writes with
// `$F0 | (numCards-1) << 2 | cardIndex`: the card's position among up to
// four cooperating cards, which split a frame by interleaving scanlines.
//
// === Packets =================================================================
//
// The port and the control program share one encoding; the opcode is bits
// 31..29 of a header word, the rest a byte address in the internal space:
//
//   1  $2000_0000 | addr     one data word (none for the strobes $28/$2C)
//   2  $4000_0000 | addr     count-1, then count words (block write)
//   3  $6...                 a renderable object (control programs only)
//
// Internal space: renderer registers at $000000-$0000FF, the object cache
// at $400000-$41FFFF, texture SRAM from $800000 (4 bytes per texel).
//
// === A frame =================================================================
//
// The driver resets the channel (DMACONTROL $1000, then 0), uploads
// textures and writes the background, CSG table and texture registers
// through the port, writes the mode register ($00), DMACMDPTR, then
// DMACONTROL with RUN|IE, and finally the control program's physical
// address to register $20 -- which starts the frame.  The renderer walks
// the program by bus mastering: a singly linked list of control blocks
// (register writes and ScanlineStart/ScanlineEnd strobes) and object
// records (triangles and bitmap tiles), each object linked in just before
// the scanline it first touches.  Each finished scanline streams through
// the descriptor list (WRITE / SKIP / STOP), and at STOP the card raises
// its interrupt.  The driver gives up after a 3 s watchdog.
//
// === Rendering ===============================================================
//
// This file is the card's host interface and the PRODUCER side of a raster
// seam shaped like the Voodoo2's (qd3d_raster.h): the walk of the control
// program, the object decode and every bus-master access stay here, and
// the walk becomes commands -- start a frame, add an object, render a
// scanline -- for the renderer in qd3d_raster.c, which runs inline (sw,
// normative), on a worker thread (thread, the default) or not at all
// (null).  A frame's pixels reach host memory when the frame completes, at
// the time its size gives it, whichever backend drew them.

#include "checkpoint.h"
#include "log.h"
#include "machine_build_opts.h"
#include "object.h"
#include "pci.h"
#include "qd3d_raster.h"
#include "scheduler.h"
#include "system.h"
#include "system_internal.h"
#include "value.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("qd3d");

// === PCI identity ==========================================================
#define QD3D_VENDOR_ID 0x106Bu // Apple
#define QD3D_DEVICE_ID 0x0009u // the QuickDraw 3D Accelerator
// Revision 2: the driver grants 0x40000 texels of texture memory (and
// 16-bpp destinations) only to a revision-2 ASIC; every other revision
// gets 0x10000 texels.  The shipping cards are revision 2.
#define QD3D_REVISION 0x02u
#define QD3D_CLASS    0x0B4000u // processor / co-processor (the driver never checks)
// The BAR size is not visible to the driver (it reads only the base);
// 4 KB covers every offset it touches.
#define QD3D_BAR_SIZE 0x1000u

// Host-visible register offsets in the BAR.
#define QD3D_PORT       0x00u
#define QD3D_DMACONTROL 0x40u
#define QD3D_DMACMDPTR  0x4Cu

// DMACONTROL bits.
#define DMACTL_RUN   0x8000u
#define DMACTL_IE    0x4000u
#define DMACTL_ACK   0x2000u
#define DMACTL_RESET 0x1000u
#define DMACTL_16BPP 0x0C00u

// Config register $40: the card's position among cooperating cards.
#define QD3D_CFG_POSITION 0x40u

// === Internal address space ================================================
#define QD3D_NREGS       64u // $00-$FF, one word each
#define QD3D_OCACHE_BASE 0x400000u
#define QD3D_OCACHE_SIZE 0x20000u // 128 KB
#define QD3D_TEX_BASE    Q3_TEX_BASE
#define QD3D_TEX_TEXELS  Q3_TEX_TEXELS // revision 2: 2^18 texels

// Renderer registers (byte addresses).
#define REG_MODE       0x00u // (cardIndex << 8) | flags
#define REG_Y          0x04u // $04-$07: per-card (Ynext << 16) | Ycur, 13.3
#define REG_BACKGROUND 0x08u // ARGB8888
#define REG_TEXCONFIG  0x0Cu // (n256 << 8) | 128-map page offset
#define REG_GO         0x20u // control-program address; writing it starts a frame
#define REG_CSG        0x24u // 32-entry CSG truth table
#define REG_SCAN_START 0x28u // strobe: render the scanline RegY selects
#define REG_SCAN_END   0x2Cu // strobe: end of the scanline group
#define REG_NPIXELS_M1 0x30u // output width - 1
#define REG_FREELIST   0x34u // object-cache free-list head (0 = overflow mode)
#define REG_MAPBASE0   0x48u // $48-$68: base of mip level 0..8 (256 ... 1)
#define REG_TEXSLOT0   0x80u // $80-$BC: per-slot size class (3 = 256², 2 = 128²)

// Mode register flags.
#define MODE_DEST16      0x0008u // destination is 16 bpp
#define MODE_PERSPECTIVE 0x0800u // depth fields are 1/w, u/v are u/w, v/w

// Object header bits.
#define HDR_OPCODE_SHIFT 29
#define HDR_TEXTURED     0x02000000u
#define HDR_BITMAP       0x00800000u

// Output descriptor commands (bits 27..24 of word 0).
#define DESC_WRITE 0x0u
#define DESC_SKIP  0x1u
#define DESC_STOP  0x8u

// More zero-length output descriptors in a row than this is a list the
// channel has run off the end of.
#define QD3D_MAX_EMPTY_DESCS 65536u
// A program longer than this is a runaway walk through garbage.
#define QD3D_MAX_RECORDS 4000000u

// Packet-parser states.
enum { PORT_HEADER = 0, PORT_DATA, PORT_COUNT, PORT_BLOCK };

// The pixel-output DMA channel's progress through its descriptor list.
typedef struct qd3d_dma {
    uint32_t next; // physical address of the next descriptor
    uint32_t cmd, remaining, addr; // the descriptor being consumed
    uint8_t loaded, stopped;
} qd3d_dma_t;

typedef struct qd3d {
    // ---- checkpointed (plain data) ----
    uint32_t reg[QD3D_NREGS];
    uint32_t dmactl;
    uint32_t cmdptr;
    uint32_t cfg_position; // config register $40
    uint32_t port_state, port_addr, port_count;
    uint32_t irq; // the INTA line
    uint32_t irq_pending; // a frame in flight: its completion event armed
    uint32_t frame_cmdptr; // the in-flight frame's descriptor list (DMACMDPTR at go)
    uint32_t frames; // frames rendered
    uint32_t last_objects; // objects in the last frame
    uint32_t last_lines; // scanlines the last frame output
    uint8_t ocache[QD3D_OCACHE_SIZE];
    uint32_t tex[QD3D_TEX_TEXELS]; // RGB, quantised to 5-6-5
    // ---- runtime only ----
    pci_device_t *dev;
    config_t *cfg;
    memory_interface_t bar_if;
    qd3d_dma_t dma;
    q3_target_t tgt; // the renderer's own state (qd3d_raster.h)
    q3_raster_t *raster; // where the renderer runs: sw, null or thread
    uint64_t frame_pixels, frame_objects; // work of the last frame (pacing)
    uint32_t frame_lines, frame_records;
} qd3d_t;

// ============================================================
// Small helpers
// ============================================================

// Big-endian 32/16-bit loads from a byte buffer.
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static float f32_bits(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

// One host word over the bus (big-endian in host memory).
static uint32_t dma_word(qd3d_t *q, uint32_t phys) {
    uint8_t b[4];
    pci_dma_read(q->dev, phys, b, 4);
    return be32(b);
}

// This card's position among cooperating cards: index and count.
static uint32_t card_index(const qd3d_t *q) {
    return (q->reg[REG_MODE >> 2] >> 8) & 3u;
}

static uint32_t card_count(const qd3d_t *q) {
    // Until the driver has written the position register the card is alone.
    if ((q->cfg_position & 0xF0u) != 0xF0u)
        return 1;
    return ((q->cfg_position >> 2) & 3u) + 1u;
}

static void update_irq(qd3d_t *q, bool on) {
    if (on == (q->irq != 0))
        return;
    q->irq = on;
    if (on)
        pci_assert_irq(q->dev);
    else
        pci_deassert_irq(q->dev);
}

// ============================================================
// Pixel output DMA
// ============================================================

// Start the channel on the descriptor list at `list`.
static void dma_begin(qd3d_t *q, uint32_t list) {
    memset(&q->dma, 0, sizeof(q->dma));
    q->dma.next = list;
}

// Load the next descriptor; false once the list has stopped.
static bool dma_load(qd3d_t *q) {
    qd3d_dma_t *d = &q->dma;
    for (uint32_t empty = 0; !d->stopped && !d->loaded; empty++) {
        // Zero-length descriptors move nothing; a long run of them is a
        // list the channel has run off the end of.
        if (empty >= QD3D_MAX_EMPTY_DESCS) {
            LOG(1, "%u empty output descriptors in a row at $%08X: channel stopped", empty, d->next);
            d->stopped = 1;
            break;
        }
        uint8_t desc[16];
        pci_dma_read(q->dev, d->next, desc, sizeof(desc));
        d->next += 16;
        uint32_t w0 = be32(desc);
        d->cmd = (w0 >> 24) & 0xFu;
        d->remaining = w0 & 0xFFFFu;
        d->addr = be32(desc + 4);
        if (d->cmd == DESC_STOP) {
            d->stopped = 1;
            break;
        }
        if (d->cmd != DESC_WRITE && d->cmd != DESC_SKIP) {
            LOG(1, "bad output descriptor $%08X at $%08X: channel stopped", w0, d->next - 16);
            d->stopped = 1;
            break;
        }
        d->loaded = d->remaining != 0;
        LOG(5, "descriptor $%08X: cmd %u, %u bytes at $%08X", d->next - 16, d->cmd, d->remaining, d->addr);
    }
    return !d->stopped;
}

// Consume `len` bytes of the pixel stream: WRITE spans go to their
// address, SKIP spans are discarded, and anything past STOP is dropped.
static void dma_stream(qd3d_t *q, const uint8_t *bytes, size_t len) {
    qd3d_dma_t *d = &q->dma;
    while (len && dma_load(q)) {
        uint32_t n = len < d->remaining ? (uint32_t)len : d->remaining;
        if (d->cmd == DESC_WRITE)
            pci_dma_write(q->dev, d->addr, bytes, n);
        d->addr += n;
        d->remaining -= n;
        bytes += n;
        len -= n;
        if (!d->remaining)
            d->loaded = 0;
    }
}

// ============================================================
// Textures
// ============================================================

// Quantise a texel to the SRAM's 16-bit 5-6-5 format (alpha is not kept),
// expanded back to 8 bits per channel by bit replication.
static uint32_t texel_store(uint32_t argb) {
    uint32_t r = (argb >> 19) & 0x1Fu, g = (argb >> 10) & 0x3Fu, b = (argb >> 3) & 0x1Fu;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

// Resolve an object's 4-bit texture page to its slot and size: 256² maps
// occupy pages 0.. upward and slots equal to their page; 128² maps occupy
// pages 15.. downward, and register $0C's low byte turns their page into a
// slot after the 256² ones.  The per-slot class registers say which is which.
static bool tex_resolve(const qd3d_t *q, uint32_t page, int *slot, int *size) {
    uint32_t cfg = q->reg[REG_TEXCONFIG >> 2];
    uint32_t n256 = (cfg >> 8) & 0xFFu;
    int s = page < n256 ? (int)page : (int)((page - (cfg & 0xFFu)) & 0xFFu);
    if (s < 0 || s > 15)
        return false;
    uint32_t cls = q->reg[(REG_TEXSLOT0 >> 2) + (uint32_t)s] & 3u;
    if (cls != 3u && cls != 2u)
        return false;
    *slot = s;
    *size = cls == 3u ? 256 : 128;
    return true;
}

// ============================================================
// Object decode
// ============================================================

// Fit attribute planes f(x,y) = a0 + dx(x-x0) + dy(y-y0) through the three
// vertices.  False for a degenerate (zero-area) triangle.
static bool obj_planes(q3_obj_t *o, float v[3][Q3_A_NATTR], int nattr) {
    float x[3], y[3];
    for (int i = 0; i < 3; i++) {
        x[i] = (float)o->x13[i] / 8.0f;
        y[i] = (float)o->y13[i] / 8.0f;
    }
    float det = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (det == 0.0f)
        return false;
    o->x0 = x[0];
    o->y0 = y[0];
    for (int k = 0; k < nattr; k++) {
        float d1 = v[1][k] - v[0][k], d2 = v[2][k] - v[0][k];
        o->a0[k] = v[0][k];
        o->dx[k] = (d1 * (y[2] - y[0]) - d2 * (y[1] - y[0])) / det;
        o->dy[k] = (d2 * (x[1] - x[0]) - d1 * (x[2] - x[0])) / det;
    }
    return true;
}

// The scanlines a triangle covers: those whose centre lies in
// (yMin, yMax] -- the half-open rule that keeps abutting edges from
// drawing a scanline twice.
static bool obj_tri_range(q3_obj_t *o) {
    int ymin = o->y13[0], ymax = o->y13[0];
    for (int i = 1; i < 3; i++) {
        if (o->y13[i] < ymin)
            ymin = o->y13[i];
        if (o->y13[i] > ymax)
            ymax = o->y13[i];
    }
    o->first = (ymin + 4) >> 3;
    o->last = (ymax - 4) >> 3;
    return o->last >= o->first;
}

// A 20-bit float (sign, exponent, 11-bit mantissa) from its top 16 bits
// and the next 4.
static float f20(uint32_t top16, uint32_t nibble) {
    return f32_bits((top16 << 16) | ((nibble & 0xFu) << 12));
}

// Decode the object record whose header word is at `w` (the first payload
// byte after the link words) into `o`.  False when it draws nothing.
static bool obj_decode(qd3d_t *q, uint32_t w, uint32_t header, q3_obj_t *o) {
    uint8_t rec[68];
    pci_dma_read(q->dev, w, rec, sizeof(rec));
    memset(o, 0, sizeof(*o));
    o->header = header;
    if (log_would_log(log_local_category(), 6)) {
        char hex[68 * 3 + 1];
        for (int i = 0; i < 68; i += 4)
            snprintf(hex + i * 2 + i / 4, 10, "%08X ", be32(rec + i));
        LOG(6, "record at $%08X: %s", w, hex);
    }
    if (header & HDR_BITMAP) {
        // A 32 x 12 one-bit tile: depth, left/top, colour (twice), 12 rows
        // with bit 31 the leftmost pixel.
        o->kind = Q3_OBJ_BITMAP;
        o->a0[Q3_A_DEPTH] = f32_bits(be32(rec + 4));
        uint32_t xy = be32(rec + 8);
        o->bx = (int)(xy >> 19);
        o->by = (int)((xy & 0xFFFFu) >> 3);
        o->colour = be32(rec + 12);
        for (int r = 0; r < 12; r++)
            o->mask[r] = be32(rec + 20 + 4 * r);
        o->first = o->by;
        o->last = o->by + 11;
        return true;
    }
    for (int i = 0; i < 3; i++) {
        uint32_t xy = be32(rec + 4 + 4 * i);
        o->x13[i] = (uint16_t)(xy >> 16);
        o->y13[i] = (uint16_t)xy;
    }
    LOG(5, "triangle $%08X: (%u,%u) (%u,%u) (%u,%u) in 13.3", header, o->x13[0], o->y13[0], o->x13[1], o->y13[1],
        o->x13[2], o->y13[2]);
    if (!obj_tri_range(o))
        return false;
    float v[3][Q3_A_NATTR];
    memset(v, 0, sizeof(v));
    for (int i = 0; i < 3; i++)
        v[i][Q3_A_DEPTH] = f32_bits(be32(rec + 16 + 4 * i));
    if (header & HDR_TEXTURED) {
        // Per vertex: alpha, the low mantissa nibbles of v and u, a bfloat16
        // mip footprint; v and u's top 16 bits; grey kd (1.7) and ks (0.8).
        o->kind = Q3_OBJ_TEXTURED;
        if (!tex_resolve(q, (header >> 4) & 0xFu, &o->tex_slot, &o->tex_size)) {
            LOG(2, "textured object on unloaded page %u: dropped", (header >> 4) & 0xFu);
            return false;
        }
        o->tex_levels = o->tex_size == 256 ? 9 : 8;
        for (int i = 0; i < 3; i++) {
            uint32_t a = be32(rec + 28 + 4 * i);
            uint32_t vu = be32(rec + 40 + 4 * i);
            uint32_t ks = be32(rec + 52 + 4 * i);
            v[i][Q3_A_ALPHA] = (float)(a >> 24) / 255.0f;
            v[i][Q3_A_V] = f20(vu >> 16, (a >> 20) & 0xFu);
            v[i][Q3_A_U] = f20(vu & 0xFFFFu, (a >> 16) & 0xFu);
            v[i][Q3_A_LOD] = f32_bits((a & 0xFFFFu) << 16);
            v[i][Q3_A_KD] = (float)((ks >> 8) & 0xFFu) / 128.0f;
            v[i][Q3_A_KS] = (float)(ks & 0xFFu) / 255.0f;
        }
    } else {
        o->kind = Q3_OBJ_GOURAUD;
        for (int i = 0; i < 3; i++) {
            uint32_t c = be32(rec + 28 + 4 * i);
            v[i][Q3_A_ALPHA] = (float)(c >> 24) / 255.0f;
            v[i][Q3_A_R] = (float)((c >> 16) & 0xFFu) / 255.0f;
            v[i][Q3_A_G] = (float)((c >> 8) & 0xFFu) / 255.0f;
            v[i][Q3_A_B] = (float)(c & 0xFFu) / 255.0f;
        }
    }
    return obj_planes(o, v, Q3_A_NATTR);
}

// ============================================================
// The producer side of the raster seam (qd3d_raster.h)
// ============================================================

// Does register byte address `rr` belong to the draw-state snapshot?
static bool reg_in_state(uint32_t rr) {
    return rr == REG_MODE || rr == REG_BACKGROUND || rr == REG_CSG ||
           (rr >= REG_MAPBASE0 && rr < REG_MAPBASE0 + 4u * Q3_MAP_LEVELS);
}

// Fill a draw-state slot from the live registers (q3_state_build_fn).
static void build_state(void *ctx, q3_draw_state_t *st) {
    qd3d_t *q = (qd3d_t *)ctx;
    uint32_t mode = q->reg[REG_MODE >> 2];
    st->background = q->reg[REG_BACKGROUND >> 2];
    st->csg = q->reg[REG_CSG >> 2];
    for (uint32_t i = 0; i < Q3_MAP_LEVELS; i++)
        st->mapbase[i] = q->reg[(REG_MAPBASE0 >> 2) + i];
    st->persp = (mode & MODE_PERSPECTIVE) != 0;
    st->dest16 = (q->dmactl & DMACTL_16BPP) != 0 || (mode & MODE_DEST16) != 0;
}

// A renderer register written, by the port or by the control program.
static void reg_write(qd3d_t *q, uint32_t rr, uint32_t data) {
    q->reg[rr >> 2] = data;
    if (reg_in_state(rr))
        q3_raster_state_dirty(q->raster);
}

// Execute one control block's seven command words.  `width` tracks
// NPixelsMinus1; `cur` is this card's latched Ycur.
static void control_block(qd3d_t *q, uint32_t w, uint32_t *width, uint32_t *cur) {
    uint8_t blk[28];
    pci_dma_read(q->dev, w, blk, sizeof(blk));
    uint32_t me = card_index(q);
    for (int i = 0; i < 7; i++) {
        uint32_t cmd = be32(blk + 4 * i);
        uint32_t op = cmd >> HDR_OPCODE_SHIFT;
        if (op == 0)
            continue; // an empty slot
        if (op != 1) {
            LOG(2, "control block $%08X word %d: opcode %u ($%08X) ignored", w, i, op, cmd);
            continue;
        }
        uint32_t rr = cmd & 0xFFu;
        if (rr == REG_SCAN_START) {
            if ((*cur & 0xFFFFu) != 0xFFFFu) {
                q3_cmd_t c;
                c.kind = Q3_CMD_SCANLINE;
                c.u.line.y = (int32_t)((*cur & 0xFFFFu) >> 3);
                c.u.line.width = *width;
                q3_raster_submit(q->raster, &c);
                q->frame_pixels += *width;
                q->frame_lines++;
            }
            continue;
        }
        if (rr == REG_SCAN_END)
            continue;
        if (i == 6)
            break; // a data-carrying write with no room for its data
        uint32_t data = be32(blk + 4 * ++i);
        if ((rr & ~3u) == REG_Y) {
            // One register for all cards; the low address bits say whose.
            if ((rr & 3u) == me)
                *cur = data;
        } else if (rr == REG_NPIXELS_M1) {
            *width = (data & 0xFFFFu) + 1u;
            if (*width > Q3_MAX_WIDTH)
                *width = Q3_MAX_WIDTH;
        } else if (rr < QD3D_NREGS * 4u) {
            reg_write(q, rr, data);
        }
    }
}

// Walk a frame's control program from register $20 and hand the renderer
// this card's scanlines: one command per object and per scanline.
static void frame_walk(qd3d_t *q) {
    uint32_t n = card_count(q), me = card_index(q);
    uint32_t width = 1, cur = 0xFFFFFFFFu, records = 0;
    q->frame_pixels = q->frame_objects = 0;
    q->frame_lines = 0;
    q3_cmd_t c;
    c.kind = Q3_CMD_FRAME;
    q3_raster_submit(q->raster, &c);
    for (uint32_t rec = q->reg[REG_GO >> 2]; rec; records++) {
        if (records >= QD3D_MAX_RECORDS) {
            LOG(1, "control program runs past %u records: frame abandoned", QD3D_MAX_RECORDS);
            break;
        }
        uint32_t next = dma_word(q, rec + 4u * me);
        uint32_t w = rec + 4u * n;
        uint32_t header = dma_word(q, w);
        if ((header >> HDR_OPCODE_SHIFT) == 3u) {
            if (obj_decode(q, w, header, &c.u.obj)) {
                c.kind = Q3_CMD_OBJECT;
                q3_raster_submit(q->raster, &c);
                q->frame_objects++;
            }
        } else {
            control_block(q, w, &width, &cur);
        }
        rec = next;
    }
    q->frames++;
    q->frame_records = records;
    q->last_objects = (uint32_t)q->frame_objects;
    q->last_lines = q->frame_lines;
}

// The in-flight frame's pixels go out: stream the renderer's output
// through the descriptor list the frame started with.  This is the moment
// they reach host memory -- the frame's completion, or whatever cuts it
// short (a reset, the next frame).
static void frame_output(qd3d_t *q) {
    q3_raster_sync(q->raster);
    q3_target_t *t = &q->tgt;
    if (t->out_failed)
        LOG(1, "frame %u: no memory for its output: its last scanlines are lost", q->frames);
    dma_begin(q, q->frame_cmdptr);
    dma_stream(q, t->out, t->out_len);
    // The list should be at its STOP now.
    if (dma_load(q))
        LOG(1, "frame %u: control program ended with output descriptors left ($%08X)", q->frames, q->dma.next - 16);
    if (log_would_log(log_local_category(), 3)) {
        uint32_t hash = 2166136261u; // FNV-1a over the frame's pixels: a cheap identity for comparisons
        for (size_t i = 0; i < t->out_len; i++)
            hash = (hash ^ t->out[i]) * 16777619u;
        LOG(3, "frame %u: %u records, %llu objects, %llu pixels, hash %08X", q->frames, q->frame_records,
            (unsigned long long)q->frame_objects, (unsigned long long)q->frame_pixels, hash);
    }
    t->out_len = 0;
}

// The frame has finished on the card: its pixels land, then the interrupt
// if enabled.
static void frame_done_event(void *source, uint64_t data) {
    (void)data;
    qd3d_t *q = (qd3d_t *)source;
    q->irq_pending = 0;
    frame_output(q);
    if (q->dmactl & DMACTL_IE)
        update_irq(q, true);
}

// A frame in flight is cut short (reset, or the next go): no interrupt,
// but its pixels still go out.
static void frame_cut_short(qd3d_t *q) {
    if (!q->irq_pending)
        return;
    remove_event(q->cfg->scheduler, frame_done_event, q);
    q->irq_pending = 0;
    frame_output(q);
}

// Register $20 written: walk the frame and finish it after the time the
// ASIC would take (about 20 Mpixel/s plus per-object setup), never
// synchronously -- the driver marks itself running only after this write.
static void frame_start(qd3d_t *q) {
    frame_cut_short(q);
    if (!(q->dmactl & DMACTL_RUN))
        LOG(2, "frame started with the output channel not running (DMACONTROL $%04X)", q->dmactl);
    q->frame_cmdptr = q->cmdptr;
    frame_walk(q);
    uint64_t ns = 20000 + q->frame_pixels * 50 + q->frame_objects * 100;
    q->irq_pending = 1;
    scheduler_new_cpu_event(q->cfg->scheduler, frame_done_event, q, 0, 0, ns);
}

// ============================================================
// The packet parser
// ============================================================

// One word written into the internal address space.
static void internal_write(qd3d_t *q, uint32_t addr, uint32_t value) {
    if (addr < QD3D_NREGS * 4u) {
        reg_write(q, addr, value);
        LOG(4, "reg $%02X = $%08X", addr, value);
        if (addr == REG_GO)
            frame_start(q);
    } else if (addr >= QD3D_OCACHE_BASE && addr < QD3D_OCACHE_BASE + QD3D_OCACHE_SIZE) {
        uint8_t *p = &q->ocache[(addr - QD3D_OCACHE_BASE) & ~3u];
        p[0] = (uint8_t)(value >> 24);
        p[1] = (uint8_t)(value >> 16);
        p[2] = (uint8_t)(value >> 8);
        p[3] = (uint8_t)value;
    } else if (addr >= QD3D_TEX_BASE && addr < QD3D_TEX_BASE + QD3D_TEX_TEXELS * 4u) {
        q3_raster_sync(q->raster); // the renderer may still be reading the SRAM
        q->tex[(addr - QD3D_TEX_BASE) >> 2] = texel_store(value);
    } else {
        LOG(2, "write to unmapped internal address $%06X = $%08X", addr, value);
    }
}

// One word written to PORT.
static void port_write(qd3d_t *q, uint32_t w) {
    switch (q->port_state) {
    case PORT_HEADER: {
        uint32_t op = w >> HDR_OPCODE_SHIFT, addr = w & 0x1FFFFFFFu;
        if (op == 1) {
            if (addr == REG_SCAN_START || addr == REG_SCAN_END)
                return; // strobes carry no data and mean nothing outside a frame
            q->port_addr = addr;
            q->port_state = PORT_DATA;
        } else if (op == 2) {
            q->port_addr = addr;
            q->port_state = PORT_COUNT;
        } else if (w != 0) {
            LOG(2, "port: unexpected header $%08X ignored", w);
        }
        return;
    }
    case PORT_DATA:
        q->port_state = PORT_HEADER;
        internal_write(q, q->port_addr, w);
        return;
    case PORT_COUNT:
        q->port_count = w + 1u;
        q->port_state = PORT_BLOCK;
        return;
    case PORT_BLOCK:
        internal_write(q, q->port_addr, w);
        q->port_addr += 4;
        if (--q->port_count == 0)
            q->port_state = PORT_HEADER;
        return;
    }
}

// ============================================================
// Host-visible registers
// ============================================================

static void dmactl_write(qd3d_t *q, uint32_t v) {
    if (v & DMACTL_RESET) {
        // Reset the channel and the parser; both SRAMs keep their contents
        // (the driver resets before every frame and relies on the object
        // cache's free list surviving).  A frame still in flight is cut
        // short: its pixels go out, its interrupt never comes.
        frame_cut_short(q);
        q->port_state = PORT_HEADER;
        update_irq(q, false);
    }
    if (v & DMACTL_ACK)
        update_irq(q, false);
    q->dmactl = v & ~(DMACTL_ACK | DMACTL_RESET);
    q3_raster_state_dirty(q->raster); // the 16-bpp bits
}

static uint32_t bar_read32(void *ctx, uint32_t off) {
    qd3d_t *q = (qd3d_t *)ctx;
    switch (off & ~3u) {
    case QD3D_DMACONTROL:
        return q->dmactl;
    case QD3D_DMACMDPTR:
        return q->cmdptr;
    default:
        return 0;
    }
}

static void bar_write32(void *ctx, uint32_t off, uint32_t v) {
    qd3d_t *q = (qd3d_t *)ctx;
    switch (off & ~3u) {
    case QD3D_PORT:
        port_write(q, v);
        return;
    case QD3D_DMACONTROL:
        LOG(4, "DMACONTROL = $%04X", v);
        dmactl_write(q, v);
        return;
    case QD3D_DMACMDPTR:
        q->cmdptr = v;
        return;
    default:
        LOG(2, "write to unused BAR offset +$%02X = $%08X ignored", off, v);
        return;
    }
}

// The driver only ever makes 32-bit accesses; narrower ones see the
// containing word.
static uint8_t bar_read8(void *ctx, uint32_t off) {
    return (uint8_t)(bar_read32(ctx, off) >> (24 - 8 * (off & 3u)));
}

static uint16_t bar_read16(void *ctx, uint32_t off) {
    return (uint16_t)(bar_read32(ctx, off) >> (16 - 8 * (off & 2u)));
}

static void bar_write8(void *ctx, uint32_t off, uint8_t v) {
    LOG(2, "byte write to BAR +$%02X = $%02X ignored", off, v);
    (void)ctx;
}

static void bar_write16(void *ctx, uint32_t off, uint16_t v) {
    LOG(2, "halfword write to BAR +$%02X = $%04X ignored", off, v);
    (void)ctx;
}

// ============================================================
// Config space beyond the generic header
// ============================================================

static bool qd3d_cfg_read(pci_device_t *dev, uint32_t reg, uint32_t *out) {
    qd3d_t *q = (qd3d_t *)dev->priv;
    if (reg == QD3D_CFG_POSITION) {
        *out = q->cfg_position;
        return true;
    }
    return false;
}

static bool qd3d_cfg_write(pci_device_t *dev, uint32_t reg, uint32_t byte, uint8_t value) {
    qd3d_t *q = (qd3d_t *)dev->priv;
    if (reg == QD3D_CFG_POSITION) {
        uint32_t shift = 8u * byte;
        q->cfg_position = (q->cfg_position & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        LOG(3, "position register = $%08X", q->cfg_position);
        return true;
    }
    return false;
}

// ============================================================
// Lifecycle
// ============================================================

static void qd3d_reset(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    qd3d_t *q = (qd3d_t *)dev->priv;
    // A frame in flight is dropped with the rest of the machine's state.
    remove_event(q->cfg->scheduler, frame_done_event, q);
    q3_raster_sync(q->raster);
    q->tgt.out_len = 0;
    update_irq(q, false);
    memset(q->reg, 0, sizeof(q->reg));
    q->dmactl = q->cmdptr = q->cfg_position = 0;
    q->port_state = PORT_HEADER;
    q->irq_pending = 0;
    q3_raster_state_dirty(q->raster);
}

static void qd3d_teardown(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    qd3d_t *q = (qd3d_t *)dev->priv;
    if (!q)
        return;
    scheduler_forget_source(q->cfg->scheduler, q);
    q3_raster_destroy(q->raster); // fences and joins the worker before the target goes
    q3_target_free(&q->tgt);
    free(q);
    dev->priv = NULL;
}

static const char *qd3d_name(const pci_device_t *dev) {
    (void)dev;
    return "QuickDraw 3D Accelerator";
}

// The plain-data head, then the output of a frame still in flight (its
// pixels reach host memory only when it completes): a byte count and the
// bytes.
static void qd3d_checkpoint_save(pci_device_t *dev, checkpoint_t *cp) {
    qd3d_t *q = (qd3d_t *)dev->priv;
    q3_raster_sync(q->raster);
    system_write_checkpoint_data(cp, q, offsetof(qd3d_t, dev));
    uint32_t len = q->irq_pending ? (uint32_t)q->tgt.out_len : 0u;
    system_write_checkpoint_data(cp, &len, sizeof(len));
    if (len)
        system_write_checkpoint_data(cp, q->tgt.out, len);
}

static void qd3d_checkpoint_restore(pci_device_t *dev, checkpoint_t *cp) {
    qd3d_t *q = (qd3d_t *)dev->priv;
    uint32_t irq = 0, len = 0;
    q3_raster_sync(q->raster);
    system_read_checkpoint_data(cp, q, offsetof(qd3d_t, dev));
    system_read_checkpoint_data(cp, &len, sizeof(len));
    q->tgt.out_len = 0;
    q->tgt.out_failed = false;
    if (len && q3_target_reserve(&q->tgt, len)) {
        system_read_checkpoint_data(cp, q->tgt.out, len);
        q->tgt.out_len = len;
    } else if (len) {
        // Keep the stream in step even without room for the pixels.
        uint8_t sink[4096];
        for (uint32_t done = 0; done < len; done += (uint32_t)sizeof(sink))
            system_read_checkpoint_data(cp, sink, len - done < sizeof(sink) ? len - done : sizeof(sink));
        q->tgt.out_failed = true;
    }
    q3_raster_state_dirty(q->raster);
    // Re-derive the line from the restored state.
    irq = q->irq;
    q->irq = 0;
    update_irq(q, irq != 0);
}

static const pci_device_ops_t qd3d_ops = {
    .reset = qd3d_reset,
    .teardown = qd3d_teardown,
    .cfg_read = qd3d_cfg_read,
    .cfg_write = qd3d_cfg_write,
    .checkpoint_save = qd3d_checkpoint_save,
    .checkpoint_restore = qd3d_checkpoint_restore,
    .name = qd3d_name,
};

// One memory BAR, a bus master with one interrupt, no expansion ROM: Open
// Firmware builds the `pci106b,9` node itself and the driver loads from
// the Extensions folder by that name.
static const pci_config_decl_t qd3d_decl = {
    .vendor_id = QD3D_VENDOR_ID,
    .device_id = QD3D_DEVICE_ID,
    .revision = QD3D_REVISION,
    .class_code = QD3D_CLASS,
    .header_type = 0x00u,
    .interrupt_pin = 1u, // INTA
    // memory space, bus master, memory write and invalidate (the driver writes $16)
    .command_writable = PCI_CMD_MEM_SPACE | PCI_CMD_MASTER | 0x0010u,
    .bar[0] = {.size = QD3D_BAR_SIZE, .kind = PCI_BAR_MEM},
    .rom_size = 0,
};

// The rasteriser is not a choice a user is offered -- there is only the
// card's own -- but pci_option="raster=thread|sw|null" picks one per boot
// for the tests and the terminal, as on the Voodoo2.  The default is the
// worker thread on every build: its output is byte-identical to the
// normative walker's.  -DGS_QD3D_RASTER_DEFAULT='"sw"' (EXTRA_CFLAGS) makes a
// build default to the walker; -DGS_QD3D_THREAD_BACKEND=0 compiles the
// thread backend out of qd3d_raster.c, and the default follows.
#ifndef GS_QD3D_RASTER_DEFAULT
#if defined(GS_QD3D_THREAD_BACKEND) && !GS_QD3D_THREAD_BACKEND
#define GS_QD3D_RASTER_DEFAULT "sw"
#else
#define GS_QD3D_RASTER_DEFAULT "thread"
#endif
#endif

static bool qd3d_accepts_option(const char *key, const char *value) {
    if (strcmp(key, "raster") == 0)
        return strcmp(value, "thread") == 0 || strcmp(value, "sw") == 0 || strcmp(value, "null") == 0;
    return false;
}

static pci_device_t *qd3d_factory(int slot_index, config_t *cfg, const rom_image_t *rom, const slot_opts_t *opts) {
    (void)rom;
    pci_device_t *dev = calloc(1, sizeof(*dev));
    qd3d_t *q = calloc(1, sizeof(*q));
    if (!dev || !q || !q3_target_init(&q->tgt, q->tex)) {
        free(q);
        free(dev);
        return NULL;
    }
    const char *raster = slot_opts_option(opts, "raster");
    q->raster = q3_raster_create(raster ? raster : GS_QD3D_RASTER_DEFAULT, &q->tgt, build_state, q);
    if (!q->raster) {
        q3_target_free(&q->tgt);
        free(q);
        free(dev);
        return NULL;
    }
    q->dev = dev;
    q->cfg = cfg;
    dev->ops = &qd3d_ops;
    dev->decl = &qd3d_decl;
    dev->priv = q;
    pci_cfg_reset(dev);

    q->bar_if.read_uint8 = bar_read8;
    q->bar_if.read_uint16 = bar_read16;
    q->bar_if.read_uint32 = bar_read32;
    q->bar_if.write_uint8 = bar_write8;
    q->bar_if.write_uint16 = bar_write16;
    q->bar_if.write_uint32 = bar_write32;
    q->bar_if.peek_uint8 = bar_read8;
    q->bar_if.peek_uint16 = bar_read16;
    q->bar_if.peek_uint32 = bar_read32;
    pci_bar_backing_iface(dev, 0, &q->bar_if, q);

    scheduler_new_event_type(cfg->scheduler, "qd3d", q, "frame-done", frame_done_event);
    q->port_state = PORT_HEADER;
    LOG(1, "seated in slot %d (revision %u, %u texels), raster backend %s", slot_index, QD3D_REVISION, QD3D_TEX_TEXELS,
        q3_raster_name(q->raster));
    return dev;
}

// ============================================================
// Object model
// ============================================================

static qd3d_t *node_card(struct object *self) {
    return (qd3d_t *)object_data(self);
}

static DEF_GETTER(r_attr_frames) {
    qd3d_t *q = node_card(self);
    return val_uint(4, q ? q->frames : 0);
}
static DEF_GETTER(r_attr_objects) {
    qd3d_t *q = node_card(self);
    return val_uint(4, q ? q->last_objects : 0);
}
static DEF_GETTER(r_attr_lines) {
    qd3d_t *q = node_card(self);
    return val_uint(4, q ? q->last_lines : 0);
}
static DEF_GETTER(r_attr_dma_control) {
    qd3d_t *q = node_card(self);
    return val_uint(4, q ? q->dmactl : 0);
}
static DEF_GETTER(r_attr_position) {
    qd3d_t *q = node_card(self);
    return val_uint(4, q ? q->cfg_position : 0);
}
static DEF_GETTER(r_attr_irq) {
    qd3d_t *q = node_card(self);
    return val_bool(q && q->irq);
}

static DEF_GETTER(r_attr_raster) {
    qd3d_t *q = node_card(self);
    return val_str(q ? q3_raster_name(q->raster) : "");
}
// Swap the raster backend in place: fence the old one, build the new one on
// the same target, then let the old one go.  The backends are equivalent by
// contract, so a run can switch at any instruction boundary.
static DEF_SETTER(r_attr_raster_set) {
    qd3d_t *q = node_card(self);
    char kind[8];
    snprintf(kind, sizeof(kind), "%s", in.s ? in.s : "");
    value_free(&in);
    if (!q)
        return val_err("renderer.raster: no card");
    if (!qd3d_accepts_option("raster", kind))
        return val_err("renderer.raster: %s is not one of thread, sw, null", kind);
    q3_raster_sync(q->raster);
    q3_raster_t *next = q3_raster_create(kind, &q->tgt, build_state, q);
    if (!next)
        return val_err("renderer.raster: could not create the %s backend", kind);
    q3_raster_destroy(q->raster);
    q->raster = next;
    return val_none();
}

static const arg_decl_t r_read_arg[] = {
    {.name = "offset", .kind = VK_INT, .doc = "Renderer register byte address ($00-$FC)"},
};
static DEF_METHOD(r_method_read) {
    qd3d_t *q = node_card(self);
    int64_t off = argv[0].i;
    if (!q || off < 0 || off > 0xFC)
        return val_err("renderer.read: offset must be $00..$FC");
    return val_uint(4, q->reg[off >> 2]);
}

static const member_t renderer_members[] = {
    {.kind = MK_ATTR,
     .name = "frames",
     .doc = "Frames the card has rendered since power-on",
     .attr = {.type = VK_UINT, .get = r_attr_frames}                                                                                                                         },
    {.kind = MK_ATTR,
     .name = "objects",
     .doc = "Objects (triangles and bitmap tiles) in the last frame",
     .attr = {.type = VK_UINT, .get = r_attr_objects}                                                                                                                        },
    {.kind = MK_ATTR,
     .name = "lines",
     .doc = "Scanlines the last frame rendered",
     .attr = {.type = VK_UINT, .get = r_attr_lines}                                                                                                                          },
    {.kind = MK_ATTR,
     .name = "dma_control",
     .doc = "DMACONTROL: $8000 run, $4000 interrupt enable, $0C00 16-bpp output",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_HEX, .get = r_attr_dma_control}                                                                                   },
    {.kind = MK_ATTR,
     .name = "position",
     .doc = "Config register $40: $F0 | (cards-1) << 2 | card index, once the driver has set it",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_HEX, .get = r_attr_position}                                                                                      },
    {.kind = MK_ATTR,   .name = "irq", .doc = "The card's interrupt line",                        .attr = {.type = VK_BOOL, .get = r_attr_irq}                               },
    {.kind = MK_ATTR,
     .name = "raster",
     .doc = "Where the renderer runs: thread (default), sw (normative) or null (pci_option=\"raster=...\"); "
            "assigning one swaps the backend in place",                                           .attr = {.type = VK_STRING, .get = r_attr_raster, .set = r_attr_raster_set}},
    {.kind = MK_METHOD,
     .name = "read",
     .doc = "Read a renderer register by its byte address",
     .method = {.args = r_read_arg, .nargs = 1, .result = VK_UINT, .fn = r_method_read}                                                                                      },
};

static const class_desc_t renderer_class = {
    .name = "qd3d_renderer",
    .members = renderer_members,
    .n_members = sizeof(renderer_members) / sizeof(renderer_members[0]),
};

static void qd3d_attach_objects(pci_device_t *dev, struct object *card_node) {
    qd3d_t *q = (qd3d_t *)dev->priv;
    if (!q || !card_node)
        return;
    struct object *r = object_new(&renderer_class, q, "renderer");
    if (r) {
        object_set_label(r, "Renderer");
        object_set_order(r, 10);
        object_attach(card_node, r);
    }
}

const pci_card_kind_t qd3d_kind = {
    .id = "qd3d",
    .display_name = "Apple QuickDraw 3D Accelerator Card",
    .attach = PCI_ATTACH_PCI,
    .card_class = "other",
    .factory = qd3d_factory,
    .accepts_option = qd3d_accepts_option,
    .attach_objects = qd3d_attach_objects,
};
