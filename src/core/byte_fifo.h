// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// byte_fifo.h
// A fixed-depth ring of bytes, for the small hardware FIFOs that sit between a
// chip and a bus.
//
// Deliberately NOT named after SCSI, though that is where both current users
// live: the structure is a plain circular buffer and naming it for its first
// caller is what stops the next one adopting it.  Today's users are the NCR
// 53C96 and Apple's MESH -- unrelated silicon (MESH is Apple's own cell, part
// 343S1146, not a 53C9x core) that both happen to need a 16-byte byte FIFO on
// the SCSI side, and carried their own identical copy of the wrap arithmetic.
//
// Two rings in the tree deliberately do NOT use this:
//
//   * The 53C825's `dfifo` (4 lanes x 134) looks identical but is not a data
//     path at all -- nothing but CTEST1/CTEST3/CTEST6 and DSTAT's DFE bit ever
//     touches it.  It models the observable behaviour of the DMA FIFO for the
//     Network Server Diagnostic Utility; the real SCRIPTS transfers go through
//     a local buffer.  Sharing a type with it would assert an abstraction that
//     is not there.
//
//   * The Lisa COPS response queue (cops.c) is the classic sacrifice-one-slot
//     head/tail ring, so its usable depth is 31 of 32.  Converting it would
//     silently give it 32 -- a behavioural change, not a refactor.  It is a
//     fine future adopter, but only as its own decision.
//
// Policy is the CALLER's.  push and pop report full/empty and change nothing,
// because what a chip does then is chip behaviour and differs even between the
// two users: the 53C96 raises a documented gross error on overflow and re-reads
// the bottom register when empty (NCR 53C94/95/96 data manual, FIFO Register,
// read/write address 02), while MESH's behaviour in either case is documented
// nowhere and is our own defensive choice.
//
// Macros rather than functions taking (buf, depth, &rd, &n): the depth comes
// from sizeof so it cannot drift from the array, and nothing redundant has to
// be stored.  That matters because these live inside chip structs whose POD
// prefix is written to a checkpoint in one block -- a `uint8_t *buf` field
// would put a host pointer in the saved image, which is the exact bug this
// codebase has had to remove before.

#ifndef BYTE_FIFO_H
#define BYTE_FIFO_H

#include <stdbool.h>
#include <stdint.h>

// Declare a ring member.  `rd` is the bottom (the next byte out) and `n` the
// count held, so all `depth` slots are usable.  POD, no pointers: safe to sit
// in the block a checkpoint writes with offsetof().
//
//     BYTE_FIFO(16) fifo;   ->   fifo.buf[16], fifo.rd, fifo.n
#define BYTE_FIFO(depth)                                                                                               \
    struct {                                                                                                           \
        uint8_t buf[depth];                                                                                            \
        uint8_t rd, n;                                                                                                 \
    }

// Store a byte.  Returns false and stores nothing if the ring is full.
#define byte_fifo_push(f, v) byte_fifo_push_((f)->buf, (uint8_t)sizeof(f)->buf, &(f)->rd, &(f)->n, (v))

// Take the bottom byte into *out.  Returns false and leaves *out alone if the
// ring is empty -- the caller decides what an empty read yields.
#define byte_fifo_pop(f, out) byte_fifo_pop_((f)->buf, (uint8_t)sizeof(f)->buf, &(f)->rd, &(f)->n, (out))

// The byte `i` positions above the bottom, without removing it.  Reading at or
// beyond the count yields 0.
#define byte_fifo_peek(f, i) byte_fifo_peek_((f)->buf, (uint8_t)sizeof(f)->buf, (f)->rd, (f)->n, (i))

#define byte_fifo_count(f) ((f)->n)
#define byte_fifo_full(f)  ((f)->n >= (uint8_t)sizeof(f)->buf)
#define byte_fifo_empty(f) ((f)->n == 0)

// Reset the cursors.  Does NOT touch the bytes: a chip whose reset is specified
// to clear an element does that itself, which is the 53C96's bottom-element
// rule (same manual section as above).
#define byte_fifo_clear(f)                                                                                             \
    do {                                                                                                               \
        (f)->rd = 0;                                                                                                   \
        (f)->n = 0;                                                                                                    \
    } while (0)

static inline bool byte_fifo_push_(uint8_t *buf, uint8_t depth, uint8_t *rd, uint8_t *n, uint8_t v) {
    if (*n >= depth)
        return false;
    buf[(uint8_t)((*rd + *n) % depth)] = v;
    (*n)++;
    return true;
}

static inline bool byte_fifo_pop_(const uint8_t *buf, uint8_t depth, uint8_t *rd, uint8_t *n, uint8_t *out) {
    if (*n == 0)
        return false;
    *out = buf[*rd];
    *rd = (uint8_t)((*rd + 1) % depth);
    (*n)--;
    return true;
}

static inline uint8_t byte_fifo_peek_(const uint8_t *buf, uint8_t depth, uint8_t rd, uint8_t n, uint8_t i) {
    if (i >= n)
        return 0;
    return buf[(uint8_t)((rd + i) % depth)];
}

#endif // BYTE_FIFO_H
