// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// SCC inputs that used to abort the emulator (code review 2026-09-03,
// 06-io-controllers unit C2, from F-40 / N-12 / N-13).
//
// Four raw asserts in scc.c sat on conditions the chip's own users can
// break.  `assert` aborts in a debug build and is compiled out entirely by
// the release profile (-DGS_FAST -DNDEBUG), so each one was either a crash
// or nothing -- never a check.  This suite drives each condition and pins
// that the model now answers instead.
//
//   rr8 `ch->index == 1`      SDLC receive modelled on channel B only.
//                             Channel A in SDLC is legal (Z8530 UM §1,
//                             "two independent full-duplex channels") and
//                             four register writes put a byte in its
//                             receive buffer.
//   scc_sdlc_send SDLC_MODE   appletalk.c's RTS retry timer and CTS
//                             handler call in without llap_send's
//                             scc_sdlc_ready check, so a guest that leaves
//                             SDLC between enqueue and retry trips it.
//   scc_sdlc_send len >= 3    host-stack contract.
//   scc_sdlc_send len <= MAX  a BUFFER BOUND -- rx_queue_enqueue memcpys
//                             len bytes into a slot that size.  This one
//                             could never have been an assert of any
//                             kind: gs_assert_fail returns, so even a live
//                             GS_ASSERT would report and then overflow.
//
// SDLC_MAX_FRAME is private to scc.c; 1024 is repeated here deliberately,
// so that changing it there without looking here fails this test rather
// than silently widening the bound it is supposed to pin.

#include "scc.h"

#include "scheduler.h"

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// Link stubs
// ============================================================

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *file, int line) {
    (void)cp, (void)data, (void)size, (void)file, (void)line;
}
void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *file, int line) {
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
event_t *scheduler_new_cpu_event(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
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
void process_packet(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx, (void)buf, (void)len;
}

// ============================================================
// Helpers
// ============================================================

static void irq_sink(void *ctx, bool active) {
    (void)ctx, (void)active;
}

#define CH_B_CTL              0 // Mac decode: +0 bCtl, +2 aCtl, +4 bData, +6 aData
#define CH_A_CTL              2
#define SDLC_MAX_FRAME_PINNED 1024

static void wr(scc_t *scc, uint32_t off, uint8_t reg, uint8_t value) {
    const memory_interface_t *mi = scc_get_memory_interface(scc);
    mi->write_uint8(scc, off, reg);
    mi->write_uint8(scc, off, value);
}

static uint8_t rd(scc_t *scc, uint32_t off, uint8_t reg) {
    const memory_interface_t *mi = scc_get_memory_interface(scc);
    mi->write_uint8(scc, off, reg);
    return (uint8_t)mi->read_uint8(scc, off);
}

// WR4 bits 5:4 = 10 selects SDLC.
#define WR4_SDLC 0x20

// Private to scc.c, repeated for the same reason as SDLC_MAX_FRAME_PINNED.
#define RR0_SYNC_HUNT    0x10
#define RR1_END_OF_FRAME 0x80

static scc_t *make(void) {
    scc_t *scc = scc_init(NULL, NULL, irq_sink, NULL, NULL);
    ASSERT_TRUE(scc != NULL);
    return scc;
}

// ============================================================
// Tests
// ============================================================

// The four writes from the finding, in order: internal loopback on, a byte
// through the transmit buffer (which loops straight back into this
// channel's receive buffer), loopback off, SDLC on.  Then read RR8.  The
// byte is an ordinary async byte -- there is no frame around it -- so
// reading it as async is the honest answer, and that is what rr8 does now
// instead of asserting the channel index.
TEST(test_sdlc_receive_on_channel_a_reads_the_byte) {
    scc_t *scc = make();

    wr(scc, CH_A_CTL, 14, 0x10); // internal loopback
    wr(scc, CH_A_CTL, 8, 0x5A); // transmit -> lands in ch A's rx buffer
    wr(scc, CH_A_CTL, 14, 0x00); // loopback off
    wr(scc, CH_A_CTL, 4, WR4_SDLC); // channel A in SDLC

    ASSERT_EQ_INT(0x5A, rd(scc, CH_A_CTL, 8));

    // ...and it really took the async path, not a half-run frame path:
    // the SDLC branch would have latched End Of Frame in RR1 and left the
    // channel in hunt.
    ASSERT_EQ_INT(0, rd(scc, CH_A_CTL, 1) & RR1_END_OF_FRAME);
    ASSERT_EQ_INT(0, rd(scc, CH_A_CTL, 0) & RR0_SYNC_HUNT);

    scc_delete(scc);
}

// Channel B's own SDLC receive must be unaffected by the condition change:
// the guard is `SDLC_MODE && index != 1`, so B in SDLC still takes the
// frame path and does NOT fall into the async read.  Without this, the fix
// could have been "always read async", which would break LocalTalk.
TEST(test_channel_b_still_takes_the_sdlc_path) {
    scc_t *scc = make();

    // The same four writes, on B.
    wr(scc, CH_B_CTL, 14, 0x10);
    wr(scc, CH_B_CTL, 8, 0x5A);
    wr(scc, CH_B_CTL, 14, 0x00);
    wr(scc, CH_B_CTL, 4, WR4_SDLC);

    ASSERT_EQ_INT(0x5A, rd(scc, CH_B_CTL, 8));

    // A short residue is end-of-frame, and draining the FIFO puts the
    // receiver back in hunt.  Neither happens on the async path, so these
    // two are what tell the branches apart.
    ASSERT_EQ_INT(RR1_END_OF_FRAME, rd(scc, CH_B_CTL, 1) & RR1_END_OF_FRAME);
    ASSERT_EQ_INT(RR0_SYNC_HUNT, rd(scc, CH_B_CTL, 0) & RR0_SYNC_HUNT);

    scc_delete(scc);
}

// appletalk.c's retry timer path: the frame was queued while channel B was
// in SDLC, the guest left SDLC before the retry fired.
TEST(test_sdlc_send_refuses_when_the_channel_left_sdlc) {
    scc_t *scc = make(); // reset leaves WR4 async

    ASSERT_TRUE(!scc_sdlc_ready(scc));

    uint8_t frame[] = {0xFF, 0x01, 0x02, 0x03};
    ASSERT_EQ_INT(-1, scc_sdlc_send(scc, frame, sizeof frame));

    scc_delete(scc);
}

// A LocalTalk frame is destination, source and type at minimum.
TEST(test_sdlc_send_refuses_a_short_frame) {
    scc_t *scc = make();
    wr(scc, CH_B_CTL, 4, WR4_SDLC);

    uint8_t frame[] = {0xFF, 0x01};
    ASSERT_EQ_INT(-1, scc_sdlc_send(scc, frame, sizeof frame));

    scc_delete(scc);
}

// The bound that matters: one byte over is refused, exactly at the bound is
// accepted.  Pinning both sides is what keeps a later "fix" from being an
// off-by-one that still overflows.
TEST(test_sdlc_send_refuses_an_oversized_frame) {
    scc_t *scc = make();
    wr(scc, CH_B_CTL, 4, WR4_SDLC);

    static uint8_t frame[SDLC_MAX_FRAME_PINNED + 1];
    memset(frame, 0xA5, sizeof frame);

    ASSERT_EQ_INT(-1, scc_sdlc_send(scc, frame, SDLC_MAX_FRAME_PINNED + 1));
    ASSERT_EQ_INT(0, scc_sdlc_send(scc, frame, SDLC_MAX_FRAME_PINNED));

    scc_delete(scc);
}

int main(void) {
    RUN(test_sdlc_receive_on_channel_a_reads_the_byte);
    RUN(test_channel_b_still_takes_the_sdlc_path);
    RUN(test_sdlc_send_refuses_when_the_channel_left_sdlc);
    RUN(test_sdlc_send_refuses_a_short_frame);
    RUN(test_sdlc_send_refuses_an_oversized_frame);
    printf("[PASS] All scc_bad_input tests passed\n");
    return 0;
}
