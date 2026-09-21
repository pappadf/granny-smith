// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Egret's send-abandon watchdog (code review 2026-09-03, 06-io-controllers
// unit D1, from F-03).
//
// egret_try_unsolicited is the whole gate on unsolicited traffic:
//
//     static bool egret_try_unsolicited(egret_t *eg) { return eg->state == EG_IDLE; }
//
// and EG_SENDING was left only on a host port-B edge.  So a response whose
// attention byte the host never takes wedged the transport for the rest of
// the run, and with it the 1-second tick AND the ADB autopoll: no clock, no
// keyboard, no mouse, on the IIsi, the LC and the Quadra 900.  Cuda has had
// a watchdog for this since it was written; Egret never got one, and the
// only primary statement of an Egret/Cuda firmware difference anywhere in
// the library (Macintosh Hardware Overview rev.2 :1793) is about ADB
// transfer-completion timing, not about a host-side abandon protocol.  The
// host driver is literally shared -- EgretMgr.a serves both.
//
// These tests drive the wire protocol the way VIA1 does: port-B edges in,
// shift-register bytes out.

#include "egret.h"

#include "adb.h"
#include "rtc.h"
#include "via.h"

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// Recording scheduler
// ============================================================
//
// egret.c names each event type as it registers it, so the tests address
// callbacks by name ("tick", "autopoll", "sendto", "resend") rather than by
// guessing at scheduling order.

#define MAX_EVENTS 16

static struct {
    event_callback_t cb;
    void *src;
    uint64_t data;
    bool live;
} s_events[MAX_EVENTS];

static struct {
    const char *name;
    event_callback_t cb;
} s_types[8];
static int s_type_count;

static event_callback_t cb_named(const char *name) {
    for (int i = 0; i < s_type_count; i++)
        if (strcmp(s_types[i].name, name) == 0)
            return s_types[i].cb;
    return NULL;
}

void scheduler_new_event_type(struct scheduler *s, const char *sn, void *src, const char *en, event_callback_t cb) {
    (void)s, (void)sn, (void)src;
    ASSERT_TRUE(s_type_count < (int)(sizeof s_types / sizeof s_types[0]));
    s_types[s_type_count].name = en;
    s_types[s_type_count].cb = cb;
    s_type_count++;
}

event_t *scheduler_new_cpu_event(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
    (void)s, (void)cycles, (void)ns;
    for (int i = 0; i < MAX_EVENTS; i++)
        if (!s_events[i].live) {
            s_events[i].cb = cb;
            s_events[i].src = src;
            s_events[i].data = data;
            s_events[i].live = true;
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
    for (int i = 0; i < MAX_EVENTS; i++)
        if (s_events[i].live && s_events[i].cb == cb)
            return true;
    return false;
}

void scheduler_forget_source(struct scheduler *s, void *source) {
    (void)s;
    for (int i = 0; i < MAX_EVENTS; i++)
        if (s_events[i].src == source)
            s_events[i].live = false;
}

static bool armed(const char *name) {
    event_callback_t cb = cb_named(name);
    ASSERT_TRUE(cb != NULL);
    return has_event(NULL, cb);
}

// Run the pending event of this type, the way the scheduler does: dequeue
// first, then call.  egret.c's periodic handlers re-arm themselves.
static void fire(const char *name) {
    event_callback_t cb = cb_named(name);
    ASSERT_TRUE(cb != NULL);
    for (int i = 0; i < MAX_EVENTS; i++)
        if (s_events[i].live && s_events[i].cb == cb) {
            void *src = s_events[i].src;
            uint64_t d = s_events[i].data;
            s_events[i].live = false;
            cb(src, d);
            return;
        }
    ASSERT_TRUE(0 && "no such event pending");
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

uint32_t rtc_get_seconds(const rtc_t *rtc) {
    (void)rtc;
    return 0;
}
void rtc_set_seconds(rtc_t *restrict rtc, uint32_t mac_seconds) {
    (void)rtc, (void)mac_seconds;
}
uint8_t rtc_pram_read(const rtc_t *rtc, uint8_t addr) {
    (void)rtc, (void)addr;
    return 0;
}
bool rtc_pram_write(rtc_t *rtc, uint8_t addr, uint8_t value) {
    (void)rtc, (void)addr, (void)value;
    return true;
}

// One ADB device with something to say, so autopoll produces a packet.
static bool s_adb_has_data;
uint8_t adb_keyboard_address(adb_t *adb) {
    (void)adb;
    return 2;
}
uint8_t adb_mouse_address(adb_t *adb) {
    (void)adb;
    return 3;
}
bool adb_iop_transact(adb_t *adb, uint8_t cmd, const uint8_t *in_data, int in_data_len, uint8_t *out_data,
                      int *out_len) {
    (void)adb, (void)cmd, (void)in_data, (void)in_data_len;
    if (!s_adb_has_data) {
        *out_len = 0;
        return false;
    }
    out_data[0] = 0x11;
    out_data[1] = 0x22;
    *out_len = 2;
    return true;
}
// Device selection is adb.c's business and has its own suite; here it only
// has to answer, so that the autopoll path produces a packet.
bool adb_autopoll_next(adb_t *adb, uint16_t enable_mask, uint8_t *cmd_out, uint8_t *out_data, int *len_out) {
    (void)enable_mask;
    *cmd_out = 0x2C; // Talk R0, address 2
    return adb_iop_transact(adb, *cmd_out, NULL, 0, out_data, len_out) && *len_out > 0;
}

// The VIA side: the shift register and the PB3 (xcvrSes) level Egret drives.
static uint8_t s_sr[64];
static int s_sr_len;
static bool s_xcvr_high;

void via_input_sr(via_t *via, uint8_t byte) {
    (void)via;
    if (s_sr_len < (int)sizeof s_sr)
        s_sr[s_sr_len++] = byte;
}
void via_input(struct via *via, int port, int pin, bool value) {
    (void)via;
    if (port == 1 && pin == 3)
        s_xcvr_high = value;
}

// ============================================================
// Driving the wire
// ============================================================

#define PB_XCVRSES (1u << 3)
#define PB_VIAFULL (1u << 4)
#define PB_SYSSES  (1u << 5)

#define PKT_PSEUDO     0x01
#define PKT_TICK       0x03
#define CMD_NOP        0x00
#define CMD_APOLL      0x01
#define CMD_WR1SECMODE 0x1B

#define EG_TX_GUARD 16 // more byte-pulses than any packet in these tests needs

static uint8_t s_pb; // the port-B level the host is driving

static void pb(egret_t *eg, uint8_t level) {
    s_pb = level;
    egret_via1_pb_input(eg, level);
}

static egret_t *setup(void) {
    memset(s_events, 0, sizeof s_events);
    s_type_count = 0;
    s_sr_len = 0;
    s_pb = 0;
    s_adb_has_data = false;

    // Non-NULL adb: egret_autopoll_event gates on the pointer, and the
    // stubs above never dereference it.
    egret_t *eg = egret_init(NULL, NULL, (struct adb *)1, (struct scheduler *)1, NULL);
    ASSERT_TRUE(eg != NULL);
    return eg;
}

// The host's side of a command packet, stopping at the attention byte:
// raise sysSes, shift the bytes out, drop sysSes.  Egret answers by pushing
// the attention byte into the SR and asserting xcvrSes.
static void host_sends(egret_t *eg, const uint8_t *cmd, int len) {
    pb(eg, PB_SYSSES);
    for (int i = 0; i < len; i++)
        egret_via1_shift_input(eg, cmd[i]);
    pb(eg, 0);
}

static void host_sends_nop(egret_t *eg) {
    const uint8_t nop[] = {PKT_PSEUDO, CMD_NOP};
    host_sends(eg, nop, 2);
}

// ...and the rest of the exchange: assert sysSes to take the attention
// byte, then pulse viaFull for each byte after it until Egret raises
// xcvrSes on the last one, then drop sysSes to close the session.
static void host_takes_response(egret_t *eg) {
    pb(eg, PB_SYSSES);
    for (int i = 0; i < EG_TX_GUARD && !s_xcvr_high; i++) {
        pb(eg, PB_SYSSES | PB_VIAFULL);
        pb(eg, PB_SYSSES);
    }
    pb(eg, 0);
}

// A command the host does see through, leaving the bus idle.
static void host_exchange(egret_t *eg, const uint8_t *cmd, int len) {
    host_sends(eg, cmd, len);
    host_takes_response(eg);
}

// ============================================================
// Tests
// ============================================================

// The finding itself: a host that walks away mid-response used to wedge
// the transport, and with it every unsolicited packet Egret ever sends.
TEST(test_an_unclaimed_response_does_not_wedge_the_transport) {
    egret_t *eg = setup();

    // Unsolicited traffic is off until the host asks for it, so ask.
    const uint8_t apoll_on[] = {PKT_PSEUDO, CMD_APOLL, 0x01};
    host_exchange(eg, apoll_on, 3);

    host_sends_nop(eg);
    ASSERT_TRUE(!s_xcvr_high); // xcvrSes asserted: packet in flight
    ASSERT_TRUE(armed("sendto")); // ...and the watchdog is running

    // The host never comes back.  The watchdog reaps.
    fire("sendto");
    ASSERT_TRUE(s_xcvr_high); // transport released

    // The proof that it is unwedged: an unsolicited packet gets out.
    // (Before the fix, egret_try_unsolicited saw EG_SENDING forever.)
    s_adb_has_data = true;
    int before = s_sr_len;
    fire("autopoll");
    ASSERT_TRUE(s_sr_len > before);

    egret_delete(eg);
}

// The reaped response is not thrown away.  An autopoll reply's ADB data
// left the device queue when the packet was built, so dropping it loses a
// keystroke outright; the packet is parked and offered again.
TEST(test_a_reaped_response_is_re_presented) {
    egret_t *eg = setup();

    host_sends_nop(eg);
    fire("sendto");
    ASSERT_TRUE(armed("resend"));

    int before = s_sr_len;
    fire("resend");
    ASSERT_EQ_INT(before + 1, s_sr_len); // the attention byte, again
    ASSERT_TRUE(!s_xcvr_high);

    // And the re-presented reply carries NO watchdog: the host is
    // mid-driver-install with interrupts masked, which is why it missed
    // the first presentation.
    ASSERT_TRUE(!armed("sendto"));

    egret_delete(eg);
}

// A tick is the one packet worth dropping: it is a clock edge, it will
// come again in a second, and re-presenting stale ones would bunch them.
TEST(test_a_reaped_tick_is_dropped_not_re_presented) {
    egret_t *eg = setup();

    const uint8_t onesec_on[] = {PKT_PSEUDO, CMD_WR1SECMODE, 0x03};
    host_exchange(eg, onesec_on, 3);

    fire("tick");
    ASSERT_TRUE(!s_xcvr_high); // a tick is on the wire
    ASSERT_TRUE(armed("sendto"));

    fire("sendto");
    ASSERT_TRUE(s_xcvr_high); // transport released
    ASSERT_TRUE(!armed("resend")); // ...and the tick is gone for good

    egret_delete(eg);
}

// The ordinary path must be untouched: a host that engages cancels the
// watchdog, so a slow-but-live exchange is never reaped underneath it.
TEST(test_host_engagement_cancels_the_watchdog) {
    egret_t *eg = setup();

    host_sends_nop(eg);
    ASSERT_TRUE(armed("sendto"));

    pb(eg, PB_SYSSES); // host asserts sysSes to take the byte
    ASSERT_TRUE(!armed("sendto"));

    egret_delete(eg);
}

// A command the host starts while a reply is parked supersedes it -- the
// host has moved on, and re-presenting on top of a fresh exchange would
// put two attention bytes on the wire.
TEST(test_a_new_command_supersedes_a_parked_reply) {
    egret_t *eg = setup();

    host_sends_nop(eg);
    fire("sendto");
    ASSERT_TRUE(armed("resend"));

    host_sends_nop(eg); // a fresh command
    fire("resend"); // the parked one must decline

    // Only the new exchange's attention byte, not a second one on top.
    ASSERT_TRUE(!s_xcvr_high);
    ASSERT_TRUE(armed("sendto")); // the fresh reply has its own watchdog

    egret_delete(eg);
}

int main(void) {
    RUN(test_an_unclaimed_response_does_not_wedge_the_transport);
    RUN(test_a_reaped_response_is_re_presented);
    RUN(test_a_reaped_tick_is_dropped_not_re_presented);
    RUN(test_host_engagement_cancels_the_watchdog);
    RUN(test_a_new_command_supersedes_a_parked_reply);
    printf("[PASS] All egret tests passed\n");
    return 0;
}
