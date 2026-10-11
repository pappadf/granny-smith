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
// - a channel's output file (scc_set_output) receives its asynchronous
//   bytes, and the port's wired ready line (scc_set_port_ready_line)
//   follows whether an output is attached;
// - the WR1 enables gate what reaches the INT pin: a pending bit whose
//   enable is off stays in RR3 but requests nothing;
// - Reset Tx Int Pending holds the Tx interrupt off until another character
//   leaves the buffer, while a freshly reset channel still interrupts once
//   when Tx interrupts are enabled (how the Mac drivers prime output).
//
// Driven through the memory interface, as the machine glue does.

#include "checkpoint.h"
#include "scc.h"

#include "scheduler.h"

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
void memory_map_remove(memory_map_t *mem, uint32_t addr, void *device) {
    (void)mem, (void)addr, (void)device;
}
void scheduler_new_event_type(struct scheduler *s, const char *sn, void *src, const char *en, event_callback_t cb) {
    (void)s, (void)sn, (void)src, (void)en, (void)cb;
}
// The last event scheduled on a (fake) scheduler, so a test can fire it.
static struct {
    int count;
    event_callback_t cb;
    void *src;
    uint64_t data, ns;
} g_ev;
event_t *scheduler_new_cpu_event_ex(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data,
                                    uint64_t cycles, uint64_t ns, bool periodic) {
    (void)periodic;
    (void)s, (void)cycles;
    g_ev.count++;
    g_ev.cb = cb;
    g_ev.src = src;
    g_ev.data = data;
    g_ev.ns = ns;
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

// A scratch path for an output file, unique per run.
static void scratch_path(char *buf, size_t n) {
    snprintf(buf, n, "/tmp/scc_port_%d.bin", (int)getpid());
}

// Reads back up to `cap` bytes of `path`; the count read.
static size_t read_back(const char *path, uint8_t *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
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

// The output file receives the asynchronous bytes, in order, as they are
// written (a reader need not wait for the file to close); `none` closes it
// and later bytes go nowhere.
TEST(test_output_file_receives_async_bytes) {
    char path[64];
    scratch_path(path, sizeof(path));
    scc_t *scc = make_async();
    ASSERT_TRUE(scc_get_output(scc, 0) == NULL);
    ASSERT_TRUE(scc_set_output(scc, 0, path));
    ASSERT_TRUE(strcmp(scc_get_output(scc, 0), path) == 0);
    send_a(scc, 0x18);
    send_a(scc, 0x1B);
    uint8_t got[8];
    ASSERT_EQ_INT(2, (int)read_back(path, got, sizeof(got)));
    ASSERT_EQ_INT(0x18, got[0]);
    ASSERT_EQ_INT(0x1B, got[1]);
    ASSERT_TRUE(scc_set_output(scc, 0, NULL));
    ASSERT_TRUE(scc_get_output(scc, 0) == NULL);
    send_a(scc, 0x41);
    ASSERT_EQ_INT(2, (int)read_back(path, got, sizeof(got)));
    scc_delete(scc);
    unlink(path);
}

// A path that cannot be opened is refused and changes nothing.
TEST(test_output_bad_path_refused) {
    scc_t *scc = make_async();
    ASSERT_TRUE(!scc_set_output(scc, 0, "/nonexistent-dir/x/y.bin"));
    ASSERT_TRUE(scc_get_output(scc, 0) == NULL);
    scc_delete(scc);
}

// The wired ready line is "not ready" with no output and "ready" while one
// is open -- here the Lisa's port A wiring: SYNC, asserted when ready.
TEST(test_ready_line_follows_output) {
    char path[64];
    scratch_path(path, sizeof(path));
    scc_t *scc = make_async();
    scc_set_port_ready_line(scc, 0, SCC_PIN_SYNC, true);
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 0) & RR0_SYNC));
    ASSERT_TRUE(scc_set_output(scc, 0, path));
    ASSERT_TRUE(rd(scc, CH_A_CTL, 0) & RR0_SYNC);
    ASSERT_TRUE(scc_set_output(scc, 0, NULL));
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 0) & RR0_SYNC));
    scc_delete(scc);
    unlink(path);
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

// A device plugged into a port hears the channel's asynchronous bytes --
// beside an output file, if both are attached -- and nothing in SDLC mode.
static uint8_t g_dev_bytes[16];
static int g_dev_n;
static void dev_tx(void *ctx, uint8_t byte) {
    (void)ctx;
    if (g_dev_n < (int)sizeof(g_dev_bytes))
        g_dev_bytes[g_dev_n++] = byte;
}
static const scc_port_device_t test_device = {.name = "testdev", .tx_byte = dev_tx};

TEST(test_device_hears_async_bytes) {
    scc_t *scc = make_async();
    g_dev_n = 0;
    scc_attach_port_device(scc, 0, &test_device, NULL);
    ASSERT_TRUE(scc_port_device(scc, 0) == &test_device);
    send_a(scc, 0x1B);
    send_a(scc, 0x45);
    ASSERT_EQ_INT(g_dev_n, 2);
    ASSERT_EQ_INT(g_dev_bytes[1], 0x45);
    wr(scc, CH_A_CTL, 4, 0x20); // SDLC: not for a device
    send_a(scc, 0x7E);
    ASSERT_EQ_INT(g_dev_n, 2);
    scc_attach_port_device(scc, 0, NULL, NULL);
    ASSERT_TRUE(scc_port_device(scc, 0) == NULL);
    scc_delete(scc);
}

// The wired ready line follows the device's ready state; unwiring leaves the
// input undriven and not asserted.
TEST(test_ready_line_follows_device) {
    scc_t *scc = make_async();
    scc_set_port_ready_line(scc, 0, SCC_PIN_SYNC, true);
    scc_attach_port_device(scc, 0, &test_device, NULL);
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 0) & RR0_SYNC)); // a device starts not ready
    scc_port_device_ready(scc, 0, true);
    ASSERT_TRUE(rd(scc, CH_A_CTL, 0) & RR0_SYNC);
    scc_port_device_ready(scc, 0, false);
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 0) & RR0_SYNC));
    // A Mac's wiring: CTS, not asserted when ready
    scc_set_port_ready_line(scc, 0, SCC_PIN_CTS, false);
    ASSERT_TRUE(rd(scc, CH_A_CTL, 0) & RR0_CTS);
    scc_port_device_ready(scc, 0, true);
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 0) & RR0_CTS));
    scc_port_device_ready(scc, 0, false);
    scc_unwire_port_ready_line(scc, 0);
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 0) & RR0_CTS));
    scc_delete(scc);
}

// Host text (`receive`) goes onto the line one character time apart: it
// waits host-side, and each character time moves one byte into the FIFO and
// schedules the next.  Clocks unknown, so the character time is 9600 baud's.
TEST(test_line_send_is_paced) {
    static int fake_scheduler;
    g_irq = false;
    memset(&g_ev, 0, sizeof g_ev);
    scc_t *scc = scc_init(NULL, (struct scheduler *)&fake_scheduler, irq_sink, NULL, NULL);
    ASSERT_TRUE(scc != NULL);
    wr(scc, CH_A_CTL, 9, 0x08); // MIE
    wr(scc, CH_A_CTL, 4, 0x44);
    int before = g_ev.count;

    ASSERT_EQ_INT((int)scc_line_send(scc, 0, (const uint8_t *)"AB", 2), 2);
    ASSERT_EQ_INT(scc_channel_rx_pending(scc, 0), 0); // nothing in the FIFO yet
    ASSERT_EQ_INT(scc_channel_line_in_pending(scc, 0), 2);
    ASSERT_EQ_INT(g_ev.count, before + 1); // one character time scheduled
    ASSERT_TRUE(g_ev.ns == 1041667);
    ASSERT_TRUE(!(rd(scc, CH_A_CTL, 0) & 0x01));

    g_ev.cb(g_ev.src, g_ev.data); // one character time later
    ASSERT_EQ_INT(scc_channel_rx_pending(scc, 0), 1);
    ASSERT_EQ_INT(scc_channel_line_in_pending(scc, 0), 1);
    ASSERT_TRUE(rd(scc, CH_A_CTL, 0) & 0x01);
    ASSERT_EQ_INT(g_ev.count, before + 2); // and the next one is due

    // Sending more while the line is busy does not restart the pacing
    ASSERT_EQ_INT((int)scc_line_send(scc, 0, (const uint8_t *)"C", 1), 1);
    ASSERT_EQ_INT(g_ev.count, before + 2);

    g_ev.cb(g_ev.src, g_ev.data);
    g_ev.cb(g_ev.src, g_ev.data);
    ASSERT_EQ_INT(scc_channel_rx_pending(scc, 0), 3);
    ASSERT_EQ_INT(scc_channel_line_in_pending(scc, 0), 0);
    ASSERT_EQ_INT(g_ev.count, before + 3); // nothing left: no further event
    ASSERT_EQ_INT(rd(scc, CH_A_CTL, 8), 'A');
    ASSERT_EQ_INT(rd(scc, CH_A_CTL, 8), 'B');
    ASSERT_EQ_INT(rd(scc, CH_A_CTL, 8), 'C');
    scc_delete(scc);
}

// A device's reply arrives in the receive FIFO with Rx Character Available.
TEST(test_device_rx_byte) {
    scc_t *scc = make_async();
    ASSERT_TRUE(scc_port_rx_byte(scc, 0, 'I'));
    ASSERT_TRUE(scc_port_rx_byte(scc, 0, 'W'));
    ASSERT_TRUE(rd(scc, CH_A_CTL, 0) & 0x01);
    ASSERT_EQ_INT(scc_channel_rx_pending(scc, 0), 2);
    scc_delete(scc);
}

int main(void) {
    RUN(test_sync_pin_survives_channel_reset);
    RUN(test_sync_pin_not_reported_in_sdlc);
    RUN(test_cts_pin_reported);
    RUN(test_pin_change_raises_ext_interrupt);
    RUN(test_output_file_receives_async_bytes);
    RUN(test_output_bad_path_refused);
    RUN(test_ready_line_follows_output);
    RUN(test_wr1_enables_gate_int);
    RUN(test_reset_tx_pending_holds_until_next_char);
    RUN(test_device_hears_async_bytes);
    RUN(test_ready_line_follows_device);
    RUN(test_device_rx_byte);
    RUN(test_line_send_is_paced);
    printf("[PASS] All scc_port tests passed\n");
    return 0;
}
