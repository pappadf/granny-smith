// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for byte_fifo.h, the shared ring behind the NCR 53C96's and Apple
// MESH's SCSI FIFOs.
//
// The point of these is the wrap.  Both chips previously carried their own copy
// of `(rd + n) % depth`, and both were only ever exercised through their
// register interfaces -- which cannot tell a correct ring from one whose index
// errors cancel out across a whole transfer.  So the tests below drive the ring
// past its wrap point repeatedly and check byte IDENTITY and ORDER, not just
// counts.

#include "byte_fifo.h"
#include "test_assert.h"

#include <string.h>

typedef BYTE_FIFO(16) fifo16_t; // the depth both real users have

// ---- the basics ------------------------------------------------------------

TEST(push_pop_preserves_order) {
    fifo16_t f;
    byte_fifo_clear(&f);
    ASSERT_TRUE(byte_fifo_empty(&f));

    for (uint8_t i = 0; i < 5; i++)
        ASSERT_TRUE(byte_fifo_push(&f, (uint8_t)(0xA0 + i)));
    ASSERT_EQ_INT(byte_fifo_count(&f), 5);

    for (uint8_t i = 0; i < 5; i++) {
        uint8_t v = 0xFF;
        ASSERT_TRUE(byte_fifo_pop(&f, &v));
        ASSERT_EQ_INT(v, 0xA0 + i); // FIFO, not LIFO
    }
    ASSERT_TRUE(byte_fifo_empty(&f));
}

TEST(all_slots_are_usable) {
    // Not 15-of-16: this ring tracks a count rather than sacrificing a slot to
    // distinguish full from empty, which is exactly how it differs from the
    // Lisa COPS queue that deliberately did not adopt it.
    fifo16_t f;
    byte_fifo_clear(&f);
    for (int i = 0; i < 16; i++)
        ASSERT_TRUE(byte_fifo_push(&f, (uint8_t)i));
    ASSERT_EQ_INT(byte_fifo_count(&f), 16);
    ASSERT_TRUE(byte_fifo_full(&f));
}

// ---- the wrap, which is the whole reason this is shared ---------------------

TEST(wraps_without_losing_or_reordering_bytes) {
    fifo16_t f;
    byte_fifo_clear(&f);

    // Offset the bottom so every subsequent operation straddles the wrap.
    for (int i = 0; i < 11; i++)
        ASSERT_TRUE(byte_fifo_push(&f, 0xEE));
    for (int i = 0; i < 11; i++) {
        uint8_t junk;
        ASSERT_TRUE(byte_fifo_pop(&f, &junk));
    }
    ASSERT_TRUE(byte_fifo_empty(&f));

    // Now run 200 bytes through a 16-deep ring in staggered batches, so the
    // read and write cursors wrap independently and repeatedly.
    uint8_t next_in = 0, next_out = 0;
    for (int round = 0; round < 40; round++) {
        int batch = (round % 7) + 1; // 1..7, never a divisor of 16
        for (int i = 0; i < batch; i++)
            if (byte_fifo_push(&f, next_in))
                next_in++;
        for (int i = 0; i < batch; i++) {
            uint8_t v = 0xFF;
            if (!byte_fifo_pop(&f, &v))
                break;
            ASSERT_EQ_INT(v, next_out); // every byte, in order, across wraps
            next_out++;
        }
    }
    ASSERT_TRUE(next_out > 100); // the loop really did run the ring around
}

// ---- the ends: reported, never silently applied -----------------------------

TEST(push_on_full_reports_and_stores_nothing) {
    fifo16_t f;
    byte_fifo_clear(&f);
    for (int i = 0; i < 16; i++)
        ASSERT_TRUE(byte_fifo_push(&f, (uint8_t)i));

    // The 17th must be refused, not silently overwrite the bottom -- the 53C96
    // turns this false into a documented gross error (ST_GE).
    ASSERT_TRUE(!byte_fifo_push(&f, 0x99));
    ASSERT_EQ_INT(byte_fifo_count(&f), 16);

    // And the refusal did not corrupt what was already held.
    for (int i = 0; i < 16; i++) {
        uint8_t v = 0xFF;
        ASSERT_TRUE(byte_fifo_pop(&f, &v));
        ASSERT_EQ_INT(v, i);
    }
}

TEST(pop_on_empty_reports_and_leaves_out_alone) {
    fifo16_t f;
    byte_fifo_clear(&f);

    // *out untouched is what lets the 53C96 substitute its bottom register and
    // MESH substitute zero -- two different documented-vs-invented policies
    // that only work if the ring itself does not pick one.
    uint8_t v = 0x5A;
    ASSERT_TRUE(!byte_fifo_pop(&f, &v));
    ASSERT_EQ_INT(v, 0x5A);
    ASSERT_EQ_INT(byte_fifo_count(&f), 0);
}

// ---- peek ------------------------------------------------------------------

TEST(peek_reads_across_the_wrap_without_consuming) {
    fifo16_t f;
    byte_fifo_clear(&f);

    // Push the bottom to 14, then fill past the end of the array.
    for (int i = 0; i < 14; i++)
        ASSERT_TRUE(byte_fifo_push(&f, 0xEE));
    for (int i = 0; i < 14; i++) {
        uint8_t junk;
        ASSERT_TRUE(byte_fifo_pop(&f, &junk));
    }
    for (uint8_t i = 0; i < 5; i++)
        ASSERT_TRUE(byte_fifo_push(&f, (uint8_t)(0x10 + i))); // spans buf[14], [15], [0]..

    for (uint8_t i = 0; i < 5; i++)
        ASSERT_EQ_INT(byte_fifo_peek(&f, i), 0x10 + i);
    ASSERT_EQ_INT(byte_fifo_count(&f), 5); // peeking consumed nothing

    // Past the count reads 0 rather than stale ring contents -- MESH traces the
    // first four bytes whether or not four are held.
    ASSERT_EQ_INT(byte_fifo_peek(&f, 5), 0);
    ASSERT_EQ_INT(byte_fifo_peek(&f, 200), 0);
}

// ---- clear -----------------------------------------------------------------

TEST(clear_resets_cursors_but_keeps_bytes) {
    // The 53C96 relies on this split: its manual (ch. 4, FIFO Register) says
    // reset zeroes the bottom element and the flags while "the contents of the
    // rest of the FIFO are not changed", so the chip zeroes buf[0] itself.
    fifo16_t f;
    byte_fifo_clear(&f);
    for (uint8_t i = 0; i < 8; i++)
        ASSERT_TRUE(byte_fifo_push(&f, (uint8_t)(0xC0 + i)));

    byte_fifo_clear(&f);
    ASSERT_TRUE(byte_fifo_empty(&f));
    ASSERT_EQ_INT(f.rd, 0);
    ASSERT_EQ_INT(f.buf[3], 0xC3); // untouched
}

// ---- depth independence ----------------------------------------------------

TEST(depth_comes_from_the_array_not_a_stored_field) {
    // sizeof-derived depth is why there is no depth field to drift.  A 4-deep
    // ring must wrap at 4 using the identical code path as the 16-deep one.
    BYTE_FIFO(4) f;
    byte_fifo_clear(&f);
    ASSERT_TRUE(sizeof(f) == 4 + 2); // buf + rd + n, no padding, no pointer

    for (int i = 0; i < 4; i++)
        ASSERT_TRUE(byte_fifo_push(&f, (uint8_t)i));
    ASSERT_TRUE(!byte_fifo_push(&f, 0xFF));

    uint8_t v;
    ASSERT_TRUE(byte_fifo_pop(&f, &v));
    ASSERT_EQ_INT(v, 0);
    ASSERT_TRUE(byte_fifo_push(&f, 0x77)); // reuses the freed slot
    for (int i = 1; i < 4; i++) {
        ASSERT_TRUE(byte_fifo_pop(&f, &v));
        ASSERT_EQ_INT(v, i);
    }
    ASSERT_TRUE(byte_fifo_pop(&f, &v));
    ASSERT_EQ_INT(v, 0x77);
}

int main(void) {
    RUN(push_pop_preserves_order);
    RUN(all_slots_are_usable);
    RUN(wraps_without_losing_or_reordering_bytes);
    RUN(push_on_full_reports_and_stores_nothing);
    RUN(pop_on_empty_reports_and_leaves_out_alone);
    RUN(peek_reads_across_the_wrap_without_consuming);
    RUN(clear_resets_cursors_but_keeps_bytes);
    RUN(depth_comes_from_the_array_not_a_stored_field);
    printf("[byte_fifo] all tests passed\n");
    return 0;
}
