// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for em_mic_ring.c: the microphone ring's consumer arithmetic
// (T2), at the uint32 wrap of the free-running indices.  The producer half
// (app/web2 micRing.ts) is tested at the same wrap by vitest.

#include "em_mic_ring.h"
#include "test_assert.h"

#include <stdint.h>

#define LEN  32768u
#define HALF (LEN / 2)

TEST(slot_is_the_index_masked_across_the_wrap) {
    ASSERT_EQ_INT(mic_ring_slot(0, LEN), 0);
    ASSERT_EQ_INT(mic_ring_slot(LEN + 5, LEN), 5);
    ASSERT_EQ_INT(mic_ring_slot(0x7FFFFFFFu, LEN), LEN - 1);
    ASSERT_EQ_INT(mic_ring_slot(0x80000000u, LEN), 0); // where the JS Int32 turns negative
    ASSERT_EQ_INT(mic_ring_slot(0xFFFFFFFFu, LEN), LEN - 1);
    ASSERT_EQ_INT(mic_ring_slot(0xFFFFFFFFu + 1u, LEN), 0);
}

TEST(enough_samples_are_taken_from_rd) {
    mic_take_t t = mic_ring_take(1000, 200, LEN, 480);
    ASSERT_TRUE(t.ok);
    ASSERT_TRUE(!t.overrun);
    ASSERT_EQ_INT(t.start, 200);
    ASSERT_EQ_INT(t.need, 480);
}

TEST(too_few_is_an_underrun_and_rd_stays) {
    mic_take_t t = mic_ring_take(300, 200, LEN, 480);
    ASSERT_TRUE(!t.ok);
    ASSERT_TRUE(!t.overrun);
    ASSERT_EQ_INT(t.start, 200);
}

TEST(the_indices_may_wrap_between_rd_and_wr) {
    uint32_t rd = 0xFFFFFF00u;
    uint32_t wr = rd + 1000u; // wrapped past zero
    mic_take_t t = mic_ring_take(wr, rd, LEN, 480);
    ASSERT_TRUE(t.ok);
    ASSERT_TRUE(!t.overrun);
    ASSERT_TRUE(t.start == rd);
    // The request straddles the wrap: slot arithmetic stays contiguous mod LEN.
    ASSERT_EQ_INT(mic_ring_slot(t.start + 0x100u, LEN), 0);
}

TEST(a_backlog_over_half_the_ring_drops_to_the_freshest) {
    uint32_t rd = 0xFFFFF000u;
    uint32_t wr = rd + HALF + 100u;
    mic_take_t t = mic_ring_take(wr, rd, LEN, 480);
    ASSERT_TRUE(t.ok);
    ASSERT_TRUE(t.overrun);
    ASSERT_TRUE(t.start == wr - 480u);
}

TEST(a_drop_never_goes_behind_what_was_consumed) {
    // A request bigger than the backlog: the old `rd = wr - need` moved rd
    // back past samples already read.
    uint32_t rd = 5000, wr = 5000 + 1000;
    mic_take_t t = mic_ring_take(wr, rd, LEN, HALF);
    ASSERT_TRUE(!t.overrun);
    ASSERT_TRUE(!t.ok);
    ASSERT_EQ_INT(t.start, rd);
}

TEST(a_request_is_capped_at_half_the_ring) {
    mic_take_t t = mic_ring_take(LEN, 0, LEN, LEN);
    ASSERT_EQ_INT(t.need, HALF);
    ASSERT_TRUE(t.ok);
    ASSERT_TRUE(t.overrun); // a full ring is a backlog over half
    ASSERT_TRUE(t.start == LEN - HALF);
    ASSERT_EQ_INT(mic_ring_take(10, 0, LEN, 0).need, 1);
}

TEST(a_lapped_consumer_resyncs) {
    // The producer ran a whole ring and more ahead: the old samples are gone.
    uint32_t rd = 0xFFFFFFF0u;
    uint32_t wr = rd + 3u * LEN;
    mic_take_t t = mic_ring_take(wr, rd, LEN, 480);
    ASSERT_TRUE(t.ok && t.overrun);
    ASSERT_TRUE(t.start == wr - 480u);
}

int main(void) {
    RUN(slot_is_the_index_masked_across_the_wrap);
    RUN(enough_samples_are_taken_from_rd);
    RUN(too_few_is_an_underrun_and_rd_stays);
    RUN(the_indices_may_wrap_between_rd_and_wr);
    RUN(a_backlog_over_half_the_ring_drops_to_the_freshest);
    RUN(a_drop_never_goes_behind_what_was_consumed);
    RUN(a_request_is_capped_at_half_the_ring);
    RUN(a_lapped_consumer_resyncs);
    return 0;
}
