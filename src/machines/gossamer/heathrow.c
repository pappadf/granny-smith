// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// heathrow.c
// Heathrow (343S1201) — the beige G3's Mac I/O controller: interrupt
// controller, thirteen DBDMA channel blocks, and the apertures of MESH,
// BMAC, the ESCC, the DAVbus (Screamer), SWIM3, the VIA that fronts Cuda,
// two ATA channels and the 8 KB NVRAM.  Unlike Grand Central it is a real
// PCI function: device $10 on the Grackle bus, vendor $106B device $0010
// revision 1, class $FF0000, one 512 KB non-prefetchable memory BAR that
// the boot program itself programs to $F3000000 through configuration
// space before any other access (ROM $FFF03084).
//
// Register truth: the Rev C device tree (every node's `reg`), the ROM's
// boot program and its NanoKernel's kind-7 interrupt handler, Apple's
// AppleHeathrow driver, Linux heathrow.h/pic.c, and the TNT model of the
// Grand Central register file this chip extends (grand_central.c).
//
// Window map (offsets from BAR0):
//   +$00010..$1C  Events2/Mask2/Clear2/Levels2  (sources 32-63, LE)
//   +$00020..$2C  Events1/Mask1/Clear1/Levels1  (sources 0-31, LE)
//   +$00030..$3F  reg30, brightness/contrast (+$32/+$33), MBCR (+$34),
//                 FCR (+$38), aux (+$3C) — byte/half/word accessible
//   +$08000       DBDMA channel n at +$8000 + n*$100, n = 0..12 (LE)
//   +$10000       MESH (sixteen byte registers on $10 centres)
//   +$11000       BMAC (gossamer_bmac.c; 16-bit LE registers)
//   +$12000       ESCC, legacy (68k) addressing
//   +$13000       ESCC, MacRISC addressing (ch-b +$00, ch-a +$20)
//   +$14000       DAVbus / Screamer (core/peripherals/davbus.c)
//   +$15000       SWIM3 (sixteen byte registers on $10 centres)
//   +$16000       VIA (16 byte registers on $200 centres, 8 KB)
//   +$20000/+$21000  ATA cells 0/1 (gossamer_ata.c)
//   +$60000       NVRAM: byte n at +16n, 8 KB (128 KB window)
//
// The interrupt controller is Grand Central's per bank.  The Gossamer
// NanoKernel's kind-7 handler (ROM $316880) writes $80000000 to Clear1
// and to Clear2, then reads Mask & Levels of both banks and classifies:
// source 20 -> IPL 7; 15/16, 0-9 and bank-2 bits 0-1 -> IPL 4; bank-2
// bit 10 (BMAC) -> IPL 3; 11-14, 17, 19, 21-29 -> IPL 2; 18 -> IPL 1.
// That is exactly the TNT scheme (tnt.h tnt_gc_t): the $80000000 Clear is
// a mode acknowledge that drops the bank's output latch, the latch is set
// by any change of an enabled source (both edges) or an enabled event
// edge, and DBDMA completions are held levels until their Clear bit is
// written.  Before the first mode acknowledge (Linux, the BSDs) the line
// is combinational ((events | levels) & mask).

#include "gossamer.h"

#include "davbus.h"
#include "dbdma.h"
#include "irq_controller.h"
#include "log.h"
#include "machine.h"
#include "object.h"
#include "of_nvram.h"
#include "pci.h"
#include "ppc.h"
#include "scc.h"
#include "scsi_mesh.h"
#include "swim3.h"
#include "via.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("heathrow");

// Window offsets
#define HR_INTS2     0x00010u // bank 2: +$10 Events2 .. +$1C Levels2
#define HR_INTS1     0x00020u // bank 1: +$20 Events1 .. +$2C Levels1
#define HR_CTRL      0x00030u // +$30..+$3F byte-accessible control block
#define HR_DBDMA     0x08000u
#define HR_DBDMA_END (HR_DBDMA + 0x100u * DBDMA_CHANNELS_HEATHROW)
#define HR_MESH      0x10000u
#define HR_BMAC      0x11000u
#define HR_SCCLEG    0x12000u
#define HR_ESCC      0x13000u
#define HR_DAVBUS    0x14000u
#define HR_SWIM3     0x15000u
#define HR_VIA       0x16000u // 8 KB
#define HR_NVRAM     0x60000u // 128 KB window
#define HR_NVRAM_END 0x80000u

// Offsets within a bank's four registers
#define INT_EVENTS 0x0u
#define INT_MASK   0x4u
#define INT_CLEAR  0x8u
#define INT_LEVELS 0xCu

// The mode acknowledge (see the file comment).
#define INT_MODE_ACK 0x80000000u

// Control block, little-endian register view
#define CTRL_MBCR 0x34u
#define CTRL_FCR  0x38u
#define CTRL_AUX  0x3Cu

// FCR bits the model reacts to (the mac-io node's bit-name methods, Linux
// heathrow.h; LE numbering)
#define FCR_SCC_CELL_EN 0x00000200u
#define FCR_RESET_SCC   0x02000000u

// ============================================================
// DBDMA channel -> interrupt source (the Rev C tree's AAPL,interrupts:
// ide@20000 `0d 02`, ide@21000 `0e 03`, bmac `2a 20 21`)
// ============================================================

int gos_dbdma_source(int chan) {
    switch (chan) {
    case GOS_DMA_MESH:
    case GOS_DMA_SWIM3:
    case GOS_DMA_SCCA_TX:
    case GOS_DMA_SCCA_RX:
    case GOS_DMA_SCCB_TX:
    case GOS_DMA_SCCB_RX:
    case GOS_DMA_AUD_OUT:
    case GOS_DMA_AUD_IN:
        return chan; // Grand Central's identity survives for 0, 1, 4-9
    case GOS_DMA_ATA0:
        return 2; // Grand Central's Ethernet-DMA bits, reassigned
    case GOS_DMA_ATA1:
        return 3;
    case GOS_DMA_BMAC_TX:
        return 32; // bank 2
    case GOS_DMA_BMAC_RX:
        return 33;
    default:
        return -1; // channel 10: no owner, no interrupt
    }
}

// ============================================================
// Interrupt controller
// ============================================================

void gos_recompute_irq(config_t *cfg) {
    gos_heathrow_t *hr = &gos_st(cfg)->hr;
    bool line = false;
    for (int b = 0; b < 2; b++) {
        if (hr->mode1[b])
            line |= (hr->latch[b] & hr->mask[b]) != 0;
        else
            line |= ((hr->events[b] | hr->levels[b]) & hr->mask[b]) != 0;
    }
    ppc_set_ext_irq(cfg->ppc, line);
}

// An enabled source edge sets the bank's output latch.
static void hr_edge(gos_heathrow_t *hr, int b, uint32_t bit) {
    hr->events[b] |= bit;
    if (hr->mask[b] & bit)
        hr->latch[b] |= bit;
}

void gos_set_source(config_t *cfg, int n, bool level) {
    if (n < 0 || n >= GOS_INT_SOURCES)
        return;
    gos_heathrow_t *hr = &gos_st(cfg)->hr;
    int b = n >> 5;
    uint32_t bit = 1u << (n & 31);
    bool was = (hr->levels[b] & bit) != 0;
    if (level) {
        hr->levels[b] |= bit;
        if (!was)
            hr_edge(hr, b, bit);
    } else {
        hr->levels[b] &= ~bit;
        // The deassertion of an enabled source latches too: the kind-7
        // handler learns a source went away only through the change that
        // brings it back in, finding Levels quiet (the TNT law).
        if (was && (hr->mask[b] & bit))
            hr->latch[b] |= bit;
    }
    if (was != level)
        LOG(3, "source %d %s (bank %d events=$%08X levels=$%08X mask=$%08X latch=$%08X)", n,
            level ? "asserted" : "cleared", b + 1, hr->events[b], hr->levels[b], hr->mask[b], hr->latch[b]);
    gos_recompute_irq(cfg);
}

static uint32_t int_read(config_t *cfg, int b, uint32_t reg) {
    gos_heathrow_t *hr = &gos_st(cfg)->hr;
    switch (reg) {
    case INT_EVENTS:
        return hr->events[b];
    case INT_MASK:
        return hr->mask[b];
    case INT_CLEAR:
        return 0; // write-only (reads 0 on the sibling 8600)
    default:
        return hr->levels[b]; // live, never latched
    }
}

static void int_write(config_t *cfg, int b, uint32_t reg, uint32_t value) {
    gossamer_state_t *st = gos_st(cfg);
    gos_heathrow_t *hr = &st->hr;
    switch (reg) {
    case INT_EVENTS:
    case INT_CLEAR:
        // Write-1-to-clear into Events, bit 31 excepted: $80000000 is the
        // NanoKernel's mode acknowledge and clears no device bits.
        hr->events[b] &= ~(value & ~INT_MODE_ACK);
        if (reg == INT_CLEAR) {
            hr->mode1[b] = (value & INT_MODE_ACK) != 0;
            if (hr->mode1[b])
                hr->latch[b] = 0; // the acknowledge drops the line
        }
        LOG(3, "clear%d $%08X -> events $%08X (mode %d)", b + 1, value, hr->events[b], hr->mode1[b]);
        // DBDMA completions are held levels until acknowledged here.
        for (int ch = 0; ch < DBDMA_CHANNELS_HEATHROW; ch++) {
            int src = gos_dbdma_source(ch);
            if (src < 0 || (src >> 5) != b)
                continue;
            uint32_t bit = 1u << (src & 31);
            if ((value & bit) && (hr->levels[b] & bit))
                gos_set_source(cfg, src, false);
        }
        break;
    case INT_MASK: {
        // Enabling a source whose event or level is already pending counts
        // as an edge for the mode-1 latch.
        uint32_t newly = value & ~hr->mask[b];
        hr->mask[b] = value;
        hr->latch[b] |= (hr->events[b] | hr->levels[b]) & newly;
        LOG(3, "mask%d = $%08X (pc=$%08X)", b + 1, value, ppc_get_pc(cfg->ppc));
        break;
    }
    default:
        // SCSI Manager 4.3 writes Levels in its clear path; nothing happens.
        LOG(3, "write to read-only Levels%d ($%08X) ignored", b + 1, value);
        break;
    }
    gos_recompute_irq(cfg);
}

// ============================================================
// Control block (+$30..+$3F): byte-addressable
// ============================================================

// The 32-bit little-endian value of the control longword at `reg`.
static uint32_t ctrl_read32le(config_t *cfg, uint32_t reg) {
    gos_heathrow_t *hr = &gos_st(cfg)->hr;
    switch (reg) {
    case 0x30u:
        // +$30/+$31 unknown on the desktop; +$32 brightness, +$33 contrast.
        return (uint32_t)hr->reg30 | ((uint32_t)hr->brightness << 16) | ((uint32_t)hr->contrast << 24);
    case CTRL_MBCR:
        return hr->mbcr;
    case CTRL_FCR:
        return hr->fcr;
    default:
        return hr->aux;
    }
}

static void fcr_changed(config_t *cfg, uint32_t old, uint32_t fcr) {
    // reset_scc: the cell is held in reset while the bit is set; the
    // falling edge releases it (Apple and Linux pulse it for 15 ms).
    if ((fcr & FCR_RESET_SCC) && !(old & FCR_RESET_SCC) && cfg->scc)
        scc_reset(cfg->scc);
    gos_ata_fcr_changed(cfg, old, fcr); // the ATA cells' enables and RESET- lines
    gos_bmac_fcr_changed(cfg, old, fcr); // the Ethernet cell's enable and reset pulse
    if (old != fcr)
        LOG(2, "FCR $%08X -> $%08X (pc=$%08X)", old, fcr, ppc_get_pc(cfg->ppc));
}

static void ctrl_write32le(config_t *cfg, uint32_t reg, uint32_t v) {
    gos_heathrow_t *hr = &gos_st(cfg)->hr;
    switch (reg) {
    case 0x30u:
        hr->reg30 = (uint16_t)v;
        hr->brightness = (uint8_t)(v >> 16);
        hr->contrast = (uint8_t)(v >> 24);
        break;
    case CTRL_MBCR:
        // The media-bay ID nibble (bits 14-12) is the board's: 7 = no bay.
        hr->mbcr = (v & ~0x7000u) | 0x7000u;
        break;
    case CTRL_FCR: {
        uint32_t old = hr->fcr;
        hr->fcr = v;
        fcr_changed(cfg, old, v);
        break;
    }
    default:
        hr->aux = v;
        break;
    }
}

// Byte `k` (0..3) of the control longword at `reg`: byte k of the
// little-endian register (the 8600 byte dump returned exactly that).
static uint8_t ctrl_read8(config_t *cfg, uint32_t off) {
    uint32_t reg = off & 0x3Cu;
    return (uint8_t)(ctrl_read32le(cfg, reg) >> (8 * (off & 3u)));
}

static void ctrl_write8(config_t *cfg, uint32_t off, uint8_t value) {
    uint32_t reg = off & 0x3Cu, shift = 8 * (off & 3u);
    uint32_t v = ctrl_read32le(cfg, reg);
    v = (v & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    ctrl_write32le(cfg, reg, v);
}

// ============================================================
// NVRAM: byte n at +$60000 + 16n (flat, no bank port)
// ============================================================

static uint8_t nvram_read(config_t *cfg, uint32_t off) {
    gos_heathrow_t *hr = &gos_st(cfg)->hr;
    uint32_t idx = (off >> 4) & (GOS_NVRAM_SIZE - 1u);
    if ((off & 0xFu) != 0)
        LOG(3, "NVRAM read off-centre +$%05X", off);
    return hr->nvram[idx];
}

static void nvram_write(config_t *cfg, uint32_t off, uint8_t value) {
    gos_heathrow_t *hr = &gos_st(cfg)->hr;
    uint32_t idx = (off >> 4) & (GOS_NVRAM_SIZE - 1u);
    if ((off & 0xFu) != 0) {
        LOG(2, "NVRAM write off-centre +$%05X = $%02X ignored", off, value);
        return;
    }
    LOG(4, "NVRAM nv[$%04X] = $%02X", idx, value);
    hr->nvram[idx] = value;
}

// ============================================================
// ESCC apertures and its DBDMA ports
// ============================================================

// MacRISC aperture (+$13000: B ctl +$00 / B data +$10 / A ctl +$20 /
// A data +$30) onto the cell's classic pins (A/B on bit 1, D/C on bit 2).
static uint32_t escc_pins(uint32_t off) {
    uint32_t ab = (off >> 5) & 1u;
    uint32_t dc = (off >> 4) & 1u;
    return (ab << 1) | (dc << 2);
}

#define SCC_DMA_TX(ch) (GOS_DMA_SCCA_TX + 2u * (ch))
#define SCC_DMA_RX(ch) (GOS_DMA_SCCA_RX + 2u * (ch))

static uint32_t scc_data_pins(unsigned ch) {
    return (ch == 0 ? 2u : 0u) | 4u;
}

static void scc_dma_kick_rx(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st->dbdma)
        return;
    for (unsigned ch = 0; ch < 2; ch++)
        if (dbdma_active(st->dbdma, (int)SCC_DMA_RX(ch)))
            dbdma_kick(st->dbdma, (int)SCC_DMA_RX(ch));
}

static int scc_port_out(void *ctx, const uint8_t *buf, int len) {
    gos_scc_dma_ctx_t *c = (gos_scc_dma_ctx_t *)ctx;
    const memory_interface_t *mi = scc_get_memory_interface(c->cfg->scc);
    for (int n = 0; n < len; n++)
        mi->write_uint8(c->cfg->scc, scc_data_pins(c->ch), buf[n]);
    scc_dma_kick_rx(c->cfg);
    return len;
}

static int scc_port_in(void *ctx, uint8_t *buf, int len) {
    gos_scc_dma_ctx_t *c = (gos_scc_dma_ctx_t *)ctx;
    const memory_interface_t *mi = scc_get_memory_interface(c->cfg->scc);
    int n = 0;
    while (n < len && scc_channel_rx_pending(c->cfg->scc, c->ch) > 0)
        buf[n++] = mi->read_uint8(c->cfg->scc, scc_data_pins(c->ch));
    return n;
}

void gos_scc_dma_init(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    for (unsigned ch = 0; ch < 2; ch++) {
        st->scc_dma_ctx[ch].cfg = cfg;
        st->scc_dma_ctx[ch].ch = ch;
        dbdma_port_t tx = {.out = scc_port_out, .in = NULL, .s_bits = NULL, .ctx = &st->scc_dma_ctx[ch]};
        dbdma_port_t rx = {.out = NULL, .in = scc_port_in, .s_bits = NULL, .ctx = &st->scc_dma_ctx[ch]};
        dbdma_set_port(st->dbdma, (int)SCC_DMA_TX(ch), &tx);
        dbdma_set_port(st->dbdma, (int)SCC_DMA_RX(ch), &rx);
    }
}

// ============================================================
// Island dispatch
// ============================================================

// One island byte: the guest's read, or an inspection (`peek`) routed to each
// chip's side-effect-free peek.
static uint8_t hr_access8(void *ctx, uint32_t off, bool peek) {
    config_t *cfg = (config_t *)ctx;
    gossamer_state_t *st = gos_st(cfg);
    if (off < HR_CTRL) {
        int b = (off < HR_INTS1) ? 1 : 0;
        uint32_t base = b ? HR_INTS2 : HR_INTS1;
        if (off >= HR_INTS2)
            return (uint8_t)(int_read(cfg, b, (off - base) & 0xCu) >> (8 * (off & 3u)));
        return 0;
    }
    if (off < 0x40u)
        return ctrl_read8(cfg, off);
    if (off >= HR_NVRAM && off < HR_NVRAM_END)
        return nvram_read(cfg, off - HR_NVRAM);
    if (off >= GOS_HR_ATA0 && off < GOS_HR_ATA_END)
        return peek ? gos_ata_peek8(cfg, off) : gos_ata_read8(cfg, off);
    uint32_t block = off & 0xFF000u;
    switch (block) {
    case HR_VIA:
    case HR_VIA + 0x1000u:
        return memory_iface_read8(via_get_memory_interface(cfg->via1), cfg->via1, off - HR_VIA, peek);
    case HR_SCCLEG:
        return memory_iface_read8(scc_get_memory_interface(cfg->scc), cfg->scc, off - HR_SCCLEG, peek);
    case HR_ESCC:
        return memory_iface_read8(scc_get_memory_interface(cfg->scc), cfg->scc, escc_pins(off - HR_ESCC), peek);
    case HR_MESH:
        return peek ? mesh_peek(st->mesh, off - HR_MESH) : mesh_read(st->mesh, off - HR_MESH);
    case HR_BMAC:
        return peek ? gos_bmac_peek8(cfg, off - HR_BMAC) : gos_bmac_read8(cfg, off - HR_BMAC);
    case HR_SWIM3: {
        unsigned reg = ((off - HR_SWIM3) >> 4) & 15u;
        return peek ? swim3_peek(&st->swim3, reg) : swim3_read(&st->swim3, reg);
    }
    default:
        break;
    }
    if (off >= HR_DBDMA && off < HR_DBDMA_END) {
        int chan = (int)((off - HR_DBDMA) >> 8);
        uint32_t v = dbdma_reg_read(st->dbdma, chan, off & 0xFCu);
        return (uint8_t)(v >> (8 * (off & 3u))); // byte k of the LE register
    }
    if ((off & 0xFF000u) == HR_DAVBUS) {
        uint32_t v = davbus_read32(&st->screamer_host, (off - HR_DAVBUS) & ~3u);
        return (uint8_t)(v >> (8 * (off & 3u)));
    }
    if (!peek)
        LOG(1, "byte read of unwired Heathrow offset +$%05X", off);
    return 0;
}

static uint8_t hr_read8(void *ctx, uint32_t off) {
    return hr_access8(ctx, off, false);
}
static uint8_t hr_peek8(void *ctx, uint32_t off) {
    return hr_access8(ctx, off, true);
}

static void hr_write8(void *ctx, uint32_t off, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    gossamer_state_t *st = gos_st(cfg);
    if (off < HR_CTRL) {
        if (off >= HR_INTS2) {
            int b = (off < HR_INTS1) ? 1 : 0;
            uint32_t base = b ? HR_INTS2 : HR_INTS1;
            uint32_t reg = (off - base) & 0xCu, shift = 8 * (off & 3u);
            uint32_t cur = (reg == INT_MASK) ? st->hr.mask[b] : 0;
            int_write(cfg, b, reg, (cur & ~(0xFFu << shift)) | ((uint32_t)value << shift));
        }
        return;
    }
    if (off < 0x40u) {
        ctrl_write8(cfg, off, value);
        return;
    }
    if (off >= HR_NVRAM && off < HR_NVRAM_END) {
        nvram_write(cfg, off - HR_NVRAM, value);
        return;
    }
    if (off >= GOS_HR_ATA0 && off < GOS_HR_ATA_END) {
        gos_ata_write8(cfg, off, value);
        return;
    }
    uint32_t block = off & 0xFF000u;
    switch (block) {
    case HR_VIA:
    case HR_VIA + 0x1000u:
        via_get_memory_interface(cfg->via1)->write_uint8(cfg->via1, off - HR_VIA, value);
        return;
    case HR_SCCLEG:
        scc_get_memory_interface(cfg->scc)->write_uint8(cfg->scc, off - HR_SCCLEG, value);
        return;
    case HR_ESCC:
        scc_get_memory_interface(cfg->scc)->write_uint8(cfg->scc, escc_pins(off - HR_ESCC), value);
        scc_dma_kick_rx(cfg);
        return;
    case HR_MESH:
        mesh_write(st->mesh, off - HR_MESH, value);
        return;
    case HR_BMAC:
        gos_bmac_write8(cfg, off - HR_BMAC, value);
        return;
    case HR_SWIM3:
        swim3_write(&st->swim3, ((off - HR_SWIM3) >> 4) & 15u, value);
        return;
    default:
        break;
    }
    LOG(1, "byte write of unwired Heathrow offset +$%05X = $%02X", off, value);
}

// 32-bit access: the little-endian register blocks.  `value` at this edge
// is the big-endian bus view; GOS_LE32 recovers the register value the
// guest composed with stwbrx.
static uint32_t hr_access32(void *ctx, uint32_t off, bool peek) {
    config_t *cfg = (config_t *)ctx;
    gossamer_state_t *st = gos_st(cfg);
    if (off >= HR_INTS2 && off < HR_CTRL) {
        int b = (off < HR_INTS1) ? 1 : 0;
        return GOS_LE32(int_read(cfg, b, (off - (b ? HR_INTS2 : HR_INTS1)) & 0xCu));
    }
    if (off >= HR_CTRL && off < 0x40u)
        return GOS_LE32(ctrl_read32le(cfg, off & 0x3Cu));
    if (off >= HR_DBDMA && off < HR_DBDMA_END) {
        int chan = (int)((off - HR_DBDMA) >> 8);
        return GOS_LE32(dbdma_reg_read(st->dbdma, chan, off & 0xFFu));
    }
    if ((off & 0xFF000u) == HR_DAVBUS)
        return GOS_LE32(davbus_read32(&st->screamer_host, off - HR_DAVBUS));
    if (off >= GOS_HR_ATA0 && off < GOS_HR_ATA_END)
        return peek ? gos_ata_peek32(cfg, off) : gos_ata_read32(cfg, off);
    if ((off & 0xFF000u) == HR_BMAC)
        return peek ? gos_bmac_peek32(cfg, off - HR_BMAC) : gos_bmac_read32(cfg, off - HR_BMAC);
    // A longword cycle to a byte-wide cell: the cell drives lane 0, the
    // most significant byte on this big-endian bus.
    return ((uint32_t)hr_access8(ctx, off, peek) << 24);
}

static uint32_t hr_read32(void *ctx, uint32_t off) {
    return hr_access32(ctx, off, false);
}
static uint32_t hr_peek32(void *ctx, uint32_t off) {
    return hr_access32(ctx, off, true);
}

static void hr_write32(void *ctx, uint32_t off, uint32_t value) {
    config_t *cfg = (config_t *)ctx;
    gossamer_state_t *st = gos_st(cfg);
    if (off >= HR_INTS2 && off < HR_CTRL) {
        int b = (off < HR_INTS1) ? 1 : 0;
        int_write(cfg, b, (off - (b ? HR_INTS2 : HR_INTS1)) & 0xCu, GOS_LE32(value));
        return;
    }
    if (off >= HR_CTRL && off < 0x40u) {
        ctrl_write32le(cfg, off & 0x3Cu, GOS_LE32(value));
        return;
    }
    if (off >= HR_DBDMA && off < HR_DBDMA_END) {
        int chan = (int)((off - HR_DBDMA) >> 8);
        dbdma_reg_write(st->dbdma, chan, off & 0xFFu, GOS_LE32(value));
        return;
    }
    if ((off & 0xFF000u) == HR_DAVBUS) {
        davbus_write32(&st->screamer_host, off - HR_DAVBUS, GOS_LE32(value));
        return;
    }
    if (off >= GOS_HR_ATA0 && off < GOS_HR_ATA_END) {
        gos_ata_write32(cfg, off, value);
        return;
    }
    if ((off & 0xFF000u) == HR_BMAC) {
        gos_bmac_write32(cfg, off - HR_BMAC, value);
        return;
    }
    hr_write8(ctx, off, (uint8_t)(value >> 24));
}

// 16-bit access decomposes into bytes, big-endian — the bus's view of a
// halfword (the factory nvramrc's `90b7 f3000032 w!`).
// The ATA data register and BMAC's registers are the exceptions: 16-bit
// ports (gossamer_ata.c, gossamer_bmac.c).
static uint16_t hr_access16(void *ctx, uint32_t off, bool peek) {
    config_t *cfg = (config_t *)ctx;
    if (off >= GOS_HR_ATA0 && off < GOS_HR_ATA_END)
        return peek ? gos_ata_peek16(cfg, off) : gos_ata_read16(cfg, off);
    if ((off & 0xFF000u) == HR_BMAC)
        return peek ? gos_bmac_peek16(cfg, off - HR_BMAC) : gos_bmac_read16(cfg, off - HR_BMAC);
    return (uint16_t)((hr_access8(ctx, off, peek) << 8) | hr_access8(ctx, off + 1, peek));
}

static uint16_t hr_read16(void *ctx, uint32_t off) {
    return hr_access16(ctx, off, false);
}
static uint16_t hr_peek16(void *ctx, uint32_t off) {
    return hr_access16(ctx, off, true);
}

static void hr_write16(void *ctx, uint32_t off, uint16_t value) {
    if (off >= GOS_HR_ATA0 && off < GOS_HR_ATA_END) {
        gos_ata_write16((config_t *)ctx, off, value);
        return;
    }
    if ((off & 0xFF000u) == HR_BMAC) {
        gos_bmac_write16((config_t *)ctx, off - HR_BMAC, value);
        return;
    }
    hr_write8(ctx, off, (uint8_t)(value >> 8));
    hr_write8(ctx, off + 1, (uint8_t)value);
}

// ============================================================
// PCI presence: device $10, BAR0 512 KB
// ============================================================

static const char *hr_pci_name(const pci_device_t *dev) {
    (void)dev;
    return "Heathrow";
}

// Identity from the Rev C tree and the Rev A/B lspci: $106B:$0010 rev 1,
// class $FF0000, BAR0 32-bit non-prefetchable 512 KB, medium DEVSEL.
static const pci_config_decl_t hr_decl = {
    .vendor_id = 0x106Bu,
    .device_id = 0x0010u,
    .revision = 0x01u,
    .class_code = 0xFF0000u,
    .header_type = 0x00u,
    .command_writable = PCI_CMD_MEM_SPACE | PCI_CMD_MASTER | 0x0010u, // + MWI (the boot program writes $0016)
    .status_reset = 0x0200u, // DEVSEL medium
    .bar = {{.size = GOS_HEATHROW_SIZE, .kind = PCI_BAR_MEM}},
};

static const pci_device_ops_t hr_pci_ops = {
    .name = hr_pci_name,
};

void gos_heathrow_pci_attach(config_t *cfg, checkpoint_t *cp) {
    gossamer_state_t *st = gos_st(cfg);
    st->heathrow_if.read_uint8 = hr_read8;
    st->heathrow_if.read_uint16 = hr_read16;
    st->heathrow_if.read_uint32 = hr_read32;
    st->heathrow_if.peek_uint8 = hr_peek8;
    st->heathrow_if.peek_uint16 = hr_peek16;
    st->heathrow_if.peek_uint32 = hr_peek32;
    st->heathrow_if.write_uint8 = hr_write8;
    st->heathrow_if.write_uint16 = hr_write16;
    st->heathrow_if.write_uint32 = hr_write32;
    st->heathrow_dev.ops = &hr_pci_ops;
    st->heathrow_dev.decl = &hr_decl;
    st->heathrow_dev.priv = cfg;
    pci_cfg_reset(&st->heathrow_dev);
    pci_bar_backing_iface(&st->heathrow_dev, 0, &st->heathrow_if, cfg);
    pci_bus_add_device(st->bus, &st->heathrow_dev, GOS_DEV_HEATHROW);
    pci_device_part(cfg, cp, &st->heathrow_dev, "pci.heathrow");
}

// ============================================================
// Power-on state
// ============================================================

void gos_heathrow_init(config_t *cfg) {
    gos_heathrow_t *hr = &gos_st(cfg)->hr;
    // Everything masked, nothing latched, the power-on clear mode.  The
    // NVRAM contents are deliberately untouched: the store is non-volatile.
    memset(hr->events, 0, sizeof(hr->events));
    memset(hr->mask, 0, sizeof(hr->mask));
    memset(hr->levels, 0, sizeof(hr->levels));
    memset(hr->latch, 0, sizeof(hr->latch));
    memset(hr->mode1, 0, sizeof(hr->mode1));
    hr->reg30 = 0;
    hr->brightness = hr->contrast = 0;
    // MBCR: no media bay (ID 7 in bits 14-12), low byte 0 — the value the
    // boot program's SPD-address selector keys on (ROM $FFF055F0: a zero
    // low byte selects I2C $A0/$A2/$A4 for DIMMs 0-2).
    hr->mbcr = 0x00007000u;
    // FCR: the cells the published tree needs enabled; the boot program rewrites it to $033EFF3A then $01BEFFFA.
    hr->fcr = 0x01BEFFFAu;
    hr->aux = 0;
}

// ============================================================
// Object nodes: machine.heathrow (interrupt controller), machine.nvram
// ============================================================

static gos_heathrow_t *hr_obj(void *ctx) {
    return &gos_st((config_t *)ctx)->hr;
}

static uint32_t hr_obj_pending(void *ctx) {
    const gos_heathrow_t *hr = hr_obj(ctx);
    return hr->events[0] | hr->levels[0];
}
static uint32_t hr_obj_enabled(void *ctx) {
    return hr_obj(ctx)->mask[0];
}
static uint32_t hr_obj_active(void *ctx) {
    const gos_heathrow_t *hr = hr_obj(ctx);
    return hr->mode1[0] ? (hr->latch[0] & hr->mask[0]) : ((hr->events[0] | hr->levels[0]) & hr->mask[0]);
}
static int hr_obj_ipl(void *ctx) {
    const gos_heathrow_t *hr = hr_obj(ctx);
    uint32_t b2 = hr->mode1[1] ? (hr->latch[1] & hr->mask[1]) : ((hr->events[1] | hr->levels[1]) & hr->mask[1]);
    return (hr_obj_active(ctx) || b2) ? 1 : 0;
}

static const irq_controller_ops_t hr_irq_ops = {
    .chip = "Heathrow",
    .pending = hr_obj_pending,
    .enabled = hr_obj_enabled,
    .active = hr_obj_active,
    .ipl = hr_obj_ipl,
};

#define HR_U32_ATTR(NAME, EXPR)                                                                                        \
    static value_t hr_attr_##NAME(struct object *self, const member_t *m) {                                            \
        (void)m;                                                                                                       \
        const gos_heathrow_t *hr = hr_obj(object_data(self));                                                          \
        value_t v = val_uint(4, (EXPR));                                                                               \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }

HR_U32_ATTR(events, hr->events[0])
HR_U32_ATTR(levels, hr->levels[0])
HR_U32_ATTR(mask, hr->mask[0])
HR_U32_ATTR(latch, hr->latch[0])
HR_U32_ATTR(events2, hr->events[1])
HR_U32_ATTR(levels2, hr->levels[1])
HR_U32_ATTR(mask2, hr->mask[1])
HR_U32_ATTR(latch2, hr->latch[1])
HR_U32_ATTR(fcr, hr->fcr)
HR_U32_ATTR(mbcr, hr->mbcr)

static value_t hr_attr_clear_mode(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, hr_obj(object_data(self))->mode1[0] ? 1u : 0u);
}

#define HR_RO_ATTR(NAME, DOC, FLAGS)                                                                                   \
    {                                                                                                                  \
        .kind = M_ATTR, .name = #NAME, .doc = DOC, .attr = {                                                           \
            .type = V_UINT,                                                                                            \
            .presentation_flags = (FLAGS),                                                                             \
            .get = hr_attr_##NAME,                                                                                     \
            .set = NULL                                                                                                \
        }                                                                                                              \
    }

static const member_t hr_members[] = {
    IRQ_CONTROLLER_MEMBERS(&hr_irq_ops)
        HR_RO_ATTR(events, "Bank 1 edge-latched events (sources 0-31)", VAL_HEX | VAL_VOLATILE),
    {.kind = M_ATTR,
                                                                                   .name = "source_levels",
                                                                                   .doc = "Bank 1 live source picture, never latched",
                                                                                   .attr = {.type = V_UINT, .presentation_flags = VAL_HEX | VAL_VOLATILE, .get = hr_attr_levels, .set = NULL}},
    HR_RO_ATTR(mask, "Bank 1 per-source enables", VAL_HEX),
    HR_RO_ATTR(latch, "Bank 1 mode-1 output latch", VAL_HEX | VAL_VOLATILE),
    HR_RO_ATTR(events2, "Bank 2 edge-latched events (sources 32-63)", VAL_HEX | VAL_VOLATILE),
    HR_RO_ATTR(levels2, "Bank 2 live source picture", VAL_HEX | VAL_VOLATILE),
    HR_RO_ATTR(mask2, "Bank 2 per-source enables", VAL_HEX),
    HR_RO_ATTR(latch2, "Bank 2 mode-1 output latch", VAL_HEX | VAL_VOLATILE),
    HR_RO_ATTR(fcr, "Feature Control Register (+$38)", VAL_HEX),
    HR_RO_ATTR(mbcr, "Media-bay control / ID register (+$34)", VAL_HEX),
    {.kind = M_ATTR,
                                                                                   .name = "clear_mode",
                                                                                   .doc = "Bank 1: 0 = power-on ((events|levels) & mask); 1 = NanoKernel acknowledge (latch & mask)",
                                                                                   .attr = {.type = V_UINT, .get = hr_attr_clear_mode, .set = NULL}                                          },
};

static const class_desc_t hr_class = {
    .name = "irq_controller",
    .members = hr_members,
    .n_members = sizeof(hr_members) / sizeof(hr_members[0]),
};

// machine.nvram: the flat 8 KB store (the tnt shape: peek/poke/dump/
// snapshot/restore/clear).
static uint8_t *nvram_store(struct object *self) {
    gossamer_state_t *st = gos_st((config_t *)object_data(self));
    return st ? st->hr.nvram : NULL;
}

static value_t nvram_method_peek(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    uint64_t addr = argv[0].u;
    if (addr >= GOS_NVRAM_SIZE)
        return val_err("nvram.peek: offset 0x%llX is outside the %u-byte store", (unsigned long long)addr,
                       GOS_NVRAM_SIZE);
    return val_uint(1, nv[addr]);
}

static value_t nvram_method_poke(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    uint64_t addr = argv[0].u;
    const value_t *bytes = &argv[1];
    if (bytes->kind != V_BYTES || !bytes->bytes.p || bytes->bytes.n == 0)
        return val_err("nvram.poke: bytes argument must be non-empty V_BYTES (e.g. 0x80:1)");
    size_t n = bytes->bytes.n;
    if (addr >= GOS_NVRAM_SIZE || addr + n > GOS_NVRAM_SIZE)
        return val_err("nvram.poke: write of %zu bytes at 0x%llX would overflow the %u-byte store", n,
                       (unsigned long long)addr, GOS_NVRAM_SIZE);
    memcpy(nv + addr, bytes->bytes.p, n);
    return val_none();
}

static value_t nvram_method_dump(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    uint64_t addr = argv[0].u, n = argv[1].u;
    if (addr >= GOS_NVRAM_SIZE || n == 0 || addr + n > GOS_NVRAM_SIZE)
        return val_err("nvram.dump: read of %llu bytes at 0x%llX would overflow the %u-byte store",
                       (unsigned long long)n, (unsigned long long)addr, GOS_NVRAM_SIZE);
    return val_bytes(nv + addr, (size_t)n);
}

static value_t nvram_method_snapshot(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    return val_bytes(nv, GOS_NVRAM_SIZE);
}

static value_t nvram_method_restore(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    const value_t *bytes = &argv[0];
    if (bytes->kind != V_BYTES || bytes->bytes.n != GOS_NVRAM_SIZE || !bytes->bytes.p)
        return val_err("nvram.restore: expected V_BYTES of length %u", GOS_NVRAM_SIZE);
    memcpy(nv, bytes->bytes.p, GOS_NVRAM_SIZE);
    return val_none();
}

static value_t nvram_method_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || !gos_st(cfg))
        return val_err("nvram not available");
    gos_nvram_clear(cfg);
    return val_bool(true);
}

// The named fields (tnt's grand_central.c has the same set, plus depth).
static const arg_decl_t nvram_getenv_args[] = {
    {.name = "name", .kind = V_STRING, .doc = "Open Firmware variable, e.g. \"boot-device\""},
};
static const arg_decl_t nvram_setenv_args[] = {
    {.name = "name",  .kind = V_STRING, .doc = "Open Firmware variable"                 },
    {.name = "value", .kind = V_STRING, .doc = "true/false, a hex number, or the string"},
};

static value_t nvram_method_getenv(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    char buf[OF_NVRAM_OF_SIZE + 1];
    if (of_nvram_getenv(nv, argv[0].s, buf, sizeof(buf)) == OF_VAR_NONE)
        return val_err("nvram.getenv: no variable '%s' (or no valid Open Firmware partition)", argv[0].s);
    return val_str(buf);
}

static value_t nvram_method_setenv(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    const char *err = of_nvram_setenv(nv, argv[0].s, argv[1].s);
    if (err)
        return val_err("nvram.setenv %s: %s", argv[0].s, err);
    return val_none();
}

static value_t nvram_attr_startup_disk(struct object *self, const member_t *m) {
    (void)m;
    uint8_t *nv = nvram_store(self);
    return val_int(nv ? of_nvram_startup_scsi(nv) : -1);
}

static value_t nvram_attr_startup_disk_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    uint8_t *nv = nvram_store(self);
    bool ok = true;
    int64_t id = val_as_i64(&in, &ok);
    value_free(&in);
    if (!nv)
        return val_err("nvram not available");
    if (!ok || id < -1 || id > 6)
        return val_err("nvram.startup_disk: a SCSI ID 0..6, or -1 for no default");
    of_nvram_set_startup_scsi(nv, (int)id, &of_nvram_defaults_g3);
    return val_none();
}

static value_t nvram_attr_size(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, GOS_NVRAM_SIZE);
}

static const arg_decl_t nvram_peek_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "byte offset (0..$1FFF)"},
};
static const arg_decl_t nvram_poke_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "byte offset (0..$1FFF)"},
    {.name = "bytes", .kind = V_BYTES, .doc = "1..N bytes to write (use the :N integer-width suffix)"},
};
static const arg_decl_t nvram_dump_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "byte offset (0..$1FFF)"},
    {.name = "n", .kind = V_UINT, .doc = "byte count"},
};
static const arg_decl_t nvram_restore_args[] = {
    {.name = "bytes", .kind = V_BYTES, .doc = "8192-byte buffer (typically from nvram.snapshot)"},
};

static const member_t nvram_members[] = {
    {.kind = M_ATTR,
     .name = "size",
     .doc = "Store size in bytes",
     .attr = {.type = V_UINT, .get = nvram_attr_size, .set = NULL}                                   },
    {.kind = M_METHOD,
     .name = "peek",
     .doc = "Read one byte (XPRAM at $1300, Name Registry at $1400, Open Firmware at $1800)",
     .method = {.args = nvram_peek_args, .nargs = 1, .result = V_UINT, .fn = nvram_method_peek}      },
    {.kind = M_METHOD,
     .name = "poke",
     .doc = "Write 1..N bytes at an offset",
     .method = {.args = nvram_poke_args, .nargs = 2, .result = V_NONE, .fn = nvram_method_poke}      },
    {.kind = M_METHOD,
     .name = "dump",
     .doc = "Read N bytes starting at an offset",
     .method = {.args = nvram_dump_args, .nargs = 2, .result = V_BYTES, .fn = nvram_method_dump}     },
    {.kind = M_METHOD,
     .name = "snapshot",
     .doc = "Read the whole 8 KB store",
     .method = {.args = NULL, .nargs = 0, .result = V_BYTES, .fn = nvram_method_snapshot}            },
    {.kind = M_METHOD,
     .name = "restore",
     .doc = "Write the whole store from a snapshot",
     .method = {.args = nvram_restore_args, .nargs = 1, .result = V_NONE, .fn = nvram_method_restore}},
    {.kind = M_METHOD,
     .name = "clear",
     .doc = "Pull the battery: back to the store a new board carries",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = nvram_method_clear}                },
    {.kind = M_METHOD,
     .name = "getenv",
     .doc = "Read an Open Firmware variable, as printenv shows it",
     .method = {.args = nvram_getenv_args, .nargs = 1, .result = V_STRING, .fn = nvram_method_getenv}},
    {.kind = M_METHOD,
     .name = "setenv",
     .doc = "Set an Open Firmware variable, as setenv does (repacks, re-checksums)",
     .method = {.args = nvram_setenv_args, .nargs = 2, .result = V_NONE, .fn = nvram_method_setenv}  },
    {.kind = M_ATTR,
     .name = "startup_disk",
     .doc = "Mac OS's default startup device as a SCSI ID (XPRAM $78-$7B); -1 = none",
     .attr = {.type = V_INT, .get = nvram_attr_startup_disk, .set = nvram_attr_startup_disk_set}     },
};

static const class_desc_t nvram_class = {
    .name = "nvram",
    .members = nvram_members,
    .n_members = sizeof(nvram_members) / sizeof(nvram_members[0]),
};

void gos_heathrow_attach_objects(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st || st->hr_object)
        return;
    st->hr_object = object_new(&hr_class, cfg, "heathrow");
    if (st->hr_object) {
        object_set_label(st->hr_object, "Heathrow");
        object_set_order(st->hr_object, 45);
        object_attach(machine_object(), st->hr_object);
    }
    st->nvram_object = object_new(&nvram_class, cfg, "nvram");
    if (st->nvram_object) {
        object_set_label(st->nvram_object, "NVRAM");
        object_set_order(st->nvram_object, 46);
        object_attach(machine_object(), st->nvram_object);
    }
}

void gos_heathrow_detach_objects(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (st && st->nvram_object) {
        object_detach(st->nvram_object);
        object_delete(st->nvram_object);
        st->nvram_object = NULL;
    }
    if (st && st->hr_object) {
        object_detach(st->hr_object);
        object_delete(st->hr_object);
        st->hr_object = NULL;
    }
}
