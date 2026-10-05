// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mailbox_ring.h
// The record ring the tree's cross-thread transports share: a byte ring of
// RECORDS {uint32 kind, uint32 len} + payload, in a region both sides can
// see, with the producer's published byte count (HEAD) and the consumer's
// consumed byte count (TAIL) in a control block of uint32 words the other
// side reads with Atomics.  This is the LaserWriter platen transport's ring
// (laserwriter_ring_protocol.h) lifted out so that the Voodoo2 GPU op ring,
// the JS<->core mailbox and the I/O worker's job rings are one
// implementation with one unit suite rather than four copies.
//
// The rules, which the protocol headers state and this file enforces:
//
//   * `len` counts the header and is a MULTIPLE OF 8.  Every record
//     therefore starts at a multiple of 8, so the remainder before the
//     ring's end is never less than the 8-byte header a PAD needs.
//   * A record never wraps: when one would not fit before the ring's end
//     the writer emits a PAD record (kind MBX_R_PAD) whose len reaches the
//     end, and the next record starts at offset 0.
//   * HEAD and TAIL are free-running byte counts (spsc_index.h).  The
//     writer publishes HEAD after its record bytes are written (release);
//     the reader publishes TAIL after it is done with the bytes.
//   * A reader that sees a len under 8, not a multiple of 8, crossing the
//     ring's end, or beyond what was published, reports the ring corrupt;
//     the caller stores LOST in its status word and stops trusting it.
//   * The header words are little-endian bytes (wasm's order, and what a
//     JS Uint32Array over shared memory reads); the payload is the
//     protocol's business.
//
// What is deliberately NOT here: waiting.  Whether a full ring fails the
// request (platen), blocks with a deadline (Voodoo2 GPU) or queues in the
// client (the mailbox) is the transport's policy; this file only reports
// "no room".  Likewise wake-ups: the caller notifies whichever word it
// published, with whatever primitive its platform has.
//
// Mirrored in app/web2/src/bus/mailboxRing.ts -- keep the two in step.

#ifndef MAILBOX_RING_H
#define MAILBOX_RING_H

#include <stdbool.h>
#include <stdint.h>

#define MBX_HDR_BYTES 8u
#define MBX_R_PAD     0u // skip to the ring start; every protocol reserves kind 0 for it
#define MBX_PAD8(n)   (((uint32_t)(n) + 7u) & ~7u)

// One side of one ring.  A transport with two rings (one each way) keeps
// two of these; a side that only writes leaves `rd` unused and vice versa.
typedef struct mbx_ring {
    volatile uint32_t *ctrl; // the control block
    int head_word; // ctrl index of HEAD (the writer publishes it)
    int tail_word; // ctrl index of TAIL (the reader publishes it)
    uint8_t *buf; // the ring bytes
    uint32_t size; // bytes, a power of two
    // Writer side: bytes reserved so far (monotonic).  HEAD lags it until
    // mbx_publish.
    uint32_t wr;
    uint32_t last_at; // ring offset of the last reserved record
    uint32_t last_wr0; // `wr` before the last reservation (monotonic)
    // Reader side: bytes consumed so far (monotonic).  TAIL is stored on
    // every mbx_consume.
    uint32_t rd;
} mbx_ring_t;

// Acquire load / seq_cst store of a control word, for the transport's own
// words (STATUS, ACK, ...) as much as for HEAD/TAIL.
static inline uint32_t mbx_load(const volatile uint32_t *ctrl, int word) {
    return __atomic_load_n(&ctrl[word], __ATOMIC_ACQUIRE);
}
static inline void mbx_store(volatile uint32_t *ctrl, int word, uint32_t v) {
    __atomic_store_n(&ctrl[word], v, __ATOMIC_SEQ_CST);
}

// Binds a ring view.  `size` must be a power of two; the ring bytes must
// start 8-aligned.  Cursors start at zero on both sides -- a transport that
// re-attaches keeps its cursors and re-binds.
void mbx_ring_init(mbx_ring_t *r, volatile uint32_t *ctrl, int head_word, int tail_word, uint8_t *buf, uint32_t size);

// --- Writer ---------------------------------------------------------------

// Reserves a record of `len` bytes (header included; rounded up to 8) that
// does not wrap, writing a PAD first when needed.  Returns the record's
// ring offset (its header is written; the payload starts MBX_HDR_BYTES
// later) or UINT32_MAX when there is no room, in which case nothing was
// written.  Nothing is published until mbx_publish.
uint32_t mbx_reserve(mbx_ring_t *r, uint32_t kind, uint32_t len);

// Payload pointer of a record reserved at `at`.
static inline uint8_t *mbx_payload(const mbx_ring_t *r, uint32_t at) {
    return r->buf + at + MBX_HDR_BYTES;
}

// Shrinks the LAST reserved record to `len` bytes (rounded up to 8; never
// larger than reserved), rewriting its header and giving the rest of the
// reservation back.  For records whose final size is known only after
// their payload is built (the Voodoo2 DRAW record grows by one vertex at a
// time until the state changes).  Only valid before mbx_publish.
void mbx_shrink_last(mbx_ring_t *r, uint32_t len);

// True when bytes are reserved but not yet published.
static inline bool mbx_unpublished(const mbx_ring_t *r) {
    return mbx_load(r->ctrl, r->head_word) != r->wr;
}

// Publishes everything reserved so far (stores HEAD).  The caller wakes
// the reader afterwards if its platform needs that.
void mbx_publish(mbx_ring_t *r);

// --- Reader ---------------------------------------------------------------

typedef struct mbx_rec {
    uint32_t kind;
    uint32_t len; // header included
    uint32_t at; // ring offset of the header
} mbx_rec_t;

// Reads the next record's header without consuming it.  Returns 1 with
// `rec` filled (PAD records are returned too; the caller skips them), 0
// when nothing complete has been published, or -1 when the framing is
// corrupt -- the writer is broken and nothing after this can be trusted;
// the caller marks the transport LOST.  `head` is the writer's published
// count, loaded by the caller (so one load serves a whole drain).
int mbx_next(mbx_ring_t *r, uint32_t head, mbx_rec_t *rec);

// Payload pointer of a record returned by mbx_next.
static inline const uint8_t *mbx_rec_payload(const mbx_ring_t *r, const mbx_rec_t *rec) {
    return r->buf + rec->at + MBX_HDR_BYTES;
}

// Consumes the record (advances `rd` past it and stores TAIL).  The
// caller wakes a writer parked on TAIL afterwards if its platform needs
// that.
void mbx_consume(mbx_ring_t *r, const mbx_rec_t *rec);

// Gives up on the ring after corrupt framing: sets `rd` to `head` so the
// reader never re-reads the bad bytes.  TAIL is not stored (the writer is
// not trusted to honour it).
void mbx_abandon(mbx_ring_t *r, uint32_t head);

#endif // MAILBOX_RING_H
