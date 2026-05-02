// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// See gs_thread.h for the rationale.

#include "gs_thread.h"

#ifdef GS_DEBUG

#include "common.h"

#include <pthread.h>
#include <stdint.h>

// 0 means "not latched yet" — we accept any caller until the worker
// records itself. After that, only that thread is allowed.
static volatile uintptr_t g_worker_tid = 0;

void gs_thread_record_worker(void) {
    g_worker_tid = (uintptr_t)pthread_self();
}

void gs_thread_assert_worker(const char *where) {
    if (g_worker_tid == 0)
        return; // pre-init: guard is open
    uintptr_t me = (uintptr_t)pthread_self();
    GS_ASSERTF(me == g_worker_tid,
               "%s called from wrong thread (caller=0x%lx worker=0x%lx) — JS path "
               "must route through the SAB queue, not Module.ccall()",
               where ? where : "(?)", (unsigned long)me, (unsigned long)g_worker_tid);
}

#endif /* GS_DEBUG */
