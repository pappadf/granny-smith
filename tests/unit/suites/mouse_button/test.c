// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// The quadrature mouse's button (VIA PB3, active low), which the ROM samples
// once per VBL and debounces over 3 ticks: every level the host sets lasts at
// least 4 VBL periods, so a press and release quicker than that reach the ROM
// as a press and then a release instead of vanishing inside the debounce
// (docs/internals/core/peripherals/mouse_control.md, "mouse.pending").

#include "mouse.h"

#include "host_input.h"
#include "scheduler.h"
#include "system_internal.h"

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// Link stubs: a scheduler with a clock the test moves, and the pins
// ============================================================


static double g_now_ns; // the guest clock
static bool g_pb3 = true; // VIA PB3 as the mouse last drove it (true = up)

// The one pending event (the mouse schedules at most one of each kind, and
// the test moves no motion, so only the button's ever exists)
static struct {
    bool armed;
    event_callback_t cb;
    void *src;
    uint64_t data;
    double at_ns;
} g_ev;

event_t *scheduler_new_cpu_event_ex(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data,
                                    uint64_t cycles, uint64_t ns, bool periodic) {
    (void)s, (void)cycles, (void)periodic;
    g_ev.armed = true;
    g_ev.cb = cb;
    g_ev.src = src;
    g_ev.data = data;
    g_ev.at_ns = g_now_ns + (double)ns;
    return NULL;
}
void scheduler_new_event_type(struct scheduler *s, const char *sn, void *src, const char *en, event_callback_t cb) {
    (void)s, (void)sn, (void)src, (void)en, (void)cb;
}
void remove_event_by_data(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data) {
    (void)s, (void)cb, (void)src, (void)data;
}
void scheduler_forget_source(struct scheduler *s, void *source) {
    (void)s, (void)source;
}
double scheduler_time_ns(struct scheduler *restrict s) {
    (void)s;
    return g_now_ns;
}
void via_input(via_t *restrict via, int port, int bit, bool value) {
    (void)via;
    if (port == 1 && bit == 3)
        g_pb3 = value;
}
void scc_dcd(scc_t *restrict scc, unsigned int ch, unsigned int dcd) {
    (void)scc, (void)ch, (void)dcd;
}
void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)cp, (void)data, (void)size, (void)tag, (void)file, (void)line;
}
void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)cp, (void)data, (void)size, (void)tag, (void)file, (void)line;
}
int system_input_mouse_move(int x, int y, const char *mode) {
    (void)x, (void)y, (void)mode;
    return 0;
}
int system_input_mouse_button(bool down, const char *mode) {
    (void)down, (void)mode;
    return 0;
}
bool system_mouse_input_pending(void) {
    return false;
}
void debug_mac_set_trace_mouse(host_input_t *hi, bool enabled) {
    (void)hi, (void)enabled;
}
struct object *adb_bus_object(void) {
    return NULL;
}

// Advance the guest clock by `ns`, firing the pending event if it falls due.
static void advance(double ns) {
    double end = g_now_ns + ns;
    while (g_ev.armed && g_ev.at_ns <= end) {
        g_now_ns = g_ev.at_ns;
        g_ev.armed = false;
        g_ev.cb(g_ev.src, g_ev.data);
    }
    g_now_ns = end;
}

// The hold mouse.c gives each level: 4 VBL periods and a millisecond
#define HOLD_NS (4.0 * (double)MAC_VBL_PERIOD_NS + 1e6)

static mouse_t *make_mouse(void) {
    static int fake_scheduler;
    g_now_ns = 1e9; // long after power-on: the first level is not inside a hold
    g_pb3 = true;
    memset(&g_ev, 0, sizeof g_ev);
    mouse_t *m = mouse_init((struct scheduler *)&fake_scheduler, NULL, NULL, NULL);
    ASSERT_TRUE(m != NULL);
    return m;
}

// ============================================================
// Tests
// ============================================================

// A press and release with no time between: the press is on the pin at once,
// the release waits out the hold, and pending covers the wait.
TEST(test_a_quick_click_holds_the_press) {
    mouse_t *m = make_mouse();
    mouse_update(m, true, 0, 0);
    mouse_update(m, false, 0, 0);
    ASSERT_TRUE(!g_pb3); // pressed
    ASSERT_TRUE(mouse_input_pending(m));
    advance(HOLD_NS * 0.9);
    ASSERT_TRUE(!g_pb3); // still pressed just before the hold ends
    advance(HOLD_NS * 0.2);
    ASSERT_TRUE(g_pb3); // released after the hold
    ASSERT_TRUE(mouse_input_pending(m)); // the release is inside its own hold
    advance(HOLD_NS);
    ASSERT_TRUE(!mouse_input_pending(m));
    mouse_delete(m);
}

// A double click between two VBLs: each of the four levels lasts a hold.
TEST(test_a_double_click_is_four_held_levels) {
    mouse_t *m = make_mouse();
    mouse_update(m, true, 0, 0);
    mouse_update(m, false, 0, 0);
    mouse_update(m, true, 0, 0);
    mouse_update(m, false, 0, 0);
    bool seen[4];
    for (int i = 0; i < 4; i++) {
        seen[i] = g_pb3;
        advance(HOLD_NS + 2e6);
    }
    ASSERT_TRUE(!seen[0] && seen[1] && !seen[2] && seen[3]);
    advance(HOLD_NS);
    ASSERT_TRUE(!mouse_input_pending(m));
    mouse_delete(m);
}

// A release that comes after the hold has passed applies at once, as before.
TEST(test_a_slow_click_is_not_delayed) {
    mouse_t *m = make_mouse();
    mouse_update(m, true, 0, 0);
    advance(HOLD_NS * 2);
    mouse_update(m, false, 0, 0);
    ASSERT_TRUE(g_pb3); // applied at once
    advance(HOLD_NS);
    ASSERT_TRUE(!mouse_input_pending(m));
    mouse_delete(m);
}

int main(void) {
    RUN(test_a_quick_click_holds_the_press);
    RUN(test_a_double_click_is_four_held_levels);
    RUN(test_a_slow_click_is_not_delayed);
    printf("[PASS] All mouse_button tests passed\n");
    return 0;
}
