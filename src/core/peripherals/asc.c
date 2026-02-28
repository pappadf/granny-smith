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
#include "log.h"
#include "system.h"

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
#define FIFO_IRQ_A_HALF 0x01 // channel A half-empty
#define FIFO_IRQ_A_FULL 0x02 // channel A completely empty
#define FIFO_IRQ_B_HALF 0x04 // channel B half-empty
#define FIFO_IRQ_B_FULL 0x08 // channel B completely empty

// ASC operating modes
#define MODE_OFF       0
#define MODE_FIFO      1
#define MODE_WAVETABLE 2

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

    // === Pointers last (not checkpointed) ===
    memory_interface_t memory_interface;
    memory_map_t *map;
    scheduler_t *scheduler;
    via_t *via; // VIA2 for interrupt delivery, set via asc_set_via()
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

// ============================================================================
// Static Helpers
// ============================================================================

// Drives the VIA2 CB1 interrupt line based on current FIFO IRQ status
static void asc_update_irq(asc_t *asc) {
    bool should_assert = (asc->fifo_irq_status != 0);
    if (should_assert == asc->irq_active)
        return; // no change
    asc->irq_active = should_assert;
    if (asc->via) {
        // CB1 on VIA2: port=1, c=0; active-low so invert the logic
        via_input_c(asc->via, 1, 0, !should_assert);
    }
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
        LOG(1, "fifo_push: channel %d overflow, dropping byte", ch);
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

// Drains one byte from a channel's FIFO; returns 0x80 (silence) if empty
static uint8_t fifo_pop(asc_t *asc, int ch) {
    if (asc->fifo_count[ch] == 0)
        return 0x80; // offset-binary silence
    uint16_t base = (ch == 0) ? CH_A_BASE : CH_B_BASE;
    uint8_t byte = asc->ram[base + asc->fifo_rptr[ch]];
    asc->fifo_rptr[ch] = (asc->fifo_rptr[ch] + 1) & FIFO_MASK;
    asc->fifo_count[ch]--;
    return byte;
}

// Checks FIFO fill levels and latches interrupt flags on downward threshold crossing.
// On the original ASC, the half-empty bit only fires when the FIFO has been filled
// past the halfway point and then drains back down below it (latch-on-transition).
static void fifo_check_irq(asc_t *asc) {
    uint8_t new_flags = 0;

    for (int ch = 0; ch < 2; ch++) {
        uint8_t half_bit = (ch == 0) ? FIFO_IRQ_A_HALF : FIFO_IRQ_B_HALF;
        uint8_t full_bit = (ch == 0) ? FIFO_IRQ_A_FULL : FIFO_IRQ_B_FULL;

        // Half-empty: latch-on-transition — only fire when crossing below threshold
        if (asc->fifo_above_half[ch] && asc->fifo_count[ch] < FIFO_HALF_THRESHOLD) {
            new_flags |= half_bit;
            asc->fifo_above_half[ch] = false; // reset until refilled past threshold
        }

        // Completely empty
        if (asc->fifo_count[ch] == 0)
            new_flags |= full_bit;
    }

    // Only update if new flags appeared (avoid re-triggering on already-set bits)
    uint8_t rising = new_flags & ~asc->fifo_irq_status;
    if (rising) {
        asc->fifo_irq_status |= rising;
        asc_update_irq(asc);
    }
}

// Writes one byte to a wavetable phase or increment register (big-endian, byte-at-a-time)
static void write_wave_reg(uint32_t *reg, int byte_pos, uint8_t data) {
    int shift = (3 - byte_pos) * 8;
    uint32_t mask = ~((uint32_t)0xFF << shift);
    *reg = (*reg & mask) | ((uint32_t)data << shift);
    *reg &= PHASE_MASK; // keep only lower 24 bits
}

// Extracts the 3-bit digital volume level (0-7) from the volume register
static int get_volume(const asc_t *asc) {
    return (asc->volume >> 5) & 0x07;
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

    case REG_MODE:
        LOG(2, "mode = %d", data);
        asc->mode = data;
        break;

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
        // Read-only; writes ignored
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
        asc->clock_rate = data;
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
    asc->via = NULL;

    // Original ASC silicon identifier (SE/30 uses 344S0053)
    asc->version = 0x00;

    // Set up the memory-mapped I/O interface
    asc->memory_interface = (memory_interface_t){
        .read_uint8 = asc_read_byte,
        .read_uint16 = asc_read_word,
        .read_uint32 = asc_read_long,
        .write_uint8 = asc_write_byte,
        .write_uint16 = asc_write_word,
        .write_uint32 = asc_write_long,
    };

    // Register the ASC in the memory map (machine will place at 0x50014000)
    memory_map_add(map, 0, ASC_MAPPED_SIZE, "ASC", &asc->memory_interface, asc);

    // Restore plain-data state from checkpoint if provided
    if (checkpoint) {
        size_t data_size = offsetof(asc_t, memory_interface);
        system_read_checkpoint_data(checkpoint, asc, data_size);
    }

    return asc;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Frees all resources associated with an ASC instance
void asc_delete(asc_t *asc) {
    if (!asc)
        return;
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

// Connects the ASC interrupt output to VIA2 CB1 for interrupt delivery
void asc_set_via(asc_t *asc, via_t *via) {
    asc->via = via;
}

// ============================================================================
// Operations (Public API)
// ============================================================================

// Renders nsamples of stereo audio into buffer (interleaved L/R, 16-bit signed).
// In FIFO mode, drains FIFO data and checks interrupt thresholds.
// In wavetable mode, advances phase accumulators and synthesises output.
void asc_render(asc_t *asc, int16_t *buffer, int nsamples) {
    if (!asc || !buffer || nsamples <= 0)
        return;

    int vol = get_volume(asc);

    if (asc->mode == MODE_FIFO) {
        // FIFO playback: drain one sample per channel per output frame
        for (int i = 0; i < nsamples; i++) {
            // Pop one byte per channel (8-bit offset binary, 0x80 = silence)
            uint8_t sample_a = fifo_pop(asc, 0);
            uint8_t sample_b = fifo_pop(asc, 1);

            // Convert from offset-binary (unsigned 0-255) to signed 16-bit
            int16_t left = (int16_t)((sample_a - 128) * vol) << 5;
            int16_t right = (int16_t)((sample_b - 128) * vol) << 5;

            // Interleaved stereo output
            buffer[i * 2] = left;
            buffer[i * 2 + 1] = right;
        }
        // Check if FIFO levels crossed the half-empty threshold
        fifo_check_irq(asc);

    } else if (asc->mode == MODE_WAVETABLE) {
        // Wavetable synthesis: advance phase accumulators and sum active voices
        for (int i = 0; i < nsamples; i++) {
            int32_t mixed = 0;
            int active_count = 0;

            for (int v = 0; v < WAVE_VOICE_COUNT; v++) {
                if (!(asc->wave_control & (1 << v)))
                    continue; // voice not enabled

                // Extract 9-bit integer from 9.15 fixed-point phase accumulator
                int index = (asc->wave_phase[v] >> 15) & WAVE_INDEX_MASK;

                // Each voice has a 512-byte table: voice 0 at 0x000, 1 at 0x200, etc.
                int table_base = v * WAVE_TABLE_SIZE;
                int8_t sample = (int8_t)asc->ram[table_base + index];

                mixed += sample;
                active_count++;

                // Advance phase accumulator by the voice's frequency increment
                asc->wave_phase[v] = (asc->wave_phase[v] + asc->wave_incr[v]) & PHASE_MASK;
            }

            // Scale by volume and expand to 16-bit; output same value to both channels
            int16_t out = 0;
            if (active_count > 0)
                out = (int16_t)((mixed * vol) << 5);

            buffer[i * 2] = out;
            buffer[i * 2 + 1] = out;
        }

    } else {
        // Mode 0 (off) or unknown: output silence
        memset(buffer, 0, (size_t)(nsamples * 2) * sizeof(int16_t));
    }
}
