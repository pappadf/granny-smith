// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// amic.c
// AMIC ("Apple Memory-mapped I/O Controller") — the PDM I/O chip: all
// I/O-space decode, the classic-Mac interrupt model (pseudo-VIA1 as a real
// 6522 core instance, a pseudo-VIA2/RBV-style slot+device bank, and the
// top-level interrupt control register driving the 601's single INT line),
// the DMA-engine register file, the sound block, and the video control
// registers.  Phase C models the full software-visible register surface;
// datapaths (DMA transfers, sound streaming, video scanout) land with
// their ladder rungs in later phases.
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

// ============================================================
// Interrupt fabric
// ============================================================

// Aggregate the pseudo-VIA2 bank: device IFR bit 7 = OR of enabled bits
// 6..0.  Slot sources fold in through the ANY SLOT bit (device bit 1).
static uint8_t via2_dev_ifr(pdm_via2_t *v2) {
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
    if (ifr & v2->dev_ier & 0x7Fu)
        ifr |= 0x80u;
    return ifr;
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
    uint8_t dev = via2_dev_ifr(&a->via2);
    if (dev & 0x80u)
        st->icr_sources |= 1u << PDM_ICR_VIA2;
    else
        st->icr_sources &= (uint8_t) ~(1u << PDM_ICR_VIA2);

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

static uint8_t via2_read(config_t *cfg, uint32_t off) {
    pdm_via2_t *v2 = &pdm_st(cfg)->amic.via2;
    switch (off) {
    case 0x02:
        return v2->slot_ifr; // active-low levels, unused bits high
    case 0x03:
        return via2_dev_ifr(v2);
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
    case 0x13: // writable mask $3B
        if (value & 0x80u)
            v2->dev_ier |= value & 0x3Bu;
        else
            v2->dev_ier &= (uint8_t) ~(value & 0x3Bu);
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
            a->scsi[0].ctrl = value & (uint8_t) ~(DMA_RST | 0x10u); // FLUSH self-clears
        }
        return;
    case 0x1009:
        if (value & DMA_RST)
            a->scsi[1].ctrl &= 0x4Cu;
        else
            a->scsi[1].ctrl = value & (uint8_t) ~(DMA_RST | 0x10u);
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
        if (value & DMA_RST)
            a->floppy.addr = 0x15000u; // reset restores the default region
        dma_ctrl_write(cfg, &a->floppy, value, 0);
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
// Sound block ($50F14000, $20 bytes)
// ============================================================
// Phase-C model: the codec control/status registers store and read back;
// the OUTPUT engine runs just far enough for the polled boot-beep contract
// — while the output-run bit (+$10 bit 0) is set, buffers complete on the
// real ping-pong cadence (BufferSize frames at the selected sample rate)
// and raise their done flags in +$18 (bit 6 pairs with the +$10000
// buffer, bit 7 with +$12000; a still-set flag raises ERR instead).  The
// PPC ROM's chime polls those flags with interrupts off — a frozen engine
// hangs boot right here.  Audio rendering arrives in Phase F.

// Nanoseconds per buffer at the current rate/size settings.
static uint64_t sound_buffer_ns(pdm_amic_t *a) {
    static const uint32_t rates[] = {22050u, 29400u, 44100u, 22050u};
    uint32_t rate = rates[(a->snd[0x10] >> 1) & 3u];
    uint32_t frames = (uint32_t)(((a->snd[0x08] & 0x07u) << 8) | a->snd[0x09]);
    if (frames == 0)
        frames = 2048;
    return (uint64_t)frames * 1000000000ull / rate;
}

// Sound-out buffer completion: raise the finished buffer's flag (or ERR if
// software hasn't consumed the previous one), flip buffers, re-arm.
static void pdm_snd_out_event(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    if (!(a->snd[0x10] & 0x01u))
        return; // stopped since scheduling
    LOG(2, "sndout buffer %d complete (snd18=$%02X)", a->snd_out_buf, a->snd[0x18]);
    uint8_t flag = a->snd_out_buf == 0 ? 0x40u : 0x80u; // bit 6 <-> +$10000
    if (a->snd[0x18] & flag)
        a->snd[0x18] |= 0x20u; // over/underrun: ERR instead of the IF
    else
        a->snd[0x18] |= flag;
    a->snd_out_buf ^= 1u;
    pdm_amic_recompute(cfg);
    scheduler_new_cpu_event(cfg->scheduler, pdm_snd_out_event, cfg, 0, 0, sound_buffer_ns(a));
}

static uint8_t sound_read(config_t *cfg, uint32_t off) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    return off < 0x20 ? a->snd[off] : 0;
}

static void sound_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    if (off >= 0x20)
        return;
    switch (off) {
    case 0x10: { // control hi: bit 0 = output DMA run
        bool was_running = (a->snd[0x10] & 0x01u) != 0;
        a->snd[off] = value;
        bool running = (value & 0x01u) != 0;
        if (running && !was_running) {
            a->snd_out_buf = 0; // playback starts with the +$10000 buffer
            LOG(2, "sndout run: %llu ns/buffer", (unsigned long long)sound_buffer_ns(a));
            remove_event(cfg->scheduler, pdm_snd_out_event, cfg);
            scheduler_new_cpu_event(cfg->scheduler, pdm_snd_out_event, cfg, 0, 0, sound_buffer_ns(a));
        } else if (!running && was_running) {
            remove_event(cfg->scheduler, pdm_snd_out_event, cfg);
        }
        break;
    }
    case 0x14: // in/out DMA status: flag bits (high nibble) are W1C
    case 0x18:
        a->snd[off] = (uint8_t)((a->snd[off] & 0xF0u & ~(value & 0xF0u)) | (value & 0x0Fu));
        break;
    default:
        a->snd[off] = value;
        break;
    }
    pdm_amic_recompute(cfg);
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
    scheduler_new_event_type(cfg->scheduler, "amic", cfg, "sndout", pdm_snd_out_event);
    scheduler_new_event_type(cfg->scheduler, "amic", cfg, "vbl", pdm_vbl_event);
}

// ============================================================
// Video control ($50F28000) and Ariel CLUT ($50F24000)
// ============================================================

// Monitor sense lines (video-onboard-ariel.md §7): the HDI-45 carries
// three open-collector sense lines A/B/C with 10k pull-ups; a dumb monitor
// hard-wires a subset to ground and the readback nibble reflects
// wired-AND(drive, strap).  The emulated monitor is the 14" AppleColor
// Hi-Res (sense code 6 = A,B floating high, C grounded) — the Phase-F
// gray-desktop profile, wired now because HMCMerge allocates the
// framebuffer window only when a monitor senses present (rung L18).
// SonoraVdSenseRg drive nibble: bit n = 0 drives line n low, 1 releases
// it ($07 = tristate); readback bits 6:4 = lines A,B,C.
#define PDM_MONITOR_SENSE 0x6u // A=1, B=1, C=0: Hi-Res 13"/14"

static uint8_t video_read(config_t *cfg, uint32_t off) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    switch (off) {
    case 0:
        return a->vid_mode;
    case 1:
        return a->vid_depth;
    case 2: {
        // Open-collector wired-AND: a line reads low when the host drives
        // it low OR the monitor straps it to ground; high otherwise.
        uint8_t lines = (uint8_t)(a->vid_sense & 0x07u & PDM_MONITOR_SENSE);
        return (uint8_t)((a->vid_sense & 0x0Fu) | (lines << 4));
    }
    case 3:
        return a->vid_test;
    case 4:
    case 5:
    case 6:
    case 7:
        return 0; // beam counters: static until the video timing exists
    default:
        return 0;
    }
}

static void video_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    switch (off) {
    case 0:
        a->vid_mode = value;
        LOG(2, "video mode = $%02X", value);
        break;
    case 1:
        a->vid_depth = value;
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

static uint8_t ariel_read(config_t *cfg, uint32_t off) {
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

static void ariel_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
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
        }
        break;
    case 2:
        a->clut_ctrl = value;
        break;
    default:
        a->clut_key = value;
        break;
    }
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
        return (uint8_t)(((a->scc[3].ctrl & DMA_IF) && (a->scc[3].ctrl & DMA_IE) ? 0x01u : 0) |
                         ((a->scc[2].ctrl & DMA_IF) && (a->scc[2].ctrl & DMA_IE) ? 0x02u : 0) |
                         ((a->scc[1].ctrl & DMA_IF) && (a->scc[1].ctrl & DMA_IE) ? 0x04u : 0) |
                         ((a->scc[0].ctrl & DMA_IF) && (a->scc[0].ctrl & DMA_IE) ? 0x08u : 0) |
                         ((a->enet_rx.ctrl & DMA_IF) && (a->enet_rx.ctrl & DMA_IE) ? 0x10u : 0) |
                         ((a->enet_tx.ctrl & DMA_IF) && (a->enet_tx.ctrl & DMA_IE) ? 0x20u : 0) |
                         ((a->floppy.ctrl & DMA_IF) && (a->floppy.ctrl & DMA_IE) ? 0x40u : 0));
    case 0xA:
        // Sound in/out flags gated by their enables ($50F14014/18)
        return (uint8_t)((((a->snd[0x14] >> 4) & (a->snd[0x14] & 0x0Fu)) ? 0x01u : 0) |
                         (((a->snd[0x18] >> 4) & (a->snd[0x18] & 0x0Fu)) ? 0x02u : 0));
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
    case OFF_SOUND:
        return sound_read(cfg, offset - OFF_SOUND);
    case OFF_ARIEL:
        return ariel_read(cfg, offset - OFF_ARIEL);
    case OFF_VIA2:
        return via2_read(cfg, offset - OFF_VIA2);
    case OFF_VIDEO:
        return video_read(cfg, offset - OFF_VIDEO);
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
    case OFF_SOUND:
        sound_write(cfg, offset - OFF_SOUND, value);
        return;
    case OFF_ARIEL:
        ariel_write(cfg, offset - OFF_ARIEL, value);
        return;
    case OFF_VIA2:
        via2_write(cfg, offset - OFF_VIA2, value);
        return;
    case OFF_VIDEO:
        video_write(cfg, offset - OFF_VIDEO, value);
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
