// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// worker_thread.c
// Thread-affinity guard; see docs/internals/core/worker_thread.md.

#include "worker_thread.h"

#ifdef GS_DEBUG

#include "gs_assert.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

// The latched worker's pthread_self(), as an integer; 0 = not latched yet.
// Published once and read from every gateway thread, hence atomic.
static _Atomic uintptr_t s_worker_tid = 0;

// Returns the calling thread's id as an integer, checking it can never be
// confused with the "not latched" sentinel.
static uintptr_t current_tid(void) {
    uintptr_t me = (uintptr_t)pthread_self();
    // pthread_t is opaque; the 0 sentinel relies on it never reading as 0
    // (true on Linux, macOS and Emscripten, where it is a non-NULL pointer).
    GS_ASSERTF(me != 0, "pthread_self() is 0 on this platform; worker_thread's sentinel needs a new encoding");
    return me;
}

// Latches the caller if nobody has yet; returns the latched thread's id.
static uintptr_t latch_or_get(uintptr_t me) {
    uintptr_t expected = 0;
    if (atomic_compare_exchange_strong_explicit(&s_worker_tid, &expected, me, memory_order_acq_rel,
                                                memory_order_acquire))
        return me; // we latched
    return expected; // already latched: the CAS loaded the holder
}

void worker_thread_latch(void) {
    uintptr_t me = current_tid();
    uintptr_t worker = latch_or_get(me);
    GS_ASSERTF(me == worker,
               "worker_thread_latch: a gateway call from thread 0x%" PRIxPTR " latched before the worker (0x%" PRIxPTR
               ") started",
               worker, me);
}

void worker_thread_check(const char *where) {
    uintptr_t me = current_tid();
    uintptr_t worker = latch_or_get(me);
    GS_ASSERTF(me == worker,
               "%s called from wrong thread (caller=0x%" PRIxPTR " worker=0x%" PRIxPTR ") — JS path "
               "must route through the SAB queue, not Module.ccall()",
               where ? where : "(?)", me, worker);
}

#endif // GS_DEBUG
