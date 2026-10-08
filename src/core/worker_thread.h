// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// worker_thread.h
// Debug-only thread-affinity guard for the JS -> C gateways (gs_eval,
// shell_dispatch, shell_complete): every gateway must run on the emulator
// (worker) thread.  Compiled out unless GS_DEBUG is defined.  Rationale and
// contract: docs/internals/core/worker_thread.md.

#ifndef WORKER_THREAD_H
#define WORKER_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef GS_DEBUG

// Latch the calling thread as "the worker".  Called once from the worker's
// startup path (core_init); asserts if a different thread already latched.
void worker_thread_latch(void);

// Assert that the calling thread is the latched worker.  The first call made
// before worker_thread_latch latches the caller instead, so no call goes
// unchecked.  `where` names the gateway in the failure message.
void worker_thread_check(const char *where);

#else // GS_DEBUG

// Release builds: no calls, and the gateway-name argument is not evaluated.
#define worker_thread_latch()      ((void)0)
#define worker_thread_check(where) ((void)0)

#endif // GS_DEBUG

#ifdef __cplusplus
}
#endif

#endif /* WORKER_THREAD_H */
