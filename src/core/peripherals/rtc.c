// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rtc.c
// Real-Time Clock (RTC) and Parameter RAM emulation for Mac Plus.

#include "rtc.h"

#include "log.h"
#include "system.h"
#include "via.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_USE_CATEGORY_NAME("rtc");

// === Private Types ===
// RTC state structure (opaque to callers)
struct rtc {
    /* Plain data fields first so we can read/write a single contiguous block
       for checkpointing using offsetof to stop before pointer fields. */
    uint32_t seconds;
    int tx_bits;
    int rx_bits;
    bool clock;
    uint8_t command;
    uint16_t shift;
    unsigned char read_only;
    unsigned char pram[256];

    /* Pointers and non-POD members last */
    via_t *via;
    struct scheduler *scheduler;
};

// diff between mac (1904) and unix (1970) epochs
// precalculated using any online epoch converter
#define MAC_TO_UNIX_EPOCH 2082844800

#define IS_READ(cmd)     (cmd & 0x80)
#define IS_EXTENDED(cmd) ((cmd >> 3 & 0x0F) == 7)

#define CMD_SECONDS_REG_0 0x01
#define CMD_SECONDS_REG_1 0x05
#define CMD_SECONDS_REG_2 0x09
#define CMD_SECONDS_REG_3 0x0D
#define CMD_TEST          0x31
#define CMD_WRITE_PROTECT 0x35

void rtc_input(rtc_t *rtc, bool disable, bool clock, bool pram);

static uint8_t read_cmd(rtc_t *rtc, uint8_t cmd) {
    // high bits set equals read operation
    assert(cmd >> 7);

    switch (cmd & 0x7F) {

    case CMD_SECONDS_REG_0:
    case CMD_SECONDS_REG_0 + 16:
        return rtc->seconds & 0xFF;

    case CMD_SECONDS_REG_1:
    case CMD_SECONDS_REG_1 + 16:
        return rtc->seconds >> 8 & 0xFF;

    case CMD_SECONDS_REG_2:
    case CMD_SECONDS_REG_2 + 16:
        return rtc->seconds >> 16 & 0xFF;

    case CMD_SECONDS_REG_3:
    case CMD_SECONDS_REG_3 + 16:
        return rtc->seconds >> 24 & 0xFF;

    case CMD_TEST:
        // Test register is write-only; reading it is unexpected but should return 0
        LOG(2, "Unexpected read from write-only TEST register");
        return 0x00;

    case CMD_WRITE_PROTECT:
        // Write-protect register is write-only
        LOG(2, "Unexpected read from write-only WRITE_PROTECT register");
        return 0x00;

    default:
        // Pattern for addresses 0x10-0x13: z010aa01 where aa are address bits
        // Fixed bits: 6=0, 5=1, 4=0. Bit 3 is an address bit, not checked.
        // Mask: 0x70 (0111 0000) checks only bits 6-5-4
        if ((cmd & 0x70) == 0x20)
            return rtc->pram[0x10 + ((cmd >> 2) & 0x03)];
        // Pattern for addresses 0x00-0x0F: z1aaaa01 where aaaa are address bits
        // Fixed bit: 6=1. Bits 5-2 are address bits, not checked.
        // Mask: 0x40 (0100 0000) checks only bit 6
        else if ((cmd & 0x40) == 0x40)
            return rtc->pram[(cmd >> 2) & 0x0F];
    }

    LOG(1, "Unknown read command: 0x%02X", cmd);
    return 0;
}

static void write_cmd(rtc_t *rtc, uint8_t cmd, uint8_t pram) {
    // High bit clear indicates write operation
    assert(cmd >> 7 == 0);

    // Write-protect command itself is always allowed
    if (cmd != CMD_WRITE_PROTECT) {
        if (rtc->read_only) {
            // Silently ignore write attempts when protected (real hardware behavior)
            LOG(2, "Write attempt while write-protected: cmd=0x%02X data=0x%02X", cmd, pram);
            return;
        }
    }

    switch (cmd) {

    case CMD_SECONDS_REG_0:
    case CMD_SECONDS_REG_0 + 16:
        rtc->seconds = rtc->seconds & 0xFFFFFF00 | pram;
        return;

    case CMD_SECONDS_REG_1:
    case CMD_SECONDS_REG_1 + 16:
        rtc->seconds = rtc->seconds & 0xFFFF00FF | (uint32_t)pram << 8;
        return;

    case CMD_SECONDS_REG_2:
    case CMD_SECONDS_REG_2 + 16:
        rtc->seconds = rtc->seconds & 0xFF00FFFF | (uint32_t)pram << 16;
        return;

    case CMD_SECONDS_REG_3:
    case CMD_SECONDS_REG_3 + 16:
        rtc->seconds = rtc->seconds & 0x00FFFFFF | (uint32_t)pram << 24;
        return;

    case CMD_TEST:
        // Test register is write-only and used to clear test mode during initialization
        LOG(3, "Write to TEST register: value=0x%02X", pram);
        // In real hardware, this clears test mode. In emulation, no action needed.
        return;

    case CMD_WRITE_PROTECT:
        // The data byte determines protection: bit 7 = 1 means protected
        // 0x35 0x55 disables protection (bit 7 = 0)
        // 0x35 0xD5 enables protection (bit 7 = 1)
        rtc->read_only = (pram >> 7) & 1;
        LOG(3, "Write protection %s", rtc->read_only ? "enabled" : "disabled");
        return;

    default:
        // Pattern for addresses 0x10-0x13: z010aa01 where aa are address bits
        // Fixed bits: 6=0, 5=1, 4=0. Bit 3 is an address bit, not checked.
        // Mask: 0x70 (0111 0000) checks only bits 6-5-4
        if ((cmd & 0x70) == 0x20)
            rtc->pram[0x10 + ((cmd >> 2) & 0x03)] = pram;
        // Pattern for addresses 0x00-0x0F: z1aaaa01 where aaaa are address bits
        // Fixed bit: 6=1. Bits 5-2 are address bits, not checked.
        // Mask: 0x40 (0100 0000) checks only bit 6
        else if ((cmd & 0x40) == 0x40)
            rtc->pram[(cmd >> 2) & 0x0F] = pram;
        else
            LOG(1, "Unknown write command: 0x%02X data=0x%02X", cmd, pram);
    }

    return;
}

// Extract address from extended command bytes
// Extended commands use bits from both cmd1 and cmd2 to form the full address
static uint8_t read_ext(rtc_t *rtc, uint8_t cmd1, uint8_t cmd2) {
    // Extract address bits: bits [2:0] of cmd1 become addr[7:5], bits [6:2] of cmd2 become addr[4:0]
    uint8_t addr_high = (cmd1 & 0x07) << 5; // bits 7:5 from cmd1
    uint8_t addr_low = (cmd2 >> 2) & 0x1F; // bits 4:0 from cmd2
    uint8_t address = addr_high | addr_low;

    LOG(3, "Extended read: cmd1=0x%02X cmd2=0x%02X addr=0x%02X", cmd1, cmd2, address);
    return rtc->pram[address];
}

static void write_ext(rtc_t *rtc, uint8_t cmd1, uint8_t cmd2, uint8_t value) {
    // Extract address bits: bits [2:0] of cmd1 become addr[7:5], bits [6:2] of cmd2 become addr[4:0]
    uint8_t addr_high = (cmd1 & 0x07) << 5; // bits 7:5 from cmd1
    uint8_t addr_low = (cmd2 >> 2) & 0x1F; // bits 4:0 from cmd2
    uint8_t address = addr_high | addr_low;

    LOG(3, "Extended write: cmd1=0x%02X cmd2=0x%02X addr=0x%02X value=0x%02X", cmd1, cmd2, address, value);
    rtc->pram[address] = value;
}

void rtc_input(rtc_t *restrict rtc, bool disable, bool clock, bool data) {
    // Only process clock signal changes
    if (clock == rtc->clock)
        return;

    rtc->clock = clock;

    // Only act on rising (positive) edges of the clock
    // Data is latched on the rising edge per [1]: set data, raise clock, lower clock
    if (!clock)
        return;

    if (disable) {
        rtc->command = 0;
        rtc->tx_bits = 0;
        rtc->rx_bits = 8;
        rtc->shift = 0; // Clear shift register for complete state reset
        return;
    }

    // Allow valid state transitions where both may be 0 momentarily, or at least one is active
    assert((rtc->rx_bits >= 0 && rtc->tx_bits >= 0) && (rtc->rx_bits > 0 || rtc->tx_bits > 0));

    if (rtc->rx_bits) {

        rtc->shift = rtc->shift << 1 | data;

        if (!--rtc->rx_bits) {

            if (!rtc->command) {

                // if it's a non-exteded read command - simply return the data
                // Non-extended read command: return data immediately
                if (IS_READ(rtc->shift) && !IS_EXTENDED(rtc->shift)) {
                    rtc->shift = read_cmd(rtc, (uint8_t)rtc->shift);
                    rtc->tx_bits = 8;
                } else { // In all other cases, we need to wait for more input
                    rtc->command = (uint8_t)rtc->shift;
                    if (IS_EXTENDED(rtc->command) && !IS_READ(rtc->command))
                        // Extended write: need cmd2 (8 bits) + data (8 bits) = 16 bits total
                        rtc->rx_bits = 16;
                    else
                        // Extended read or normal write: need one more byte (8 bits)
                        rtc->rx_bits = 8;
                }
            } else if (IS_EXTENDED(rtc->command)) {

                if (IS_READ(rtc->command)) {
                    // Extended read: look up PRAM value and switch to transmit mode
                    rtc->shift = read_ext(rtc, rtc->command, (uint8_t)rtc->shift);
                    rtc->tx_bits = 8;
                } else {
                    // Extended write: we have cmd2 in lower 8 bits, data in upper 8 bits
                    write_ext(rtc, rtc->command, rtc->shift >> 8, rtc->shift & 0xFF);
                    rtc->rx_bits = 8;
                }

                rtc->command = 0;
            } else { // normal (non extended) write command

                assert(!IS_READ(rtc->command));
                write_cmd(rtc, rtc->command, (uint8_t)rtc->shift);
                rtc->command = 0;
                rtc->rx_bits = 8;
            }
        }
    } else {

        via_input(rtc->via, 1, 0, rtc->shift >> 7 & 1);

        if (!--rtc->tx_bits) {
            rtc->command = 0;
            rtc->rx_bits = 8;
        } else
            rtc->shift <<= 1;
    }

    assert((rtc->rx_bits && !rtc->tx_bits) || (!rtc->rx_bits && rtc->tx_bits));
}

static void one_second_interrupt(void *source, uint64_t data) {
    rtc_t *rtc = (rtc_t *)source;

    rtc->seconds++;

    LOG(1, "one_second_interrupt: seconds=%u", rtc->seconds);

    via_input_c(rtc->via, 0, 1, 0);
    via_input_c(rtc->via, 0, 1, 1);

    scheduler_new_cpu_event(rtc->scheduler, &one_second_interrupt, rtc, 0, 0, 1000000000ULL);
}

static uint32_t wall_clock_seconds(void) {
    assert(sizeof(time_t) >= 4);

    return (uint32_t)(time(NULL) + MAC_TO_UNIX_EPOCH);
}

void rtc_set_via(rtc_t *restrict rtc, via_t *via) {
    rtc->via = via;
}

rtc_t *rtc_init(struct scheduler *restrict scheduler, checkpoint_t *checkpoint) {
    rtc_t *rtc = (rtc_t *)malloc(sizeof(rtc_t));
    if (rtc == NULL)
        return NULL;

    memset(rtc, 0, sizeof(rtc_t));

    rtc->scheduler = scheduler;

    rtc->seconds = wall_clock_seconds();

    LOG(1, "rtc_init: seconds=%u", rtc->seconds);

    // Register event type for checkpointing
    scheduler_new_event_type(scheduler, "rtc", rtc, "one_second", &one_second_interrupt);

    // Load from checkpoint if provided. Read a single contiguous block containing
    // the plain-data members of rtc_t (everything before the first pointer).
    if (checkpoint) {
        size_t data_size = offsetof(rtc_t, via);
        system_read_checkpoint_data(checkpoint, rtc, data_size);
        // Do NOT schedule the default one-second event here; it will be restored
        // from the scheduler's checkpointed event queue in scheduler_start().
        LOG(1, "rtc_init: restored from checkpoint");
    } else {
        // Fresh boot: schedule periodic one-second interrupt
        scheduler_new_cpu_event(rtc->scheduler, &one_second_interrupt, rtc, 0, 0, 1000000000ULL);
        LOG(1, "rtc_init: scheduled one-second interrupt");
    }

    return rtc;
}

void rtc_delete(rtc_t *rtc) {
    if (!rtc)
        return;
    LOG(1, "rtc_delete: freeing rtc seconds=%u", rtc->seconds);
    free(rtc);
}

void rtc_checkpoint(rtc_t *restrict rtc, checkpoint_t *checkpoint) {
    if (!rtc || !checkpoint)
        return;
    // Write the contiguous plain-data portion of rtc_t in a single operation.
    size_t data_size = offsetof(rtc_t, via);
    system_write_checkpoint_data(checkpoint, rtc, data_size);
}
