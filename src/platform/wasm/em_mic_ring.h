// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_mic_ring.h
// The microphone ring's consumer arithmetic, pure so it is unit-tested
// natively (tests/unit/suites/mic_ring) at the index wrap.  em_audio_in.c
// does the atomics and the copy around it; app/web2's micRing.ts is the
// producer half.
//
// The indices are free-running uint32; the ring length is a power of two,
// so a slot is `index & (len - 1)` on both sides -- the same function of the
// same integer in C and in JS, across the 2^32 wrap too.  (The ring used to
// be 24576 long, indexed with `%`: JS's `%` of a negative Int32 is negative,
// so after 2^31 samples the producer wrote up to ~48 KB BEFORE the ring.)

#ifndef EM_MIC_RING_H
#define EM_MIC_RING_H

#include <stdbool.h>
#include <stdint.h>

// Where the consumer reads a request from.
typedef struct mic_take {
    bool ok; // `need` samples are available from `start`
    bool overrun; // the backlog passed half the ring and was dropped
    uint32_t start; // first sample, free-running
    uint32_t need; // samples to read: the request, capped at half the ring
} mic_take_t;

// A request for `need` samples against producer index `wr` and consumer
// index `rd`.  The browser captures on wall time and the guest consumes on
// emulated time, so the producer can get ahead; a backlog over half the ring
// is dropped down to the freshest `need` samples (rd = wr - min(need, avail))
// -- never behind what was already consumed, which the old `rd = wr - need`
// could reach when `need` exceeded the backlog.
mic_take_t mic_ring_take(uint32_t wr, uint32_t rd, uint32_t ring_len, uint32_t need);

// The slot of a free-running index.
static inline uint32_t mic_ring_slot(uint32_t index, uint32_t ring_len) {
    return index & (ring_len - 1);
}

#endif // EM_MIC_RING_H
