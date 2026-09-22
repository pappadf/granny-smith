// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The Lisa COPS real-time clock (code review 2026-09-03, F-27).
//
// The Lisa's clock lives in the COPS, and the model answered its read command
// with five zero bytes.  That is not "unset" — it is impossible.  Day-of-year
// is 1-based, so 000 is not a date, and the year nibble is anchored at 1980,
// so 0 is below the Office System's floor of 1981.  LOS opened a "Lisa
// clock/calendar is not set properly" note on every boot and two integration
// rows dismissed it by warping the cursor onto its OK button.
//
// The wire format is the boot ROM's.  READCLK (RM248.M.TEXT) sends $02 and
// expects $80, a byte masked $F0 against $E0, then five more bytes;
// parameter memory reserves "$1BA-1BF : Clock setting (Ey,dd,dh,hm,ms,st)"
// (RM248.E.TEXT).  DSPCLK (RM248.B.TEXT) pins the field widths by loading
// CLKDATA+2 as a longword and rotating out 1 day digit, 2 hour, 2 minute and
// 2 second — leaving one nibble, the tenths, undisplayed.
//
// Setting it is a different shape: $2C, then sixteen one-nibble $1X commands
// MSB-first (TODSET), then $25.  Sixteen and not eleven because the first
// five digits are the alarm — which is the one inferred quantity here, being
// what is left over rather than something a source states.  The check that
// makes the reading credible is that the eleven clock digits come out
// contiguous across the SET1/SET2 boundary, and the round-trip below is what
// pins it: whatever the host writes, it reads back.

#include "cops.h"

#include "scheduler.h"
#include "via.h"

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// A VIA the COPS can talk to
// ============================================================
//
// The host jams a command by driving all of port A (DDRA = $FF); the COPS
// hands bytes back by putting them on port A and pulsing CA1.  Both sides of
// that are here, which is all this suite needs of a VIA.

static uint8_t s_porta_out; // what the "host" is driving
static uint8_t s_delivered[64]; // bytes the COPS handed back
static int s_delivered_len;
static uint8_t s_pending_byte; // assembled from the per-pin via_input calls

uint8_t via_port_direction(const via_t *via, unsigned which) {
    (void)via, (void)which;
    return 0xFF; // the host drives port A
}
uint8_t via_port_output(const via_t *via, unsigned which) {
    (void)via, (void)which;
    return s_porta_out;
}
uint8_t via_get_ifr(const via_t *via) {
    (void)via;
    return 0; // the host has always consumed the previous byte
}
void via_input(via_t *via, int port, int pin, bool value) {
    (void)via, (void)port;
    if (value)
        s_pending_byte |= (uint8_t)(1u << pin);
    else
        s_pending_byte &= (uint8_t) ~(1u << pin);
}
void via_input_c(via_t *via, int port, int c, bool value) {
    (void)via, (void)port, (void)c;
    // The rising edge is the COPS saying "byte ready".
    if (value && s_delivered_len < (int)sizeof s_delivered)
        s_delivered[s_delivered_len++] = s_pending_byte;
}
void via_output(via_t *via, int port, uint8_t value) {
    (void)via, (void)port, (void)value;
}

// ============================================================
// A scheduler that fires on demand
// ============================================================

#define MAX_EVENTS 16
static struct {
    event_callback_t cb;
    void *src;
    bool live;
} s_events[MAX_EVENTS];

event_t *scheduler_new_cpu_event(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
    (void)s, (void)data, (void)cycles, (void)ns;
    for (int i = 0; i < MAX_EVENTS; i++)
        if (!s_events[i].live) {
            s_events[i].cb = cb;
            s_events[i].src = src;
            s_events[i].live = true;
            return NULL;
        }
    return NULL;
}
void remove_event(struct scheduler *restrict s, event_callback_t cb, void *src) {
    (void)s;
    for (int i = 0; i < MAX_EVENTS; i++)
        if (s_events[i].live && s_events[i].cb == cb && s_events[i].src == src)
            s_events[i].live = false;
}
void scheduler_new_event_type(struct scheduler *s, const char *sn, void *src, const char *en, event_callback_t cb) {
    (void)s, (void)sn, (void)src, (void)en, (void)cb;
}
void scheduler_forget_source(struct scheduler *s, void *source) {
    (void)s;
    for (int i = 0; i < MAX_EVENTS; i++)
        if (s_events[i].src == source)
            s_events[i].live = false;
}
void scheduler_start(struct scheduler *restrict s) {
    (void)s;
}

// Fire everything currently queued, in passes.  A pass snapshots the live
// slots and runs those, so a self-re-arming event (the CRDY toggler runs
// forever by design) cannot starve the response pump — which needs several
// passes of its own, one per byte of a reply.
static void run_events(void) {
    for (int pass = 0; pass < 16; pass++) {
        bool snapshot[MAX_EVENTS];
        bool any = false;
        for (int i = 0; i < MAX_EVENTS; i++) {
            snapshot[i] = s_events[i].live;
            any |= snapshot[i];
        }
        if (!any)
            return;
        for (int i = 0; i < MAX_EVENTS; i++) {
            if (!snapshot[i] || !s_events[i].live)
                continue;
            event_callback_t cb = s_events[i].cb;
            void *src = s_events[i].src;
            s_events[i].live = false;
            cb(src, 0);
        }
    }
}

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
// The warp loop reads the OS's live cursor globals; this suite drives no
// mouse, so there is never a cursor to find.
bool lisa_mmu_get_cursor(int ctx, int *x, int *y) {
    (void)ctx, (void)x, (void)y;
    return false;
}

// ============================================================
// Driving the COPS
// ============================================================

static cops_t *setup(void) {
    memset(s_events, 0, sizeof s_events);
    s_delivered_len = 0;
    s_pending_byte = 0;
    cops_t *c = cops_init((via_t *)1, (struct scheduler *)1, NULL);
    ASSERT_TRUE(c != NULL);
    return c;
}

// The host jams one command byte onto port A.
static void command(cops_t *c, uint8_t cmd) {
    s_porta_out = cmd;
    cops_via_output(c, 0, cmd);
}

// Read the clock and return the six bytes after the $80 lead-in.
static void read_clock(cops_t *c, uint8_t out[6]) {
    s_delivered_len = 0;
    command(c, 0x02);
    run_events();
    ASSERT_EQ_INT(7, s_delivered_len);
    ASSERT_EQ_INT(0x80, s_delivered[0]);
    memcpy(out, &s_delivered[1], 6);
}

// A full set sequence: $2C, sixteen nibbles MSB-first, $25.
static void set_clock(cops_t *c, const uint8_t digits[16]) {
    command(c, 0x2C);
    for (int i = 0; i < 16; i++)
        command(c, (uint8_t)(0x10 | (digits[i] & 0x0F)));
    command(c, 0x25);
}

// ============================================================
// Tests
// ============================================================

// The reply has to be the shape READCLK reads, or the ROM waits forever
// mid-sequence: $80, then a byte whose high nibble is $E, then five more.
TEST(test_the_reply_has_the_shape_readclk_expects) {
    cops_t *c = setup();
    uint8_t r[6];
    read_clock(c, r);
    ASSERT_EQ_INT(0xE0, r[0] & 0xF0);
    cops_delete(c);
}

// Power-on is 1 January 1984, 00:00:00.0 — inside the Office System's
// 1981..1995 window and the year the Lisa 2 shipped.  Not the host wall
// clock, which four bits of year cannot reach: 1980 + 15 = 1995.
//
//   E y | d d | d h | h m | m s | s t
//   E 4 | 0 0 | 1 0 | 0 0 | 0 0 | 0 0
TEST(test_the_clock_powers_up_at_new_year_1984) {
    cops_t *c = setup();
    uint8_t r[6];
    read_clock(c, r);

    ASSERT_EQ_INT(0x04, r[0] & 0x0F); // year: 1980 + 4
    ASSERT_EQ_INT(0x00, r[1]); // day hundreds, tens
    ASSERT_EQ_INT(0x10, r[2]); // day units = 1, hour tens = 0
    ASSERT_EQ_INT(0x00, r[3]); // hour units, minute tens
    ASSERT_EQ_INT(0x00, r[4]); // minute units, second tens
    ASSERT_EQ_INT(0x00, r[5]); // second units, tenths

    cops_delete(c);
}

// Whatever the host sets, it reads back.  This is the property that matters
// and the one that does not depend on the inferred alarm width being exactly
// right — as long as the eleven clock digits are taken contiguously from the
// same place on both paths.
//
// 1991, day 234, 17:45:09.6.
TEST(test_a_set_clock_reads_back) {
    cops_t *c = setup();
    // 5 alarm digits, then year, ddd, hh, mm, ss, t.
    const uint8_t digits[16] = {9, 9, 9, 9, 9, 11, 2, 3, 4, 1, 7, 4, 5, 0, 9, 6};
    set_clock(c, digits);

    uint8_t r[6];
    read_clock(c, r);
    ASSERT_EQ_INT(0xEB, r[0]); // marker + year 11 -> 1991
    ASSERT_EQ_INT(0x23, r[1]); // day 2 3 _
    ASSERT_EQ_INT(0x41, r[2]); // day _ _ 4, hour 1 _
    ASSERT_EQ_INT(0x74, r[3]); // hour _ 7, minute 4 _
    ASSERT_EQ_INT(0x50, r[4]); // minute _ 5, second 0 _
    ASSERT_EQ_INT(0x96, r[5]); // second _ 9, tenths 6

    cops_delete(c);
}

// The alarm digits are skipped, not stored: changing only them must leave
// the clock alone.  If the offset were wrong the year would move with them.
TEST(test_the_alarm_digits_are_not_the_clock) {
    cops_t *c = setup();
    const uint8_t a[16] = {0, 0, 0, 0, 0, 5, 0, 1, 2, 0, 3, 0, 4, 0, 5, 0};
    const uint8_t b[16] = {7, 7, 7, 7, 7, 5, 0, 1, 2, 0, 3, 0, 4, 0, 5, 0};
    uint8_t r1[6], r2[6];

    set_clock(c, a);
    read_clock(c, r1);
    set_clock(c, b);
    read_clock(c, r2);

    ASSERT_TRUE(memcmp(r1, r2, 6) == 0);
    cops_delete(c);
}

// A sequence that stops early leaves the clock alone rather than applying
// half of it: the host either set the time or it did not.
TEST(test_an_abandoned_set_changes_nothing) {
    cops_t *c = setup();
    uint8_t before[6], after[6];
    read_clock(c, before);

    command(c, 0x2C);
    for (int i = 0; i < 9; i++) // nine of sixteen, then silence
        command(c, (uint8_t)(0x10 | 7));
    command(c, 0x25);

    read_clock(c, after);
    ASSERT_TRUE(memcmp(before, after, 6) == 0);
    cops_delete(c);
}

// $1n nibbles outside a set sequence are not clock data — the ROM writes
// nibbles in other contexts, and collecting them would corrupt the time.
TEST(test_stray_nibbles_are_ignored) {
    cops_t *c = setup();
    uint8_t before[6], after[6];
    read_clock(c, before);

    for (int i = 0; i < 16; i++)
        command(c, (uint8_t)(0x10 | 6)); // no $2C first
    command(c, 0x25);

    read_clock(c, after);
    ASSERT_TRUE(memcmp(before, after, 6) == 0);
    cops_delete(c);
}

int main(void) {
    RUN(test_the_reply_has_the_shape_readclk_expects);
    RUN(test_the_clock_powers_up_at_new_year_1984);
    RUN(test_a_set_clock_reads_back);
    RUN(test_the_alarm_digits_are_not_the_clock);
    RUN(test_an_abandoned_set_changes_nothing);
    RUN(test_stray_nibbles_are_ignored);
    printf("[PASS] All COPS clock tests passed\n");
    return 0;
}
