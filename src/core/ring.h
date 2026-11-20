// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ring.h
// A fixed-depth ring of typed elements: byte_fifo.h's twin for queues whose
// element is a struct or a level rather than a byte (a mouse event, a button
// level).
//
// The same representation as BYTE_FIFO -- the bottom index `rd` and the
// count `n`, so every slot is usable -- and the same rules: POD with no
// pointers, so a ring sits inside a device's checkpointed prefix; the depth
// comes from the array, so it cannot drift; and policy (what a full ring
// drops or merges) stays with the caller.  Helpers are macros because they
// work on any element type.

#ifndef RING_H
#define RING_H

#include <stdbool.h>
#include <stdint.h>

// Declare a ring member of `depth` elements of `type`:
//     RING(struct ev, 16) q;   ->   q.buf[16], q.rd, q.n
#define RING(type, depth)                                                                                              \
    struct {                                                                                                           \
        type buf[depth];                                                                                               \
        uint8_t rd, n;                                                                                                 \
        _Static_assert((depth) > 0 && (depth) <= 255, "RING depth must fit its uint8_t cursors");                      \
    }

// The ring's depth, from its array
#define ring_depth(r) ((unsigned)(sizeof((r)->buf) / sizeof((r)->buf[0])))

#define ring_count(r) ((unsigned)(r)->n)
#define ring_empty(r) ((r)->n == 0)
#define ring_full(r)  ((unsigned)(r)->n >= ring_depth(r))

// The element `i` positions above the bottom (0 = the oldest), as an lvalue.
// The caller keeps `i` below the count.
#define ring_at(r, i) ((r)->buf[((unsigned)(r)->rd + (unsigned)(i)) % ring_depth(r)])

// The newest element, as an lvalue; the ring must not be empty
#define ring_back(r) ring_at((r), (r)->n - 1u)

// Append `v`; false (and nothing stored) when the ring is full
#define ring_push(r, v) (ring_full(r) ? false : (ring_at((r), (r)->n) = (v), (r)->n++, true))

// Drop the oldest element; the ring must not be empty
#define ring_drop(r)                                                                                                   \
    do {                                                                                                               \
        (r)->rd = (uint8_t)(((unsigned)(r)->rd + 1u) % ring_depth(r));                                                 \
        (r)->n--;                                                                                                      \
    } while (0)

// Empty the ring (the elements are left as they are)
#define ring_clear(r)                                                                                                  \
    do {                                                                                                               \
        (r)->rd = 0;                                                                                                   \
        (r)->n = 0;                                                                                                    \
    } while (0)

// True when the cursors are in range: what a restore checks before using a
// ring read from a file the user supplies
#define ring_valid(r) ((unsigned)(r)->rd < ring_depth(r) && (unsigned)(r)->n <= ring_depth(r))

#endif // RING_H
