// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// SCC behaviour a device plugged into a port relies on
// (docs/core/peripherals/scc.md, "Interrupt request" and "Devices on a
// port"), each pinned because the Lisa OS RS-232 driver broke without it:
//
// - input pins a device drives (scc_set_input_pin) show in RR0 the way the
//   chip reports them -- SYNC only in asynchronous mode -- and survive a
//   channel reset, because they are the cable's, not the chip's;
// - the transmit byte hook (scc_set_tx_byte_sink) sees asynchronous bytes;
// - the WR1 enables gate what reaches the INT pin: a pending bit whose
//   enable is off stays in RR3 but requests nothing;
// - Reset Tx Int Pending holds the Tx interrupt off until another character
//   leaves the buffer, while a freshly reset channel still interrupts once
//   when Tx interrupts are enabled (how the Mac drivers prime output).
//
// Driven through the memory interface, as the machine glue does.

#include "scc.h"

#include "scheduler.h"

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// Link stubs
// ============================================================

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)tag;
    (void)cp, (void)data, (void)size, (void)file, (void)line;
}
void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)tag;
    (void)cp, (void)data, (void)size, (void)file, (void)line;
}
void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                    void *device) {
    (void)mem, (void)addr, (void)size, (void)name, (void)iface, (void)device;
}
void memory_map_remove(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                       void *device) {
    (void)mem, (void)addr, (void)size, (void)name, (void)iface, (void)device;
}
void scheduler_new_event_type(struct scheduler *s, const char *sn, void *src, const char *en, event_callback_t cb) {
    (void)s, (void)sn, (void)src, (void)en, (void)cb;
}
event_t *scheduler_new_cpu_event_ex(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data,
                                    uint64_t cycles, uint64_t ns, bool periodic) {
    (void)periodic;
    (void)s, (void)cb, (void)src, (void)data, (void)cycles, (void)ns;
    return NULL;
}
void remove_event(struct scheduler *restrict s, event_callback_t cb, void *src) {
    (void)s, (void)cb, (void)src;
}
bool has_event(struct scheduler *restrict s, event_callback_t cb) {
    (void)s, (void)cb;
    return false;
}
void scheduler_forget_source(struct scheduler *s, void *source) {
    (void)s, (void)source;
}
uint64_t scheduler_cpu_cycles(struct scheduler *restrict s) {
    (void)s;
    return 0;
}
double scheduler_time_ns(struct scheduler *restrict s) {
    (void)s;
    return 0.0;
}
void remove_event_by_data(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data) {
    (void)s, (void)cb, (void)src, (void)data;
}

// ============================================================
// Helpers
// ============================================================

static bool g_irq; // the INT pin as the SCC last drove it

static void irq_sink(void *ctx, bool active) {
    (void)ctx;
    g_irq = active;
}

#define CH_B_CTL  0 // Mac decode: +0 bCtl, +2 aCtl, +4 bData, +6 aData
#define CH_A_CTL  2
#define CH_A_DATA 6

#define RR0_SYNC 0x10
#define RR0_CTS  0x20
#define RR3_A_TX 0x10

static void wr(scc_t *scc, uint32_t off, uint8_t reg, uint8_t value) {
    const memory_interface_t *mi = scc_get_memory_interface(scc);
    mi->write_uint8(scc, off, reg); // point at the register
    mi->write_uint8(scc, off, value); // then write it
}

static uint8_t rd(scc_t *scc, uint32_t off, uint8_t reg) {
    const memory_interface_t *mi = scc_get_memory_interface(scc);
    mi->write_uint8(scc, off, reg); // point at the register
    return (uint8_t)mi->read_uint8(scc, off);
}

// Writes one byte to channel A's transmit buffer (WR8, the data port).
static void send_a(scc_t *scc, uint8_t byte) {
    scc_get_memory_interface(scc)->write_uint8(scc, CH_A_DATA, byte);
}

// A chip with interrupts on (WR9 MIE) and channel A in asynchronous mode
// (WR4 $44: x16 clock, one stop bit), as the Lisa driver programs it.
static scc_t *make_async(void) {
    g_irq = false;
    scc_t *scc = scc_init(NULL, NULL, irq_sink, NULL, NULL);
    ASSERT_TRUE(scc != NULL);
    wr(scc, CH_A_CTL, 9, 0x08); // MIE
    wr(scc, CH_A_CTL, 4, 0x44);
    return scc;
}

// Records what the transmit hook saw.
static uint8_t g_sent[16];
static int g_sent_len;

static void tx_sink(void *ctx, uint8_t byte) {
    (void)ctx;
    if (g_sent_len < (int)sizeof(g_sent))
        g_sent[g_sent_len++] = byte;
}

// ============================================================
// Tests
// ============================================================

// A driven SYNC input reads as RR0 bit 4 in asynchronous mode, and a
// channel reset (WR9 $8A, what the Lisa driver issues on open) keeps it:
// once the guest selects asynchronous mode again, the level is back.
TEST(test_sync_pin_survives_channel_reset) {
    scc_t *scc = make_async();
    scc_set_input_pin(scc, 0, SCC_PIN_SYNC, true);
    ASSERT_TRUE(rd(scc, CH_A_CTL, 0) & RR0_SYNC);
    wr(scc, CH_A_CTL, 9, 0x8A); // channel A reset (MIE kept on, no vector)
    wr(scc, CH_A_CTL, 4, 0x44);
    ASSERT_TRUE(rd(scc, CH_A_CTL, 0) & RR0_SYNC);
    scc_set_input_pin(scc, 0, SCC_PIN_SYNC, false);
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 0) & RR0_SYNC));
    scc_delete(scc);
}

// Outside asynchronous mode RR0 bit 4 is the receiver's hunt state, not the
// pin: a driven SYNC must not show there in SDLC mode.
TEST(test_sync_pin_not_reported_in_sdlc) {
    scc_t *scc = make_async();
    scc_set_input_pin(scc, 0, SCC_PIN_SYNC, true);
    wr(scc, CH_A_CTL, 4, 0x20); // SDLC
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 0) & RR0_SYNC));
    scc_delete(scc);
}

// CTS is a plain modem input: reported in RR0 bit 5 in any mode.
TEST(test_cts_pin_reported) {
    scc_t *scc = make_async();
    scc_set_input_pin(scc, 0, SCC_PIN_CTS, true);
    ASSERT_TRUE(rd(scc, CH_A_CTL, 0) & RR0_CTS);
    scc_set_input_pin(scc, 0, SCC_PIN_CTS, false);
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 0) & RR0_CTS));
    scc_delete(scc);
}

// A driven input changing raises the External/Status interrupt when the
// guest enabled it for that input (WR15) and Ext interrupts (WR1 bit 0).
TEST(test_pin_change_raises_ext_interrupt) {
    scc_t *scc = make_async();
    wr(scc, CH_A_CTL, 15, 0x10); // SYNC/HUNT IE
    wr(scc, CH_A_CTL, 1, 0x01); // Ext int enable
    ASSERT_TRUE(!(g_irq));
    scc_set_input_pin(scc, 0, SCC_PIN_SYNC, true);
    ASSERT_TRUE(g_irq);
    scc_delete(scc);
}

// The transmit hook receives asynchronous bytes, in order.
TEST(test_tx_byte_sink_sees_async_bytes) {
    scc_t *scc = make_async();
    g_sent_len = 0;
    scc_set_tx_byte_sink(scc, 0, tx_sink, NULL);
    send_a(scc, 0x18);
    send_a(scc, 0x1B);
    ASSERT_EQ_INT(2, g_sent_len);
    ASSERT_EQ_INT(0x18, g_sent[0]);
    ASSERT_EQ_INT(0x1B, g_sent[1]);
    scc_set_tx_byte_sink(scc, 0, NULL, NULL);
    send_a(scc, 0x41);
    ASSERT_EQ_INT(2, g_sent_len); // detached
    scc_delete(scc);
}

// A pending Tx interrupt whose enable is turned off stays in RR3 but no
// longer drives INT; turning the enable back on brings the request back.
TEST(test_wr1_enables_gate_int) {
    scc_t *scc = make_async();
    wr(scc, CH_A_CTL, 1, 0x02); // Tx IE on an empty, freshly reset channel
    ASSERT_TRUE(g_irq);
    ASSERT_TRUE(rd(scc, CH_A_CTL, 3) & RR3_A_TX);
    wr(scc, CH_A_CTL, 1, 0x00); // "no more ints this port"
    ASSERT_TRUE(!(g_irq));
    ASSERT_TRUE(rd(scc, CH_A_CTL, 3) & RR3_A_TX); // still pending
    wr(scc, CH_A_CTL, 1, 0x02);
    ASSERT_TRUE(g_irq);
    scc_delete(scc);
}

// Reset Tx Int Pending (WR0 $28) clears the Tx interrupt, and re-enabling
// Tx interrupts with nothing sent does not raise it again; writing a
// character (which leaves the buffer at once) does.
TEST(test_reset_tx_pending_holds_until_next_char) {
    scc_t *scc = make_async();
    wr(scc, CH_A_CTL, 1, 0x02);
    ASSERT_TRUE(g_irq);
    scc_get_memory_interface(scc)->write_uint8(scc, CH_A_CTL, 0x28); // Reset Tx Int Pending
    ASSERT_TRUE(!(g_irq));
    wr(scc, CH_A_CTL, 1, 0x00);
    wr(scc, CH_A_CTL, 1, 0x02);
    ASSERT_TRUE(!(g_irq));
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 3) & RR3_A_TX));
    send_a(scc, 0x41);
    ASSERT_TRUE(g_irq);
    scc_delete(scc);
}

int main(void) {
    RUN(test_sync_pin_survives_channel_reset);
    RUN(test_sync_pin_not_reported_in_sdlc);
    RUN(test_cts_pin_reported);
    RUN(test_pin_change_raises_ext_interrupt);
    RUN(test_tx_byte_sink_sees_async_bytes);
    RUN(test_wr1_enables_gate_int);
    RUN(test_reset_tx_pending_holds_until_next_char);
    printf("[PASS] All scc_port tests passed\n");
    return 0;
}
