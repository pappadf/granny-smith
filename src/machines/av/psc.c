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

    // --- pointers (not checkpointed) ---
    config_t *cfg;
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
// UTSC ($300 lo / $304 hi; ~1 MHz monotonic)
// ============================================================

static uint64_t psc_utsc(config_t *cfg) {
    return (uint64_t)(scheduler_time_ns(cfg->scheduler) / 1000.0) & 0xFFFFFFFFFFFFull;
}

// ============================================================
// PSC register block ($50F31000-$50F32FFF)
// ============================================================

// Big-endian byte lane of a 32-bit value.
static inline uint8_t lane32(uint32_t v, uint32_t off) {
    return (uint8_t)(v >> (8 * (3 - (off & 3))));
}

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

    switch (off & ~3u) {
    case 0x200: // sndComCtl (word) + neighbours — latches
    case 0x204:
    case 0x208:
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
        return 0; // PSC_ISR: no DMA channels interrupting until Phase D
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

    switch (off & ~3u) {
    case 0x200:
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
