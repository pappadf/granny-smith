// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// seam.c -- the thread-neutral half of job.h: which thread is the emulator
// thread, the seam itself, the table lock, and the calling thread's job.
// Nothing here knows about queues or the mailbox (job.c), so the object
// model and the shell tables can link it alone.

#include "job.h"

#include <pthread.h>
#include <stdio.h>

static pthread_t g_emu_thread;
static bool g_emu_thread_set;
static pthread_mutex_t g_tables;
static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;
static job_post_fn g_post; // set by job.c when the job thread exists

// Per thread: the job it runs (job.c sets these on the job thread).
static _Thread_local gs_job_t *t_job;
static _Thread_local uint32_t t_client;
static _Thread_local const bool *t_cancel;

void job_layer_init(void) {
    g_emu_thread = pthread_self();
    g_emu_thread_set = true;
}

bool job_on_emulator_thread(void) {
    return !g_emu_thread_set || pthread_equal(pthread_self(), g_emu_thread);
}

void job_seam_set_poster(job_post_fn post) {
    g_post = post;
}

void job_seam_set_current(gs_job_t *job, uint32_t client, const bool *cancel) {
    t_job = job;
    t_client = client;
    t_cancel = cancel;
}

gs_job_t *job_current(void) {
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
