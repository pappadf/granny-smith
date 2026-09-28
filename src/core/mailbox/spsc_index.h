// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// spsc_index.h
// The index arithmetic every single-producer / single-consumer ring in the
// tree computes by hand: free-running unsigned counters with exactly one
// writer each (the producer owns HEAD, the consumer owns TAIL), a
// power-of-two size, and the slot of a counter being `counter & (size - 1)`
// -- the same function of the same integer on both sides, across the 2^32
// wrap too.  Bytes for the record rings (mailbox_ring.h), samples for the
// audio and microphone rings, commands for the Voodoo2 raster thread: the
// unit differs, the arithmetic does not.
//
// Nothing here is atomic: the caller loads the other side's counter with
// acquire semantics and publishes its own with release (or seq_cst) and
// passes the values in.  What this header guarantees is that "how much is
// there", "how much fits" and "where is it" are written once.

#ifndef SPSC_INDEX_H
#define SPSC_INDEX_H

#include <stdbool.h>
#include <stdint.h>

// True when `size` is a usable ring size: a non-zero power of two.
static inline bool spsc_size_ok(uint32_t size) {
    return size != 0 && (size & (size - 1u)) == 0;
}

// Units published by the producer and not yet consumed: head - tail, mod
// 2^32.  Valid while the producer never runs more than `size` ahead, which
// spsc_room enforces on its side.
static inline uint32_t spsc_avail(uint32_t head, uint32_t tail) {
    return head - tail;
}

// Units the producer may still write before the ring is full.
static inline uint32_t spsc_room(uint32_t size, uint32_t head, uint32_t tail) {
    return size - spsc_avail(head, tail);
}

// The slot a free-running counter addresses.
static inline uint32_t spsc_slot(uint32_t counter, uint32_t size) {
    return counter & (size - 1u);
}

// Units from `counter`'s slot to the end of the ring: how much can be
// written or read contiguously before the ring wraps.
static inline uint32_t spsc_to_end(uint32_t counter, uint32_t size) {
    return size - spsc_slot(counter, size);
}

#endif // SPSC_INDEX_H
