// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// seam.c -- the thread-neutral half of job.h: which thread is the emulator
// thread, the seam itself, the table lock, and the calling thread's job.
// Nothing here knows about queues or the mailbox (job.c), so the object
// model and the shell tables can link it alone.

#include "job.h"

#include "gs_assert.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

// The emulator thread's pthread_self(), as an integer; 0 = not latched yet.
// Published once and read from every thread that asks, hence atomic.
static _Atomic uintptr_t g_emu_tid;
static pthread_mutex_t g_tables;
static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;
static job_post_fn g_post; // set by job.c when the job thread exists

// Per thread: the job it runs (job.c sets these on the job thread).
static _Thread_local job_t *t_job;
static _Thread_local uint32_t t_client;
static _Thread_local const bool *t_cancel;

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

void job_seam_set_poster(job_post_fn post) {
    g_post = post;
}

void job_seam_set_current(job_t *job, uint32_t client, const bool *cancel) {
    t_job = job;
    t_client = client;
    t_cancel = cancel;
}

job_t *job_current(void) {
    return t_job;
}

uint32_t job_current_client(void) {
    return t_client;
}

bool job_current_cancelled(void) {
    return t_cancel && __atomic_load_n(t_cancel, __ATOMIC_ACQUIRE);
}

void job_on_emulator(void (*fn)(void *ud), void *ud) {
    if (job_on_emulator_thread() || !t_job || !g_post) {
        fn(ud);
        return;
    }
    g_post(fn, ud);
}

// Recursive: a public table entry calls another (shell_var_set ->
// shell_binding_let), and a function's activation nests a definition.
static void tables_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_tables, &a);
    pthread_mutexattr_destroy(&a);
}

void job_tables_lock(void) {
    pthread_once(&g_tables_once, tables_init);
    pthread_mutex_lock(&g_tables);
}

void job_tables_unlock(void) {
    pthread_mutex_unlock(&g_tables);
}

// A deferred answer that failed, noted by job.c when the completion lands
// and taken by the job thread right after its seam call returns.  Lives
// here so the object model, which reads it, links without the queue.
static bool g_call_failed;
static char g_call_error[256];

void job_seam_note_failure(const char *error) {
    g_call_failed = true;
    snprintf(g_call_error, sizeof g_call_error, "%s", error ? error : "failed");
}

bool job_call_take_failure(char *err, size_t cap) {
    bool failed = g_call_failed;
    if (failed && err && cap)
        snprintf(err, cap, "%s", g_call_error);
    g_call_failed = false;
    return failed;
}
