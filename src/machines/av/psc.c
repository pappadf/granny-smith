// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// psc.c
// PSC interrupt controller + sound/DSP latches (Phase B scope; the DMA
// engine arrives in Phase D).  See psc.h for the contract references.
//
// Modelling rules that are load-bearing (psc.md §5, IMPLEMENTATION.md §9):
//   * every interrupt register read is repeat-stable (values change only on
//     CPU-synchronous events — the ROM's double-read loops terminate)
//   * IER writes are VIA-style sense-bit ($80|bits sets, bits clears)
//   * IR / IFR writes are write-1-to-clear for latched bits; level sources
//     re-assert on the next derivation
//   * `sndPhase` ($20C) is a free-running frame counter derived from
//     emulated time — CycloneBeep spin-waits on it at IPL 7 with a constant
//     value hanging the ROM forever (singer.md §7)
//   * `dspOverRun` ($21C) is a sense-bit latch with no DSP behind it
//   * the UTSC ($300/$304) is a monotonic 48-bit counter (~1 MHz here; the
//     real tick source is undocumented — psc.md §7)

#include "psc.h"

#include "av.h"
#include "singer.h" // AV_SINGER_STAT presentation

#include "cpu.h"
#include "log.h"
#include "scheduler.h"
#include "system.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("psc");

// VIA2-window latched bits (write-1-to-clear); the rest are level-derived.
// The FDC bit is a LEVEL: the New Age deasserts its INT when the host reads
// the interrupt status (new-age.md §5), which is what clears the IFR bit —
// the driver's Handler never writes the IFR.
#define AV_PSC_VIA2_LATCH_MASK (1u << AV_PSC_VIA2_SNDFRM)

// CmdStat action bits, stored as (hardware word >> 8) — every architected
// bit of the register lives in the high byte (psc.md §2.7).
#define PSC_CS_IF      0x01 // bit 8: interrupt flag (set at completion)
#define PSC_CS_DIR     0x02 // bit 9: 1 = device→memory
#define PSC_CS_TERMCNT 0x04 // bit 10: terminal count reached
#define PSC_CS_ENABLED 0x08 // bit 11: set armed (hardware clears at completion)
#define PSC_CS_IE      0x10 // bit 12: interrupt enable for this set

// One DMA channel: two {Addr, Cnt, CmdStat} register sets + the control
// word's stateful bits (psc.md §2.6-§2.7).
typedef struct av_psc_chan {
    uint32_t addr[2]; // 32-bit physical buffer address per set
    uint32_t cnt[2]; // byte count / residual per set
    uint8_t cs[2]; // CmdStat action bits (PSC_CS_*) per set
    uint8_t active_set; // control bit 0: which set the engine runs
    bool pause; // PAUSE latched (FROZEN reads 1 while paused)
    bool cie; // channel interrupt enable
    bool berr; // bus error latched
    bool sense_last; // last-written SENSE bit, reflected in reads (the
                     // ROM's SerialHAL compares the whole word against
                     // $C400 after a $8800 SWRESET write)
} av_psc_chan_t;

struct av_psc {
    // --- plain data (checkpointed up to the first pointer field) ---
    uint8_t via2_level; // live VIA2-window source levels
    uint8_t via2_latched; // latched VIA2-window bits (FDC, sound frame)
    uint8_t via2_ier; // VIA2-window interrupt enables
    uint8_t sint_active; // slot-int sources currently asserting (bit set = active)
    uint8_t l_level[4]; // live L3-L6 source levels
    uint8_t l_latched[4]; // latched L3-L6 bits (60 Hz)
    uint8_t l_ier[4]; // L3-L6 enables
    uint8_t berrie; // $800 bus-error interrupt enable (bit 0)
    uint8_t psctest[4]; // $400 test register latch (no known function)
    uint8_t snd[0x1C]; // $200-$21B sound-block latches ($20C reads computed)
    uint8_t dsp_overrun; // $21C sense-bit latch (pdspReset/pdspResetEn/pdspFrameOvr)
    av_psc_chan_t chan[AV_PSC_DMA_CHANNELS]; // the 7-channel DMA engine

    // --- pointers (not checkpointed) ---
    config_t *cfg;
    av_psc_dreq_fn dreq_fn; // live SCSI DREQ, published in the VIA2 IFR
    void *dreq_ctx;
    av_psc_mem_read_fn mem_read; // guest-physical accessors for DMA
    av_psc_mem_write_fn mem_write;
    void *mem_ctx;
    av_psc_dsp_fn dsp_fn; // dspOverRun latch observer (the DSP glue)
    void *dsp_ctx;
};

static inline av_psc_t *psc_of(config_t *cfg) {
    return ((av_state_t *)cfg->machine_context)->psc;
}

// ============================================================
// IPL derivation
// ============================================================

static void psc_update_via2_ipl(av_psc_t *psc) {
    uint8_t ifr = (uint8_t)((psc->via2_level | psc->via2_latched) & 0x7F);
    av_update_ipl(psc->cfg, AV_IRQ_VIA2, (ifr & psc->via2_ier & 0x7F) != 0);
}

static void psc_update_level_ipl(av_psc_t *psc, int level) {
    static const int src[4] = {AV_IRQ_L3, AV_IRQ_L4, AV_IRQ_L5, AV_IRQ_L6};
    uint8_t ir = (uint8_t)((psc->l_level[level] | psc->l_latched[level]) & 0x7F);
    av_update_ipl(psc->cfg, src[level], (ir & psc->l_ier[level] & 0x7F) != 0);
}

// Recompute the VIA2 slot bit (CA1 aggregate of SInt sources) then the IPL.
static void psc_update_slot_bit(av_psc_t *psc) {
    if (psc->sint_active)
        psc->via2_level |= (1u << AV_PSC_VIA2_SLOT_CA1);
    else
        psc->via2_level &= (uint8_t) ~(1u << AV_PSC_VIA2_SLOT_CA1);
    psc_update_via2_ipl(psc);
}

// ============================================================
// Source inputs
// ============================================================

void av_psc_via2_source(av_psc_t *psc, int bit, bool active) {
    if (active)
        psc->via2_level |= (uint8_t)(1u << bit);
    else
        psc->via2_level &= (uint8_t) ~(1u << bit);
    psc_update_via2_ipl(psc);
}

void av_psc_via2_latch(av_psc_t *psc, int bit) {
    psc->via2_latched |= (uint8_t)(1u << bit);
    psc_update_via2_ipl(psc);
}

void av_psc_slot_source(av_psc_t *psc, int bit, bool active) {
    if (active)
        psc->sint_active |= (uint8_t)(1u << bit);
    else
        psc->sint_active &= (uint8_t) ~(1u << bit);
    psc_update_slot_bit(psc);
}

void av_psc_level_source(av_psc_t *psc, int level, int bit, bool active) {
    if (active)
        psc->l_level[level] |= (uint8_t)(1u << bit);
    else
        psc->l_level[level] &= (uint8_t) ~(1u << bit);
    psc_update_level_ipl(psc, level);
}

void av_psc_level_latch(av_psc_t *psc, int level, int bit) {
    psc->l_latched[level] |= (uint8_t)(1u << bit);
    psc_update_level_ipl(psc, level);
}

void av_psc_tick60(av_psc_t *psc) {
    av_psc_level_latch(psc, AV_PSC_L6, 0); // L660HZ
}

void av_psc_set_dsp_hook(av_psc_t *psc, av_psc_dsp_fn fn, void *ctx) {
    psc->dsp_fn = fn;
    psc->dsp_ctx = ctx;
}

uint16_t av_psc_snd_read16(av_psc_t *psc, uint32_t off) {
    off &= 0x1F;
    return (uint16_t)((psc->snd[off] << 8) | psc->snd[off + 1]);
}

uint32_t av_psc_snd_read32(av_psc_t *psc, uint32_t off) {
    return ((uint32_t)av_psc_snd_read16(psc, off) << 16) | av_psc_snd_read16(psc, off + 2);
}

void av_psc_dsp_frame_overrun(av_psc_t *psc) {
    psc->dsp_overrun |= 0x04; // pdspFrameOvr, sticky until the host clears it
    av_psc_level_source(psc, AV_PSC_L5, 1, true); // FRMOVRN re-latches while set
}

// ============================================================
// VIA2 window ($50F02000: $1A00 IFR / $1C00 IER / $1E00 SInt)
// ============================================================

uint8_t av_psc_via2_read(config_t *cfg, uint32_t addr) {
    av_psc_t *psc = psc_of(cfg);
    uint32_t off = (addr & 0x3FFFFu) - 0x2000u;
    switch (off) {
    case 0x1A00: {
        uint8_t ifr = (uint8_t)((psc->via2_level | psc->via2_latched) & 0x7F);
        if (ifr & psc->via2_ier & 0x7F)
            ifr |= 0x80; // 6522-style: bit 7 = any enabled source pending
        // Bit 0 is NOT an interrupt source on this platform: it publishes the
        // SCSI chip's live DREQ.  The SCSI Manager's HAL polls exactly this
        // address and bit (InitItt.c `case kCyclone`: dreqAddr = $50F03A00,
        // intDREQbitNum = 0; the SCSI *interrupt* is bit 3) and treats DREQ
        // as the success signal for a select — "the target selected OK and
        // went to the expected phase, and the chip is asking for the command
        // byte".  Without it the HAL sees no DREQ, reads the select's
        // interrupt instead, concludes the target went to an unexpected
        // phase, and retries the select forever.  It contributes to neither
        // the pending-summary bit nor the IPL.
        if (psc->dreq_fn && psc->dreq_fn(psc->dreq_ctx))
            ifr |= 1u;
        return ifr;
    }
    case 0x1C00:
        return (uint8_t)(psc->via2_ier | 0x80); // 6522-style IER readback
    case 0x1E00:
        // Slot-interrupt lines, active LOW (slots C/D/E bits 3-5, VBL bit 6).
        return (uint8_t)~psc->sint_active;
    default:
        return 0;
    }
}

void av_psc_via2_write(config_t *cfg, uint32_t addr, uint8_t value) {
    av_psc_t *psc = psc_of(cfg);
    uint32_t off = (addr & 0x3FFFFu) - 0x2000u;
    switch (off) {
    case 0x1A00:
        // Write-1-to-clear on the latched bits; level bits re-derive.
        psc->via2_latched &= (uint8_t) ~(value & AV_PSC_VIA2_LATCH_MASK);
        psc_update_via2_ipl(psc);
        break;
    case 0x1C00:
        if (value & 0x80)
            psc->via2_ier |= (uint8_t)(value & 0x7F);
        else
            psc->via2_ier &= (uint8_t)~value;
        psc_update_via2_ipl(psc);
        break;
    case 0x1E00:
        // VIA2InitCyclone ORs one byte of zeros in here; no writable state.
        LOG(3, "VIA2 SInt write $%02X ignored (pc=%08X)", value, cpu_get_pc(cfg->cpu));
        break;
    default:
        break;
    }
}

// ============================================================
// Sound block ($200-$21F; singer.md §7a)
// ============================================================

// Free-running sndPhase: offset (frames into the 2*sndSize double buffer)
// in bits 6-21, 1/64-frame fraction in bits 0-5, derived from emulated time
// at the programmed codec rate.
static uint32_t psc_snd_phase(av_psc_t *psc) {
    static const uint32_t rates[4] = {24000, 32000, 48000, 48000};
    uint32_t com = (uint32_t)((psc->snd[0x00] << 8) | psc->snd[0x01]); // sndComCtl
    uint32_t rate = rates[(com >> 9) & 3];
    uint32_t snd_size = (uint32_t)((psc->snd[0x18] << 8) | psc->snd[0x19]);
    uint32_t wrap = snd_size ? 2 * snd_size : 0x10000; // frames per pass
    double ns = scheduler_time_ns(psc->cfg->scheduler);
    uint64_t total_64ths = (uint64_t)(ns * (double)rate * 64.0 / 1e9);
    uint32_t offset = (uint32_t)((total_64ths >> 6) % wrap);
    return ((offset & 0xFFFFu) << 6) | (uint32_t)(total_64ths & 63u);
}

// ============================================================
// DMA engine (psc.md §2.6-§3)
// ============================================================

// Big-endian byte lane of a 32-bit value.
static inline uint8_t lane32(uint32_t v, uint32_t off) {
    return (uint8_t)(v >> (8 * (3 - (off & 3))));
}

// PSC_ISR: bit (31−n) = channel n interrupting — a set's IF && IE, gated
// by the channel's CIE (BFFFO-compatible bit order, psc.md §2.4).
static uint32_t psc_isr_value(av_psc_t *psc) {
    uint32_t isr = 0;
    for (int n = 0; n < AV_PSC_DMA_CHANNELS; n++) {
        av_psc_chan_t *ch = &psc->chan[n];
        bool pending = ((ch->cs[0] & PSC_CS_IF) && (ch->cs[0] & PSC_CS_IE)) ||
                       ((ch->cs[1] & PSC_CS_IF) && (ch->cs[1] & PSC_CS_IE));
        if (pending && ch->cie)
            isr |= 1u << (31 - n);
    }
    return isr;
}

// Re-derive the L4 DMA source (bit 3) from PSC_ISR.
static void psc_update_dma_ipl(av_psc_t *psc) {
    av_psc_level_source(psc, AV_PSC_L4, 3, psc_isr_value(psc) != 0);
}

// Terminal count on the active set: hardware clears ENABLED, sets TERMCNT
// and IF, and switches the active set to the other one (psc.md §3.3).
static void psc_dma_complete(av_psc_t *psc, int n) {
    av_psc_chan_t *ch = &psc->chan[n];
    int s = ch->active_set;
    ch->cs[s] = (uint8_t)((ch->cs[s] & ~PSC_CS_ENABLED) | PSC_CS_TERMCNT | PSC_CS_IF);
    ch->active_set ^= 1;
    LOG(2, "DMA ch%d set %d complete; active set -> %d", n, s, ch->active_set);
    psc_update_dma_ipl(psc);
}

void av_psc_set_dreq_query(av_psc_t *psc, av_psc_dreq_fn fn, void *ctx) {
    psc->dreq_fn = fn;
    psc->dreq_ctx = ctx;
}

void av_psc_set_memory_hooks(av_psc_t *psc, av_psc_mem_read_fn rd, av_psc_mem_write_fn wr, void *ctx) {
    psc->mem_read = rd;
    psc->mem_write = wr;
    psc->mem_ctx = ctx;
}

bool av_psc_dma_ready(av_psc_t *psc, int chan) {
    av_psc_chan_t *ch = &psc->chan[chan];
    return !ch->pause && (ch->cs[ch->active_set] & PSC_CS_ENABLED) && ch->cnt[ch->active_set] != 0;
}

int av_psc_dma_dir(av_psc_t *psc, int chan) {
    av_psc_chan_t *ch = &psc->chan[chan];
    if (!av_psc_dma_ready(psc, chan))
        return -1;
    return (ch->cs[ch->active_set] & PSC_CS_DIR) ? 1 : 0;
}

// Common transfer core.  `to_memory` mirrors DIR (1 = device→memory).
static int psc_dma_transfer(av_psc_t *psc, int n, const uint8_t *in, uint8_t *out, int len, bool to_memory) {
    av_psc_chan_t *ch = &psc->chan[n];
    int s = ch->active_set;
    if (ch->pause || !(ch->cs[s] & PSC_CS_ENABLED))
        return 0;
    if (((ch->cs[s] & PSC_CS_DIR) != 0) != to_memory)
        return 0; // direction mismatch — the set is armed the other way
    uint32_t remain = ch->cnt[s]; // (MACE-receive chain mode not modelled)
    if (remain == 0)
        return 0;
    if (ch->addr[s] >= 0x40000000u) {
        // The PSC cannot DMA into ROM/NuBus space (Radar #1059322): latch a
        // bus error instead of transferring.
        ch->berr = true;
        LOG(1, "DMA ch%d bus error: addr $%08X >= $40000000", n, ch->addr[s]);
        return 0;
    }
    uint32_t count = (uint32_t)len < remain ? (uint32_t)len : remain;
    for (uint32_t i = 0; i < count; i++) {
        if (to_memory)
            psc->mem_write(psc->mem_ctx, ch->addr[s] + i, in[i], 1);
        else
            out[i] = (uint8_t)psc->mem_read(psc->mem_ctx, ch->addr[s] + i, 1);
    }
    ch->addr[s] += count;
    ch->cnt[s] -= count;
    if (ch->cnt[s] == 0)
        psc_dma_complete(psc, n);
    return (int)count;
}

int av_psc_dma_device_in(av_psc_t *psc, int chan, const uint8_t *buf, int len) {
    return psc_dma_transfer(psc, chan, buf, NULL, len, true);
}

int av_psc_dma_device_out(av_psc_t *psc, int chan, uint8_t *buf, int len) {
    return psc_dma_transfer(psc, chan, NULL, buf, len, false);
}

// --- Channel control word ($C00 + chan*$10, word; action bits in the high
// byte, kmSET in the low byte) ---

static uint8_t psc_ctrl_read(av_psc_t *psc, int n, uint32_t lane) {
    av_psc_chan_t *ch = &psc->chan[n];
    if (lane == 0) {
        // CIRQ (bit 8) = OR of both sets' IF, independent of enables.
        uint8_t hi = 0;
        if ((ch->cs[0] | ch->cs[1]) & PSC_CS_IF)
            hi |= 0x01; // CIRQ
        if (ch->pause)
            hi |= 0x04 | 0x40; // PAUSE latched + FROZEN (stops immediately)
        if (ch->cie)
            hi |= 0x10;
        if (ch->berr)
            hi |= 0x20;
        if (ch->sense_last)
            hi |= 0x80; // SENSE reads back latched
        return hi;
    }
    if (lane == 1)
        return ch->active_set; // kmSET
    return 0;
}

static void psc_ctrl_write(av_psc_t *psc, int n, uint32_t lane, uint8_t value) {
    av_psc_chan_t *ch = &psc->chan[n];
    if (lane != 0)
        return; // every writable bit lives in the high byte
    bool sense = (value & 0x80) != 0;
    uint8_t bits = (uint8_t)(value & 0x7F);
    ch->sense_last = sense;
    if (bits & 0x04) { // PAUSE
        ch->pause = sense;
        LOG(3, "DMA ch%d %s", n, sense ? "pause" : "run");
    }
    if ((bits & 0x08) && sense) { // SWRESET: paused, both sets disarmed
        ch->pause = true;
        ch->cs[0] &= (uint8_t)~PSC_CS_ENABLED;
        ch->cs[1] &= (uint8_t)~PSC_CS_ENABLED;
        LOG(2, "DMA ch%d swreset", n);
    }
    if (bits & 0x10) // CIE
        ch->cie = sense;
    if ((bits & 0x20) && !sense) // BERR: writable-to-clear
        ch->berr = false;
    // DMAFLUSH (0x02) self-clears instantly — nothing is buffered here.
    psc_update_dma_ipl(psc);
}

// --- Channel register sets ($1000 + chan*$20, set 1 at +$10) ---

static uint8_t psc_set_read(av_psc_t *psc, int n, int s, uint32_t reg_off) {
    av_psc_chan_t *ch = &psc->chan[n];
    if (reg_off < 4)
        return lane32(ch->addr[s], reg_off);
    if (reg_off < 8)
        return lane32(ch->cnt[s], reg_off);
    if (reg_off == 8)
        return ch->cs[s]; // CmdStat high byte
    return 0; // CmdStat low byte (SETMASK — unused) + reserved
}

static void psc_set_write(av_psc_t *psc, int n, int s, uint32_t reg_off, uint8_t value) {
    av_psc_chan_t *ch = &psc->chan[n];
    if (reg_off < 4) {
        uint32_t shift = 8 * (3 - reg_off);
        ch->addr[s] = (ch->addr[s] & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        return;
    }
    if (reg_off < 8) {
        uint32_t shift = 8 * (3 - (reg_off & 3));
        ch->cnt[s] = (ch->cnt[s] & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        return;
    }
    if (reg_off == 8) {
        // Sense-bit write on the CmdStat action bits.
        bool sense = (value & 0x80) != 0;
        uint8_t bits = (uint8_t)(value & (PSC_CS_IF | PSC_CS_DIR | PSC_CS_TERMCNT | PSC_CS_ENABLED | PSC_CS_IE));
        if (sense)
            ch->cs[s] |= bits;
        else
            ch->cs[s] &= (uint8_t)~bits;
        psc_update_dma_ipl(psc);
        return;
    }
}

// ============================================================
// UTSC ($300 lo / $304 hi; ~1 MHz monotonic)
// ============================================================

static uint64_t psc_utsc(config_t *cfg) {
    return (uint64_t)(scheduler_time_ns(cfg->scheduler) / 1000.0) & 0xFFFFFFFFFFFFull;
}

// ============================================================
// PSC register block ($50F31000-$50F32FFF)
// ============================================================

uint8_t av_psc_reg_read(config_t *cfg, uint32_t addr) {
    av_psc_t *psc = psc_of(cfg);
    uint32_t off = (addr & 0x3FFFFu) - 0x31000u;

    // Level 3-6 interrupt register pairs ($130..$164, byte-wide).
    if (off >= 0x130 && off <= 0x167) {
        int level = (int)((off - 0x130) >> 4); // $130 L3, $140 L4, ...
        uint32_t reg = off & 0xF;
        if (level > 3)
            return 0;
        if (reg == 0) { // IR
            uint8_t ir = (uint8_t)((psc->l_level[level] | psc->l_latched[level]) & 0x7F);
            if (ir)
                ir |= 0x80; // bit 7 = OR of all pending on this level
            return ir;
        }
        if (reg == 4) // IER
            return psc->l_ier[level];
        return 0;
    }

    // Channel control words ($C00 + chan*$10, word).
    if (off >= 0xC00 && off < 0xC00 + 0x10 * AV_PSC_DMA_CHANNELS) {
        int n = (int)((off - 0xC00) >> 4);
        uint32_t sub = off & 0xF;
        return sub < 2 ? psc_ctrl_read(psc, n, sub) : 0;
    }

    // Channel register sets ($1000 + chan*$20; set 1 at +$10).
    if (off >= 0x1000 && off < 0x1000 + 0x20 * AV_PSC_DMA_CHANNELS) {
        int n = (int)((off - 0x1000) >> 5);
        int s = (int)((off >> 4) & 1);
        return psc_set_read(psc, n, s, off & 0xF);
    }

    switch (off & ~3u) {
    case 0x208: // singerStat — board straps + valid-data presentation
        return lane32(AV_SINGER_STAT, off);
    case 0x200: // sndComCtl (word) + neighbours — latches
    case 0x204:
    case 0x210:
    case 0x214:
    case 0x218:
        return psc->snd[off & 0x1F];
    case 0x20C: // sndPhase — computed free-runner
        return lane32(psc_snd_phase(psc), off);
    case 0x21C:
        return (off & 3) == 0 ? psc->dsp_overrun : psc->snd[off & 0x1F];
    case 0x300: // UTSC least-significant longword
        return lane32((uint32_t)psc_utsc(cfg), off);
    case 0x304: // UTSC most-significant (16 valid bits)
        return lane32((uint32_t)(psc_utsc(cfg) >> 32), off);
    case 0x400:
        return psc->psctest[off & 3];
    case 0x800:
        return (off & 3) == 0 ? psc->berrie : 0;
    case 0x804:
        return lane32(psc_isr_value(psc), off); // PSC_ISR
    default:
        return 0;
    }
}

void av_psc_reg_write(config_t *cfg, uint32_t addr, uint8_t value) {
    av_psc_t *psc = psc_of(cfg);
    uint32_t off = (addr & 0x3FFFFu) - 0x31000u;

    if (off >= 0x130 && off <= 0x167) {
        int level = (int)((off - 0x130) >> 4);
        uint32_t reg = off & 0xF;
        if (level > 3)
            return;
        if (reg == 0) {
            // IR write-back-to-clear (latched bits only; singer.md §7a.5).
            psc->l_latched[level] &= (uint8_t) ~(value & 0x7F);
            psc_update_level_ipl(psc, level);
        } else if (reg == 4) {
            // Sense-bit enable write (psc.md §2.1).
            if (value & 0x80)
                psc->l_ier[level] |= (uint8_t)(value & 0x7F);
            else
                psc->l_ier[level] &= (uint8_t)~value;
            psc_update_level_ipl(psc, level);
        }
        return;
    }

    // Channel control words + register sets.
    if (off >= 0xC00 && off < 0xC00 + 0x10 * AV_PSC_DMA_CHANNELS) {
        int n = (int)((off - 0xC00) >> 4);
        uint32_t sub = off & 0xF;
        if (sub < 2)
            psc_ctrl_write(psc, n, sub, value);
        return;
    }
    if (off >= 0x1000 && off < 0x1000 + 0x20 * AV_PSC_DMA_CHANNELS) {
        int n = (int)((off - 0x1000) >> 5);
        int s = (int)((off >> 4) & 1);
        psc_set_write(psc, n, s, off & 0xF, value);
        return;
    }

    switch (off & ~3u) {
    case 0x200:
        psc->snd[off & 0x1F] = value;
        if ((off & 3) == 1) // second lane of the sndComCtl word landed
            LOG(2, "sndComCtl = $%04X (pc=%08X)", av_psc_snd_read16(psc, 0), cpu_get_pc(cfg->cpu));
        return;
    case 0x204:
    case 0x208:
    case 0x20C: // sndPhase is read-only; latch the bytes anyway (harmless)
    case 0x210:
    case 0x214:
    case 0x218:
        psc->snd[off & 0x1F] = value;
        return;
    case 0x21C:
        if ((off & 3) == 0) {
            // dspOverRun: sense-bit convention on bits 0-2 (dsp3210.md §8).
            if (value & 0x80)
                psc->dsp_overrun |= (uint8_t)(value & 0x07);
            else
                psc->dsp_overrun &= (uint8_t) ~(value & 0x07);
            LOG(2, "dspOverRun write $%02X -> $%02X (pc=%08X)", value, psc->dsp_overrun, cpu_get_pc(cfg->cpu));
            // FRMOVRN (L5 bit 1) is a level view of the sticky bit: it
            // re-latches until the host clears pdspFrameOvr itself.
            av_psc_level_source(psc, AV_PSC_L5, 1, (psc->dsp_overrun & 0x04) != 0);
            if (psc->dsp_fn)
                psc->dsp_fn(psc->dsp_ctx, psc->dsp_overrun, value);
        }
        return;
    case 0x400:
        psc->psctest[off & 3] = value;
        return;
    case 0x800:
        if ((off & 3) == 0)
            psc->berrie = (uint8_t)(value & 1);
        return;
    default:
        LOG(3, "PSC write $%04X = $%02X ignored (pc=%08X)", off, value, cpu_get_pc(cfg->cpu));
        return;
    }
}

// ============================================================
// Lifecycle
// ============================================================

av_psc_t *av_psc_init(config_t *cfg, checkpoint_t *cp) {
    av_psc_t *psc = calloc(1, sizeof(*psc));
    if (!psc)
        return NULL;
    psc->cfg = cfg;
    if (cp) {
        size_t data_size = offsetof(av_psc_t, cfg);
        system_read_checkpoint_data(cp, psc, data_size);
    }
    return psc;
}

void av_psc_delete(av_psc_t *psc) {
    free(psc);
}

void av_psc_checkpoint(av_psc_t *psc, checkpoint_t *cp) {
    if (!psc || !cp)
        return;
    size_t data_size = offsetof(av_psc_t, cfg);
    system_write_checkpoint_data(cp, psc, data_size);
}
