// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_spike_baton.c — THROWAWAY SPIKE, not for merging.
//
// Proves (or disproves) the "job coroutine under a baton" execution-model
// idea in a real PROXY_TO_PTHREAD build: a second pthread whose only purpose is to be a suspendable stack,
// and a "baton" word such that exactly one of {emulator thread, job thread}
// runs at any instant.  The emulator thread hands the baton over from its
// tick, parks in a futex, and takes it back when the job yields.  While it
// holds the baton the job thread calls real object-model leaves — one that
// goes through WasmFS/OPFS (storage.list_dir), two that read guest state
// (scheduler.running, machine.cpu.instr_count) — and writes to stdout.
//
// Everything it learns is emitted as console lines prefixed "SPIKE " so the
// Playwright spec (tests/e2e/web2-specs/spike-baton.spec.ts) can collect
// them.  Nothing here touches the per-instruction path.

#include <emscripten.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "api.h"

#define SPIKE_ROUNDS        40
#define SPIKE_EVERY_TICKS   15
#define SPIKE_WAIT_SLICE_MS 4.0
#define SPIKE_WAIT_MAX_MS   2000.0

// 0 = emulator thread holds the baton, 1 = job thread holds it.
static _Atomic uint32_t g_baton = 0;
static _Atomic uint32_t g_job_up = 0;
static pthread_t g_job_thr;
static double g_create_t0;

// Written by the job thread while it holds the baton; read by the emulator
// thread after it gets the baton back.  The baton hand-off (seq_cst store +
// acquire load) is the only synchronisation, on purpose: that is the claim
// being tested.
static char g_out_list[16384];
static char g_out_sched[256];
static char g_out_instr[256];
static int g_rc_list, g_rc_sched, g_rc_instr;
static double g_job_ms;
static int g_round;
static uint32_t g_job_tid_seen;

static void *job_main(void *arg) {
    (void)arg;
    atomic_store_explicit(&g_job_up, 1, memory_order_release);
    for (;;) {
        // Park until the emulator thread hands us the baton.  A futex wait
        // can return early (-EINTR from a mailbox notification, or
        // -EWOULDBLOCK), so always re-check the word.
        while (atomic_load_explicit(&g_baton, memory_order_acquire) != 1)
            emscripten_futex_wait(&g_baton, 0, 1000.0);

        double t0 = emscripten_get_now();
        g_job_tid_seen = (uint32_t)(uintptr_t)pthread_self();
        // Leaf 1: WasmFS/OPFS directory listing from this thread.
        g_rc_list = gs_eval("storage.list_dir", "[\"/opfs/images\"]", g_out_list, sizeof g_out_list);
        // Leaf 2 and 3: guest-state reads through the object tree.
        g_rc_sched = gs_eval("scheduler.running", NULL, g_out_sched, sizeof g_out_sched);
        g_rc_instr = gs_eval("machine.cpu.instr_count", NULL, g_out_instr, sizeof g_out_instr);
        // stdout from a non-emulator pthread.
        printf("SPIKE stdout from job thread, round %d\n", g_round);
        fflush(stdout);
        g_job_ms = emscripten_get_now() - t0;

        // Yield: hand the baton back and wake the parked emulator thread.
        atomic_store_explicit(&g_baton, 0, memory_order_seq_cst);
        emscripten_futex_wake(&g_baton, INT32_MAX);
    }
    return NULL;
}

void spike_baton_start(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    g_create_t0 = emscripten_get_now();
    int rc = pthread_create(&g_job_thr, &attr, job_main, NULL);
    double dt = emscripten_get_now() - g_create_t0;
    pthread_attr_destroy(&attr);
    emscripten_log(EM_LOG_CONSOLE, "SPIKE pthread_create rc=%d blocked-emulator-thread %.2f ms (emu tid %u)", rc, dt,
                   (unsigned)(uintptr_t)pthread_self());
}

void spike_baton_tick(int tick) {
    static int announced_up = 0;
    if (!atomic_load_explicit(&g_job_up, memory_order_acquire))
        return;
    if (!announced_up) {
        announced_up = 1;
        emscripten_log(EM_LOG_CONSOLE, "SPIKE job thread up %.1f ms after pthread_create (tick %d)",
                       emscripten_get_now() - g_create_t0, tick);
    }
    if (tick % SPIKE_EVERY_TICKS != 0 || g_round >= SPIKE_ROUNDS)
        return;
    g_round++;

    // Hand off.
    double t0 = emscripten_get_now();
    atomic_store_explicit(&g_baton, 1, memory_order_seq_cst);
    emscripten_futex_wake(&g_baton, INT32_MAX);

    // Park until it comes back, in bounded slices.  Deliberately NOT calling
    // emscripten_current_thread_process_queued_calls() here: input callbacks
    // are proxied tasks that touch guest state, and while the job holds the
    // baton nobody else may.  (Design consequence for §4.2.3/§4.5.1: the
    // idle-wait drain must happen only while the emulator thread holds it.)
    int slices = 0;
    while (atomic_load_explicit(&g_baton, memory_order_acquire) != 0) {
        emscripten_futex_wait(&g_baton, 1, SPIKE_WAIT_SLICE_MS);
        slices++;
        if (emscripten_get_now() - t0 > SPIKE_WAIT_MAX_MS) {
            emscripten_log(EM_LOG_CONSOLE, "SPIKE FAIL round %d: baton not returned within %.0f ms", g_round,
                           SPIKE_WAIT_MAX_MS);
            atomic_store_explicit(&g_baton, 0, memory_order_seq_cst);
            return;
        }
    }
    double rt = emscripten_get_now() - t0;

    // Count list entries: the result is a JSON list of strings.
    int entries = 0;
    for (const char *p = g_out_list; *p; p++)
        if (*p == '"')
            entries++;
    entries /= 2;

    emscripten_log(EM_LOG_CONSOLE,
                   "SPIKE round %d ok: roundtrip %.3f ms, job %.3f ms, handoff overhead %.3f ms, slices %d | "
                   "list_dir rc=%d entries=%d | scheduler.running rc=%d %s | instr_count rc=%d %s | job tid %u",
                   g_round, rt, g_job_ms, rt - g_job_ms, slices, g_rc_list, entries, g_rc_sched, g_out_sched,
                   g_rc_instr, g_out_instr, (unsigned)g_job_tid_seen);
    if (g_round == SPIKE_ROUNDS)
        emscripten_log(EM_LOG_CONSOLE, "SPIKE done");
}
