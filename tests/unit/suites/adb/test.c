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
// The observable for the filter: a CMD transition reads the VIA shift
// register exactly once (adb.c reads SR directly there rather than waiting
// for the VIA's shift-complete callback, same BUG-004).  A filtered write
// reads nothing.
//
// The suite also covers adb_autopoll_next (unit D3, from R-2 / N-05), the
// one auto-poll engine that Egret, Cuda and the SWIM IOP now share.  Four
// copies of that loop disagreed four ways; the IIfx's two walked addresses
// 1..15 numerically, so ADDRESS 0 WAS NEVER POLLED, while the DevMap test
// three lines away was already bit-per-address over 0..15.  The IOP ADB
// Driver ERS (library/serial/apple-iop-adb-driver-ers/markdown.md:62,70)
// defines the mask over 0..15 and the order as most-recently-used, with a
// fallback that polls every address ignoring the mask while SRQ persists.

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
int system_input_key(int adb_code, bool down) {
    (void)adb_code, (void)down;
    return 0;
}
int system_input_key_raw(uint8_t byte) {
    (void)byte;
    return -1;
}
// keyboard.type()'s character lookup, and the key-name resolver the object
// surface uses; this suite drives raw port-B writes and the auto-poll engine,
// neither of which goes near either.
int debug_mac_resolve_ascii(char c, bool *shift) {
    (void)c, (void)shift;
    return -1;
}
int debug_mac_resolve_key_name(const char *name) {
    (void)name;
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

// === Auto-poll ==============================================================

// Talk register 0 to `addr`, the command every auto-poll issues.
#define TALK_R0(addr) ((uint8_t)(((addr) << 4) | 0x0C))

// Move a device with a Listen R3.  Handler $FE means "change address,
// preserve the handler" -- the form an OS's enumeration uses.
static void move_device(adb_t *adb, uint8_t from, uint8_t to) {
    uint8_t data[2] = {to, 0xFE};
    uint8_t out[8];
    int n = 0;
    adb_iop_transact(adb, (uint8_t)((from << 4) | 0x0B), data, 2, out, &n);
}

static bool poll(adb_t *adb, uint16_t mask, uint8_t *cmd) {
    uint8_t out[8];
    int n = 0;
    return adb_autopoll_next(adb, mask, cmd, out, &n);
}

// Address 0 is a legal ADB address and the IIfx's walk could never reach it.
TEST(test_address_zero_is_polled) {
    adb_t *adb = setup();

    move_device(adb, 2, 0); // keyboard from its power-on address to 0
    ASSERT_EQ_INT(0, adb_keyboard_address(adb));
    ASSERT_EQ_INT((1 << 0) | (1 << 3), adb_device_mask(adb));

    adb_keyboard_event(adb, key_down, 0x00);

    uint8_t cmd = 0;
    ASSERT_TRUE(poll(adb, 0, &cmd));
    ASSERT_EQ_INT(TALK_R0(0), cmd);

    adb_delete(adb);
}

// ERS clause 1: "poll the most recently used device ... until it receives
// data".  A mouse in continuous motion is re-polled, not round-robined
// past.
TEST(test_the_most_recently_used_device_is_polled_again) {
    adb_t *adb = setup();
    uint8_t cmd = 0;

    adb_mouse_event(adb, false, 4, 0);
    ASSERT_TRUE(poll(adb, 0, &cmd));
    ASSERT_EQ_INT(TALK_R0(3), cmd);

    adb_mouse_event(adb, false, 4, 0);
    ASSERT_TRUE(poll(adb, 0, &cmd));
    ASSERT_EQ_INT(TALK_R0(3), cmd);

    adb_delete(adb);
}

// ERS clause 2: "or until another device asserts Service Request".  There
// is no SRQ line in this model, so a device with data stands in for one
// pulling SRQ low -- and honouring the clause is what keeps a mouse in
// continuous motion from starving the keyboard, which is exactly what
// "re-poll the MRU device" alone would do.
TEST(test_another_device_with_data_interrupts_the_re_poll) {
    adb_t *adb = setup();
    uint8_t cmd = 0;

    adb_mouse_event(adb, false, 4, 0);
    ASSERT_TRUE(poll(adb, 0, &cmd));
    ASSERT_EQ_INT(TALK_R0(3), cmd);

    // Still dragging, and now typing.
    adb_mouse_event(adb, false, 4, 0);
    adb_keyboard_event(adb, key_down, 0x00);

    ASSERT_TRUE(poll(adb, 0, &cmd));
    ASSERT_EQ_INT(TALK_R0(2), cmd); // the keyboard, not the mouse again

    adb_delete(adb);
}

// The enable mask excludes an address, so the first scan skips it...
TEST(test_the_enable_mask_is_honoured) {
    adb_t *adb = setup();
    uint8_t cmd = 0;

    // Both have data.  The scan reaches the keyboard at 2 before the mouse
    // at 3, so with the mask ignored the keyboard would win -- which is
    // what makes the MOUSE the discriminating choice here.
    adb_mouse_event(adb, false, 4, 0);
    adb_keyboard_event(adb, key_down, 0x00);

    ASSERT_TRUE(poll(adb, 1 << 3, &cmd));
    ASSERT_EQ_INT(TALK_R0(3), cmd);

    adb_delete(adb);
}

// ...and ERS clause 3: "If after polling all of the enabled devices, SRQ is
// active, and no data was received from any of the devices, SRQ polling
// will continue, polling ALL device addresses, ignoring the enable mask."
TEST(test_the_fallback_ignores_the_mask) {
    adb_t *adb = setup();
    uint8_t cmd = 0;

    // Only the mouse is moving, and only the keyboard is enabled.  No
    // enabled device answers, but the bus is not quiet -- so the mask is
    // dropped and the mouse is polled.
    adb_mouse_event(adb, false, 4, 0);

    ASSERT_TRUE(poll(adb, 1 << 2, &cmd));
    ASSERT_EQ_INT(TALK_R0(3), cmd);

    adb_delete(adb);
}

// A quiet bus answers nothing, and says so, on both passes.
TEST(test_a_quiet_bus_returns_false) {
    adb_t *adb = setup();
    uint8_t cmd = 0;

    ASSERT_TRUE(!poll(adb, 0, &cmd));
    ASSERT_TRUE(!poll(adb, 1 << 2, &cmd));

    adb_delete(adb);
}

// === Register 3 ============================================================
//
// Guide 2e Table 8-15 (single-file.md:7726-7739): bit 15 reserved 0, bit 14
// "exceptional event, device specific; always 1 if not used", bit 13
// "Service Request enable; 1 = enabled", bit 12 reserved 0, bits 11-8 the
// address, bits 7-0 the handler ID.  The model answered $0X -- bits 14 and
// 13 both clear, which reads as "an exceptional event is in progress and I
// cannot service-request".  The $6X it answers now is DERIVED from that
// table, not quoted: the Guide has no register-3 content table.

// Talk R3 through the same path a transport uses.  Talk is $C | register,
// so Talk R3 is $F -- and Guide 2e :7603 is why every device answers it:
// "A device times out if it has no data to send; however, a device must
// respond to a Talk Register 3 command."
static void talk_r3(adb_t *adb, uint8_t addr, uint8_t *out) {
    int n = 0;
    ASSERT_TRUE(adb_iop_transact(adb, (uint8_t)((addr << 4) | 0x0F), NULL, 0, out, &n));
    ASSERT_EQ_INT(2, n);
}

TEST(test_register_three_reports_its_upper_bits) {
    adb_t *adb = setup();
    uint8_t r3[8];

    talk_r3(adb, 2, r3);
    ASSERT_EQ_INT(0x62, r3[0]); // no exceptional event, SRQ enabled, address 2

    talk_r3(adb, 3, r3);
    ASSERT_EQ_INT(0x63, r3[0]);

    // A $FE move takes the address with it and leaves the upper bits alone
    // -- including bit 13, even though the value written here has it clear.
    // Nothing ties bit 13 to the $FE form, and reading it from there would
    // turn a device's Service Request off during an ordinary enumeration.
    move_device(adb, 2, 9);
    talk_r3(adb, 9, r3);
    ASSERT_EQ_INT(0x69, r3[0]);

    adb_delete(adb);
}

// :7810-7812: "To disable a device's ability to send a Service Request
// signal, set bit 13 in register 3 to 0 by using a Listen Register 3 command
// with a Device Handler ID of $00... To enable the Service Request ability,
// set this bit to 1."
TEST(test_listen_r3_carries_the_srq_bit) {
    adb_t *adb = setup();
    uint8_t r3[8];
    uint8_t out[8];
    int n = 0;

    // Listen R3, handler $00, bit 13 clear: same address, SRQ off.  This is
    // the form the Guide names, and the only one that touches the bit.
    uint8_t off[2] = {0x02, 0x00}; // bits 13/14 clear, address 2
    adb_iop_transact(adb, (uint8_t)((2 << 4) | 0x0B), off, 2, out, &n);
    talk_r3(adb, 2, r3);
    ASSERT_EQ_INT(0x42, r3[0]); // bit 13 gone, bit 14 still set

    // ...and back on.
    uint8_t on[2] = {0x22, 0x00};
    adb_iop_transact(adb, (uint8_t)((2 << 4) | 0x0B), on, 2, out, &n);
    talk_r3(adb, 2, r3);
    ASSERT_EQ_INT(0x62, r3[0]);

    // ...and the $FE move does not touch it, in either direction.
    uint8_t off2[2] = {0x02, 0x00};
    adb_iop_transact(adb, (uint8_t)((2 << 4) | 0x0B), off2, 2, out, &n);
    move_device(adb, 2, 2); // $FE, bit 13 clear in the written value
    talk_r3(adb, 2, r3);
    ASSERT_EQ_INT(0x42, r3[0]); // still off, not re-enabled by the move

    adb_delete(adb);
}

// The bit is real state, so it changes behaviour: a device told not to
// service-request cannot interrupt the auto-poll to announce itself.  It is
// still polled and still answers -- it just waits its turn.
TEST(test_a_device_with_srq_off_does_not_interrupt_the_poll) {
    adb_t *adb = setup();
    uint8_t cmd = 0;
    uint8_t out[8];
    int n = 0;

    // Keyboard: SRQ off, address unchanged.
    uint8_t off[2] = {0x02, 0x00};
    adb_iop_transact(adb, (uint8_t)((2 << 4) | 0x0B), off, 2, out, &n);

    adb_mouse_event(adb, false, 4, 0);
    ASSERT_TRUE(poll(adb, 0, &cmd));
    ASSERT_EQ_INT(TALK_R0(3), cmd); // mouse is the MRU device now

    // Still dragging, and now typing.  With SRQ enabled the keyboard would
    // take this poll (see test_another_device_with_data_interrupts_the_
    // re_poll); with it off, the mouse keeps the bus.
    adb_mouse_event(adb, false, 4, 0);
    adb_keyboard_event(adb, key_down, 0x00);
    ASSERT_TRUE(poll(adb, 0, &cmd));
    ASSERT_EQ_INT(TALK_R0(3), cmd);

    // The keystroke is not lost: once the mouse goes quiet the scan reaches
    // the keyboard on its own.
    ASSERT_TRUE(poll(adb, 0, &cmd));
    ASSERT_EQ_INT(TALK_R0(2), cmd);

    adb_delete(adb);
}

int main(void) {
    RUN(test_rtc_bit_banging_is_not_an_adb_transition);
    RUN(test_an_st_change_still_lands_under_rtc_traffic);
    RUN(test_the_shadow_starts_idle);
    RUN(test_address_zero_is_polled);
    RUN(test_the_most_recently_used_device_is_polled_again);
    RUN(test_another_device_with_data_interrupts_the_re_poll);
    RUN(test_the_enable_mask_is_honoured);
    RUN(test_the_fallback_ignores_the_mask);
    RUN(test_a_quiet_bus_returns_false);
    RUN(test_register_three_reports_its_upper_bits);
    RUN(test_listen_r3_carries_the_srq_bit);
    RUN(test_a_device_with_srq_off_does_not_interrupt_the_poll);
    printf("[PASS] All ADB tests passed\n");
    return 0;
}
