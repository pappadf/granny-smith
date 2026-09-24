// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// byteq.h
// A growable byte queue with a bound: bytes appended at the tail, taken from
// the head.
//
// Taking bytes advances the head rather than moving what is left, so draining
// a queue in small pieces costs linear time -- the copies it replaces moved
// the whole remainder down after every 512-byte read, quadratic in what was
// queued (10-network N-21).  The space is reclaimed when the queue empties,
// or by moving the remainder down once the head has passed half the buffer:
// by then at least as many bytes were taken as are moved.
//
// Every append names its bound, so a queue needs no setup: a zeroed byteq_t
// is empty.  An append that would take the queue past its bound, or cannot
// get the memory, appends nothing and says so; what that means is the
// caller's (drop and count, or abort the job).
//
// Not byte_fifo.h: that is a fixed-depth ring with no heap, for hardware
// FIFOs whose struct is saved into a checkpoint whole.

#ifndef GS_BYTEQ_H
#define GS_BYTEQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *bytes;
    size_t head; // offset of the next byte out
    size_t len; // bytes held, from head
    size_t cap;
} byteq_t;

static inline size_t byteq_len(const byteq_t *q) {
    return q->len;
}

// The bytes held, contiguous from the head; valid until the next append.
static inline const uint8_t *byteq_data(const byteq_t *q) {
    return q->len ? q->bytes + q->head : NULL;
}

// Append `n` bytes, holding at most `max`.  False, and nothing appended, past
// the bound or out of memory.
static inline bool byteq_append(byteq_t *q, const void *data, size_t n, size_t max) {
    if (n == 0)
        return true;
    if (n > max || q->len > max - n)
        return false;
    size_t end = q->head + q->len + n;
    if (end > q->cap) {
        size_t cap = q->cap ? q->cap : 4096;
        while (cap < end)
            cap *= 2;
        uint8_t *grown = (uint8_t *)realloc(q->bytes, cap);
        if (!grown)
            return false;
        q->bytes = grown;
        q->cap = cap;
    }
    memcpy(q->bytes + q->head + q->len, data, n);
    q->len += n;
    return true;
}

// Drop `n` bytes from the head (all of them, if fewer are held).
static inline void byteq_consume(byteq_t *q, size_t n) {
    if (n > q->len)
        n = q->len;
    q->head += n;
    q->len -= n;
    if (q->len == 0) {
        q->head = 0;
    } else if (q->head > q->cap / 2) {
        memmove(q->bytes, q->bytes + q->head, q->len);
        q->head = 0;
    }
}

// Take up to `cap` bytes into `out`; returns how many.
static inline size_t byteq_read(byteq_t *q, void *out, size_t cap) {
    size_t n = cap < q->len ? cap : q->len;
    if (n) {
        memcpy(out, q->bytes + q->head, n);
        byteq_consume(q, n);
    }
    return n;
}

// Empty the queue, keeping its memory.
static inline void byteq_clear(byteq_t *q) {
    q->head = 0;
    q->len = 0;
}

// Empty the queue and release its memory.
static inline void byteq_free(byteq_t *q) {
    free(q->bytes);
    q->bytes = NULL;
    q->head = q->len = q->cap = 0;
}

#endif // GS_BYTEQ_H
