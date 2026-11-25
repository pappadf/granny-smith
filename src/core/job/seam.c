// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// seam.c -- which thread is the emulator thread (job.h): the latch and the
// gateways' thread check.  Nothing here knows about jobs or the mailbox
// (job.c), so the object model links it alone.

#include "job.h"

#include "gs_assert.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

// The emulator thread's pthread_self(), as an integer; 0 = not latched yet.
// Published once and read from every thread that asks, hence atomic.
static _Atomic uintptr_t g_emu_tid;

// The calling thread's id as an integer.  pthread_t is opaque; the 0
// sentinel relies on it never reading as 0 (true on Linux, macOS and
// Emscripten, where it is a non-NULL pointer), which a debug build checks.
static uintptr_t current_tid(void) {
    uintptr_t me = (uintptr_t)pthread_self();
    GS_ASSERTF(me != 0, "pthread_self() is 0 on this platform; the emulator-thread latch needs a new encoding");
    return me;
}

// Latches `me` if nobody has yet; returns the latched thread's id.
static uintptr_t latch_or_get(uintptr_t me) {
    uintptr_t expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_emu_tid, &expected, me, memory_order_acq_rel, memory_order_acquire))
        return me; // we latched
    return expected; // already latched: the CAS loaded the holder
}

void job_layer_init(void) {
    uintptr_t me = current_tid();
    uintptr_t emu = latch_or_get(me);
    // Only a debug build's gateway check latches before core_init; one
    // that did from another thread has found a call off the emulator thread.
    GS_ASSERTF(me == emu,
               "job_layer_init: a gateway call from thread 0x%" PRIxPTR
               " latched before the emulator thread (0x%" PRIxPTR ") started",
               emu, me);
    (void)emu;
}

bool job_on_emulator_thread(void) {
    uintptr_t emu = atomic_load_explicit(&g_emu_tid, memory_order_acquire);
    return emu == 0 || emu == (uintptr_t)pthread_self();
}

#ifdef GS_DEBUG
void job_emulator_thread_check(const char *where) {
    uintptr_t me = current_tid();
    // The first check made before the latch latches its caller, so no call
    // goes unchecked; core_init's latch then asserts it is that thread.
    uintptr_t emu = latch_or_get(me);
    GS_ASSERTF(me == emu,
               "%s called from wrong thread (caller=0x%" PRIxPTR " emulator=0x%" PRIxPTR
               "): every entry from the page goes through the mailbox",
               where ? where : "(?)", me, emu);
}
#endif
