// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// byteq.h: order, the bound, and reclaiming the consumed head.  See Makefile.

#include "byteq.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

// Bytes come out in the order they went in, across any interleaving of
// appends and reads, and the held bytes are contiguous from byteq_data().
TEST(bytes_come_out_in_order) {
    byteq_t q = {0};
    static uint8_t model[1 << 16];
    size_t model_head = 0, model_tail = 0;
    uint8_t next = 0, chunk[97];
    for (int round = 0; round < 2000; round++) {
        size_t in = (size_t)(round * 37 % 61) + 1; // 1..61 bytes in
        for (size_t i = 0; i < in; i++) {
            chunk[i] = next;
            model[model_tail++] = next++;
        }
        ASSERT_TRUE(byteq_append(&q, chunk, in, 1u << 20));
        size_t out = (size_t)(round * 53 % 97); // 0..96 bytes out
        size_t got = byteq_read(&q, chunk, out);
        ASSERT_EQ_INT((int)(out < model_tail - model_head ? out : model_tail - model_head), (int)got);
        ASSERT_EQ_INT(0, memcmp(chunk, model + model_head, got));
        model_head += got;
        ASSERT_EQ_INT((int)(model_tail - model_head), (int)byteq_len(&q));
        if (byteq_len(&q))
            ASSERT_EQ_INT(0, memcmp(byteq_data(&q), model + model_head, byteq_len(&q)));
    }
    byteq_free(&q);
    ASSERT_EQ_INT(0, (int)byteq_len(&q));
    ASSERT_TRUE(byteq_data(&q) == NULL);
}

// An append that would pass the bound appends nothing; up to it is fine.
TEST(the_bound_holds) {
    byteq_t q = {0};
    uint8_t buf[100];
    memset(buf, 7, sizeof buf);
    ASSERT_TRUE(byteq_append(&q, buf, 60, 100));
    ASSERT_TRUE(!byteq_append(&q, buf, 41, 100));
    ASSERT_EQ_INT(60, (int)byteq_len(&q));
    ASSERT_TRUE(byteq_append(&q, buf, 40, 100));
    ASSERT_TRUE(!byteq_append(&q, buf, 1, 100));
    byteq_consume(&q, 50); // room again once bytes are taken
    ASSERT_TRUE(byteq_append(&q, buf, 50, 100));
    ASSERT_TRUE(!byteq_append(&q, buf, (size_t)-1, (size_t)-2)); // no overflow in the check
    byteq_free(&q);
}

// Taking bytes moves the head, not the bytes; the head is reclaimed once it
// passes half the buffer, and at once when the queue empties -- so draining
// in small pieces is linear, and the buffer does not creep.
TEST(the_consumed_head_is_reclaimed) {
    byteq_t q = {0};
    uint8_t buf[4096];
    memset(buf, 1, sizeof buf);
    ASSERT_TRUE(byteq_append(&q, buf, sizeof buf, 1u << 20));
    size_t cap = q.cap;
    byteq_consume(&q, 512);
    ASSERT_EQ_INT(512, (int)q.head); // moved, not copied down
    byteq_consume(&q, cap / 2); // past half: the remainder moves down
    ASSERT_EQ_INT(0, (int)q.head);
    ASSERT_EQ_INT((int)(sizeof buf - 512 - cap / 2), (int)byteq_len(&q));
    for (int i = 0; i < 1000; i++) { // a steady trickle never grows it
        ASSERT_TRUE(byteq_append(&q, buf, 100, 1u << 20));
        byteq_consume(&q, 100);
    }
    ASSERT_EQ_INT((int)cap, (int)q.cap);
    byteq_consume(&q, byteq_len(&q));
    ASSERT_EQ_INT(0, (int)q.head);
    byteq_clear(&q);
    byteq_free(&q);
}

int main(void) {
    RUN(bytes_come_out_in_order);
    RUN(the_bound_holds);
    RUN(the_consumed_head_is_reclaimed);
    printf("byteq: all tests passed\n");
    return 0;
}
