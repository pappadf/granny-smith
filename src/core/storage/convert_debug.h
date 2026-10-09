// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// convert_debug.h
// DEBUG(convert-timing): temporary instrumentation for disk conversions
// (files.convert) that are slow on some browser hosts.  Counts the host
// file calls and the time spent in them and in deflate; files.convert
// prints the totals to the console when it finishes.  Remove once the
// cause is known.  The counters are not synchronised: a read the guest
// makes during a conversion lands in them too.

#ifndef CONVERT_DEBUG_H
#define CONVERT_DEBUG_H

#include <stdint.h>
#include <time.h>

typedef struct {
    uint64_t calls;
    uint64_t bytes;
    double ms;
} gs_dbg_counter_t;

extern gs_dbg_counter_t g_dbg_host_open; // gs_source_host opens
extern gs_dbg_counter_t g_dbg_host_read; // gs_source_host preads
extern gs_dbg_counter_t g_dbg_udif_write; // UDIF writer output (stdio)
extern gs_dbg_counter_t g_dbg_deflate; // UDIF writer compression

static inline double gs_dbg_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static inline void gs_dbg_add(gs_dbg_counter_t *c, uint64_t bytes, double t0) {
    c->calls++;
    c->bytes += bytes;
    c->ms += gs_dbg_now_ms() - t0;
}

#endif // CONVERT_DEBUG_H
