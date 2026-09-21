// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// SCC interrupt vector (RR2), code review 2026-09-03, 06-io-controllers
// unit C1 / N-02.
//
// Z8530 UM section 5.3.3: "RR2 contains the interrupt vector written into
// WR2.  When this register is accessed in Channel A, the vector returned is
// the unmodified value written into WR2.  When this register is accessed in
// Channel B, the vector returned includes status information in bits 1, 2
// and 3 or in bits 6, 5 and 4."  Figure 5-21 draws D7..D0 as V7..V0 -- plain
// vector bits, only three of which the chip overwrites.
//
// rr2() built channel B's answer out of the status code ALONE and threw the
// programmed vector away, so a driver that dispatches on RR2B -- which is
// the documented way to use the chip, and what the A/UX and PDM native
// LocalTalk paths do -- got a vector with V7, V6, V5, V4 and V0 all zero.
// The bug is invisible to any guest that programs WR2 = 0, which is most of
// the corpus, and that is why nothing caught it.
//
// Sitting in rr2() was `assert((scc->ch[0].rr[2] & 0xC0) == 0)`, a leftover
// from a version that did merge.  WR2 writes land straight in rr[2], so any
// guest programming a vector in the top of the page -- a perfectly ordinary
// thing to do -- aborted the emulator.  test_high_vector_is_not_fatal pins
// that it no longer can.
//
// The suite drives the chip through its memory interface, the way the VIA
// glue does: write the register number to the control port, then read or
// write the value.

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

#define CH_B_CTL 0 // Mac decode: +0 bCtl, +2 aCtl, +4 bData, +6 aData
#define CH_A_CTL 2

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

// A vector with something in every field the chip must NOT touch:
// V7..V4 = 1101, V0 = 1.  Only V3..V1 (mask $0E) are the chip's to write.
#define VEC      0xD9
#define VEC_KEPT (VEC & ~0x0E) // $D1 -- what must survive every read

// Enabling Tx interrupts while the transmit buffer is empty (which it is
// after reset) latches that channel's Tx-pending bit in RR3.  It is the
// cheapest guest-reachable way to put a known interrupt on the chip.
static void raise_tx_pending(scc_t *scc, uint32_t ctl) {
    wr(scc, ctl, 1, 0x02);
}

static scc_t *make(void) {
    scc_t *scc = scc_init(NULL, NULL, irq_sink, NULL, NULL);
    ASSERT_TRUE(scc != NULL);
    wr(scc, CH_A_CTL, 2, VEC); // WR2 is shared; channel A is where drivers write it
    return scc;
}

// ============================================================
// Tests
// ============================================================

// Channel A hands back exactly what was written, with no status at all.
TEST(test_channel_a_vector_is_unmodified) {
    scc_t *scc = make();
    ASSERT_EQ_INT(VEC, rd(scc, CH_A_CTL, 2));
    scc_delete(scc);
}

// Channel B, nothing pending: status field = 011 (UM 5.3.3), everything
// else the guest programmed still there.  The bug returned a bare $06.
TEST(test_channel_b_merges_vector_when_idle) {
    scc_t *scc = make();
    ASSERT_EQ_INT(VEC_KEPT | 0x06, rd(scc, CH_B_CTL, 2));
    scc_delete(scc);
}

// Channel B with channel B's Tx interrupt pending: status 000, and again
// the vector's own bits survive.  The bug returned a bare $00 -- so a
// driver's jump table indexed at offset zero no matter what it programmed.
TEST(test_channel_b_merges_vector_with_tx_status) {
    scc_t *scc = make();
    raise_tx_pending(scc, CH_B_CTL);
    ASSERT_EQ_INT(VEC_KEPT | 0x00, rd(scc, CH_B_CTL, 2));
    scc_delete(scc);
}

// Channel A's Tx interrupt is a different status code (100), which is how
// the two halves of the same jump table stay apart.  Pinning both codes
// keeps the merge from being "mask everything to the same constant".
TEST(test_channel_a_tx_status_differs) {
    scc_t *scc = make();
    raise_tx_pending(scc, CH_A_CTL);
    ASSERT_EQ_INT(VEC_KEPT | 0x08, rd(scc, CH_B_CTL, 2));
    scc_delete(scc);
}

// A vector with bits set in D7/D6 used to abort the emulator on the first
// RR2B read.  Nothing about those bits is special.
TEST(test_high_vector_is_not_fatal) {
    scc_t *scc = scc_init(NULL, NULL, irq_sink, NULL, NULL);
    ASSERT_TRUE(scc != NULL);
    wr(scc, CH_A_CTL, 2, 0xC0);
    ASSERT_EQ_INT(0xC0 | 0x06, rd(scc, CH_B_CTL, 2));
    scc_delete(scc);
}

int main(void) {
    RUN(test_channel_a_vector_is_unmodified);
    RUN(test_channel_b_merges_vector_when_idle);
    RUN(test_channel_b_merges_vector_with_tx_status);
    RUN(test_channel_a_tx_status_differs);
    RUN(test_high_vector_is_not_fatal);
    printf("[PASS] All scc_vector tests passed\n");
    return 0;
}
