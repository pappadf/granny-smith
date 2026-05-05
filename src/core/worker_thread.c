// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// See worker_thread.h for the rationale.

#include "worker_thread.h"

#ifdef GS_DEBUG

#include "common.h"

#include <pthread.h>
#include <stdint.h>

// 0 means "not latched yet" — we accept any caller until the worker
// records itself. After that, only that thread is allowed.
static volatile uintptr_t g_worker_tid = 0;

void worker_thread_record(void) {
    g_worker_tid = (uintptr_t)pthread_self();
}

void worker_thread_assert(const char *where) {
    if (g_worker_tid == 0)
        return; // pre-init: guard is open
    uintptr_t me = (uintptr_t)pthread_self();
    GS_ASSERTF(me == g_worker_tid,
               "%s called from wrong thread (caller=0x%lx worker=0x%lx) — JS path "
               "must route through the SAB queue, not Module.ccall()",
               where ? where : "(?)", (unsigned long)me, (unsigned long)g_worker_tid);
}

#endif /* GS_DEBUG */
