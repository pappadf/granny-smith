// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// asc.c
// Implements the Apple Sound Chip (ASC) peripheral for the SE/30.
//
// The ASC provides two audio modes:
//   - FIFO mode (mode 1): dual-channel 8-bit PCM playback with interrupt-driven
//     buffer refill via VIA2 CB1.
//   - Wavetable mode (mode 2): four-voice synthesis using 9.15 fixed-point phase
//     accumulators into 512-byte waveform tables stored in internal SRAM.
//
// The SE/30 uses the original discrete ASC (344S0053, VERSION = $00).
// Registers are byte-accessible at offsets 0x800+ from the ASC base address.
// The internal SRAM occupies offsets 0x000-0x7FF (2048 bytes).
//
// SRAM layout by mode:
//   FIFO mode:      0x000-0x3FF = FIFO A (left), 0x400-0x7FF = FIFO B (right)
//   Wavetable mode: 0x000-0x1FF = voice 0, 0x200-0x3FF = voice 1,
//                   0x400-0x5FF = voice 2, 0x600-0x7FF = voice 3

// ============================================================================
// Includes
// ============================================================================

#include "asc.h"
#include "audio_out.h"
#include "log.h"
#include "machine_profile.h" // machine_object()
#include "object.h"
#include "system.h"
#include "value.h"

// Forward declaration — the `machine.sound` class descriptor lives with the
// object-model section near the bottom of the file; asc_init references it.
extern const class_desc_t asc_sound_class;

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

LOG_USE_CATEGORY_NAME("asc");

// Total internal SRAM size (2 KB = four 512-byte banks)
#define ASC_RAM_SIZE 2048

// FIFO depth per channel (two 1024-byte circular buffers)
#define FIFO_SIZE 1024

// FIFO pointer wrap mask
#define FIFO_MASK (FIFO_SIZE - 1)

// FIFO half-empty threshold: IRQ latches when count crosses below this going down
#define FIFO_HALF_THRESHOLD 512

// Channel base offsets within SRAM (FIFO A = first 1 KB, FIFO B = second 1 KB)
#define CH_A_BASE 0x000
#define CH_B_BASE 0x400

// Wavetable voice count and table size per voice
#define WAVE_VOICE_COUNT 4
#define WAVE_TABLE_SIZE  512

// Wavetable index mask (9-bit integer portion of 9.15 fixed-point phase)
#define WAVE_INDEX_MASK 0x1FF

// 24-bit phase accumulator mask (9.15 fixed-point)
#define PHASE_MASK 0x00FFFFFF

// Register offsets from ASC base address
#define REG_VERSION      0x800
#define REG_MODE         0x801
#define REG_CONTROL      0x802
#define REG_FIFO_CONTROL 0x803
#define REG_FIFO_IRQ     0x804
#define REG_WAVE_CONTROL 0x805
#define REG_VOLUME       0x806
#define REG_CLOCK_RATE   0x807
#define REG_PLAY_REC_A   0x80A
#define REG_PLAY_REC_B   0x80B
#define REG_TEST         0x80F

// Wavetable phase/increment registers: 4 bytes each, 4 voices
// Phase 0 at 0x810, Incr 0 at 0x814, Phase 1 at 0x818, etc.
#define REG_WAVE_BASE 0x810

// Total mapped size: SRAM (0x000-0x7FF) + registers (0x800-0x82F)
#define ASC_MAPPED_SIZE 0x830

// FIFO IRQ status bit positions
#define FIFO_IRQ_A_HALF 0x01 // channel A half-empty (drained below 512 bytes)
#define FIFO_IRQ_A_FULL 0x02 // channel A full / overflow
#define FIFO_IRQ_B_HALF 0x04 // channel B half-empty (drained below 512 bytes)
#define FIFO_IRQ_B_FULL 0x08 // channel B full / overflow

// ASC operating modes
#define MODE_OFF       0
#define MODE_FIFO      1
#define MODE_WAVETABLE 2

// Host-push batch size in frames. The drain event stays per-sample in
// emulated time (FIFO pops and IRQ threshold checks are sample-exact, as the
// ROM POST requires); only the pushes across the platform boundary are
// batched. 64 frames ≈ 2.9 ms at 22,257 Hz — small against the worklet's
// ~83 ms target depth.
#define ASC_PUSH_BATCH 64

// ascClockRate (0x807) → sample rate in Hz. 0 = 22,257 Hz (Mac master
// clock/1136), 2 = 22,050 Hz, 3 = 44,100 Hz; 1 is not a documented setting
// and falls back to the default rate.
static const uint32_t asc_rate_table[4] = {22257, 22257, 22050, 44100};

// Matching drain-event periods in nanoseconds of emulated time (1e9 / rate)
static const uint64_t asc_period_table[4] = {44929, 44929, 45351, 22676};

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Apple Sound Chip state; plain data first for checkpointing
struct asc {
    // === Plain data (checkpointed via memcpy up to the 'memory_interface' field) ===

    // Internal SRAM (2 KB): holds FIFO data or wavetable waveforms
    uint8_t ram[ASC_RAM_SIZE];

    // Control registers
    uint8_t version; // 0x800: read-only chip type ($00 = original ASC)
    uint8_t mode; // 0x801: 0=off, 1=FIFO, 2=wavetable
    uint8_t control; // 0x802: bit 1=stereo, bit 0=PWM select
    uint8_t fifo_control; // 0x803: bit 7=FIFO clear strobe
    uint8_t fifo_irq_status; // 0x804: per-channel half/full flags (read-clears)
    uint8_t wave_control; // 0x805: voice enable bits (bits 0-3)
    uint8_t volume; // 0x806: bits 5-7 = 3-bit digital volume level
    uint8_t clock_rate; // 0x807: sample rate select
    uint8_t play_rec_a; // 0x80A: channel A play/record mode
    uint8_t play_rec_b; // 0x80B: channel B play/record mode
    uint8_t test_reg; // 0x80F: hardware test register

    // Wavetable phase accumulators and frequency increments (4 voices)
    uint32_t wave_phase[WAVE_VOICE_COUNT]; // 9.15 fixed-point current phase
    uint32_t wave_incr[WAVE_VOICE_COUNT]; // 9.15 fixed-point increment per sample

    // FIFO read/write pointers and fill counts (0=channel A, 1=channel B)
    uint16_t fifo_wptr[2]; // write position within channel's 1024-byte FIFO
    uint16_t fifo_rptr[2]; // read position within channel's 1024-byte FIFO
    uint16_t fifo_count[2]; // number of bytes currently in each FIFO

    // Tracks whether each channel was previously above the half-empty threshold,
    // so the IRQ only fires on the downward crossing (latch-on-transition)
    bool fifo_above_half[2];

    // Interrupt output state (true = VIA2 CB1 driven low)
    bool irq_active;

    // Last byte the DAC consumed per channel — the real chip repeats it on
    // FIFO underflow (hold-last-byte), which doubles as underrun concealment
    uint8_t fifo_last[2];

    // === Pointers / transient state last (not checkpointed) ===
    memory_interface_t memory_interface;
    memory_map_t *map;
    scheduler_t *scheduler;

    // Chipset IRQ sink: the chip model stays chipset-agnostic. GLUE machines
    // install the VIA2-CB1 adapter via asc_set_via(); RBV machines wire
    // rbv_set_snd_irq (flag bit 4); OSS routes to its own mask model.
    void (*irq_fn)(void *ctx, bool active);
    void *irq_ctx;

    // Board speaker mix (re-set by the machine after every asc_init)
    asc_mix_t mix;

    // Producer push batch: mono int16 frames accumulated by the drain event
    // and flushed to audio_out in blocks (transient — not checkpointed; a
    // restore drops at most ASC_PUSH_BATCH-1 pending frames)
    int16_t out_buf[ASC_PUSH_BATCH];
    int out_count;

    struct object *object; // `machine.sound` node; NULL when not attached
};

// ============================================================================
// Forward Declarations
// ============================================================================

static uint8_t asc_read_byte(void *device, uint32_t addr);
static void asc_write_byte(void *device, uint32_t addr, uint8_t data);
static uint16_t asc_read_word(void *device, uint32_t addr);
static void asc_write_word(void *device, uint32_t addr, uint16_t data);
static uint32_t asc_read_long(void *device, uint32_t addr);
static void asc_write_long(void *device, uint32_t addr, uint32_t data);
static void asc_fifo_drain_callback(void *source, uint64_t data);
static void asc_schedule_fifo_drain(asc_t *asc);
static void asc_cancel_fifo_drain(asc_t *asc);

// ============================================================================
// Static Helpers
// ============================================================================

// Drives the chipset interrupt sink based on current FIFO IRQ status
static void asc_update_irq(asc_t *asc) {
    bool should_assert = (asc->fifo_irq_status != 0);
    if (should_assert == asc->irq_active)
        return; // no change
    asc->irq_active = should_assert;
    if (asc->irq_fn)
        asc->irq_fn(asc->irq_ctx, should_assert);
}

// Resets both FIFO channels to empty state (triggered by fifo_control bit 7 toggle)
static void fifo_clear(asc_t *asc) {
    LOG(2, "fifo_clear: resetting both FIFOs");
    for (int ch = 0; ch < 2; ch++) {
        asc->fifo_wptr[ch] = 0;
        asc->fifo_rptr[ch] = 0;
        asc->fifo_count[ch] = 0;
        asc->fifo_above_half[ch] = false;
    }
}

// Appends one byte to a channel's FIFO at the internal write pointer.
// In FIFO mode the CPU address within a channel's range is irrelevant —
// all writes feed the circular buffer sequentially.
static void fifo_push(asc_t *asc, int ch, uint8_t byte) {
    if (asc->fifo_count[ch] >= FIFO_SIZE) {
        // FIFO overflow: set the full/overflow bit in FIFO_IRQ (§6.3 in docs)
        uint8_t full_bit = (ch == 0) ? FIFO_IRQ_A_FULL : FIFO_IRQ_B_FULL;
        if (!(asc->fifo_irq_status & full_bit)) {
            LOG(1, "fifo_push: channel %d overflow, setting full IRQ", ch);
            asc->fifo_irq_status |= full_bit;
            asc_update_irq(asc);
        }
        return;
    }
    uint16_t base = (ch == 0) ? CH_A_BASE : CH_B_BASE;
    asc->ram[base + asc->fifo_wptr[ch]] = byte;
    asc->fifo_wptr[ch] = (asc->fifo_wptr[ch] + 1) & FIFO_MASK;
    asc->fifo_count[ch]++;

    // Track that this channel has been filled above the half-empty point
    if (asc->fifo_count[ch] >= FIFO_HALF_THRESHOLD)
        asc->fifo_above_half[ch] = true;
}

// Drains one byte from a channel's FIFO. On underflow the real chip keeps
// outputting the last byte it consumed (hold-last-byte) — faithful, and it
// doubles as natural underrun concealment host-side.
static uint8_t fifo_pop(asc_t *asc, int ch) {
    if (asc->fifo_count[ch] == 0)
        return asc->fifo_last[ch];
    uint16_t base = (ch == 0) ? CH_A_BASE : CH_B_BASE;
    uint8_t byte = asc->ram[base + asc->fifo_rptr[ch]];
    asc->fifo_rptr[ch] = (asc->fifo_rptr[ch] + 1) & FIFO_MASK;
    asc->fifo_count[ch]--;
    asc->fifo_last[ch] = byte;
    return byte;
}

// Checks FIFO fill levels and latches half-empty IRQ on downward threshold crossing.
// On the original ASC, the half-empty bit only fires when the FIFO has been filled
// past the halfway point and then drains back down below it (latch-on-transition).
// The full/overflow bit is set separately in fifo_push() on write overflow.
static void fifo_check_irq(asc_t *asc) {
    uint8_t new_flags = 0;

    for (int ch = 0; ch < 2; ch++) {
        uint8_t half_bit = (ch == 0) ? FIFO_IRQ_A_HALF : FIFO_IRQ_B_HALF;

        // Half-empty: latch-on-transition — only fire when crossing below threshold
        if (asc->fifo_above_half[ch] && asc->fifo_count[ch] < FIFO_HALF_THRESHOLD) {
            new_flags |= half_bit;
            asc->fifo_above_half[ch] = false; // reset until refilled past threshold
        }
    }

    // Only update if new flags appeared (avoid re-triggering on already-set bits)
    uint8_t rising = new_flags & ~asc->fifo_irq_status;
    if (rising) {
        asc->fifo_irq_status |= rising;
        asc_update_irq(asc);
    }
}

// Extracts the 3-bit digital volume level (0-7) from the volume register
static int get_volume(const asc_t *asc) {
    return (asc->volume >> 5) & 0x07;
}

// Current sample rate in Hz from the clock-rate register
static uint32_t asc_rate_hz(const asc_t *asc) {
    return asc_rate_table[asc->clock_rate & 3];
}

// Saturates a 32-bit mix result to int16
static int16_t sat16(int32_t v) {
    if (v > INT16_MAX)
        return INT16_MAX;
    if (v < INT16_MIN)
        return INT16_MIN;
    return (int16_t)v;
}

// Flushes the pending push batch to the shared host audio stream
static void asc_flush(asc_t *asc) {
    if (asc->out_count <= 0)
        return;
    audio_out_push(asc->out_buf, asc->out_count, get_volume(asc));
    asc->out_count = 0;
}

// Produces one stereo frame of chip output in DAC units scaled to int16.
// FIFO mode: channel A drives the left DAC; in stereo (ascChipControl bit 1)
// channel B drives the right, in mono channel A feeds both converters.
// Wavetable mode: voices 0+1 sum to the left channel, 2+3 to the right
// (Guide to the Macintosh Family Hardware); each voice is scaled to half
// full-scale so a two-voice sum spans int16 exactly.
static void asc_produce_frame(asc_t *asc, int16_t *left, int16_t *right) {
    if (asc->mode == MODE_FIFO) {
        uint8_t a = fifo_pop(asc, 0);
        uint8_t b = fifo_pop(asc, 1); // both FIFOs drain regardless of routing
        fifo_check_irq(asc);
        bool stereo = (asc->control >> 1) & 1;
        *left = (int16_t)(((int)a - 128) << 8);
        *right = stereo ? (int16_t)(((int)b - 128) << 8) : *left;
        return;
    }

    // Wavetable synthesis: in mode 2 all four voices free-run — the phase
    // accumulators advance every sample clock with no enable gate (the boot
    // chime never touches $805). ascWaveOneShot ($805) selects one-shot
    // stop-at-wrap behavior per voice, which no shipped code path exercises
    // yet; it is stored but not otherwise modeled.
    int32_t pair[2] = {0, 0};
    for (int v = 0; v < WAVE_VOICE_COUNT; v++) {
        // Extract 9-bit integer from 9.15 fixed-point phase accumulator
        int index = (asc->wave_phase[v] >> 15) & WAVE_INDEX_MASK;
        // Wavetable bytes are offset-binary (0x80 = zero), same DAC
        // convention as the FIFO path; the Sound Manager's beep writes a
        // 0x80-centered sine that wraps to garbage under a plain int8 cast.
        int sample = (int)asc->ram[v * WAVE_TABLE_SIZE + index] - 128;
        pair[v >> 1] += sample;
        // Advance phase accumulator by the voice's frequency increment
        asc->wave_phase[v] = (asc->wave_phase[v] + asc->wave_incr[v]) & PHASE_MASK;
    }
    *left = sat16(pair[0] << 7);
    *right = sat16(pair[1] << 7);
}

// Scheduler callback: the ASC sample-rate producer. One frame per tick of
// emulated time — the chip's DAC consuming FIFO bytes / walking wavetable
// phase accumulators — so FIFO IRQ thresholds fire sample-exactly with or
// without a host audio sink (the ROM POST requires it headlessly). The
// resulting mono speaker frames are batched and pushed to audio_out.
static void asc_fifo_drain_callback(void *source, uint64_t data) {
    asc_t *asc = (asc_t *)source;
    (void)data;

    // Only produce while the chip is running (FIFO or wavetable mode)
    if (asc->mode != MODE_FIFO && asc->mode != MODE_WAVETABLE)
        return;

    int16_t left, right;
    asc_produce_frame(asc, &left, &right);

    // Board-level speaker fold: SE/30 mixes both channels into the speaker,
    // IIx/IIcx take left. The board mix is a passive analog combine, so it
    // averages rather than saturating — with the DACs' offset-binary DC bias
    // a digital sum would rail at INT16_MIN and flatten the waveform.
    int16_t mono = (asc->mix == ASC_MIX_SUM) ? (int16_t)(((int32_t)left + right) >> 1) : left;
    asc->out_buf[asc->out_count++] = mono;
    if (asc->out_count >= ASC_PUSH_BATCH)
        asc_flush(asc);

    // Re-schedule for the next sample period
    asc_schedule_fifo_drain(asc);
}

// Schedules the next producer event at the current ASC sample rate
static void asc_schedule_fifo_drain(asc_t *asc) {
    if (!asc->scheduler)
        return;
    scheduler_new_cpu_event(asc->scheduler, &asc_fifo_drain_callback, asc, 0, 0, asc_period_table[asc->clock_rate & 3]);
}

// Cancels any pending FIFO drain event
static void asc_cancel_fifo_drain(asc_t *asc) {
    if (!asc->scheduler)
        return;
    remove_event(asc->scheduler, &asc_fifo_drain_callback, asc);
}

// Writes one byte to a wavetable phase or increment register (big-endian, byte-at-a-time)
static void write_wave_reg(uint32_t *reg, int byte_pos, uint8_t data) {
    int shift = (3 - byte_pos) * 8;
    uint32_t mask = ~((uint32_t)0xFF << shift);
    *reg = (*reg & mask) | ((uint32_t)data << shift);
    *reg &= PHASE_MASK; // keep only lower 24 bits
}

// ============================================================================
// Memory Interface
// ============================================================================

// Handles byte reads from the ASC address space (SRAM + registers)
static uint8_t asc_read_byte(void *device, uint32_t addr) {
    asc_t *asc = (asc_t *)device;

    // SRAM region (0x000-0x7FF): direct read regardless of mode
    if (addr < ASC_RAM_SIZE) {
        LOG(4, "read ram[0x%03X] = 0x%02X", addr, asc->ram[addr]);
        return asc->ram[addr];
    }

    // Register region
    switch (addr) {
    case REG_VERSION:
        return asc->version;

    case REG_MODE:
        return asc->mode;

    case REG_CONTROL:
        return asc->control;

    case REG_FIFO_CONTROL:
        return asc->fifo_control;

    case REG_FIFO_IRQ: {
        // Read-clears: capture current flags then reset them
        uint8_t flags = asc->fifo_irq_status;
        asc->fifo_irq_status = 0;
        asc_update_irq(asc); // deassert CB1 since flags are now cleared
        LOG(3, "read fifo_irq_status = 0x%02X (cleared)", flags);
        return flags;
    }

    case REG_WAVE_CONTROL:
        return asc->wave_control;

    case REG_VOLUME:
        return asc->volume;

    case REG_CLOCK_RATE:
        return asc->clock_rate;

    case REG_PLAY_REC_A:
        return asc->play_rec_a;

    case REG_PLAY_REC_B:
        return asc->play_rec_b;

    case REG_TEST:
        return asc->test_reg;

    default:
        break;
    }

    // Wavetable phase/increment registers (0x810-0x82F)
    if (addr >= REG_WAVE_BASE && addr < REG_WAVE_BASE + WAVE_VOICE_COUNT * 8) {
        int reg_index = (addr - REG_WAVE_BASE) / 4;
        int byte_pos = (addr - REG_WAVE_BASE) % 4;
        int voice = reg_index / 2;
        bool is_incr = (reg_index & 1);

        if (voice < WAVE_VOICE_COUNT) {
            uint32_t val = is_incr ? asc->wave_incr[voice] : asc->wave_phase[voice];
            int shift = (3 - byte_pos) * 8;
            return (uint8_t)((val >> shift) & 0xFF);
        }
    }

    LOG(2, "read unknown addr 0x%03X", addr);
    return 0;
}

// Handles byte writes to the ASC address space (SRAM + registers)
static void asc_write_byte(void *device, uint32_t addr, uint8_t data) {
    asc_t *asc = (asc_t *)device;

    // SRAM region (0x000-0x7FF): behaviour depends on current mode
    if (addr < ASC_RAM_SIZE) {
        if (asc->mode == MODE_FIFO) {
            // In FIFO mode, the address within each channel's range is irrelevant.
            // All writes to 0x000-0x3FF feed FIFO A; 0x400-0x7FF feed FIFO B.
            int ch = (addr < CH_B_BASE) ? 0 : 1;
            fifo_push(asc, ch, data);
            LOG(4, "fifo push ch%d byte=0x%02X count=%d", ch, data, asc->fifo_count[ch]);
        } else {
            // In off or wavetable mode, writes go directly to SRAM
            asc->ram[addr] = data;
            LOG(4, "write ram[0x%03X] = 0x%02X", addr, data);
        }
        return;
    }

    // Register region
    switch (addr) {
    case REG_VERSION:
        // Read-only register; ignore writes
        LOG(3, "write to read-only VERSION register ignored");
        break;

    case REG_MODE: {
        uint8_t old_mode = asc->mode;
        LOG(2, "mode = %d", data);
        asc->mode = data;
        // Start or stop the sample-rate producer when the chip starts/stops
        // running (FIFO or wavetable mode; mode 0 = off)
        bool was_active = (old_mode == MODE_FIFO || old_mode == MODE_WAVETABLE);
        bool now_active = (data == MODE_FIFO || data == MODE_WAVETABLE);
        if (now_active && !was_active)
            asc_schedule_fifo_drain(asc);
        else if (!now_active && was_active) {
            asc_cancel_fifo_drain(asc);
            asc_flush(asc); // deliver any tail frames before going quiet
        }
        break;
    }

    case REG_CONTROL:
        LOG(3, "control = 0x%02X (stereo=%d)", data, (data >> 1) & 1);
        asc->control = data;
        break;

    case REG_FIFO_CONTROL: {
        // Detect rising edge on bit 7 to trigger FIFO clear
        bool old_strobe = asc->fifo_control & 0x80;
        bool new_strobe = data & 0x80;
        asc->fifo_control = data;
        if (!old_strobe && new_strobe)
            fifo_clear(asc);
        break;
    }

    case REG_FIFO_IRQ:
        // Writable for hardware diagnostics; the diagnostic ROM writes test
        // patterns and reads them back to verify register accessibility.
        LOG(3, "write fifo_irq_status = 0x%02X", data);
        asc->fifo_irq_status = data;
        asc_update_irq(asc);
        break;

    case REG_WAVE_CONTROL:
        LOG(3, "wave_control = 0x%02X", data);
        asc->wave_control = data;
        break;

    case REG_VOLUME:
        LOG(3, "volume = 0x%02X (level=%d)", data, (data >> 5) & 7);
        asc->volume = data;
        break;

    case REG_CLOCK_RATE:
        LOG(3, "clock_rate = %d", data);
        if ((data & 3) != (asc->clock_rate & 3)) {
            // Rate switch: flush at the old rate, retime the producer event,
            // and restart the host stream at the new rate
            asc_flush(asc);
            asc->clock_rate = data;
            audio_out_set_rate(asc_rate_hz(asc));
            if (asc->mode == MODE_FIFO || asc->mode == MODE_WAVETABLE) {
                asc_cancel_fifo_drain(asc);
                asc_schedule_fifo_drain(asc);
            }
        } else {
            asc->clock_rate = data;
        }
        break;

    case REG_PLAY_REC_A:
        LOG(3, "play_rec_a = 0x%02X", data);
        asc->play_rec_a = data;
        break;

    case REG_PLAY_REC_B:
        LOG(3, "play_rec_b = 0x%02X", data);
        asc->play_rec_b = data;
        break;

    case REG_TEST:
        asc->test_reg = data;
        break;

    default:
        // Wavetable phase/increment registers (0x810-0x82F)
        if (addr >= REG_WAVE_BASE && addr < REG_WAVE_BASE + WAVE_VOICE_COUNT * 8) {
            int reg_index = (addr - REG_WAVE_BASE) / 4;
            int byte_pos = (addr - REG_WAVE_BASE) % 4;
            int voice = reg_index / 2;
            bool is_incr = (reg_index & 1);

            if (voice < WAVE_VOICE_COUNT) {
                uint32_t *reg = is_incr ? &asc->wave_incr[voice] : &asc->wave_phase[voice];
                write_wave_reg(reg, byte_pos, data);
                LOG(3, "wave voice %d %s byte %d = 0x%02X (val=0x%06X)", voice, is_incr ? "incr" : "phase", byte_pos,
                    data, *reg & PHASE_MASK);
            }
        } else {
            LOG(2, "write unknown addr 0x%03X = 0x%02X", addr, data);
        }
        break;
    }
}

// Handles 16-bit reads by combining two byte reads (ASC has 8-bit data bus)
static uint16_t asc_read_word(void *device, uint32_t addr) {
    uint16_t hi = asc_read_byte(device, addr);
    uint16_t lo = asc_read_byte(device, addr + 1);
    return (hi << 8) | lo;
}

// Handles 16-bit writes by splitting into two byte writes
static void asc_write_word(void *device, uint32_t addr, uint16_t data) {
    asc_write_byte(device, addr, (uint8_t)(data >> 8));
    asc_write_byte(device, addr + 1, (uint8_t)(data & 0xFF));
}

// Handles 32-bit reads by combining four byte reads
static uint32_t asc_read_long(void *device, uint32_t addr) {
    uint32_t b0 = asc_read_byte(device, addr);
    uint32_t b1 = asc_read_byte(device, addr + 1);
    uint32_t b2 = asc_read_byte(device, addr + 2);
    uint32_t b3 = asc_read_byte(device, addr + 3);
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

// Handles 32-bit writes by splitting into four byte writes
static void asc_write_long(void *device, uint32_t addr, uint32_t data) {
    asc_write_byte(device, addr, (uint8_t)(data >> 24));
    asc_write_byte(device, addr + 1, (uint8_t)(data >> 16));
    asc_write_byte(device, addr + 2, (uint8_t)(data >> 8));
    asc_write_byte(device, addr + 3, (uint8_t)(data & 0xFF));
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Allocates and initialises an ASC instance, optionally restoring from checkpoint
asc_t *asc_init(memory_map_t *map, scheduler_t *scheduler, checkpoint_t *checkpoint) {
    asc_t *asc = (asc_t *)malloc(sizeof(asc_t));
    if (!asc)
        return NULL;
    memset(asc, 0, sizeof(asc_t));

    asc->map = map;
    asc->scheduler = scheduler;
    asc->mix = ASC_MIX_CH_A; // machines override via asc_set_mix

    // Original ASC silicon identifier (SE/30 uses 344S0053)
    asc->version = 0x00;

    // DAC hold-last-byte state starts at offset-binary silence
    asc->fifo_last[0] = 0x80;
    asc->fifo_last[1] = 0x80;

    // Set up the memory-mapped I/O interface
    asc->memory_interface = (memory_interface_t){
        .read_uint8 = asc_read_byte,
        .read_uint16 = asc_read_word,
        .read_uint32 = asc_read_long,
        .write_uint8 = asc_write_byte,
        .write_uint16 = asc_write_word,
        .write_uint32 = asc_write_long,
    };

    // Register with memory map if provided (NULL = machine handles registration)
    if (map)
        memory_map_add(map, 0, ASC_MAPPED_SIZE, "ASC", &asc->memory_interface, asc);

    // Restore plain-data state from checkpoint if provided
    if (checkpoint) {
        size_t data_size = offsetof(asc_t, memory_interface);
        system_read_checkpoint_data(checkpoint, asc, data_size);
    }

    // Register the FIFO drain event type for checkpoint save/restore
    if (scheduler)
        scheduler_new_event_type(scheduler, "asc", asc, "fifo_drain", &asc_fifo_drain_callback);

    // If restoring from a checkpoint with the chip running, restart the
    // producer event (rate comes from the restored clock-rate register)
    if (asc->mode == MODE_FIFO || asc->mode == MODE_WAVETABLE)
        asc_schedule_fifo_drain(asc);

    // Open the shared host audio stream: mono int16 at the chip's rate
    audio_out_open(asc_rate_hz(asc), 1);

    // Object-tree binding: `machine.sound` facade + shared capture sink
    asc->object = object_new(&asc_sound_class, asc, "sound");
    if (asc->object) {
        object_set_label(asc->object, "Sound");
        object_set_order(asc->object, 110);
        object_attach(machine_object(), asc->object);
        audio_out_capture_attach(asc->object);
    }

    return asc;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Return the ASC memory-mapped I/O interface for machine-level address decode
const memory_interface_t *asc_get_memory_interface(asc_t *asc) {
    return &asc->memory_interface;
}

// Frees all resources associated with an ASC instance
void asc_delete(asc_t *asc) {
    if (!asc)
        return;
    if (asc->object) {
        audio_out_capture_detach();
        object_detach(asc->object);
        object_delete(asc->object);
        asc->object = NULL;
    }
    if (asc->map)
        memory_map_remove(asc->map, 0, ASC_MAPPED_SIZE, "ASC", &asc->memory_interface, asc);
    free(asc);
}

// ============================================================================
// Lifecycle: Checkpointing
// ============================================================================

// Saves ASC plain-data state to a checkpoint (everything before the pointer fields)
void asc_checkpoint(asc_t *restrict asc, checkpoint_t *checkpoint) {
    if (!asc || !checkpoint)
        return;
    size_t data_size = offsetof(asc_t, memory_interface);
    system_write_checkpoint_data(checkpoint, asc, data_size);
}

// ============================================================================
// Wiring
// ============================================================================

// Connects the ASC interrupt output to VIA2 CB1 for interrupt delivery.
// The IRQ output is active-low and idles HIGH at power-on; drive CB1 to its
// idle state immediately so the VIA's edge detector has a known reference
// edge before the first asc_update_irq() asserts the line LOW.  Without this,
// the first overflow (e.g. MacTest sub-test 3 phase 1) calls
// via_input_c(.., false) on a CB1 line that was zero-initialised to LOW —
// no edge is observed and IFR_CB1 never latches.
// Adapts the generic IRQ output to a VIA2 CB1 line (active-low)
static void asc_via_cb1_adapter(void *ctx, bool active) {
    via_input_c((via_t *)ctx, /*port B*/ 1, /*c index 0 = CB1*/ 0, !active);
}

void asc_set_via(asc_t *asc, via_t *via) {
    asc_set_irq_handler(asc, via ? asc_via_cb1_adapter : NULL, via);
    if (via)
        via_input_c(via, /*port B*/ 1, /*c index 0 = CB1*/ 0, /*idle HIGH*/ true);
}

// Installs the chipset-specific interrupt sink (see the struct field docs).
// `fn` receives the logical IRQ level: true = interrupt asserted.
void asc_set_irq_handler(asc_t *asc, void (*fn)(void *ctx, bool active), void *ctx) {
    asc->irq_fn = fn;
    asc->irq_ctx = ctx;
}

// Selects the board's speaker mix (SE/30 sums L+R; IIx/IIcx take left).
// Not checkpointed — machines call this unconditionally after asc_init.
void asc_set_mix(asc_t *asc, asc_mix_t mix) {
    asc->mix = mix;
}

// ============================================================================
// Object-model surface: `machine.sound` on ASC machines
// ============================================================================
//
// A read-only facade over the guest-owned chip state (rate/volume/mode),
// plus the deterministic capture surface shared with the Plus module:
// `sound.capture.*` (attached from audio_out) and `sound.match`.

static asc_t *asc_self_from(struct object *self) {
    return (asc_t *)object_data(self);
}

static value_t asc_attr_sample_rate(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, asc_rate_hz(asc_self_from(self)));
}

static value_t asc_attr_volume(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, (uint64_t)get_volume(asc_self_from(self)));
}

static value_t asc_attr_mode(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, asc_self_from(self)->mode);
}

// Side-effect-free debug views of the FIFO engine.  The guest-visible
// FIFO-IRQ status register (0x804) is read-clears, so inspecting it via
// memory.peek perturbs the guest; these attributes read the model state
// directly for stall diagnosis.
static value_t asc_attr_fifo_count_a(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, asc_self_from(self)->fifo_count[0]);
}
static value_t asc_attr_fifo_count_b(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, asc_self_from(self)->fifo_count[1]);
}
static value_t asc_attr_fifo_irq_status(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, asc_self_from(self)->fifo_irq_status);
}
static value_t asc_attr_fifo_armed_a(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(asc_self_from(self)->fifo_above_half[0]);
}
static value_t asc_attr_fifo_armed_b(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(asc_self_from(self)->fifo_above_half[1]);
}

// `sound.match(reference)` — sample-exact compare of the last capture against
// a golden PCM WAV (delegates to the shared capture sink; same contract as
// the Plus sound class and screen.match).
static value_t asc_method_match(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return audio_out_match_value(argv[0].s);
}

static const arg_decl_t asc_match_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "Reference WAV path (PCM int16)"},
};

static const member_t asc_sound_members[] = {
    {.kind = M_ATTR,
     .name = "sample_rate",
     .flags = VAL_RO,
     .doc = "Output sample rate in Hz (from ascClockRate)",
     .attr = {.type = V_UINT, .get = asc_attr_sample_rate, .set = NULL}},
    {.kind = M_ATTR,
     .name = "volume",
     .flags = VAL_RO,
     .doc = "Guest-set output level (ascVolControl bits 5-7, 0..7)",
     .attr = {.type = V_UINT, .get = asc_attr_volume, .set = NULL}},
    {.kind = M_ATTR,
     .name = "mode",
     .flags = VAL_RO,
     .doc = "Chip mode (0 = off, 1 = FIFO, 2 = wavetable)",
     .attr = {.type = V_UINT, .get = asc_attr_mode, .set = NULL}},
    {.kind = M_ATTR,
     .name = "fifo_count_a",
     .flags = VAL_RO,
     .doc = "Bytes currently in FIFO A (debug view, no side effects)",
     .attr = {.type = V_UINT, .get = asc_attr_fifo_count_a, .set = NULL}},
    {.kind = M_ATTR,
     .name = "fifo_count_b",
     .flags = VAL_RO,
     .doc = "Bytes currently in FIFO B (debug view, no side effects)",
     .attr = {.type = V_UINT, .get = asc_attr_fifo_count_b, .set = NULL}},
    {.kind = M_ATTR,
     .name = "fifo_irq_status",
     .flags = VAL_RO,
     .doc = "FIFO IRQ status flags without the read-clears side effect",
     .attr = {.type = V_UINT, .get = asc_attr_fifo_irq_status, .set = NULL}},
    {.kind = M_ATTR,
     .name = "fifo_armed_a",
     .flags = VAL_RO,
     .doc = "Half-empty latch armed for FIFO A (filled above half since last fire)",
     .attr = {.type = V_BOOL, .get = asc_attr_fifo_armed_a, .set = NULL}},
    {.kind = M_ATTR,
     .name = "fifo_armed_b",
     .flags = VAL_RO,
     .doc = "Half-empty latch armed for FIFO B (filled above half since last fire)",
     .attr = {.type = V_BOOL, .get = asc_attr_fifo_armed_b, .set = NULL}},
    {.kind = M_METHOD,
     .name = "match",
     .doc = "Compare the last capture against a reference WAV (true if identical)",
     .method = {.args = asc_match_args, .nargs = 1, .result = V_BOOL, .fn = asc_method_match}},
};

const class_desc_t asc_sound_class = {
    .name = "sound",
    .members = asc_sound_members,
    .n_members = sizeof(asc_sound_members) / sizeof(asc_sound_members[0]),
};
