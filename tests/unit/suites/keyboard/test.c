// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Mac Plus keyboard: the INQUIRY timeout that an INSTANT supersedes
// (code review 2026-09-03, 06-io-controllers unit C2, from F-40).
//
// keyboard_tx_callback's INSTANT-with-empty-queue branch asserted that no
// timeout event was armed.  It is an ordinary guest sequence that arms one:
//
//   INQUIRY  -> schedules a 250 ms timeout AND the small tx delay
//   tx fires -> queue empty, active_cmd is INQUIRY, so tx_pending = true
//               and NOTHING cancels the timeout
//   INSTANT  -> schedules another tx delay
//   tx fires -> queue still empty, active_cmd is INSTANT: assert, with the
//               timeout very much armed
//
// The Mac ROM polls with INQUIRY and drops to INSTANT when it wants an
// answer now; two polls with no key in between is all it takes.  This was
// the most reachable of the six sites in the finding, and the report gave
// no reachability argument for it at all.
//
// Left armed rather than asserted (release builds compile the assert out),
// the stale timer puts a second, unsolicited NULL_RESPONSE on the VIA shift
// register 250 ms later -- a keyboard reply to a command the Mac never
// sent.  So the test checks both halves: the INSTANT is answered, and the
// timer is gone.
//
// The scheduler here is a recording stub with real add/find/remove
// semantics keyed on (callback, source), which is exactly the surface
// keyboard.c uses.  Nothing fires on its own; fire_event() runs a pending
// event the way the scheduler would.

#include "keyboard.h"

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// Recording scheduler
// ============================================================

#define MAX_EVENTS 16

static struct {
    event_callback_t cb;
    void *src;
    uint64_t data;
    bool live;
} s_events[MAX_EVENTS];

static int find_event(event_callback_t cb) {
    for (int i = 0; i < MAX_EVENTS; i++)
        if (s_events[i].live && s_events[i].cb == cb)
            return i;
    return -1;
}

event_t *scheduler_new_cpu_event(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
    (void)s, (void)cycles, (void)ns;
    for (int i = 0; i < MAX_EVENTS; i++)
        if (!s_events[i].live) {
            s_events[i] = (typeof(s_events[0])){cb, src, data, true};
            return NULL;
        }
    ASSERT_TRUE(0 && "event table full");
    return NULL;
}

void remove_event(struct scheduler *restrict s, event_callback_t cb, void *src) {
    (void)s;
    for (int i = 0; i < MAX_EVENTS; i++)
        if (s_events[i].live && s_events[i].cb == cb && s_events[i].src == src)
            s_events[i].live = false;
}

bool has_event(struct scheduler *restrict s, event_callback_t cb) {
    (void)s;
    return find_event(cb) >= 0;
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

// Run the one pending event with this callback, the way the scheduler does:
// dequeue first, then call.
static void fire_event(event_callback_t cb) {
    int i = find_event(cb);
    ASSERT_TRUE(i >= 0);
    s_events[i].live = false;
    cb(s_events[i].src, s_events[i].data);
}

// ============================================================
// Link stubs
// ============================================================

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *file, int line) {
    (void)cp, (void)data, (void)size, (void)file, (void)line;
}
void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *file, int line) {
    (void)cp, (void)data, (void)size, (void)file, (void)line;
}

// The VIA shift register is the keyboard's only output.
static uint8_t s_sr[64];
static int s_sr_len;

void via_input_sr(via_t *via, uint8_t value) {
    (void)via;
    if (s_sr_len < (int)sizeof s_sr)
        s_sr[s_sr_len++] = value;
}

// ============================================================
// Helpers
// ============================================================

#define CMD_INQUIRY   0x10
#define CMD_INSTANT   0x14
#define NULL_RESPONSE 0x7B

// keyboard.c's two event callbacks are static, so they are identified here
// the way the scheduler sees them -- by which slot each landed in.  INQUIRY
// schedules the timeout FIRST and the tx delay second (keyboard.c's switch
// runs before the common scheduling tail), which is the ordering these
// helpers rely on and which fire_event's search does not depend on.
static event_callback_t s_timeout_cb, s_tx_cb;

static keyboard_t *setup(void) {
    memset(s_events, 0, sizeof s_events);
    s_sr_len = 0;
    s_timeout_cb = s_tx_cb = NULL;

    keyboard_t *kbd = keyboard_init(NULL, NULL, NULL, NULL);
    ASSERT_TRUE(kbd != NULL);

    // Send an INQUIRY and learn the two callbacks from what it scheduled:
    // the timeout goes in first, the tx delay second.
    keyboard_input(kbd, CMD_INQUIRY);
    ASSERT_TRUE(s_events[0].live && s_events[1].live);
    s_timeout_cb = s_events[0].cb;
    s_tx_cb = s_events[1].cb;
    ASSERT_TRUE(s_timeout_cb != s_tx_cb);

    return kbd;
}

// ============================================================
// Tests
// ============================================================

// The whole sequence, and the assertion that used to abort here.
TEST(test_instant_after_inquiry_cancels_the_stale_timeout) {
    keyboard_t *kbd = setup();

    // The INQUIRY's tx delay expires with nothing to send. The 250 ms
    // timeout stays armed -- that is correct, it is what will answer the
    // INQUIRY if no key arrives.
    fire_event(s_tx_cb);
    ASSERT_TRUE(has_event(NULL, s_timeout_cb));
    ASSERT_EQ_INT(0, s_sr_len);

    // Now the Mac asks for an answer immediately.
    keyboard_input(kbd, CMD_INSTANT);
    fire_event(s_tx_cb);

    // Answered once...
    ASSERT_EQ_INT(1, s_sr_len);
    ASSERT_EQ_INT(NULL_RESPONSE, s_sr[0]);

    // ...and the INQUIRY's timeout is gone, so there is no second,
    // unsolicited reply 250 ms from now.
    ASSERT_TRUE(!has_event(NULL, s_timeout_cb));

    keyboard_delete(kbd);
}

// The timeout must survive an INQUIRY that is merely waiting -- cancelling
// it unconditionally would be the other way to silence the assert, and it
// would break the poll the timer exists for.
TEST(test_a_waiting_inquiry_keeps_its_timeout) {
    keyboard_t *kbd = setup();

    fire_event(s_tx_cb);
    ASSERT_TRUE(has_event(NULL, s_timeout_cb));

    // Left to run, it answers the INQUIRY.
    fire_event(s_timeout_cb);
    ASSERT_EQ_INT(1, s_sr_len);
    ASSERT_EQ_INT(NULL_RESPONSE, s_sr[0]);

    keyboard_delete(kbd);
}

// A key arriving while the INQUIRY waits is sent at once and cancels the
// timeout on its own path -- unchanged by this fix, pinned so that the new
// cancellation cannot be mistaken for the only one.
TEST(test_a_key_answers_the_inquiry_and_cancels_the_timeout) {
    keyboard_t *kbd = setup();

    fire_event(s_tx_cb);
    keyboard_update(kbd, key_down, 0x00); // 'A' on a Plus keyboard

    ASSERT_EQ_INT(1, s_sr_len);
    ASSERT_TRUE(!has_event(NULL, s_timeout_cb));

    keyboard_delete(kbd);
}

int main(void) {
    RUN(test_instant_after_inquiry_cancels_the_stale_timeout);
    RUN(test_a_waiting_inquiry_keeps_its_timeout);
    RUN(test_a_key_answers_the_inquiry_and_cancels_the_timeout);
    printf("[PASS] All keyboard tests passed\n");
    return 0;
}
