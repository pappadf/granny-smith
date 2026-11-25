# The emulator-thread latch and thread-affinity guard

Owning sources: `src/core/job/seam.c`, `src/core/job/job.h` (the
thread-neutral half of the job layer).

## 1. Responsibilities & design

The PROXY_TO_PTHREAD WASM build runs `main()` — and therefore the
scheduler, the devices, the OPFS handles and the object tree — on a single
emulator pthread. Calling any of that from another thread is unsound:
concurrent SharedArrayBuffer writes, OPFS file handles owned elsewhere,
racy scheduler state.

Every JS → C entry point is supposed to route through the mailbox
(`src/core/mailbox/mailbox.h`), so the actual dispatch happens inside the
emulator thread's drain; the job thread reaches guest state only through
the seam (`src/core/job/job.h`), which lands on the emulator thread too. A
regression that adds a `Module.ccall('em_*', ...)` shortcut from the main
JS thread, or a leaf called straight from the job thread, silently
violates this invariant.

One latch records which thread is the emulator thread: `core_init` calls
`job_layer_init()` first, and `job_on_emulator_thread()` answers from it
(the job layer's own routing, the logger, the object model). The guard
turns the invariant into a one-line check on the same latch: the gateway
functions call `EMU_THREAD_CHECK(where)` to verify they are running on it:
`object_eval` (the object model), `shell_complete` (tab completion), and
the AppleTalk entry points that touch emulator state, `llap_receive` (an
inbound LLAP frame) and `afp_handle_command` (an AFP request). A
ccall-from-main regression trips a `GS_ASSERTF` inside the gateway, the
page logs a fatal, and the e2e harness fails fast.

(The guard used to be a second latch in `worker_thread.c`, where "worker"
meant the emulator thread while `io_worker.h`'s worker is the I/O thread;
the two latches were set back to back in `core_init`.)

## 2. Key types & files

| API | Purpose |
|---|---|
| `job_layer_init()` | Latch the calling thread as the emulator thread; called once, first, from `core_init` (`src/core/core_init.c`). Asserts that no other thread latched before it. |
| `job_on_emulator_thread()` | True on the emulator thread, and before the latch (single-threaded start-up and unit tests). |
| `EMU_THREAD_CHECK(where)` | Debug builds: assert the caller is the emulator thread; `where` names the gateway in the failure message. |

The latched id is an `_Atomic uintptr_t` (release/acquire through a
compare-and-swap), since it is published by one thread and read by every
thread that asks.

## 3. Behaviour/algorithms

- **No unchecked window.** The first `EMU_THREAD_CHECK()` made before
  `job_layer_init()` latches the calling thread itself, once-init
  style, so a wrong-thread gateway call during initialisation is caught as
  soon as the real emulator thread latches (the latch asserts that it is
  the thread already holding the slot).
- **Sentinel.** 0 means "not latched". `pthread_t` is opaque and POSIX does
  not promise it never reads as 0; on Linux, macOS and Emscripten it is a
  non-NULL pointer. The latch and the guard assert `pthread_self() != 0`,
  so a port where that fails is told immediately rather than running with
  the guard silently open.

### 3.1 Performance — explicit design choice

The check fires once per gateway call (once per `gsEval` or shell command)
and is in no hot path (CPU loop, scheduler). To keep even that off the
production build, the guard is compiled out unless `GS_DEBUG` is
defined: `make MODE=debug` and `MODE=sanitize` enable it; the default
release build gets no calls and no symbol references — the release
macro does not even evaluate the gateway-name argument.

## 4. Object-model / shell surface

None of its own; it guards the entry points of the object model
(`object_eval`), of the shell's completer (`shell_complete`) and of the
AppleTalk stack (`llap_receive`, `afp_handle_command`).

## 5. Checkpointing

Nothing is checkpointed: the latch is process state, not machine state.

## 6. Testing

To exercise the guard, build with `MODE=debug` and run the e2e suite. Any
added entry point that bypasses the mailbox fires the assertion in the
first test that touches it.

## 7. Known debts

None known.

## 8. See also

- `docs/internals/core/object/object-model.md` — `object_eval`, the main gateway.
- `docs/guide/web.md` — the mailbox and the threads of the browser build.
