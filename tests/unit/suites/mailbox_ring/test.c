// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the record-ring primitive (mailbox_ring.c) and the index
// arithmetic it sits on (spsc_index.h).  A writer and a reader share one
// small ring in one process; the tests are the rules the header states:
// records are multiples of 8, never wrap, a PAD reaches the end, no room is
// reported without side effects, corrupt framing is reported, and the free
// running counters work across the 2^32 wrap.

#include "common.h"
#include "test_assert.h"
#include "mailbox/mailbox_ring.h"
#include "mailbox/spsc_index.h"

#include <stdint.h>
#include <string.h>

#define RING 256u
#define HEAD 6
#define TAIL 7

static volatile uint32_t ctrl[32];
static uint8_t ring_bytes[RING + 64] __attribute__((aligned(64)));
static mbx_ring_t w, r; // the writer's and the reader's view of the same ring

static void fresh(void) {
    memset((void *)ctrl, 0, sizeof(ctrl));
    memset(ring_bytes, 0xEE, sizeof(ring_bytes));
    mbx_ring_init(&w, ctrl, HEAD, TAIL, ring_bytes, RING);
    mbx_ring_init(&r, ctrl, HEAD, TAIL, ring_bytes, RING);
}

// Writes one record of `payload` bytes of value `fill`; returns its offset.
static uint32_t put(uint32_t kind, uint32_t payload, uint8_t fill) {
    uint32_t at = mbx_reserve(&w, kind, MBX_HDR_BYTES + payload);
    if (at != UINT32_MAX)
        memset(mbx_payload(&w, at), fill, payload);
    return at;
}

// Reads the next non-PAD record, consuming PADs on the way.  Returns 1 with
// rec filled, 0 for nothing, -1 for corrupt.
static int get(mbx_rec_t *rec) {
    for (;;) {
        uint32_t head = mbx_load(ctrl, HEAD);
        int got = mbx_next(&r, head, rec);
        if (got <= 0)
            return got;
        if (rec->kind == MBX_R_PAD) {
            mbx_consume(&r, rec);
            continue;
        }
        return 1;
    }
}

TEST(spsc_arithmetic_holds_across_the_wrap) {
    ASSERT_TRUE(spsc_size_ok(256) && spsc_size_ok(1) && !spsc_size_ok(0) && !spsc_size_ok(24576));
    ASSERT_EQ_INT(spsc_avail(10, 4), 6);
    ASSERT_EQ_INT(spsc_avail(3, 0xFFFFFFFDu), 6); // head wrapped past tail
    ASSERT_EQ_INT(spsc_room(256, 3, 0xFFFFFFFDu), 250);
    ASSERT_EQ_INT(spsc_slot(0xFFFFFFFFu, 256), 255);
    ASSERT_EQ_INT(spsc_slot(0xFFFFFFFFu + 1u, 256), 0); // the counter wrapped
    ASSERT_EQ_INT(spsc_to_end(0xFFFFFFF8u, 256), 8);
}

TEST(a_record_round_trips_and_len_is_a_multiple_of_8) {
    fresh();
    uint32_t at = put(7, 5, 0xAB); // 8 + 5 = 13 -> 16
    ASSERT_EQ_INT(at, 0);
    ASSERT_TRUE(mbx_unpublished(&w)); // reserved, not published
    mbx_rec_t rec;
    ASSERT_EQ_INT(get(&rec), 0); // nothing published yet
    mbx_publish(&w);
    ASSERT_EQ_INT(mbx_load(ctrl, HEAD), 16);
    ASSERT_EQ_INT(get(&rec), 1);
    ASSERT_EQ_INT(rec.kind, 7);
    ASSERT_EQ_INT(rec.len, 16);
    ASSERT_EQ_INT(mbx_rec_payload(&r, &rec)[0], 0xAB);
    mbx_consume(&r, &rec);
    ASSERT_EQ_INT(mbx_load(ctrl, TAIL), 16);
    ASSERT_EQ_INT(get(&rec), 0);
}

TEST(a_record_that_would_cross_the_end_gets_a_pad_first) {
    fresh();
    // Fill to 8 bytes short of the end: 31 records of 8 = 248.
    for (int i = 0; i < 31; i++)
        ASSERT_TRUE(put(1, 0, 0) != UINT32_MAX);
    mbx_publish(&w);
    mbx_rec_t rec;
    for (int i = 0; i < 31; i++) {
        ASSERT_EQ_INT(get(&rec), 1);
        mbx_consume(&r, &rec);
    }
    // A 16-byte record does not fit in the 8 that remain: PAD, then it starts at 0.
    uint32_t at = put(2, 8, 0x5A);
    ASSERT_EQ_INT(at, 0);
    mbx_publish(&w);
    // The reader sees the PAD (8 bytes, to the end) and then the record.
    uint32_t head = mbx_load(ctrl, HEAD);
    ASSERT_EQ_INT(mbx_next(&r, head, &rec), 1);
    ASSERT_EQ_INT(rec.kind, MBX_R_PAD);
    ASSERT_EQ_INT(rec.len, 8);
    ASSERT_EQ_INT(rec.at, 248);
    mbx_consume(&r, &rec);
    ASSERT_EQ_INT(mbx_next(&r, head, &rec), 1);
    ASSERT_EQ_INT(rec.kind, 2);
    ASSERT_EQ_INT(rec.at, 0);
    ASSERT_EQ_INT(rec.len, 16);
    mbx_consume(&r, &rec);
    ASSERT_EQ_INT(mbx_load(ctrl, TAIL), 248 + 8 + 16);
}

TEST(no_room_is_reported_without_side_effects) {
    fresh();
    // The ring is 256: two 120-byte records fill 240; a third of 120 needs a
    // 16-byte PAD plus 120 = 136 > 16 free -> no room, nothing written.
    ASSERT_TRUE(put(1, 112, 1) != UINT32_MAX);
    ASSERT_TRUE(put(1, 112, 2) != UINT32_MAX);
    uint32_t wr_before = w.wr;
    uint8_t snapshot[RING];
    memcpy(snapshot, ring_bytes, RING);
    ASSERT_EQ_INT(put(1, 112, 3), UINT32_MAX);
    ASSERT_EQ_INT(w.wr, wr_before);
    ASSERT_TRUE(memcmp(snapshot, ring_bytes, RING) == 0);
    // A record larger than the ring is never reservable.
    ASSERT_EQ_INT(mbx_reserve(&w, 1, RING + 8), UINT32_MAX);
    // Consuming makes room again.
    mbx_publish(&w);
    mbx_rec_t rec;
    ASSERT_EQ_INT(get(&rec), 1);
    mbx_consume(&r, &rec);
    ASSERT_TRUE(put(1, 112, 3) != UINT32_MAX);
}

TEST(shrink_last_gives_the_unused_reservation_back) {
    fresh();
    uint32_t at = put(9, 120, 0x11); // reserve the maximum, 128 bytes
    ASSERT_EQ_INT(at, 0);
    ASSERT_EQ_INT(w.wr, 128);
    mbx_shrink_last(&w, MBX_HDR_BYTES + 20); // 28 -> 32
    ASSERT_EQ_INT(w.wr, 32);
    ASSERT_EQ_INT(put(3, 0, 0), 32); // the next record starts right after
    mbx_publish(&w);
    mbx_rec_t rec;
    ASSERT_EQ_INT(get(&rec), 1);
    ASSERT_EQ_INT(rec.kind, 9);
    ASSERT_EQ_INT(rec.len, 32);
    mbx_consume(&r, &rec);
    ASSERT_EQ_INT(get(&rec), 1);
    ASSERT_EQ_INT(rec.kind, 3);
    // Shrinking never grows.
    mbx_shrink_last(&w, 1000);
    ASSERT_EQ_INT(w.wr, 40);
}

TEST(corrupt_framing_is_reported_and_abandon_skips_it) {
    fresh();
    ASSERT_TRUE(put(4, 8, 0) != UINT32_MAX);
    mbx_publish(&w);
    // A len that is not a multiple of 8.
    WR_LE32(ring_bytes + 4, 12);
    mbx_rec_t rec;
    ASSERT_EQ_INT(get(&rec), -1);
    // A len beyond what was published.
    WR_LE32(ring_bytes + 4, 64);
    ASSERT_EQ_INT(get(&rec), -1);
    // A len under the header.
    WR_LE32(ring_bytes + 4, 0);
    ASSERT_EQ_INT(get(&rec), -1);
    // Abandon: the reader moves to head and sees nothing more.
    mbx_abandon(&r, mbx_load(ctrl, HEAD));
    ASSERT_EQ_INT(get(&rec), 0);
    ASSERT_EQ_INT(r.rd, 16);
}

TEST(many_records_keep_order_across_thousands_of_wraps) {
    fresh();
    uint32_t next_put = 0, next_get = 0;
    mbx_rec_t rec;
    for (int i = 0; i < 20000; i++) {
        // Write with varying sizes until full, then drain some, and check
        // that every record comes out in order with its own number.
        uint32_t payload = (uint32_t)(i * 7) % 40u;
        uint32_t at = mbx_reserve(&w, 100 + (next_put & 0xFF), MBX_HDR_BYTES + 4 + payload);
        if (at == UINT32_MAX) {
            mbx_publish(&w);
            for (int k = 0; k < 3; k++) {
                if (get(&rec) != 1)
                    break;
                ASSERT_EQ_INT(RD_LE32(mbx_rec_payload(&r, &rec)), next_get);
                ASSERT_EQ_INT(rec.kind, 100 + (next_get & 0xFF));
                ASSERT_EQ_INT(rec.len & 7u, 0);
                ASSERT_TRUE(rec.at + rec.len <= RING);
                next_get++;
                mbx_consume(&r, &rec);
            }
            continue;
        }
        WR_LE32(mbx_payload(&w, at), next_put);
        next_put++;
    }
    mbx_publish(&w);
    while (get(&rec) == 1) {
        ASSERT_EQ_INT(RD_LE32(mbx_rec_payload(&r, &rec)), next_get);
        next_get++;
        mbx_consume(&r, &rec);
    }
    ASSERT_EQ_INT(next_get, next_put);
    ASSERT_TRUE(next_put > 5000);
}

TEST(the_counters_work_when_they_start_near_the_wrap) {
    fresh();
    // Both sides agree to start at 2^32 - 40: the ring slot is 216.
    w.wr = 0xFFFFFFD8u;
    r.rd = 0xFFFFFFD8u;
    mbx_store(ctrl, TAIL, r.rd);
    mbx_store(ctrl, HEAD, w.wr);
    ASSERT_EQ_INT(put(5, 24, 0x77), 216); // 32 bytes: fits before the end exactly
    ASSERT_EQ_INT(put(6, 0, 0), 248); // 8 bytes: the last slot
    ASSERT_EQ_INT(put(8, 0, 0), 0); // wraps the counter past 2^32
    ASSERT_EQ_INT(w.wr, 8);
    mbx_publish(&w);
    mbx_rec_t rec;
    ASSERT_EQ_INT(get(&rec), 1);
    ASSERT_EQ_INT(rec.kind, 5);
    mbx_consume(&r, &rec);
    ASSERT_EQ_INT(get(&rec), 1);
    ASSERT_EQ_INT(rec.kind, 6);
    mbx_consume(&r, &rec);
    ASSERT_EQ_INT(get(&rec), 1);
    ASSERT_EQ_INT(rec.kind, 8);
    ASSERT_EQ_INT(rec.at, 0);
    mbx_consume(&r, &rec);
    ASSERT_EQ_INT(r.rd, 8);
    ASSERT_EQ_INT(mbx_load(ctrl, TAIL), 8);
}

int main(void) {
    RUN(spsc_arithmetic_holds_across_the_wrap);
    RUN(a_record_round_trips_and_len_is_a_multiple_of_8);
    RUN(a_record_that_would_cross_the_end_gets_a_pad_first);
    RUN(no_room_is_reported_without_side_effects);
    RUN(shrink_last_gives_the_unused_reservation_back);
    RUN(corrupt_framing_is_reported_and_abandon_skips_it);
    RUN(many_records_keep_order_across_thousands_of_wraps);
    RUN(the_counters_work_when_they_start_near_the_wrap);
    return 0;
}
