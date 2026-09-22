// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// SCC checkpoint round-trip.
//
// `scc_checkpoint` wrote both channels as ONE block of
// `offsetof(ch_t, scc) * 2` bytes starting at ch[0]. That assumes the two
// channels are packed at the PREFIX size. They are not: the stride is
// sizeof(ch_t), eight bytes larger because of the `scc` back-pointer each
// channel carries. Measured on this tree: prefix 11424, sizeof 11432.
//
// So the block ran eight bytes past the end of ch[0]'s prefix and straight
// over ch[0]'s `scc` pointer -- a host heap address, written into every save
// file, which is the thing 05-chipsets-irq F-09 swept for and missed here --
// and then stopped eight bytes short of the end of ch[1]'s, silently
// dropping channel B's `brg` (baud rate generator: time constant, counter,
// enable, clock source), `loopback_prev_dtr` and `rx_special`.
//
// Channel B is the LocalTalk channel on a Mac, and `rx_special` is the SDLC
// end-of-frame latch the PDM native LocalTalk driver waits on.
//
// The test below is save -> restore -> save, comparing the two streams. It
// fails on the pointer half directly: the restored instance re-binds its own
// back-pointer, so with the bug present the second stream carries a
// different heap address at that offset and the comparison breaks. The
// ordering matters -- an assertion that merely round-trips one instance
// would pass, because a save that drops the same eight bytes both times is
// self-consistent.
//
// (The neighbouring scsi_checkpoint suite's header says its fix makes SCSI
// save "the way via_t, scc_t and rtc_t do". scc_t was the one doing it
// wrong.)

#include "scc.h"

#include "scheduler.h"

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// Recording checkpoint stream (the scsi_checkpoint pattern)
// ============================================================

static uint8_t s_buf[2][65536];
static size_t s_w[2], s_r;
static int s_slot; // which buffer a save writes into

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)tag;
    (void)cp, (void)file, (void)line;
    ASSERT_TRUE(s_w[s_slot] + size <= sizeof(s_buf[0]));
    memcpy(s_buf[s_slot] + s_w[s_slot], data, size);
    s_w[s_slot] += size;
}

void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)tag;
    (void)cp, (void)file, (void)line;
    ASSERT_TRUE(s_r + size <= s_w[0]);
    memcpy(data, s_buf[0] + s_r, size);
    s_r += size;
}

// ============================================================
// Link stubs
// ============================================================

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
void appletalk_scc_notify(void *ctx, unsigned int ch) {
    (void)ctx, (void)ch;
}
void remove_event_by_data(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data) {
    (void)s, (void)cb, (void)src, (void)data;
}
int platform_bsr32(uint32_t v) {
    int n = 31;
    while (n >= 0 && !(v & (1u << n)))
        n--;
    return n;
}
// The AppleTalk stack's inbound hook; this suite drives no frames.
void process_packet(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx, (void)buf, (void)len;
}

// ============================================================
// Helpers
// ============================================================

// scc.c calls irq_cb unconditionally (scc.c:319) -- every machine supplies
// one, so the suite does too rather than making the product NULL-tolerant
// for a test's convenience.
static int s_irq_calls;
static void irq_sink(void *ctx, bool active) {
    (void)ctx, (void)active;
    s_irq_calls++;
}

#define CH_B_CTL 0 // Mac decode: +0 bCtl, +2 aCtl, +4 bData, +6 aData
#define CH_A_CTL 2

static void wr(scc_t *scc, uint32_t off, uint8_t reg, uint8_t value) {
    const memory_interface_t *mi = scc_get_memory_interface(scc);
    mi->write_uint8(scc, off, reg); // point at the register
    mi->write_uint8(scc, off, value); // then write it
}

// Give a channel a distinctive, fully-populated BRG: a time constant in both
// halves of WR12/WR13, and WR14 bit 0 to enable it.
static void stage_brg(scc_t *scc, uint32_t ctl, uint16_t tc) {
    wr(scc, ctl, 12, (uint8_t)(tc & 0xFF));
    wr(scc, ctl, 13, (uint8_t)(tc >> 8));
    wr(scc, ctl, 14, 0x01);
}

// ============================================================
// Tests
// ============================================================

// Save -> restore -> save must produce byte-identical streams. It is the
// property the whole checkpoint format depends on, and the one the stride
// bug broke: a host pointer rode in the stream, so the second save carried a
// different address.
TEST(test_save_restore_save_is_byte_identical) {
    s_w[0] = s_w[1] = s_r = 0;

    s_slot = 0;
    scc_t *a = scc_init(NULL, NULL, irq_sink, NULL, NULL);
    ASSERT_TRUE(a != NULL);
    scc_set_clocks(a, 7833600, 3686400);
    stage_brg(a, CH_B_CTL, 0x1234); // channel B, the LocalTalk one
    stage_brg(a, CH_A_CTL, 0x5678);
    scc_checkpoint(a, (checkpoint_t *)1);

    // Restore into a second instance, then save that one.
    s_r = 0;
    s_slot = 1;
    scc_t *b = scc_init(NULL, NULL, irq_sink, NULL, (checkpoint_t *)1);
    ASSERT_TRUE(b != NULL);
    scc_checkpoint(b, (checkpoint_t *)1);

    ASSERT_EQ_INT((int)s_w[0], (int)s_w[1]);
    if (memcmp(s_buf[0], s_buf[1], s_w[0]) != 0) {
        size_t i = 0;
        while (i < s_w[0] && s_buf[0][i] == s_buf[1][i])
            i++;
        fprintf(stderr, "[FAIL] streams diverge at byte %zu of %zu ($%02X vs $%02X)\n", i, s_w[0], s_buf[0][i],
                s_buf[1][i]);
        exit(1);
    }

    scc_delete(a);
    scc_delete(b);
}

// Both channels' register files must survive, not just channel A's -- the
// stride bug truncated the SECOND channel. WR12/WR13 read back through
// RR12/RR13 on an 8530.
TEST(test_both_channels_registers_survive) {
    s_w[0] = s_w[1] = s_r = 0;

    s_slot = 0;
    scc_t *a = scc_init(NULL, NULL, irq_sink, NULL, NULL);
    ASSERT_TRUE(a != NULL);
    stage_brg(a, CH_B_CTL, 0x1234);
    stage_brg(a, CH_A_CTL, 0x5678);
    scc_checkpoint(a, (checkpoint_t *)1);

    s_r = 0;
    s_slot = 1;
    scc_t *b = scc_init(NULL, NULL, irq_sink, NULL, (checkpoint_t *)1);
    ASSERT_TRUE(b != NULL);

    const memory_interface_t *mi = scc_get_memory_interface(b);
    for (int ch = 0; ch < 2; ch++) {
        uint32_t ctl = ch == 0 ? CH_B_CTL : CH_A_CTL;
        uint16_t want = ch == 0 ? 0x1234 : 0x5678;
        mi->write_uint8(b, ctl, 12);
        uint8_t lo = mi->read_uint8(b, ctl);
        mi->write_uint8(b, ctl, 13);
        uint8_t hi = mi->read_uint8(b, ctl);
        ASSERT_EQ_INT((int)((hi << 8) | lo), (int)want);
    }

    scc_delete(a);
    scc_delete(b);
}

// The external loopback cable is a property of the two ports together, so
// it lives outside the per-channel blocks -- and it was outside the stream
// entirely (N-16).  A restore quietly unplugged it.
TEST(test_the_loopback_cable_survives) {
    s_w[0] = s_w[1] = s_r = 0;

    s_slot = 0;
    scc_t *a = scc_init(NULL, NULL, irq_sink, NULL, NULL);
    ASSERT_TRUE(a != NULL);
    scc_set_external_loopback(a, true);
    ASSERT_TRUE(scc_get_external_loopback(a));
    scc_checkpoint(a, (checkpoint_t *)1);

    s_r = 0;
    s_slot = 1;
    scc_t *b = scc_init(NULL, NULL, irq_sink, NULL, (checkpoint_t *)1);
    ASSERT_TRUE(b != NULL);
    ASSERT_TRUE(scc_get_external_loopback(b));

    scc_delete(a);
    scc_delete(b);
}

int main(void) {
    RUN(test_save_restore_save_is_byte_identical);
    RUN(test_both_channels_registers_survive);
    RUN(test_the_loopback_cable_survives);
    printf("[PASS] All scc_checkpoint tests passed\n");
    return 0;
}
