// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Optional thread-affinity guard for the object-model bridge.
//
// Why this header exists
// ----------------------
// The PROXY_TO_PTHREAD WASM build runs `main()` (and therefore the
// scheduler, devices, OPFS handles, and the object tree) on a single
// worker pthread. Calling any of that from another thread is unsound:
// concurrent SAB writes, OPFS file handles owned elsewhere, racy
// scheduler state.
//
// Every JS → C entry point is *supposed* to route through one of the
// SAB-backed queues in em_main.c (`g_cmd_*`, `g_gs_*`) so the actual
// dispatch happens inside `shell_poll()` on the worker. A regression
// that adds a `Module.ccall('em_*', ...)` shortcut from the main JS
// thread silently violates this invariant. The first such regression
// (M10c, May 2026) sat in the tree for several commits before
// surfacing as 60–90 s checkpoint latency in CI.
//
// This header provides a one-line invariant check: at the worker's
// startup we latch `pthread_self()`, and gateway functions
// (`gs_eval`, `gs_inspect`, `dispatch_command`) call
// `gs_thread_assert_worker()` to verify they're running there. A
// future ccall-from-main regression trips a `GS_ASSERT` inside the
// gateway, the page logs a fatal, and the e2e harness fails fast.
//
// Performance — explicit design choice
// ------------------------------------
// The check fires once per gateway call (so once per gsEval / shell
// command) and is NOT in any hot path (CPU loop, scheduler). To keep
// the cost off the critical path entirely, the entire facility is
// compiled out unless `GS_DEBUG` is defined. `make MODE=debug` and
// `MODE=sanitize` enable it; `MODE=release` (default, used by CI for
// the deployed bundle) gets no calls and no symbol references.
//
// Catching regressions
// --------------------
// To exercise the guard intentionally, build with `MODE=debug` and run
// the existing e2e suite. Any added entry point that bypasses the SAB
// queues fires the assertion in the very first test that touches it.

#ifndef GS_THREAD_H
#define GS_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef GS_DEBUG

// Latch the current thread as "the worker". Call once from the worker's
// startup path (shell_init).
void gs_thread_record_worker(void);

// Abort with GS_ASSERTF if the calling thread isn't the latched worker.
// `where` shows up in the failure message (file:line are added by the
// macro layer); pass a static string identifying the gateway.
void gs_thread_assert_worker(const char *where);

#else /* GS_DEBUG */

static inline void gs_thread_record_worker(void) {}
static inline void gs_thread_assert_worker(const char *where) {
    (void)where;
}

#endif /* GS_DEBUG */

#ifdef __cplusplus
}
#endif

#endif /* GS_THREAD_H */
