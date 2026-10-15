# Worker-thread affinity guard

Owning sources: `src/core/worker_thread.c`, `src/core/worker_thread.h`.

## 1. Responsibilities & design

The PROXY_TO_PTHREAD WASM build runs `main()` — and therefore the
scheduler, the devices, the OPFS handles and the object tree — on a single
worker pthread. Calling any of that from another thread is unsound:
concurrent SharedArrayBuffer writes, OPFS file handles owned elsewhere,
racy scheduler state.

Every JS → C entry point is supposed to route through the mailbox
(`src/core/mailbox/mailbox.h`), so the actual dispatch happens inside the
emulator thread's drain; the job thread reaches guest state only through
the seam (`src/core/job/job.h`), which lands on the worker too. A
regression that adds a `Module.ccall('em_*', ...)` shortcut from the main
JS thread, or a leaf called straight from the job thread, silently
violates this invariant.

The guard turns that invariant into a one-line check: the worker latches
its `pthread_self()` at startup, and the gateway functions call
`worker_thread_check()` to verify they are running on it: `object_eval` (the
object model), `shell_complete` (tab completion), and the AppleTalk entry
points that touch emulator state, `llap_receive` (an inbound LLAP frame) and
`afp_handle_command` (an AFP request). A ccall-from-main regression trips a `GS_ASSERTF`
inside the gateway, the page logs a fatal, and the e2e harness fails fast.

## 2. Key types & files

| API | Purpose |
|---|---|
| `worker_thread_latch()` | Latch the calling thread as the worker; called once from `core_init` (`src/core/core_init.c`). |
| `worker_thread_check(where)` | Assert the caller is the latched worker; `where` names the gateway in the failure message. |

The latched id is an `_Atomic uintptr_t` (release/acquire through a
compare-and-swap), since it is published by one thread and read by every
thread that enters a gateway.

## 3. Behaviour/algorithms

- **No unchecked window.** The first `worker_thread_check()` made before
  `worker_thread_latch()` latches the calling thread itself, once-init
  style, so a wrong-thread gateway call during initialisation is caught as
  soon as the real worker latches (the latch asserts that it is the thread
  already holding the slot).
- **Sentinel.** 0 means "not latched". `pthread_t` is opaque and POSIX does
  not promise it never reads as 0; on Linux, macOS and Emscripten it is a
  non-NULL pointer. The guard asserts `pthread_self() != 0` on every use,
  so a port where that fails is told immediately rather than running with
  the guard silently open.

### 3.1 Performance — explicit design choice

The check fires once per gateway call (once per `gsEval` or shell command)
and is in no hot path (CPU loop, scheduler). To keep even that off the
production build, the facility is compiled out unless `GS_DEBUG` is
defined: `make MODE=debug` and `MODE=sanitize` enable it; the default
release build gets no calls and no symbol references — the release
stubs are macros, so the gateway-name argument is not even evaluated.

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
