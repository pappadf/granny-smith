// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ADB: the ST-transition filter (code review 2026-09-03, 06-io-controllers
// unit D2, from F-17 / R-5b).
//
// PB5:PB4 on VIA1 are the ADB transaction-state lines ST1:ST0 (Guide 2e
// :4155-4161; the state table at :7545-7552 reads 0 = command, 1 = even,
// 2 = odd, 3 = idle).  PB0-PB2 on the same port are the RTC's bit-banged
// serial lines, and the ROM drives them constantly without meaning to touch
// ST.  Real hardware ignores a port-B write whose ST lines do not change
// electrically, so the model has to as well (BUG-004) -- otherwise every
// RTC clock edge looks like a fresh ADB state transition.
//
// That filter, and the port-B shadow it needs, lived in FOUR machine files
// (se30.c, iicx.c, iici.c, q700.c), with the explanation in exactly one of
// them.  None of the four shadows was checkpointed: all three initialisers
// set $30 at machine init and a restore got $30 back regardless of the
// VIA's actual port-B output.  It is one field in adb_t now, inside the
// checkpointed block.
//
// There was no ADB unit suite at all before this, which is why a filter
// could be copied four times and explained once.
//
// The observable: a CMD transition reads the VIA shift register exactly
// once (adb.c reads SR directly there rather than waiting for the VIA's
// shift-complete callback, same BUG-004).  A filtered write reads nothing.

#include "adb.h"

#include "scheduler.h"
#include "via.h"

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
void system_input_key(int key, bool down) {
    (void)key, (void)down;
}
// keyboard.type()'s character lookup; this suite drives raw port-B writes.
int debug_mac_resolve_ascii(char c, bool *shift) {
    (void)c, (void)shift;
    return -1;
}

event_t *scheduler_new_cpu_event(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
    (void)s, (void)cb, (void)src, (void)data, (void)cycles, (void)ns;
    return NULL;
}
void remove_event(struct scheduler *restrict s, event_callback_t cb, void *src) {
    (void)s, (void)cb, (void)src;
}
void scheduler_new_event_type(struct scheduler *s, const char *sn, void *src, const char *en, event_callback_t cb) {
    (void)s, (void)sn, (void)src, (void)en, (void)cb;
}
void scheduler_forget_source(struct scheduler *s, void *source) {
    (void)s, (void)source;
}
double scheduler_time_ns(struct scheduler *restrict s) {
    (void)s;
    return 0.0;
}

// The VIA side.  sr_reads is the whole instrument: adb.c reads the shift
// register exactly once per state transition that it acts on.
static int s_sr_reads;
static uint8_t s_sr_value;

uint8_t via_read_sr(via_t *via) {
    (void)via;
    s_sr_reads++;
    return s_sr_value;
}
void via_cancel_pending_shift(via_t *via) {
    (void)via;
}
void via_input(via_t *via, int port, int pin, bool value) {
    (void)via, (void)port, (void)pin, (void)value;
}
void via_input_sr(via_t *via, uint8_t byte) {
    (void)via, (void)byte;
}

// ============================================================
// Helpers
// ============================================================

// ST1:ST0 in PB5:PB4 (Guide 2e :7545-7552).
#define ST_CMD  0x00
#define ST_EVEN 0x10
#define ST_ODD  0x20
#define ST_IDLE 0x30

// The RTC's three lines, which the ROM drives independently of ST.
#define RTC_BITS 0x07

static adb_t *setup(void) {
    s_sr_reads = 0;
    s_sr_value = 0x2C; // Talk R0 to address 2 (the keyboard)
    adb_t *adb = adb_init((via_t *)1, (struct scheduler *)1, NULL);
    ASSERT_TRUE(adb != NULL);
    return adb;
}

// ============================================================
// Tests
// ============================================================

// A port-B write that changes only the RTC lines is not an ADB event, no
// matter how many of them the ROM makes.
TEST(test_rtc_bit_banging_is_not_an_adb_transition) {
    adb_t *adb = setup();

    // Get into CMD once, legitimately.
    adb_port_b_output(adb, ST_CMD);
    ASSERT_EQ_INT(1, s_sr_reads);

    // Now bit-bang the RTC underneath it: eight clock edges with data, the
    // shape of one byte to the RTC.  ST never moves.
    for (int i = 0; i < 8; i++) {
        adb_port_b_output(adb, ST_CMD | 0x01); // data
        adb_port_b_output(adb, ST_CMD | 0x03); // clock high
        adb_port_b_output(adb, ST_CMD | 0x01); // clock low
    }
    ASSERT_EQ_INT(1, s_sr_reads); // still one: none of that was ADB

    adb_delete(adb);
}

// ...and a write that DOES move ST is an event, even when the RTC lines
// move in the same write, which is what the ROM actually does.
TEST(test_an_st_change_still_lands_under_rtc_traffic) {
    adb_t *adb = setup();

    adb_port_b_output(adb, ST_CMD | RTC_BITS);
    ASSERT_EQ_INT(1, s_sr_reads);

    // CMD -> IDLE -> CMD, with the RTC lines flapping throughout.
    adb_port_b_output(adb, ST_IDLE);
    adb_port_b_output(adb, ST_IDLE | 0x05);
    adb_port_b_output(adb, ST_CMD | 0x02);
    ASSERT_EQ_INT(2, s_sr_reads);

    adb_delete(adb);
}

// The shadow starts at idle, so the ROM's first write -- which on every one
// of these machines is an idle-state port-B init -- is correctly filtered
// rather than being read as a spurious IDLE transition.
TEST(test_the_shadow_starts_idle) {
    adb_t *adb = setup();

    // Two idle writes, differing only below ST: nothing should happen, and
    // in particular the first one must not be treated as a transition.
    adb_port_b_output(adb, ST_IDLE);
    adb_port_b_output(adb, ST_IDLE | RTC_BITS);
    ASSERT_EQ_INT(0, s_sr_reads);

    // The first real transition is the one after it.
    adb_port_b_output(adb, ST_CMD);
    ASSERT_EQ_INT(1, s_sr_reads);

    adb_delete(adb);
}

int main(void) {
    RUN(test_rtc_bit_banging_is_not_an_adb_transition);
    RUN(test_an_st_change_still_lands_under_rtc_traffic);
    RUN(test_the_shadow_starts_idle);
    printf("[PASS] All ADB tests passed\n");
    return 0;
}
