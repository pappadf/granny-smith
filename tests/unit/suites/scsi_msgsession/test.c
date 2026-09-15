// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for scsi_msgsession.c — the initiator-side SCSI message
// conversation shared by Apple's MESH and the Symbios 53C8xx SCRIPTS engine.
//
// Before this existed the two chips each had their own copy of the parse loop
// and neither had a direct test. The only live negotiation anywhere in the
// corpus is 29 SDTR exchanges inside one AIX boot (ans-aix-installed-boot);
// every other workload sends IDENTIFY and nothing else. So the capability
// clamping, the width answer and the reject path were all effectively
// untested. They are pinned here instead.

#include "scsi_msgsession.h"
#include "test_assert.h"

#include <string.h>

// The two real capability sets, each from its own part's documentation.
//
//   MESH: sync_params (0xD0) packs the offset into four bits — offset =
//   value >> 4 — so 15 is the deepest it can express, and the transfer period
//   is (x + 2) * 40 ns with x == 0 meaning 100 ns, so SDTR period 25 is its
//   floor. Narrow part.
static const scsi_msg_caps_t MESH_CAPS = {.min_period = 25, .max_offset = 15, .wide = false};
//   53C825: the SXFER register's MO4-MO0 table tops out at exactly 16, and the
//   part does Fast SCSI, so 100 ns is its floor too. Wide part.
static const scsi_msg_caps_t SYM_CAPS = {.min_period = 25, .max_offset = 16, .wide = true};

static void feed(scsi_msgsession_t *s, const uint8_t *bytes, size_t n) {
    for (size_t i = 0; i < n; i++)
        ASSERT_TRUE(scsi_msg_collect(s, bytes[i]));
}

static size_t drain(scsi_msgsession_t *s, uint8_t *out, size_t max) {
    size_t n = 0;
    uint8_t b;
    while (n < max && scsi_msg_next(s, &b))
        out[n++] = b;
    return n;
}

// ---- IDENTIFY: the only thing most workloads ever send ---------------------

TEST(identify_alone_is_consumed_with_no_reply) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0xC0}; // IDENTIFY, disconnect permitted, LUN 0
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &MESH_CAPS, &r);
    ASSERT_TRUE(r.identify);
    ASSERT_TRUE(!r.sdtr && !r.wdtr && !r.rejected && !r.incomplete);
    ASSERT_TRUE(!scsi_msg_pending(&s)); // nothing owed back
    ASSERT_EQ_INT(s.out_len, 0); // and the stream was consumed
}

// ---- SDTR: clamped to the part, not echoed ---------------------------------

// AIX's real offer, measured on ans-aix-installed-boot: period 25 (100 ns),
// offset 16. That is exactly the 53C825's maximum, so it passes through whole
// — which is why the old unclamped echo looked correct.
TEST(sdtr_within_the_parts_limits_is_agreed_verbatim) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0xC0, 0x01, 0x03, 0x01, 25, 16};
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &SYM_CAPS, &r);
    ASSERT_TRUE(r.identify && r.sdtr);
    ASSERT_EQ_INT(r.period, 25);
    ASSERT_EQ_INT(r.offset, 16);

    uint8_t reply[8];
    ASSERT_EQ_INT((int)drain(&s, reply, sizeof(reply)), 5);
    ASSERT_EQ_INT(reply[0], 0x01); // EXTENDED
    ASSERT_EQ_INT(reply[1], 0x03); // length
    ASSERT_EQ_INT(reply[2], 0x01); // SDTR
    ASSERT_EQ_INT(reply[3], 25);
    ASSERT_EQ_INT(reply[4], 16);
}

// The same offer to MESH must come back clamped: its offset field is four bits
// wide, so 16 is a depth it cannot express.
TEST(sdtr_offset_is_clamped_to_what_the_part_can_express) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0xC0, 0x01, 0x03, 0x01, 25, 16};
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &MESH_CAPS, &r);
    ASSERT_TRUE(r.sdtr);
    ASSERT_EQ_INT(r.offset, 15); // 16 -> 15, the four-bit maximum
    ASSERT_EQ_INT(r.period, 25);

    uint8_t reply[8];
    ASSERT_EQ_INT((int)drain(&s, reply, sizeof(reply)), 5);
    ASSERT_EQ_INT(reply[4], 15); // and the initiator is TOLD 15, not 16
}

// A period is a duration, so a smaller number is a FASTER bus: the cap is a
// floor on the value. An initiator demanding 50 ns must be answered 100 ns.
TEST(sdtr_period_floor_slows_an_offer_the_part_cannot_run) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0x01, 0x03, 0x01, 12, 8}; // 48 ns, offset 8
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &SYM_CAPS, &r);
    ASSERT_TRUE(r.sdtr);
    ASSERT_EQ_INT(r.period, 25); // raised to the 100 ns floor
    ASSERT_EQ_INT(r.offset, 8); // already within reach, untouched

    uint8_t reply[8];
    ASSERT_EQ_INT((int)drain(&s, reply, sizeof(reply)), 5);
    ASSERT_EQ_INT(reply[3], 25);
}

// A slower offer than the part's floor is fine and must pass through: the
// clamp is a limit, not a target.
TEST(sdtr_slower_than_the_floor_is_left_alone) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0x01, 0x03, 0x01, 50, 4}; // 200 ns
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &MESH_CAPS, &r);
    ASSERT_EQ_INT(r.period, 50);
    ASSERT_EQ_INT(r.offset, 4);
}

// ---- WDTR: answered by both, agreed only by the wide one -------------------

TEST(wdtr_is_agreed_by_a_wide_part) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0x01, 0x02, 0x03, 0x01}; // WDTR, 16-bit
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &SYM_CAPS, &r);
    ASSERT_TRUE(r.wdtr && r.wide);

    uint8_t reply[8];
    ASSERT_EQ_INT((int)drain(&s, reply, sizeof(reply)), 4);
    ASSERT_EQ_INT(reply[0], 0x01);
    ASSERT_EQ_INT(reply[1], 0x02);
    ASSERT_EQ_INT(reply[2], 0x03);
    ASSERT_EQ_INT(reply[3], 1);
}

// MESH used to have no WDTR case at all, so a wide offer fell through and was
// dropped without a word — the initiator then waited for a reply that was
// never coming. A narrow part must answer 8-bit.
TEST(wdtr_to_a_narrow_part_is_answered_eight_bit_not_dropped) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0x01, 0x02, 0x03, 0x01};
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &MESH_CAPS, &r);
    ASSERT_TRUE(r.wdtr);
    ASSERT_TRUE(!r.wide);

    uint8_t reply[8];
    ASSERT_EQ_INT((int)drain(&s, reply, sizeof(reply)), 4);
    ASSERT_EQ_INT(reply[3], 0); // 8-bit: a reply, not silence
}

// ---- messages we do not implement ------------------------------------------

// SCSI-2 6.6.2: an unsupported extended message is REJECTED. Both chips used
// to walk past it silently.
TEST(unknown_extended_message_is_rejected) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0x01, 0x02, 0x7F, 0x00}; // some extended code we do not do
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &SYM_CAPS, &r);
    ASSERT_TRUE(!r.sdtr && !r.wdtr);

    uint8_t reply[8];
    ASSERT_EQ_INT((int)drain(&s, reply, sizeof(reply)), 1);
    ASSERT_EQ_INT(reply[0], 0x07); // MESSAGE REJECT
}

TEST(message_reject_from_the_initiator_is_reported) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0x07};
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &MESH_CAPS, &r);
    ASSERT_TRUE(r.rejected);
    ASSERT_TRUE(!scsi_msg_pending(&s)); // a reject is not itself answered
}

// ---- partial arrival --------------------------------------------------------

// An extended message spread across two MESSAGE OUT phases must not be parsed
// from its truncated half, and must not be thrown away either.
TEST(a_half_arrived_extended_message_waits_for_the_rest) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t first[] = {0xC0, 0x01, 0x03, 0x01}; // header, params missing
    feed(&s, first, sizeof(first));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &SYM_CAPS, &r);
    ASSERT_TRUE(r.incomplete);
    ASSERT_TRUE(!r.sdtr);
    ASSERT_TRUE(!scsi_msg_pending(&s));
    ASSERT_EQ_INT(s.out_len, 4); // kept, not discarded

    const uint8_t rest[] = {25, 16};
    feed(&s, rest, sizeof(rest));
    scsi_msg_complete(&s, &SYM_CAPS, &r);
    ASSERT_TRUE(r.sdtr && !r.incomplete);
    ASSERT_EQ_INT(r.period, 25);
    ASSERT_EQ_INT(r.offset, 16);
    ASSERT_EQ_INT(s.out_len, 0);
}

// A length byte that runs past the buffer must be treated as "still arriving",
// never as a licence to read past the end.
TEST(a_length_byte_past_the_end_does_not_read_out_of_bounds) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0x01, 0xFF, 0x01}; // claims 255 parameter bytes
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &SYM_CAPS, &r);
    ASSERT_TRUE(r.incomplete);
    ASSERT_EQ_INT(s.out_len, 3);
}

// ---- overflow ---------------------------------------------------------------

// Neither chip reported a full buffer; both dropped the byte and carried on,
// which is how a negotiation hangs. collect() now says so.
TEST(collect_reports_a_full_buffer_instead_of_dropping_silently) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    for (int i = 0; i < SCSI_MSG_OUT_MAX; i++)
        ASSERT_TRUE(scsi_msg_collect(&s, 0x80));
    ASSERT_TRUE(!scsi_msg_collect(&s, 0x80)); // refused, and says so
    ASSERT_EQ_INT(s.out_len, SCSI_MSG_OUT_MAX);
}

// ---- session lifetime -------------------------------------------------------

// Everything dies with the connection: a reply left unread must not surface in
// the next conversation, which would present a phantom MESSAGE IN.
TEST(reset_clears_a_reply_that_was_never_read) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0x01, 0x03, 0x01, 25, 8};
    feed(&s, msg, sizeof(msg));
    scsi_msg_result_t r;
    scsi_msg_complete(&s, &SYM_CAPS, &r);
    ASSERT_TRUE(scsi_msg_pending(&s));

    scsi_msg_reset(&s);
    ASSERT_TRUE(!scsi_msg_pending(&s));
    ASSERT_EQ_INT(s.out_len, 0);
}

// Two negotiations in one connection: the second reply must replace the first,
// not append to it.
TEST(a_second_negotiation_replaces_the_first_reply) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t a[] = {0x01, 0x03, 0x01, 25, 8};
    feed(&s, a, sizeof(a));
    scsi_msg_result_t r;
    scsi_msg_complete(&s, &SYM_CAPS, &r);

    uint8_t reply[8];
    ASSERT_EQ_INT((int)drain(&s, reply, sizeof(reply)), 5);

    const uint8_t b[] = {0x01, 0x03, 0x01, 50, 4};
    feed(&s, b, sizeof(b));
    scsi_msg_complete(&s, &SYM_CAPS, &r);
    ASSERT_EQ_INT((int)drain(&s, reply, sizeof(reply)), 5);
    ASSERT_EQ_INT(reply[3], 50);
    ASSERT_EQ_INT(reply[4], 4);
}

// ---- the shape the drivers actually send ------------------------------------

TEST(identify_then_sdtr_in_one_stream_is_parsed_in_order) {
    scsi_msgsession_t s;
    scsi_msg_reset(&s);
    const uint8_t msg[] = {0xC0, 0x01, 0x03, 0x01, 25, 16, 0x01, 0x02, 0x03, 0x01};
    feed(&s, msg, sizeof(msg));

    scsi_msg_result_t r;
    scsi_msg_complete(&s, &SYM_CAPS, &r);
    ASSERT_TRUE(r.identify && r.sdtr && r.wdtr);
    ASSERT_EQ_INT(r.period, 25);
    ASSERT_EQ_INT(r.offset, 16);
    ASSERT_TRUE(r.wide);
    ASSERT_EQ_INT(s.out_len, 0);
}

int main(void) {
    RUN(identify_alone_is_consumed_with_no_reply);
    RUN(sdtr_within_the_parts_limits_is_agreed_verbatim);
    RUN(sdtr_offset_is_clamped_to_what_the_part_can_express);
    RUN(sdtr_period_floor_slows_an_offer_the_part_cannot_run);
    RUN(sdtr_slower_than_the_floor_is_left_alone);
    RUN(wdtr_is_agreed_by_a_wide_part);
    RUN(wdtr_to_a_narrow_part_is_answered_eight_bit_not_dropped);
    RUN(unknown_extended_message_is_rejected);
    RUN(message_reject_from_the_initiator_is_reported);
    RUN(a_half_arrived_extended_message_waits_for_the_rest);
    RUN(a_length_byte_past_the_end_does_not_read_out_of_bounds);
    RUN(collect_reports_a_full_buffer_instead_of_dropping_silently);
    RUN(reset_clears_a_reply_that_was_never_read);
    RUN(a_second_negotiation_replaces_the_first_reply);
    RUN(identify_then_sdtr_in_one_stream_is_parsed_in_order);
    printf("[scsi_msgsession] all tests passed\n");
    return 0;
}
