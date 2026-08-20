// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// amic.c
// AMIC ("Apple Memory-mapped I/O Controller") — the PDM I/O chip: all
// I/O-space decode, the classic-Mac interrupt model (pseudo-VIA1 as a real
// 6522 core instance, a pseudo-VIA2/RBV-style slot+device bank, and the
// top-level interrupt control register driving the 601's single INT line),
// the DMA-engine register file, and the VBL raster.  The sound block
// dispatches to awacs.c, video control and the Ariel CLUT to ariel.c;
// remaining DMA datapaths (SCSI, floppy, SCC, Ethernet) land with their
// ladder rungs in later phases.
//
// Register truth: Apple, "Power Macintosh Computers" Developer Note (1994)
// Fig 2-2 and pp. 15-23, the 8100 schematic set, and the shipping 1994-03
// ROM's hardware-init writes.  Behavioral notes cite the corresponding
// per-device model decisions (interrupt latch semantics, active-low slot
// reads, write-1-to-clear conventions).

#include "pdm.h"

#include "log.h"
#include "ppc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "via.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("amic");

// Island offsets (relative to $50F00000)
#define OFF_VIA1  0x00000u // ..$01FFF: 6522 map, reg n at n*$200
#define OFF_SCC   0x04000u
#define OFF_EPROM 0x08000u // Ethernet ID PROM
#define OFF_MACE  0x0A000u
#define OFF_SCSIA 0x10000u
#define OFF_SCSIB 0x11000u
#define OFF_SOUND 0x14000u // $20-byte AWACS/sound-DMA block
#define OFF_SWIM3 0x16000u
#define OFF_ARIEL 0x24000u // 4 byte regs
#define OFF_VIA2  0x26000u // slot/device interrupt + config bank
#define OFF_VIDEO 0x28000u // mode/depth/sense/test + beam counters
#define OFF_ICR   0x2A000u // +0 ICR, +8/+A DMA flags
#define OFF_DIAG  0x2C000u // bit 0 must read 1
#define OFF_DMA   0x31000u // DMA register file (through $322xx)

// DMA channel control bits (common vocabulary)
#define DMA_RST 0x01u
#define DMA_RUN 0x02u
#define DMA_IE  0x08u
#define DMA_IF  0x80u

static void pdm_scsi_pump_arm(config_t *cfg); // SCSI DMA service loop (below)

// ============================================================
// Interrupt fabric
// ============================================================

// Aggregate the pseudo-VIA2 bank: device IFR bit 7 = OR of enabled bits
// 6..0.  Slot sources fold in through the ANY SLOT bit (device bit 1); the
// SCSI DRQ bits (0 = Curio, 2 = 53CF96) read the chips' DREQ outputs LIVE
// — the SCSI Manager's Ck4DREQ polls them, never latches or enables them.
static uint8_t via2_dev_ifr(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    pdm_via2_t *v2 = &st->amic.via2;
    // Slot flags: register reads active-low; internal any-slot aggregate
    // works on the asserted (low) bits gated by the slot IER.
    uint8_t slot_asserted = (uint8_t)(~v2->slot_ifr & 0x7Cu);
    uint8_t levels = v2->dev_levels;
    if (slot_asserted & v2->slot_ier & 0x78u)
        levels |= 0x02u; // ANY SLOT
    else
        levels &= ~0x02u;
    v2->dev_levels = levels;
    uint8_t ifr = levels & 0x7Fu;
    if (st->scsi96[0] && scsi_53c96_dreq(st->scsi96[0]))
        ifr |= 0x01u; // SCSI-A DRQ
    if (st->scsi96[1] && scsi_53c96_dreq(st->scsi96[1]))
        ifr |= 0x04u; // SCSI-B DRQ
    if (ifr & v2->dev_ier & 0x7Fu)
        ifr |= 0x80u;
    return ifr;
}

// 53C9x INT pin levels (level-sensitive: the HAL declares the interrupt
// LEVEL and clears it only by reading the chip's Interrupt register, which
// drops the pin; writes of the "clear" values to the IFR are no-ops).
void pdm_amic_set_scsi_irq(config_t *cfg, int chip, bool level) {
    pdm_via2_t *v2 = &pdm_st(cfg)->amic.via2;
    uint8_t bit = chip ? 0x40u : 0x08u;
    if (level)
        v2->dev_levels |= bit;
    else
        v2->dev_levels &= (uint8_t)~bit;
    pdm_amic_recompute(cfg);
}

// SWIM3's own IRQ pin (SwimIntReq*): pseudo-VIA2 device bit 5, dispatched
// at 68k level 2.  Level-sensitive like the 53C9x INT lines — the chip
// drops it when the driver reads (and clears) its Interrupt register.
void pdm_amic_set_fdc_irq(config_t *cfg, bool level) {
    pdm_via2_t *v2 = &pdm_st(cfg)->amic.via2;
    if (level)
        v2->dev_levels |= 0x20u;
    else
        v2->dev_levels &= (uint8_t)~0x20u;
    pdm_amic_recompute(cfg);
}

// NuBus slot /NMRQ levels.  Each connector's line runs from the slot to an
// AMIC pin (the bridge is not in the path), and the slot bank presents them
// ACTIVE LOW: slot $B is bit 2 ... slot $E bit 5, so an asserted line CLEARS
// its bit.  Level-sensitive — the card drops /NMRQ when its handler has
// serviced it; nothing here is write-1-to-clear.
void pdm_amic_set_slot_irq(config_t *cfg, int slot, bool level) {
    pdm_via2_t *v2 = &pdm_st(cfg)->amic.via2;
    uint8_t bit = (uint8_t)(1u << (slot - 9));
    if (level)
        v2->slot_ifr &= (uint8_t)~bit;
    else
        v2->slot_ifr |= bit;
    pdm_amic_recompute(cfg);
}

void pdm_amic_set_source(config_t *cfg, int bit, bool level) {
    pdm_state_t *st = pdm_st(cfg);
    uint8_t old = st->icr_sources;
    if (level)
        st->icr_sources |= (uint8_t)(1u << bit);
    else
        st->icr_sources &= (uint8_t) ~(1u << bit);
    if (st->icr_sources != old)
        pdm_amic_recompute(cfg);
}

// The combinational DMA-channel half of the flags mirror ($50F2A008):
// channel IF & IE per selector (bits 0-6).
static uint8_t dma_irq_summary(pdm_amic_t *a) {
    return (uint8_t)(((a->scc[3].ctrl & DMA_IF) && (a->scc[3].ctrl & DMA_IE) ? 0x01u : 0) |
                     ((a->scc[2].ctrl & DMA_IF) && (a->scc[2].ctrl & DMA_IE) ? 0x02u : 0) |
                     ((a->scc[1].ctrl & DMA_IF) && (a->scc[1].ctrl & DMA_IE) ? 0x04u : 0) |
                     ((a->scc[0].ctrl & DMA_IF) && (a->scc[0].ctrl & DMA_IE) ? 0x08u : 0) |
                     ((a->enet_rx.ctrl & DMA_IF) && (a->enet_rx.ctrl & DMA_IE) ? 0x10u : 0) |
                     ((a->enet_tx.ctrl & DMA_IF) && (a->enet_tx.ctrl & DMA_IE) ? 0x20u : 0) |
                     ((a->floppy.ctrl & DMA_IF) && (a->floppy.ctrl & DMA_IE) ? 0x40u : 0));
}

// Recompute the ICR picture and drive the 601 external-interrupt line.
// INTMODE=1 (the shipping state) is "interrupt on CHANGE": any change of
// the source picture — assertion OR deassertion — latches CPUINT until
// acked; a source that merely stays asserted does not re-latch after the
// ack.  Both halves are load-bearing:
//   - assertion-only latching (no level re-latch after ack) keeps the
//     machine out of a livelock while the early 68k boot runs at IPL 7
//     with a pending-but-unserviced VIA source;
//   - DEASSERTION latching is how the nanokernel learns a source went
//     away: it re-reads the flags, finds 0, and clears the 68k
//     emulator's posted interrupt level — without it the emulator
//     redelivers the stale level forever (the 68k dispatcher's "no
//     source" jump-table entry exists precisely for these change
//     interrupts).
void pdm_amic_recompute(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    pdm_amic_t *a = &st->amic;

    // Fold the pseudo-VIA2 aggregate into the source picture first
    uint8_t dev = via2_dev_ifr(cfg);
    if (dev & 0x80u)
        st->icr_sources |= 1u << PDM_ICR_VIA2;
    else
        st->icr_sources &= (uint8_t) ~(1u << PDM_ICR_VIA2);

    // ...and the DMA source: the per-engine flag registers, gated by their
    // own enables, summarize combinationally through the $50F2A008/$0A
    // mirror bytes into one ICR source (handlers ack in the engine
    // registers; the mirrors are never written).
    if (dma_irq_summary(a) | pdm_awacs_irq_summary(a))
        st->icr_sources |= 1u << PDM_ICR_DMA;
    else
        st->icr_sources &= (uint8_t) ~(1u << PDM_ICR_DMA);

    uint8_t changed = st->icr_sources ^ a->icr_seen;
    a->icr_seen = st->icr_sources;
    if (changed)
        a->icr_latch = 1;
    if (cfg->ppc)
        ppc_set_ext_irq(cfg->ppc, a->icr_mode ? a->icr_latch != 0 : st->icr_sources != 0);
}

// ============================================================
// Pseudo-VIA2 bank ($50F26000)
// ============================================================

// The VIA2 bank partial-decodes on the LOW FIVE address bits, mirroring the
// 32-byte register file across the whole $50F26000-$50F27FFF window.  This
// is load-bearing: the SCSI HAL addresses the bank compactly ($50F26003/
// $50F26013), while the generic level-2 interrupt dispatcher reads the same
// registers at classic-VIA stride ($50F26000+$1A03 for the IFR, +$1C13 for
// the IER — register $D/$E windows with the matching byte lane, whose low
// five bits alias to the compact offsets).  Without the mirror the
// dispatcher reads zeros, computes "no source", and never services the
// asserted SCSI level — an interrupt storm that starves the whole 68k.
static uint8_t via2_read(config_t *cfg, uint32_t off) {
    pdm_via2_t *v2 = &pdm_st(cfg)->amic.via2;
    off &= 0x1Fu;
    switch (off) {
    case 0x02:
        return v2->slot_ifr; // active-low levels, unused bits high
    case 0x03:
        return via2_dev_ifr(cfg);
    case 0x12:
        return v2->slot_ier;
    case 0x13:
        return v2->dev_ier;
    default:
        return off < 0x08 ? v2->misc[off] : 0;
    }
}

static void via2_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_via2_t *v2 = &pdm_st(cfg)->amic.via2;
    off &= 0x1Fu; // low-five-bit partial decode (see via2_read)
    switch (off) {
    case 0x02:
        // Only the VBL latch is software-clearable: writing $40 deasserts
        // it (sets the bit back to 1 — the register reads active-low).
        // Slot bits are live levels owned by the cards.
        if (value & 0x40u)
            v2->slot_ifr |= 0x40u;
        break;
    case 0x03:
        // VIA-style flag write with bit 7 ignored; the SCSI/FDC/slot bits
        // are levels, so writes are no-ops for them (the HAL's $88/$82
        // clears are satisfied by the level model).
        break;
    case 0x12: // set/clear convention, writable mask $78
        if (value & 0x80u)
            v2->slot_ier |= value & 0x78u;
        else
            v2->slot_ier &= (uint8_t) ~(value & 0x78u);
        break;
    case 0x13: // writable mask $7B (bit 6 = SCSI-B enable, the HAL's $C8)
        if (value & 0x80u)
            v2->dev_ier |= value & 0x7Bu;
        else
            v2->dev_ier &= (uint8_t) ~(value & 0x7Bu);
        break;
    default:
        if (off < 0x08)
            v2->misc[off] = value;
        break;
    }
    pdm_amic_recompute(cfg);
}

// ============================================================
// DMA register file ($50F31000 base)
// ============================================================

// Channel-control write with the common bit vocabulary: RST self-clears
// and stops the channel (SCSI keeps DIR and the bus-speed field), IF is
// write-1-to-clear, the rest stores.
static void dma_ctrl_write(config_t *cfg, pdm_dma_ch_t *ch, uint8_t value, uint8_t keep_on_rst) {
    if (value & DMA_RST) {
        ch->ctrl &= keep_on_rst;
        ch->count = 0;
    } else {
        uint8_t w = value & (uint8_t) ~(DMA_RST | DMA_IF);
        ch->ctrl = (uint8_t)((ch->ctrl & DMA_IF & ~(value & DMA_IF)) | w);
    }
    pdm_amic_recompute(cfg);
}

// The DMA window's physical base ($50F31000-01; the low two bytes are
// ignored — the window is 256 KB aligned, so they are always zero).  Every
// channel's buffer lives inside this one contiguous window.
static uint32_t dma_window_base(pdm_amic_t *a) {
    return ((uint32_t)a->dma_base[0] << 24) | ((uint32_t)a->dma_base[1] << 16);
}

// Byte lane helpers for the 32-bit address registers (MSB at +0)
static void addr_write_byte(uint32_t *addr, uint32_t lane, uint8_t v) {
    uint32_t shift = 8 * (3 - lane);
    *addr = (*addr & ~(0xFFu << shift)) | ((uint32_t)v << shift);
}

static uint8_t addr_read_byte(uint32_t addr, uint32_t lane) {
    return (uint8_t)(addr >> (8 * (3 - lane)));
}

static uint8_t dma_read(config_t *cfg, uint32_t off) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    switch (off) {
    case 0x0000:
    case 0x0001:
    case 0x0002:
    case 0x0003:
        return a->dma_base[off];
    case 0x0C20:
        // Enet Tx SET0/SET1 read 1 = "buffer empty/available"
        return a->enet_tx.ctrl | 0x60u;
    case 0x1000:
    case 0x1001:
    case 0x1002:
    case 0x1003:
        return addr_read_byte(a->scsi[0].addr, off & 3);
    case 0x1004:
    case 0x1005:
    case 0x1006:
    case 0x1007:
        return addr_read_byte(a->scsi[1].addr, off & 3);
    case 0x1008:
        return a->scsi[0].ctrl;
    case 0x1009:
        return a->scsi[1].ctrl;
    case 0x1010:
    case 0x1011:
    case 0x1012:
    case 0x1013:
        return addr_read_byte(a->scsi[0].addr, off & 3); // current = base (no engine yet)
    case 0x1014:
    case 0x1015:
    case 0x1016:
    case 0x1017:
        return addr_read_byte(a->scsi[1].addr, off & 3);
    case 0x1028:
        return a->enet_rx.ctrl;
    case 0x1030:
        return a->enet_rx_head;
    case 0x1034:
        return a->enet_rx_tail;
    case 0x1044:
        return (uint8_t)(a->enet_tx_count[0] >> 8);
    case 0x1045:
        return (uint8_t)a->enet_tx_count[0];
    case 0x1054:
        return (uint8_t)(a->enet_tx_count[1] >> 8);
    case 0x1055:
        return (uint8_t)a->enet_tx_count[1];
    case 0x1060:
    case 0x1061:
    case 0x1062:
    case 0x1063:
        return addr_read_byte(a->floppy.addr, off & 3);
    case 0x1064:
        return (uint8_t)(a->floppy.count >> 8);
    case 0x1065:
        return (uint8_t)a->floppy.count;
    case 0x1068:
        return a->floppy.ctrl;
    case 0x1100:
        return (uint8_t)(a->dma_berr_en >> 8);
    case 0x1101:
        return (uint8_t)a->dma_berr_en;
    case 0x1102:
        return (uint8_t)(a->dma_berr_flag >> 8);
    case 0x1103:
        return (uint8_t)a->dma_berr_flag;
    default:
        // SCC channels: $1080/$1090/$10A0/$10B0 + {addr32, count16, ctrl}
        if (off >= 0x1080 && off < 0x10C0) {
            pdm_dma_ch_t *ch = &a->scc[(off - 0x1080) >> 4];
            uint32_t r = off & 0xFu;
            if (r < 4)
                return addr_read_byte(ch->addr, r);
            if (r == 4)
                return (uint8_t)((ch->count >> 8) & 0x1Fu);
            if (r == 5)
                return (uint8_t)ch->count;
            if (r == 8)
                return ch->ctrl;
        }
        return 0;
    }
}

static void dma_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    switch (off) {
    case 0x0000:
    case 0x0001:
    case 0x0002:
    case 0x0003:
        a->dma_base[off] = value;
        return;
    case 0x0C20:
        dma_ctrl_write(cfg, &a->enet_tx, value, 0);
        return;
    case 0x1000:
    case 0x1001:
    case 0x1002:
    case 0x1003:
        addr_write_byte(&a->scsi[0].addr, off & 3, value);
        return;
    case 0x1004:
    case 0x1005:
    case 0x1006:
    case 0x1007:
        addr_write_byte(&a->scsi[1].addr, off & 3, value);
        return;
    case 0x1008:
        // Bus-speed bits 3:2 share the register and must stick (the ROM's
        // AMIC-2 probe); DIR survives RST.
        if (value & DMA_RST) {
            a->scsi[0].ctrl &= 0x4Cu; // keep DIR + speed bits
        } else {
            LOG(4, "scsi dma A ctrl=$%02X addr=$%08X", value, a->scsi[0].addr);
            a->scsi[0].ctrl = value & (uint8_t) ~(DMA_RST | 0x10u); // FLUSH self-clears
            if (value & DMA_RUN)
                pdm_scsi_pump_arm(cfg);
        }
        return;
    case 0x1009:
        if (value & DMA_RST) {
            a->scsi[1].ctrl &= 0x4Cu;
        } else {
            a->scsi[1].ctrl = value & (uint8_t) ~(DMA_RST | 0x10u);
            if (value & DMA_RUN)
                pdm_scsi_pump_arm(cfg);
        }
        return;
    case 0x1028:
        dma_ctrl_write(cfg, &a->enet_rx, value, 0);
        return;
    case 0x1030:
        a->enet_rx_head = value;
        return;
    case 0x1034:
        a->enet_rx_tail = value;
        return;
    case 0x1044:
        a->enet_tx_count[0] = (uint16_t)((a->enet_tx_count[0] & 0x00FFu) | ((value & 0x0Fu) << 8));
        return;
    case 0x1045:
        a->enet_tx_count[0] = (uint16_t)((a->enet_tx_count[0] & 0xFF00u) | value);
        return;
    case 0x1054:
        a->enet_tx_count[1] = (uint16_t)((a->enet_tx_count[1] & 0x00FFu) | ((value & 0x0Fu) << 8));
        return;
    case 0x1055:
        a->enet_tx_count[1] = (uint16_t)((a->enet_tx_count[1] & 0xFF00u) | value);
        return;
    case 0x1060:
    case 0x1061:
    case 0x1062:
    case 0x1063:
        addr_write_byte(&a->floppy.addr, off & 3, value);
        return;
    case 0x1064:
        a->floppy.count = (uint16_t)((a->floppy.count & 0x00FFu) | (value << 8));
        return;
    case 0x1065:
        a->floppy.count = (uint16_t)((a->floppy.count & 0xFF00u) | value);
        return;
    case 0x1068:
        // DMARST re-points the channel at its default region: the floppy
        // channel is hard-wired into the window's SECOND 64 KB at offset
        // $15000.  The .Sony driver derives its whole track-cache pointer
        // from this readback minus the window base ($50F31000), so the
        // register must hold the full PHYSICAL address, not the offset —
        // the ROM programs a non-zero window base ($05780000 at HWInit).
        if (value & DMA_RST)
            a->floppy.addr = dma_window_base(a) + 0x15000u;
        dma_ctrl_write(cfg, &a->floppy, value, 0);
        LOG(4, "floppy dma ctrl=$%02X addr=$%08X count=%u", a->floppy.ctrl, a->floppy.addr, a->floppy.count);
        return;
    case 0x1100:
        a->dma_berr_en = (uint16_t)((a->dma_berr_en & 0x00FFu) | (value << 8));
        return;
    case 0x1101:
        a->dma_berr_en = (uint16_t)((a->dma_berr_en & 0xFF00u) | value);
        return;
    case 0x1102:
    case 0x1103:
        return; // flag register: inert
    default:
        if (off >= 0x1080 && off < 0x10C0) {
            pdm_dma_ch_t *ch = &a->scc[(off - 0x1080) >> 4];
            uint32_t r = off & 0xFu;
            if (r < 4)
                addr_write_byte(&ch->addr, r, value);
            else if (r == 4)
                ch->count = (uint16_t)((ch->count & 0x00FFu) | ((value & 0x1Fu) << 8));
            else if (r == 5)
                ch->count = (uint16_t)((ch->count & 0xFF00u) | value);
            else if (r == 8)
                dma_ctrl_write(cfg, ch, value, 0x70u); // keep RELOAD/FROZEN/PAUSE
        }
        return;
    }
}

// ============================================================
// SCSI: 53C9x register files + AMIC DMA datapath
// ============================================================
// Chip register files at island +$10000 (Curio) / +$11000 (53CF96, 8100),
// 16-byte stride, with the handshaked pseudo-DMA aperture at +$100 of
// each.  The AMIC SCSI DMA channels are single-shot and address-only —
// no count register, no ping-pong: transfer length is governed entirely
// by the 53C9x transfer counter, and AMIC just services DREQ from/to the
// advancing address while RUN is set.  The pump below is the functional
// model of that service loop, cadenced like the AV PSC pump; FLUSH
// self-clears immediately because nothing is staged (bytes move whole).

#define PDM_SCSI_PUMP_NS  10000.0 // 10 us cadence while a channel runs
#define PDM_SCSI_PUMP_MAX 2048 // bytes per firing and channel

// Physical RAM byte access through the identity page table (the DMA buffer
// sits wherever the HMC mapped it; the 7100/8100 fixed bank windows are
// not host-identity).  Shared by the SCSI pump and the floppy movers.
static uint8_t *dma_host_ptr(uint32_t phys) {
    uint32_t page = phys >> PAGE_SHIFT;
    if (page >= (uint32_t)g_page_count)
        return NULL;
    uint8_t *host = g_page_table[page].host_base;
    return host ? host + (phys & ((1u << PAGE_SHIFT) - 1u)) : NULL;
}

static uint8_t pdm_scsi_io_read(config_t *cfg, int chip, uint32_t off) {
    scsi_53c96_t *c = pdm_st(cfg)->scsi96[chip];
    if (!c) {
        LOG(2, "read of absent SCSI chip %d at +$%03X", chip, off);
        return 0;
    }
    if (off < 0x100u)
        return scsi_53c96_read(c, (off & 0xFFu) >> 4);
    if (off < 0x200u)
        return scsi_53c96_pdma_read8(c); // handshaked aperture
    LOG(2, "read of undecoded SCSI space chip %d +$%03X", chip, off);
    return 0;
}

static void pdm_scsi_io_write(config_t *cfg, int chip, uint32_t off, uint8_t value) {
    scsi_53c96_t *c = pdm_st(cfg)->scsi96[chip];
    if (!c) {
        LOG(2, "write of absent SCSI chip %d at +$%03X = $%02X", chip, off, value);
        return;
    }
    if (off < 0x100u) {
        scsi_53c96_write(c, (off & 0xFFu) >> 4, value);
        pdm_amic_recompute(cfg); // INT/DREQ levels may have moved
        return;
    }
    if (off < 0x200u) {
        scsi_53c96_pdma_write8(c, value);
        return;
    }
    LOG(2, "write of undecoded SCSI space chip %d +$%03X", chip, off);
}

// Only move bytes while the bus is in an information-transfer phase the
// chip's DMA command covers — mirrors the AV pump's gate.
static bool pdm_scsi_data_phase(config_t *cfg) {
    int ph = scsi_get_bus_phase(cfg->scsi);
    return ph == scsi_data_in || ph == scsi_data_out || ph == scsi_command;
}

static void pdm_scsi_pump_event(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    pdm_state_t *st = pdm_st(cfg);
    pdm_amic_t *a = &st->amic;
    bool any_running = false;
    for (int chip = 0; chip < 2; chip++) {
        pdm_dma_ch_t *ch = &a->scsi[chip];
        scsi_53c96_t *c96 = st->scsi96[chip];
        if (!c96 || !(ch->ctrl & DMA_RUN))
            continue;
        any_running = true;
        if (chip == 1)
            continue; // no bus behind the 8100 fast chip: DREQ never asserts
        bool mem_to_scsi = (ch->ctrl & 0x40u) != 0; // DIR
        int moved = 0;
        while (moved < PDM_SCSI_PUMP_MAX && pdm_scsi_data_phase(cfg) && scsi_53c96_dreq(c96)) {
            uint8_t *host = dma_host_ptr(ch->addr);
            if (!host)
                break; // window points outside RAM: drop the request
            if (mem_to_scsi) {
                scsi_53c96_pdma_write8(c96, *host);
            } else {
                if (!g_page_table[ch->addr >> PAGE_SHIFT].writable)
                    break;
                *host = scsi_53c96_pdma_read8(c96);
            }
            ch->addr++;
            moved++;
        }
        if (moved) {
            LOG(4, "pump chip%d moved %d, addr now $%08X dreq=%d phase=%d", chip, moved, ch->addr, scsi_53c96_dreq(c96),
                scsi_get_bus_phase(cfg->scsi));
            pdm_amic_recompute(cfg);
        }
    }
    if (any_running)
        scheduler_new_cpu_event(cfg->scheduler, pdm_scsi_pump_event, cfg, 0, 0, (uint64_t)PDM_SCSI_PUMP_NS);
}

// Arm the pump when a SCSI channel starts running (ctrl-write hook; the
// event keeps itself alive while any channel has RUN set).
static void pdm_scsi_pump_arm(config_t *cfg) {
    remove_event(cfg->scheduler, pdm_scsi_pump_event, cfg);
    scheduler_new_cpu_event(cfg->scheduler, pdm_scsi_pump_event, cfg, 0, 0, (uint64_t)PDM_SCSI_PUMP_NS);
}

// ============================================================
// Floppy: the AMIC DMA pump behind SWIM3
// ============================================================
// Unlike the SCSI channels there is no free-running service loop here.
// SWIM3 raises FDC_REQ only while a sector is under the head, and the
// transfer's length is the sector's — so the engine (swim3_xfer.c) moves
// its bytes inside one scheduler slot and calls these two movers, which
// are the AMIC half: window addressing, the 16-bit down-counter, and the
// DMA-complete interrupt the raw-read path terminates on.
//
// Addressing (docs/machines/pdm/swim3.md, "AMIC DMA"): the channel is
// hard-wired into the window's second 64 KB, so only the LOW 16 bits of
// the address advance — a transfer that would run off the end wraps
// inside that 64 KB rather than walking into the next region.

// One step of the channel address + count after a byte has moved.  When
// the count reaches zero the channel stops and raises DMAIF, which is how
// raw/copy-protect reads terminate (§6.3).
static void fd_dma_advance(config_t *cfg, pdm_dma_ch_t *ch) {
    ch->addr = (ch->addr & 0xFFFF0000u) | ((ch->addr + 1u) & 0xFFFFu);
    if (ch->count > 0 && --ch->count == 0) {
        ch->ctrl = (uint8_t)((ch->ctrl & ~DMA_RUN) | DMA_IF);
        LOG(4, "floppy dma terminal count, IF set");
        pdm_amic_recompute(cfg);
    }
}

bool pdm_amic_fd_dma_running(config_t *cfg) {
    return (pdm_st(cfg)->amic.floppy.ctrl & DMA_RUN) != 0;
}

bool pdm_amic_fd_dma_to_device(config_t *cfg) {
    return (pdm_st(cfg)->amic.floppy.ctrl & 0x40u) != 0; // DMADIR: 1 = memory -> SWIM3
}

// Memory -> SWIM3 (write / format streams).  False when the channel is
// stopped or its address is outside physical RAM.
bool pdm_amic_fd_dma_get(config_t *cfg, uint8_t *out) {
    pdm_dma_ch_t *ch = &pdm_st(cfg)->amic.floppy;
    if (!(ch->ctrl & DMA_RUN))
        return false;
    uint8_t *host = dma_host_ptr(ch->addr);
    if (!host)
        return false;
    *out = *host;
    fd_dma_advance(cfg, ch);
    return true;
}

// SWIM3 -> memory (read streams).
bool pdm_amic_fd_dma_put(config_t *cfg, uint8_t value) {
    pdm_dma_ch_t *ch = &pdm_st(cfg)->amic.floppy;
    if (!(ch->ctrl & DMA_RUN))
        return false;
    uint8_t *host = dma_host_ptr(ch->addr);
    if (!host || !g_page_table[ch->addr >> PAGE_SHIFT].writable)
        return false;
    *host = value;
    fd_dma_advance(cfg, ch);
    return true;
}

// ============================================================
// VBL — AMIC's video timing core (video-onboard-ariel.md §6)
// ============================================================

// The emulated monitor is the Hi-Res 640×480 at 66⅔ Hz (sense code 6, the
// mode the ROM selects for PDM_MONITOR_SENSE).  The frame period 3/200 s
// is an exact cycle count on every PDM clock (60/66/80 MHz).
static uint64_t vbl_period_cycles(config_t *cfg) {
    return (uint64_t)cfg->machine->freq * 3u / 200u;
}

// Start of vertical blanking: assert the slot IFR VBL flag (bit 6,
// ACTIVE-LOW — resolving the dossier's §11.6 polarity suspect: the ROM's
// SonoraWaitVSync clears the flag with a $40 write, then spins until bit
// 6 READS 0, so assertion drives the bit low).  Free-running raster; the
// enable bit only gates the interrupt, never the flag.
static void pdm_vbl_event(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    pdm_via2_t *v2 = &pdm_st(cfg)->amic.via2;
    v2->slot_ifr &= (uint8_t)~0x40u;
    pdm_amic_recompute(cfg);
    pdm_video_vbl(cfg); // guest drawing since the last frame needs re-upload
    scheduler_new_cpu_event(cfg->scheduler, pdm_vbl_event, cfg, 0, vbl_period_cycles(cfg), 0);
}

// Arm the free-running VBL on a fresh boot (checkpoint restore rebinds
// the pending event through the registered type instead).
void pdm_amic_start_vbl(config_t *cfg) {
    remove_event(cfg->scheduler, pdm_vbl_event, cfg);
    scheduler_new_cpu_event(cfg->scheduler, pdm_vbl_event, cfg, 0, vbl_period_cycles(cfg), 0);
}

// Register the event types (called from pdm.c before scheduler_start so
// checkpoint restore can rebind them).
void pdm_amic_register_events(config_t *cfg) {
    scheduler_new_event_type(cfg->scheduler, "amic", cfg, "vbl", pdm_vbl_event);
    scheduler_new_event_type(cfg->scheduler, "amic", cfg, "scsi_pump", pdm_scsi_pump_event);
}

// ============================================================
// ICR ($50F2A000)
// ============================================================

static uint8_t icr_read(config_t *cfg, uint32_t off) {
    pdm_state_t *st = pdm_st(cfg);
    pdm_amic_t *a = &st->amic;
    switch (off) {
    case 0x0:
        return (uint8_t)((a->icr_latch << 7) | (a->icr_mode << 6) | (st->icr_sources & 0x3Fu));
    case 0x8:
        // DMA flag mirror: channel IF & IE per selector (bits 0-6)
        return dma_irq_summary(a);
    case 0xA:
        // Sound in/out flags gated by their enables ($50F14014/18)
        return pdm_awacs_irq_summary(a);
    default:
        return 0;
    }
}

static void icr_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_state_t *st = pdm_st(cfg);
    pdm_amic_t *a = &st->amic;
    if (off != 0)
        return; // the flag mirrors are read-only
    uint8_t new_mode = (value >> 6) & 1u;
    if (new_mode != a->icr_mode)
        a->icr_latch = 0; // toggling INTMODE clears the latch and drops INT
    a->icr_mode = new_mode;
    if (value & 0x80u) {
        // Ack: clear the latch; still-asserted sources re-latch only on
        // their next assertion edge (see pdm_amic_recompute).
        a->icr_latch = 0;
        LOG(3, "ICR ack (sources=$%02X)", st->icr_sources);
    }
    pdm_amic_recompute(cfg);
}

// ============================================================
// Island dispatch
// ============================================================

void pdm_amic_init(config_t *cfg) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    memset(a, 0, sizeof(*a));
    a->via2.slot_ifr = 0x7Fu; // active-low: nothing asserted, unused bits high
    a->vid_mode = 0x9Fu; // blanked + invalid mode code
    a->floppy.addr = 0x15000u; // floppy DMA address reset value
}

uint8_t pdm_amic_read(config_t *cfg, uint32_t offset) {
    uint32_t block = offset & 0x3F000u;
    switch (block) {
    case OFF_VIA1:
    case OFF_VIA1 + 0x1000:
        return via_get_memory_interface(cfg->via1)->read_uint8(cfg->via1, offset);
    case OFF_SCC:
        // ESCC in Curio: +0 bCtl / +2 aCtl / +4 bData / +6 aData — the
        // low offset bits carry the chip's A/B and D/C pins directly.
        return scc_get_memory_interface(cfg->scc)->read_uint8(cfg->scc, offset - OFF_SCC);
    case OFF_SCSIA:
        return pdm_scsi_io_read(cfg, 0, offset - OFF_SCSIA);
    case OFF_SCSIB:
        return pdm_scsi_io_read(cfg, 1, offset - OFF_SCSIB);
    case OFF_SOUND:
        return pdm_awacs_read(cfg, offset - OFF_SOUND);
    case OFF_SWIM3:
    case OFF_SWIM3 + 0x1000: // 16 registers at stride $200 span both blocks
        return pdm_swim3_read(cfg, offset - OFF_SWIM3);
    case OFF_ARIEL:
        return pdm_ariel_read(cfg, offset - OFF_ARIEL);
    case OFF_VIA2:
    case OFF_VIA2 + 0x1000: // classic-VIA-stride aliases of the bank
        return via2_read(cfg, offset - OFF_VIA2);
    case OFF_VIDEO:
        return pdm_video_ctl_read(cfg, offset - OFF_VIDEO);
    case OFF_ICR:
        return icr_read(cfg, offset - OFF_ICR);
    case OFF_DIAG:
        return 0x01u; // bit 0 must read 1 or boot detours to the ROM monitor
    case OFF_DMA:
    case OFF_DMA + 0x1000:
        return dma_read(cfg, offset - OFF_DMA);
    default:
        LOG(2, "read of unwired island offset $%05X", offset);
        return 0;
    }
}

void pdm_amic_write(config_t *cfg, uint32_t offset, uint8_t value) {
    uint32_t block = offset & 0x3F000u;
    switch (block) {
    case OFF_VIA1:
    case OFF_VIA1 + 0x1000:
        via_get_memory_interface(cfg->via1)->write_uint8(cfg->via1, offset, value);
        return;
    case OFF_SCC:
        scc_get_memory_interface(cfg->scc)->write_uint8(cfg->scc, offset - OFF_SCC, value);
        return;
    case OFF_SCSIA:
        pdm_scsi_io_write(cfg, 0, offset - OFF_SCSIA, value);
        return;
    case OFF_SCSIB:
        pdm_scsi_io_write(cfg, 1, offset - OFF_SCSIB, value);
        return;
    case OFF_SOUND:
        pdm_awacs_write(cfg, offset - OFF_SOUND, value);
        return;
    case OFF_SWIM3:
    case OFF_SWIM3 + 0x1000: // 16 registers at stride $200 span both blocks
        pdm_swim3_write(cfg, offset - OFF_SWIM3, value);
        return;
    case OFF_ARIEL:
        pdm_ariel_write(cfg, offset - OFF_ARIEL, value);
        return;
    case OFF_VIA2:
    case OFF_VIA2 + 0x1000:
        via2_write(cfg, offset - OFF_VIA2, value);
        return;
    case OFF_VIDEO:
        pdm_video_ctl_write(cfg, offset - OFF_VIDEO, value);
        return;
    case OFF_ICR:
        icr_write(cfg, offset - OFF_ICR, value);
        return;
    case OFF_DIAG:
        return;
    case OFF_DMA:
    case OFF_DMA + 0x1000:
        dma_write(cfg, offset - OFF_DMA, value);
        return;
    default:
        LOG(2, "write of unwired island offset $%05X = $%02X", offset, value);
        return;
    }
}
