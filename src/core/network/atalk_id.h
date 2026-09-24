// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// atalk_id.h
// One allocator for every id the stack hands out and must not hand out twice
// while the first holder lives: ATP transaction ids, NBP enumerators, ADSP
// connection ids, ASP session ids, AFP fork refnums and volume ids.
//
// There were six, and they disagreed.  Three searched the live table (TIDs,
// enumerators, ADSP ids) but, when every id was taken, returned one anyway --
// a duplicate.  Two did not search at all: AFP fork refnums skipped only 0,
// so after 65,535 opens a new fork could get the refnum a still-open one held
// and FPRead on the old refnum read the new file; ASP session ids were the low
// byte of a counter (10-network F-08, F-06).

#ifndef ATALK_ID_H
#define ATALK_ID_H

#include <stdbool.h>
#include <stdint.h>

// Is `id` held by a live entry?  `ctx` is the caller's.
typedef bool (*atalk_id_in_use_fn)(uint32_t id, const void *ctx);

// The next id in [lo, hi] after *cursor, wrapping, that `in_use` says is
// free; *cursor moves past it.  False, with *out untouched, if every id in the
// range is held.  lo <= hi.
static inline bool atalk_id_alloc(uint32_t *cursor, uint32_t lo, uint32_t hi, atalk_id_in_use_fn in_use,
                                  const void *ctx, uint32_t *out) {
    uint32_t span = hi - lo + 1;
    uint32_t id = *cursor;
    for (uint32_t n = 0; n < span; n++) {
        if (id < lo || id > hi)
            id = lo;
        uint32_t next = (id == hi) ? lo : id + 1;
        if (!in_use(id, ctx)) {
            *cursor = next;
            *out = id;
            return true;
        }
        id = next;
    }
    return false;
}

#endif // ATALK_ID_H
