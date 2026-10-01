// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// reader.c
// A read-ahead window over a byte source, for header parsing (internal.h).

#include "internal.h"

int peel_reader_init(peel_reader_t *r, peel_source_t *src) {
    memset(r, 0, sizeof(*r));
    r->src = src;
    r->size = peel_source_size(src);
    r->buf = malloc(PEEL_READER_WINDOW);
    return r->buf ? 0 : -12;
}

void peel_reader_free(peel_reader_t *r) {
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

const uint8_t *peel_reader_at(peel_reader_t *r, uint64_t off, size_t n) {
    if (n > PEEL_READER_WINDOW || off > r->size || n > r->size - off)
        return NULL;
    // Already in the window?
    if (off >= r->base && off - r->base <= r->len && n <= r->len - (size_t)(off - r->base))
        return r->buf + (off - r->base);
    // Refill the window starting at `off`, as far as the source goes.
    uint64_t want = r->size - off < PEEL_READER_WINDOW ? r->size - off : PEEL_READER_WINDOW;
    if (peel_source_read_exact(r->src, off, r->buf, (size_t)want) != 0) {
        r->len = 0;
        return NULL;
    }
    r->base = off;
    r->len = (size_t)want;
    return r->buf;
}
