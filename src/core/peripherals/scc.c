// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scc.c
// Zilog SCC (Serial Communications Controller) emulation for Mac Plus.

#include "scc.h"

#include "appletalk.h"
#include "cpu.h"
#include "log.h"
#include "platform.h"
#include "scheduler.h"
#include "system.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDLC_MAX_FRAME         1024
#define RX_BUF_SIZE            1024
#define TX_BUF_SIZE            1024
#define RX_PENDING_QUEUE_DEPTH 8

LOG_USE_CATEGORY_NAME("scc");

static inline bool scc_should_log(int level) {
    return log_would_log(_log_get_local_category(), level);
}

// Emit a short hexdump preview for SDLC frames so AppleTalk traces stay readable
static void log_frame_preview(int level, const char *label, const uint8_t *data, size_t len) {
    if (!scc_should_log(level))
        return;

    if (!data || len == 0) {
        LOG(level, "%s len=%zu (no data)", label ? label : "frame", len);
        return;
    }

    size_t preview = len < 12 ? len : 12;
    char hex[3 * 12 + 1];
    size_t offset = 0;
    for (size_t i = 0; i < preview && offset < sizeof(hex); ++i) {
        int written = snprintf(hex + offset, sizeof(hex) - offset, "%02X%s", data[i], (i + 1 < preview) ? " " : "");
        if (written <= 0)
            break;
        offset += (size_t)written;
    }
    if (offset >= sizeof(hex))
        hex[sizeof(hex) - 1] = '\0';
    else
        hex[offset] = '\0';

    LOG(level, "%s len=%zu bytes=%s%s", label ? label : "frame", len, hex, (len > preview) ? " ..." : "");
}

struct ch;
typedef struct ch ch_t;

struct ch {

    int pointer;
    int index;

    uint8_t wr[16];
    uint8_t rr[16];

    // transmit buffer
    struct {
        uint8_t buf[TX_BUF_SIZE];
        int len;
    } tx;

    // receive buffer
    struct {
        uint8_t buf[RX_BUF_SIZE];
        size_t head, tail;
    } rx;

    // incoming sdlc frame
    struct {
        uint8_t buf[SDLC_MAX_FRAME];
        size_t len;
    } sdlc_in;

    struct {
        struct {
            size_t len;
            uint8_t buf[SDLC_MAX_FRAME];
        } frames[RX_PENDING_QUEUE_DEPTH];
        int head;
        int tail;
        int count;
    } pending_rx;

    // Baud Rate Generator state
    struct {
        uint16_t time_constant; // WR13:WR12
        uint16_t counter; // Current count
        bool enabled; // WR14 bit 0
        bool pclk_source; // WR14 bit 1 (0=RTxC, 1=PCLK)
    } brg;

    scc_t *scc;
};

struct scc {

    ch_t ch[2];

    struct scheduler *scheduler;
    memory_map_t *memory_map;
    memory_interface_t memory_interface;

    // Per-instance IRQ callback routing
    scc_irq_fn irq_cb;
    void *cb_context;
};

#define SDLC_MODE(ch)  ((ch->wr[4] >> 4 & 3) == 2)
#define TX_EMPTY(ch)   (ch->tx.len == 0)
#define RX_EMPTY(ch)   (ch->rx.head == ch->rx.tail)
#define RX_LEN(ch)     (ch->rx.head - ch->rx.tail)
#define RX_ENABLED(ch) (ch->wr[3] & 1)

// transmit/receive buffer status and external status
#define RR0_TX_UNDERRUN_EOM   0x40
#define RR0_SYNC_HUNT         0x10
#define RR0_DCD               0x08
#define RR0_TX_BUFFER_EMPTY   0x04
#define RR0_RX_CHAR_AVAILABLE 0x01

// special receive condition status
#define RR1_END_OF_FRAME 0x80

// interrupt pending register (only in ch a - always 0 in ch b)
#define RR3_CHANNEL_B_EXT 0x01
#define RR3_CHANNEL_B_TX  0x02
#define RR3_CHANNEL_B_RX  0x04
#define RR3_CHANNEL_A_EXT 0x08
#define RR3_CHANNEL_A_TX  0x10
#define RR3_CHANNEL_A_RX  0x20

// transmit/receive interrupt and data transfer mode definition
#define WR1_EXT_INT 0x01

// receive parameters and control
#define WR3_ENTER_HUNT_MODE 0x10
#define WR3_ADDRESS_SEARCH  0x04
#define WR3_RX_ENABLE       0x01

#define WR9_MIE         0x08
#define WR9_STATUS_HIGH 0x10

#define WR15_DCD           0x08
#define WR15_ZERO_COUNT_IE 0x02
#define WR15_TX_UNDERRUN   0x40

// forward declarations
static void reset_ch(scc_t *restrict scc, int ch);
static void update_irqs(scc_t *scc);

// BRG zero-count callback (source=scc, data=channel index)
static void brg_zero_count_callback(void *source, uint64_t data) {
    scc_t *scc = (scc_t *)source;
    ch_t *ch = &scc->ch[data];

    LOG(4, "brg_zero_count: ch=%d counter reached 0", ch->index);

    // Reload counter from time constant
    ch->brg.counter = ch->brg.time_constant;

    // If zero count interrupt is enabled, generate External/Status interrupt
    if ((ch->wr[15] & WR15_ZERO_COUNT_IE) && (ch->wr[1] & WR1_EXT_INT)) {
        LOG(4, "brg_zero_count: generating interrupt (WR15 bit1=1, WR1 bit0=1)");
        // Set External/Status interrupt in RR3
        scc->ch[0].rr[3] |= (ch->index ? RR3_CHANNEL_B_EXT : RR3_CHANNEL_A_EXT);
        update_irqs(scc);
    }

    // Reschedule next BRG event if still enabled
    if (ch->brg.enabled && scc->scheduler) {
        // Schedule next zero-count at time_constant + 1 cycles
        scheduler_new_cpu_event(scc->scheduler, brg_zero_count_callback, scc, (uint64_t)ch->index,
                                (ch->brg.time_constant + 1), 0);
    }
}

// Start or restart the baud rate generator for a channel
static void brg_start(ch_t *ch) {
    if (!ch->scc->scheduler) {
        LOG(2, "brg_start: ch=%d - no scheduler available", ch->index);
        return;
    }

    // Cancel any existing BRG event for this channel (matched by data=index)
    remove_event_by_data(ch->scc->scheduler, brg_zero_count_callback, ch->scc, (uint64_t)ch->index);

    if (!ch->brg.enabled) {
        LOG(4, "brg_start: ch=%d - BRG disabled, not scheduling", ch->index);
        return;
    }

    LOG(4, "brg_start: ch=%d time_constant=0x%04X counter=0x%04X", ch->index, ch->brg.time_constant, ch->brg.counter);

    // Schedule BRG zero-count event (source=scc, data=channel index)
    scheduler_new_cpu_event(ch->scc->scheduler, brg_zero_count_callback, ch->scc, (uint64_t)ch->index,
                            (ch->brg.counter + 1), 0);
}

// Stop the baud rate generator for a channel
static void brg_stop(ch_t *ch) {
    if (!ch->scc->scheduler) {
        return;
    }

    LOG(4, "brg_stop: ch=%d", ch->index);
    remove_event_by_data(ch->scc->scheduler, brg_zero_count_callback, ch->scc, (uint64_t)ch->index);
}

// Update the interrupt request lines based on pending SCC interrupts
static void update_irqs(scc_t *scc) {
    bool should_fire = scc->ch[0].rr[3] && (scc->ch[0].wr[9] & WR9_MIE);
    LOG(4, "update_irqs: rr3=0x%02X wr9=0x%02X MIE=%d -> %s", scc->ch[0].rr[3], scc->ch[0].wr[9],
        !!(scc->ch[0].wr[9] & WR9_MIE), should_fire ? "FIRE" : "clear");
    scc->irq_cb(scc->cb_context, should_fire);
}

// Check for incoming SDLC frames and move them to the receive buffer
void check_rx(ch_t *ch) {
    // ch_t* ch = &scc->ch[c];

    assert(SDLC_MODE(ch));
    assert(RX_ENABLED(ch));

    // if the receiver is not in hunt mode, no data is received
    if (!(ch->rr[0] & 0x10))
        return;

    if (ch->sdlc_in.len == 0)
        return;

    // in address search mode - only report frames that matches (or broadcasts)
    if (ch->wr[3] & WR3_ADDRESS_SEARCH)
        if (ch->sdlc_in.buf[0] != 0xFF && ch->sdlc_in.buf[0] != ch->wr[6])
            return;

    ch->rr[0] &= ~RR0_SYNC_HUNT;
    ch->rr[0] |= RR0_RX_CHAR_AVAILABLE;
    ch->rr[1] &= ~RR1_END_OF_FRAME;

    size_t frame_len = ch->sdlc_in.len;
    uint8_t dest = ch->sdlc_in.buf[0];

    ch->rx.tail = 0;
    ch->rx.head = ch->sdlc_in.len + 2;
    memcpy(ch->rx.buf, ch->sdlc_in.buf, ch->sdlc_in.len);

    ch->sdlc_in.len = 0;

    uint8_t irq_mode = ch->wr[1] >> 3 & 3;

    if (irq_mode == 1 || irq_mode == 2) {

        // exit early if no rx interrupts are enabled in wr1
        if (!(ch->wr[1] & 0x18))
            return;

        ch->scc->ch[0].rr[3] |= ch ? RR3_CHANNEL_B_RX : RR3_CHANNEL_A_RX;

        // only support irq on first char for now
        assert((ch->wr[1] >> 3 & 3) == 1);

        update_irqs(ch->scc);
    }

    LOG(4, "scc:rx deliver len=%zu dest=0x%02X irq_mode=%u rr3=0x%02X pending=%d", frame_len, dest, irq_mode,
        ch->scc->ch[0].rr[3], ch->pending_rx.count);
    log_frame_preview(7, "scc:rx bytes", ch->rx.buf, frame_len);
}

// Reset the receive queue for a channel
static void rx_queue_reset(ch_t *ch) {
    if (!ch)
        return;
    ch->pending_rx.head = 0;
    ch->pending_rx.tail = 0;
    ch->pending_rx.count = 0;
}

// Enqueue a frame into the pending receive queue
static int rx_queue_enqueue(ch_t *ch, const uint8_t *buf, size_t len) {
    if (!ch || !buf || len == 0 || len > SDLC_MAX_FRAME)
        return -1;
    if (ch->pending_rx.count >= RX_PENDING_QUEUE_DEPTH)
        return -1;
    int slot = ch->pending_rx.head;
    memcpy(ch->pending_rx.frames[slot].buf, buf, len);
    ch->pending_rx.frames[slot].len = len;
    ch->pending_rx.head = (slot + 1) % RX_PENDING_QUEUE_DEPTH;
    ch->pending_rx.count++;
    return 0;
}

// Schedule the next pending receive frame if the channel is ready
static void scc_schedule_rx_if_ready(ch_t *ch) {
    if (!ch)
        return;
    if (!RX_ENABLED(ch))
        return;
    if (ch->sdlc_in.len != 0)
        return;
    if (ch->pending_rx.count == 0)
        return;
    // Real SCC re-enters hunt automatically once ready; mirror that here so queued frames flow
    bool hunt_before = (ch->rr[0] & RR0_SYNC_HUNT) != 0;
    if (!hunt_before)
        ch->rr[0] |= RR0_SYNC_HUNT;

    int slot = ch->pending_rx.tail;
    size_t len = ch->pending_rx.frames[slot].len;
    memcpy(ch->sdlc_in.buf, ch->pending_rx.frames[slot].buf, len);
    ch->sdlc_in.len = len;
    ch->pending_rx.tail = (slot + 1) % RX_PENDING_QUEUE_DEPTH;
    ch->pending_rx.count--;
    if (scc_should_log(4)) {
        LOG(4, "scc:rx dispatch len=%zu pending=%d hunt_prev=%d rr0=0x%02X", len, ch->pending_rx.count, hunt_before,
            ch->rr[0]);
    }
    check_rx(ch);
}

// interrupt vector
static uint8_t rr2(scc_t *scc, int ch) {
    // there should only be 1 shared wr9
    assert(scc->ch[0].wr[9] == scc->ch[1].wr[9]);

    // channel a returns unmodified vector; channel b returns modified vector with status
    if (ch == 0) {
        // channel a: return the unmodified interrupt vector from wr2
        return scc->ch[0].wr[2];
    }

    // channel b: vector always includes status
    // just support "low" (status in bit 0-2) for now
    assert(!(scc->ch[0].wr[9] & WR9_STATUS_HIGH));

    if (scc->ch[0].rr[3]) { // if interrups are pending...

        // two highest bits always zero
        assert((scc->ch[0].rr[2] & 0xC0) == 0);

        // [x] table 4-1: interrupt priority
        // same order as the bits in rr3 (msb to lsb)
        int irq = platform_bsr32(scc->ch[0].rr[3]);

        assert(irq >= 0 && irq < 6);

        // [x] table 4-2 or 7-4: status encoded in the vector
        int irq_status[6] = {0x02, 0x00, 0x04, 0x0A, 0x08, 0x0C};

        return irq_status[irq];
    } else
        return 0x05; // if no interrupts pending, v3, v2, v1 = 011
}

static uint8_t rr8(ch_t *ch) {
    // ch_t* ch = &scc->ch[c];

    // clear any pending rx interrupt
    ch->scc->ch[0].rr[3] &= ch->index ? ~RR3_CHANNEL_B_RX : ~RR3_CHANNEL_A_RX;
    update_irqs(ch->scc);

    // if rx fifo is empty - do nothing
    if (RX_EMPTY(ch))
        return 0xFF;

    // Check if local loopback mode is enabled (WR14 bit 4)
    bool loopback_mode = (ch->wr[14] & 0x10) != 0;

    // In loopback mode, we use simple async mode (not SDLC)
    if (loopback_mode) {
        // Simple byte read from circular buffer
        uint8_t value = ch->rx.buf[ch->rx.tail++];

        // If buffer is now empty, clear the RX available flag
        if (RX_EMPTY(ch)) {
            ch->rx.head = ch->rx.tail = 0;
            ch->rr[0] &= ~RR0_RX_CHAR_AVAILABLE;
        }

        LOG(4, "rr8: loopback mode, read byte=0x%02X, remaining=%zu", value, RX_LEN(ch));
        return value;
    }

    // Original SDLC mode handling
    // for now only support sdlc mode
    assert(SDLC_MODE(ch));
    assert(ch->index == 1);

    if (RX_LEN(ch) < 3)
        ch->rr[1] |= RR1_END_OF_FRAME;

    if (RX_LEN(ch) < 2)
        ch->rr[0] &= ~RR0_RX_CHAR_AVAILABLE;

    uint8_t value = ch->rx.buf[ch->rx.tail++];

    if (RX_EMPTY(ch)) {
        ch->rx.head = ch->rx.tail = 0;
        ch->rr[0] |= RR0_SYNC_HUNT;
        if (scc_should_log(4)) {
            LOG(4, "scc:rx fifo drained pending=%d", ch->pending_rx.count);
        }
        scc_schedule_rx_if_ready(ch);
    }

    return value;
}

static void tx_underrun(ch_t *ch) {
    ch->rr[0] |= RR0_TX_UNDERRUN_EOM;

    if (!TX_EMPTY(ch)) {
        LOG(4, "scc:tx underrun len=%d", ch->tx.len);
        log_frame_preview(7, "scc:tx bytes", ch->tx.buf, (size_t)ch->tx.len);
        process_packet(ch->tx.buf, ch->tx.len);
        ch->tx.len = 0;
    }
}

// command register
static void wr0(ch_t *ch, uint8_t value) {
    ch->pointer = value & 7;

    // decode bit 7 and 6
    switch (value >> 6 & 3) {

    case 0: // null code
    case 2: // reset tx crc
        break;

    case 3: // reset tx underrrun
        // The mpp driver will set the tx underrrun/eom latch after every sdlc packet.
        // Instead simulating an underrun a little later, we can simply fake it here and now.
        tx_underrun(ch);
        break;

    default:
        assert(0);
    }

    // decode bit 5 to 3
    switch (value >> 3 & 7) {
    case 0: // null code
        break;

    case 1: // point high
        ch->pointer += 8;
        break;

    case 2: // reset ext/status interrupts
        ch->scc->ch[0].rr[3] &= ch->index ? ~RR3_CHANNEL_B_EXT : ~RR3_CHANNEL_A_EXT;
        update_irqs(ch->scc);
        break;

    case 3: // channel reset
        reset_ch(ch->scc, ch->index);
        break;

    case 4: // enable int on next rx char
        break;

    case 5: // reset tx int pending
        ch->scc->ch[0].rr[3] &= ch->index ? ~RR3_CHANNEL_B_TX : ~RR3_CHANNEL_A_TX;
        update_irqs(ch->scc);
        break;

    case 6: // error reset
        ch->rr[1] &= 0x0F;
        break;

    default:
        assert(0);
    }
}

// receive parameters and control
static void wr3(ch_t *ch, uint8_t value) {
    uint8_t prev_wr3 = ch->wr[3];
    bool enter_hunt_cmd = (value & WR3_ENTER_HUNT_MODE) != 0;
    uint8_t new_wr3 = value & ~WR3_ENTER_HUNT_MODE; // hunt bit is a command, not latched
    uint8_t bits_set = new_wr3 & (new_wr3 ^ prev_wr3);
    uint8_t bits_cleared = (~new_wr3) & (new_wr3 ^ prev_wr3);

    ch->wr[3] = new_wr3;

    if (bits_cleared & WR3_RX_ENABLE) {
        ch->rr[0] &= ~RR0_RX_CHAR_AVAILABLE;

        // clear any pending rx interrupt
        ch->scc->ch[0].rr[3] &= ch->index ? ~RR3_CHANNEL_B_RX : ~RR3_CHANNEL_A_RX;
        update_irqs(ch->scc);
    }

    bool hunt_requested = false;
    if (enter_hunt_cmd && (ch->wr[3] & WR3_RX_ENABLE)) {
        ch->rr[0] |= RR0_SYNC_HUNT;
        hunt_requested = true;
    }

    if (bits_set & WR3_RX_ENABLE) {
        ch->rr[0] |= RR0_SYNC_HUNT;
        ch->rx.head = ch->rx.tail = 0;
        hunt_requested = true;
    }

    if (hunt_requested)
        scc_schedule_rx_if_ready(ch);
}

// transmit buffer
static void wr8(ch_t *c, uint8_t value) {
    // not yet implemented: we assume that the underrun interrupt is not enabled
    assert(!(c->wr[15] & WR15_TX_UNDERRUN));

    c->rr[0] &= ~RR0_TX_UNDERRUN_EOM;

    // clear tx irq
    c->scc->ch[0].rr[3] &= c->index ? ~RR3_CHANNEL_B_TX : ~RR3_CHANNEL_A_TX;
    update_irqs(c->scc);

    // let's simplify - tx buffer immediately empty
    c->rr[0] |= RR0_TX_BUFFER_EMPTY;
    LOG(4, "wr8 ch=%d value=0x%02X, wr1=0x%02X (TX int enable=%d), wr14=0x%02X (loopback=%d)", c->index, value,
        c->wr[1], !!(c->wr[1] & 0x02), c->wr[14], !!(c->wr[14] & 0x10));

    // Check if Local Loopback mode is enabled (WR14 bit 4)
    if (c->wr[14] & 0x10) {
        // Local loopback: transmitted data is immediately received
        LOG(4, "wr8: Local loopback enabled, routing TX to RX");

        // Add byte to RX buffer if there's space
        if (c->rx.head < RX_BUF_SIZE) {
            c->rx.buf[c->rx.head++] = value;

            // Set RX character available flag
            c->rr[0] |= RR0_RX_CHAR_AVAILABLE;

            // Set "All Sent" bit in RR1 (bit 0) - transmission complete in loopback
            c->rr[1] |= 0x01;

            // If RX interrupts are enabled, raise the interrupt
            // Check WR1 bits 4:3 for RX interrupt mode (not 00 = disabled)
            uint8_t rx_int_mode = (c->wr[1] >> 3) & 0x03;
            if (rx_int_mode != 0) {
                c->scc->ch[0].rr[3] |= (c->index ? RR3_CHANNEL_B_RX : RR3_CHANNEL_A_RX);
                LOG(4, "wr8: RX interrupt enabled in loopback, setting rr3=0x%02X", c->scc->ch[0].rr[3]);
                update_irqs(c->scc);
            }
        }
    }

    // if tx interrupts are enabled (wr1 bit 1) and buffer is empty, raise the interrupt
    if (c->wr[1] & 0x02) {
        c->scc->ch[0].rr[3] |= (c->index ? RR3_CHANNEL_B_TX : RR3_CHANNEL_A_TX);
        LOG(4, "wr8: TX interrupt enabled, setting rr3=0x%02X", c->scc->ch[0].rr[3]);
        update_irqs(c->scc);
    }
    int prev_len = c->tx.len;
    assert(c->tx.len < TX_BUF_SIZE);
    c->tx.buf[c->tx.len++] = value;
    if (prev_len == 0)
        LOG(6, "scc:tx start first=0x%02X", value);
}

// master interrupt control
static void wr9(scc_t *scc, uint8_t value) {
    // there should only be 1 shared wr9
    assert(scc->ch[0].wr[9] == scc->ch[1].wr[9]);

    LOG(4, "wr9: value=0x%02X (MIE=%d, reset_cmd=%d)", value, !!(value & WR9_MIE), (value >> 6) & 3);

    // check for hardware reset command (bits 7 & 6)
    uint8_t reset_cmd = (value >> 6) & 3;

    switch (reset_cmd) {
    case 0: // null command
        break;
    case 1: // channel reset b
        reset_ch(scc, 1);
        break;
    case 2: // channel reset a
        reset_ch(scc, 0);
        break;
    case 3: // force hardware reset
        scc_reset(scc);
        break;
    }

    // set wr9 in both channels
    scc->ch[0].wr[9] = scc->ch[1].wr[9] = value;

    // always update irqs after writing wr9, as mie or reset state may have changed
    update_irqs(scc);
}

// read access from cpu bus
static uint8_t read_uint8(void *s, uint32_t addr) {
    scc_t *scc = (scc_t *)s;

    // address pin 1 is connected to A/B (A = 1)
    int ab = addr >> 1 & 1;

    // address pin 2 is connected to D/C (D = 1)
    int dc = addr >> 2 & 1;

    int ch = !ab;
    int reg = dc ? 8 : scc->ch[ch].pointer;

    LOG(4, "scc_read: addr=0x%X ch=%d dc=%d reg=%d", addr, ch, dc, reg);

    scc->ch[ch].pointer = 0;

    assert(reg >= 0 && reg < 16);

    switch (reg) {

    case 0x02:
        return rr2(scc, ch);

    case 0x03:
        // only exist in ch a - always read as 0 in ch b
        return ch ? 0 : scc->ch[ch].rr[3];

    case 0x08:
        return rr8(&scc->ch[ch]);

    case 12: // rr12 returns the value stored in wr12
    case 13: // rr13 returns the value stored in wr13
    case 15: // rr15 returns the value stored in wr15
        return scc->ch[ch].wr[reg];

    default:
        return scc->ch[ch].rr[reg];
    }
}

static uint16_t scc_read_uint16(void *scc, uint32_t addr) {
    // unused;
    (void)scc;
    (void)addr;

    assert(0);

    return 0;
}

static uint32_t scc_read_uint32(void *scc, uint32_t addr) {
    // unused;
    (void)scc;
    (void)addr;

    assert(0);

    return 0;
}

// write access from cpu bus
static void scc_write_uint8(void *s, uint32_t addr, uint8_t value) {
    scc_t *scc = (scc_t *)s;

    // address pin 1 is connected to A/B (A = 1)
    int ab = addr >> 1 & 1;

    // address pin 2 is connected to D/C (D = 1)
    int dc = addr >> 2 & 1;

    int ch = !ab;
    int reg = dc ? 8 : scc->ch[ch].pointer;

    LOG(4, "scc_write: addr=0x%X ch=%d dc=%d reg=%d value=0x%02X", addr, ch, dc, reg, value);

    scc->ch[ch].pointer = 0;

    assert(reg >= 0 && reg < 16);

    switch (reg) {
    case 0x00:
        wr0(&scc->ch[ch], value);
        break;

    case 0x01:
        // write register 1 - interrupt enables
        LOG(4, "wr1 ch=%d: value=0x%02X (TX int enable=%d, RX int=%d)", ch, value, !!(value & 0x02), (value >> 3) & 3);
        scc->ch[ch].wr[1] = value;
        // if tx int enabled (bit 1) and buffer empty, fire interrupt immediately
        if ((value & 0x02) && (scc->ch[ch].rr[0] & RR0_TX_BUFFER_EMPTY)) {
            scc->ch[0].rr[3] |= (ch ? RR3_CHANNEL_B_TX : RR3_CHANNEL_A_TX);
            LOG(4, "wr1: TX int enabled and buffer empty, setting rr3=0x%02X", scc->ch[0].rr[3]);
            update_irqs(scc);
        }
        break;

    case 0x02:
        scc->ch[0].wr[2] = scc->ch[1].wr[2] = value;
        scc->ch[0].rr[2] = scc->ch[1].rr[2] = value;
        break;

    case 0x03:
        wr3(&scc->ch[ch], value);
        break;

    case 0x08:
        wr8(&scc->ch[ch], value);
        break;

    case 0x09:
        wr9(scc, value);
        break;

    case 0x0C:
        // WR12 - BRG time constant low byte
        scc->ch[ch].wr[12] = value;
        scc->ch[ch].brg.time_constant = (scc->ch[ch].wr[13] << 8) | value;
        LOG(4, "wr12 ch=%d: time_constant=0x%04X", ch, scc->ch[ch].brg.time_constant);
        // Reload counter if BRG is enabled
        if (scc->ch[ch].brg.enabled) {
            scc->ch[ch].brg.counter = scc->ch[ch].brg.time_constant;
            brg_start(&scc->ch[ch]);
        }
        break;

    case 0x0D:
        // WR13 - BRG time constant high byte
        scc->ch[ch].wr[13] = value;
        scc->ch[ch].brg.time_constant = (value << 8) | scc->ch[ch].wr[12];
        LOG(4, "wr13 ch=%d: time_constant=0x%04X", ch, scc->ch[ch].brg.time_constant);
        // Reload counter if BRG is enabled
        if (scc->ch[ch].brg.enabled) {
            scc->ch[ch].brg.counter = scc->ch[ch].brg.time_constant;
            brg_start(&scc->ch[ch]);
        }
        break;

    case 0x0E:
        // WR14 - Miscellaneous control bits
        scc->ch[ch].wr[14] = value;

        // Handle Local Loopback mode (bit 4)
        // When enabled, TX output is routed to RX input
        bool loopback = !!(value & 0x10);

        // Handle BRG configuration
        scc->ch[ch].brg.pclk_source = !!(value & 0x02);
        bool prev_enabled = scc->ch[ch].brg.enabled;
        scc->ch[ch].brg.enabled = !!(value & 0x01);
        LOG(4, "wr14 ch=%d: value=0x%02X (BRG enable=%d, source=%s, loopback=%d)", ch, value, scc->ch[ch].brg.enabled,
            scc->ch[ch].brg.pclk_source ? "PCLK" : "RTxC", loopback);

        // Start or stop BRG based on enable bit
        if (scc->ch[ch].brg.enabled && !prev_enabled) {
            // BRG was just enabled - reload counter and start
            scc->ch[ch].brg.counter = scc->ch[ch].brg.time_constant;
            brg_start(&scc->ch[ch]);
        } else if (!scc->ch[ch].brg.enabled && prev_enabled) {
            // BRG was just disabled
            brg_stop(&scc->ch[ch]);
        }
        break;

    default:
        scc->ch[ch].wr[reg] = value;
        break;
    }
}

static void scc_write_uint16(void *scc, uint32_t addr, uint16_t value) {
    // unused;
    (void)scc;
    (void)addr;
    (void)value;

    assert(0);
}

static void scc_write_uint32(void *scc, uint32_t addr, uint32_t value) {
    // unused;
    (void)scc;
    (void)addr;
    (void)value;

    assert(0);
}

int scc_sdlc_send(scc_t *restrict scc, uint8_t *buf, size_t len) {
    ch_t *ch = &scc->ch[1];

    assert(SDLC_MODE(ch));

    assert(len >= 3);
    assert(len <= SDLC_MAX_FRAME);

    if (rx_queue_enqueue(ch, buf, len) != 0) {
        LOG(1, "scc:rx queue full dropping len=%zu", len);
        return -1;
    }

    if (scc_should_log(4)) {
        LOG(4, "scc:rx queue+ len=%zu dest=0x%02X pending=%d inflight=%zu hunt=%d", len, buf[0], ch->pending_rx.count,
            ch->sdlc_in.len, !!(ch->rr[0] & RR0_SYNC_HUNT));
    }
    log_frame_preview(7, "scc:rx enqueue", buf, len);

    scc_schedule_rx_if_ready(ch);

    return 0;
}

void scc_dcd(scc_t *restrict scc, unsigned int ch, unsigned int dcd) {
    assert(ch < 2 && dcd < 2);

    dcd = !dcd; // dcd is acitive low - not sure if the bit in rr0 should reflect this or not

    if (dcd == (scc->ch[ch].rr[0] >> 3 & 1))
        return;

    scc->ch[ch].rr[0] ^= RR0_DCD;
    LOG(11, "scc:dcd ch=%u new=%u rr0=0x%02X wr15=0x%02X", ch, dcd, scc->ch[ch].rr[0], scc->ch[ch].wr[15]);

    if ((scc->ch[ch].wr[15] & WR15_DCD) && (scc->ch[ch].wr[1] & WR1_EXT_INT)) {

        scc->ch[0].rr[3] |= ch ? RR3_CHANNEL_B_EXT : RR3_CHANNEL_A_EXT;

        update_irqs(scc);
    }
}

static void reset_ch(scc_t *restrict scc, int ch) {
    // save registers that must survive a channel reset
    // wr9: master interrupt enable (global)
    // wr2: interrupt vector (shared/global logic)
    uint8_t save_wr9 = scc->ch[ch].wr[9];
    uint8_t save_wr2 = scc->ch[ch].wr[2];

    // clear the channel structure
    memset(&scc->ch[ch], 0, sizeof(ch_t));

    // restore the pointers and index
    scc->ch[ch].index = ch;
    scc->ch[ch].scc = scc;

    // restore the preserved registers
    scc->ch[ch].wr[9] = save_wr9;
    scc->ch[ch].wr[2] = save_wr2;

    // initialize BRG state
    scc->ch[ch].brg.time_constant = 0;
    scc->ch[ch].brg.counter = 0;
    scc->ch[ch].brg.enabled = false;
    scc->ch[ch].brg.pclk_source = false;

    rx_queue_reset(&scc->ch[ch]);

    // the transmit buffer is empty upon reset
    scc->ch[ch].rr[0] |= RR0_TX_BUFFER_EMPTY;
}

void scc_reset(scc_t *restrict scc) {
    reset_ch(scc, 0);
    reset_ch(scc, 1);

    update_irqs(scc);
}

scc_t *scc_init(memory_map_t *map, struct scheduler *scheduler, scc_irq_fn irq_cb, void *cb_context,
                checkpoint_t *checkpoint) {
    scc_t *scc = (scc_t *)malloc(sizeof(scc_t));

    if (scc == NULL)
        return NULL;

    memset(scc, 0, sizeof(scc_t));

    scc->scheduler = scheduler;
    scc->irq_cb = irq_cb;
    scc->cb_context = cb_context;

    scc->memory_interface.read_uint8 = &read_uint8;
    scc->memory_interface.read_uint16 = &scc_read_uint16;
    scc->memory_interface.read_uint32 = &scc_read_uint32;

    scc->memory_interface.write_uint8 = &scc_write_uint8;
    scc->memory_interface.write_uint16 = &scc_write_uint16;
    scc->memory_interface.write_uint32 = &scc_write_uint32;

    // Register with memory map if provided (NULL = machine handles registration)
    if (map)
        memory_map_add(map, 0x00800000, 0x00400000, "SCC", &scc->memory_interface, scc);

    scc_reset(scc);

    // Register BRG event type for checkpoint save/restore
    if (scheduler) {
        scheduler_new_event_type(scheduler, "scc", scc, "brg", &brg_zero_count_callback);
    }

    // If a checkpoint is provided, restore channel plain-data (everything up to the
    // embedded scc pointer inside ch_t). The ch_t.scc pointer will be re-linked below.
    if (checkpoint) {
        size_t ch_data_size = offsetof(ch_t, scc);
        // Read contiguous plain-data for both channels
        system_read_checkpoint_data(checkpoint, scc->ch, ch_data_size * 2);

        // Re-link channel back-pointers and ensure index is correct
        for (int i = 0; i < 2; i++) {
            scc->ch[i].scc = scc;
            scc->ch[i].index = i;
        }

        // Update IRQ state after restoring registers
        update_irqs(scc);
    }

    return scc;
}

// Return the SCC memory-mapped I/O interface for machine-level address decode
const memory_interface_t *scc_get_memory_interface(scc_t *scc) {
    return &scc->memory_interface;
}

void scc_delete(scc_t *scc) {
    if (!scc)
        return;
    free(scc);
}

void scc_checkpoint(scc_t *restrict scc, checkpoint_t *checkpoint) {
    if (!scc || !checkpoint)
        return;

    // Write contiguous plain-data portion of each channel up to the embedded scc pointer.
    size_t ch_data_size = offsetof(ch_t, scc);
    system_write_checkpoint_data(checkpoint, scc->ch, ch_data_size * 2);

    // Note: we intentionally do not save the scc back-pointer, nor the memory_interface
    // function pointers or the mapping pointer. Those are runtime-specific and re-initialized
    // on startup.
}
