// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mailbox_ring.c -- see mailbox_ring.h.  Compiles natively (the unit suites
// drive it from both ends in one process) and under Emscripten; the
// atomics are the compiler's builtins.

#include "mailbox_ring.h"

#include "common.h"
#include "spsc_index.h"

#include <string.h>

void mbx_ring_init(mbx_ring_t *r, volatile uint32_t *ctrl, int head_word, int tail_word, uint8_t *buf, uint32_t size) {
    memset(r, 0, sizeof(*r));
    r->ctrl = ctrl;
    r->head_word = head_word;
    r->tail_word = tail_word;
    r->buf = buf;
    r->size = size;
}

uint32_t mbx_reserve(mbx_ring_t *r, uint32_t kind, uint32_t len) {
    len = MBX_PAD8(len < MBX_HDR_BYTES ? MBX_HDR_BYTES : len);
    if (len > r->size)
        return UINT32_MAX;
    uint32_t at = spsc_slot(r->wr, r->size);
    // A PAD to the end first when the record would cross it: at least 8
    // bytes remain there, since every record starts 8-aligned.
    uint32_t pad = at + len > r->size ? spsc_to_end(r->wr, r->size) : 0;
    uint32_t tail = mbx_load(r->ctrl, r->tail_word);
    if (spsc_room(r->size, r->wr, tail) < pad + len)
        return UINT32_MAX;
    if (pad) {
        WR_LE32(r->buf + at, MBX_R_PAD);
        WR_LE32(r->buf + at + 4, pad);
        r->wr += pad;
        at = 0;
    }
    WR_LE32(r->buf + at, kind);
    WR_LE32(r->buf + at + 4, len);
    r->last_at = at;
    r->last_wr0 = r->wr;
    r->wr += len;
    return at;
}

void mbx_shrink_last(mbx_ring_t *r, uint32_t len) {
    len = MBX_PAD8(len < MBX_HDR_BYTES ? MBX_HDR_BYTES : len);
    uint32_t reserved = r->wr - r->last_wr0;
    if (len > reserved)
        len = reserved;
    WR_LE32(r->buf + r->last_at + 4, len);
    r->wr = r->last_wr0 + len;
}

void mbx_publish(mbx_ring_t *r) {
    mbx_store(r->ctrl, r->head_word, r->wr);
}

int mbx_next(mbx_ring_t *r, uint32_t head, mbx_rec_t *rec) {
    if (spsc_avail(head, r->rd) < MBX_HDR_BYTES)
        return 0;
    uint32_t at = spsc_slot(r->rd, r->size);
    uint32_t kind = RD_LE32(r->buf + at);
    uint32_t len = RD_LE32(r->buf + at + 4);
    if (len < MBX_HDR_BYTES || (len & 7u) || at + len > r->size || spsc_avail(head, r->rd) < len)
        return -1;
    rec->kind = kind;
    rec->len = len;
    rec->at = at;
    return 1;
}

void mbx_consume(mbx_ring_t *r, const mbx_rec_t *rec) {
    r->rd += rec->len;
    mbx_store(r->ctrl, r->tail_word, r->rd);
}

void mbx_abandon(mbx_ring_t *r, uint32_t head) {
    r->rd = head;
}
