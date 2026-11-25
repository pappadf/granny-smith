# Jobs: scripts as tasks on the emulator thread

Owning sources: `src/core/job/job.c`, `src/core/job/job.h` (the job layer),
`src/core/job/seam.c` (the emulator-thread latch), `src/core/shell/job_glue.c`
(the scheduler's side), `src/core/lua/gs_lua.c` (the engine's side: tasks are
Lua coroutines).

## 1. Responsibilities & design

The emulator thread owns all guest state and runs nothing of unbounded
length: it ticks frames and drains the mailbox.  A script is unbounded --
a line typed at the terminal, a daemon statement, a headless `script=` file
-- so a script is a **job**: posted as `REQ_SCRIPT`, queued FIFO, one
active at a time, and run as a **task**, a coroutine of the script engine
that the drain steps.

A step (`job_layer_service`, called from every `mailbox_drain`) resumes the
active task until it ends or **suspends**:

- after a tree call that started a mode the job's client waits for
  (`scheduler.run(N)`, `debug.step`): the task waits until the mode ends;
- while a leaf's deferred answer is outstanding (an I/O leaf whose work runs
  on the I/O worker): the task waits until the answer lands;
- when its time slice is spent (4 ms, checked from the engine's instruction
  hook): the task waits for nothing and resumes at the next drain.

So `scheduler.run(N)` inside a script means "run N" on every platform, a
script that computes forever leaves the machine running (it gets a slice
per drain, the machine a frame between), and a tree call is a plain C call:
there is no second thread, nothing to marshal and no lock.

The job API is unchanged from the job-thread design it replaced: the request
(`REQ_SCRIPT`), cancel (`REQ_CANCEL` / `job_cancel_client`), the mode owner,
the mode wait, the output and annotation records and the result
(`EVT_LOG` / `EVT_RESULT`).  A client cannot tell.

**Calls that cannot suspend.**  A tree call made where the engine cannot
yield -- a breakpoint condition or logpoint template (run during a frame),
a chunk a request runs synchronously (`shell.run`, `shell.eval` from the
page), a `require` at load time -- never defers (the leaf does its work
there and then) and waits for a mode it started by running frames with the
platform's **frame runner** (`job_set_frame_runner`).  Headless registers
one; the browser does not, so there such a call does not wait.

## 2. Key types & files

| API | Purpose |
|---|---|
| `job_submit_script(client, req_id, src, len)` | Queue a job (the mailbox's `serve_script`). |
| `job_layer_service(m)` / `job_layer_has_work()` | The drain's side: deliver output, start or resume the active task for one step, write finished jobs' results. `has_work` keeps an idle platform from parking while a task can run. |
| `job_cancel(client, req)` / `job_cancel_client(client)` | Set the flag; stop the mode the job started and its deferred I/O; the task unwinds when it next runs (the engine's hook raises the interrupt). A queued job never starts. |
| `job_task_call_begin(can_suspend)` / `job_task_call_end(mark)` | Around each tree call the engine makes: end says whether the task must suspend now. Calls nest (a call's leaf may run a script). |
| `job_call_defer()` / `job_call_complete*()` / `job_call_take_failure()` / `job_call_take_answer()` | A leaf's deferred answer: the token the leaf answers with later; the failure or the whole value the engine takes when the task resumes. |
| `job_output_append()` / `job_annotate()` | `out.c`'s sink and the annotation records (`value_begin`, `value`, `error`) of the running job. |
| `job_set_frame_runner(fn)` | The platform's frame runner for calls that cannot suspend. |
| `job_glue_task_new/step/free` | The engine (`gs_lua.c`): a coroutine running `gs.shell_line(src)`; a step is one `lua_resume`. |
| `job_glue_mode_waits/mode_id/stop_mode(s)` | The scheduler's side (`job_glue.c`): whether the client waits for its mode (bounded, or unbounded on a client whose scripts wait for the machine to stop; the browser's terminal does not). |
| `job_layer_init()` / `job_on_emulator_thread()` / `EMU_THREAD_CHECK(where)` | The emulator-thread latch and the gateways' guard (section 4). |

## 3. Behaviour/algorithms

- **One step per drain.**  The active job runs at most one step per
  `job_layer_service`; output is flushed before and after it, and finished
  jobs' results (the prompt, or `{"error":"command failed"|"cancelled"}`)
  are written in the order the jobs finished, after their output.
- **The wait.**  A suspended task is runnable again when no deferred answer
  is open and `job_glue_mode_waits(client)` is false.  The mode a call
  started is recorded on the job, so a cancel stops that run and not the
  client's others.
- **The engine's side.**  The bridge wraps each `node_set` / `node_call` in
  `job_task_call_begin` / `_end`; when end says suspend it yields
  (`lua_yieldk`) with a continuation that checks the cancel flag, raises a
  deferred failure, and replaces the provisional value with the deferred
  answer.  Only the task's own coroutine yields to the drain -- a coroutine
  a script made itself yields to the script -- and only where Lua can
  (`lua_isyieldable`: not inside a `require`, a `string.gsub` callback or a
  metamethod called from C).  `dofile` is yieldable (`lua_pcallk`), so a
  `script=` file, which headless runs as `dofile("path")`, suspends inside
  it like anywhere else.
- **Cancel.**  The flag is the engine's instruction hook's to read: the
  running chunk unwinds at its next count hook with `gs.interrupt_signal`,
  which the job reports as cancelled.

### 3.1 Performance

A tree call costs what the leaf costs: no marshalling, no wake-ups, no lock.
The time slice bounds how long a computing script holds the emulator
thread between frames (4 ms; the hook reads the clock every 1000
instructions).

## 4. The emulator-thread latch and thread-affinity guard

The PROXY_TO_PTHREAD WASM build runs `main()` -- and therefore the
scheduler, the devices, the OPFS handles, the object tree and the scripts --
on a single emulator pthread.  Calling any of that from another thread is
unsound.  Every JS → C entry point routes through the mailbox, whose drain
runs on the emulator thread; a regression that adds a `Module.ccall('em_*')`
shortcut from the main JS thread silently violates this.

One latch records which thread is the emulator thread: `core_init` calls
`job_layer_init()` first, and `job_on_emulator_thread()` answers from it
(the logger asks).  The gateway functions call `EMU_THREAD_CHECK(where)` to
verify they run on it: `object_eval`, `shell_complete`, and the AppleTalk
entry points `llap_receive` and `afp_handle_command`.  A ccall-from-main
regression trips a `GS_ASSERTF` inside the gateway.  The first check made
before the latch latches its caller, so no call goes unchecked; 0 means
"not latched", and the latch asserts `pthread_self() != 0`.  The check is
compiled out unless `GS_DEBUG` (`make MODE=debug`, `MODE=sanitize`).

## 5. Checkpointing

Nothing is checkpointed: jobs, tasks and the latch are process state, not
machine state.

## 6. Testing

`tests/unit/suites/mailbox` drives the job layer with stub tasks: a job
answers the prompt or an error, a task whose call starts a mode waits for
it and a cancel ends both, an I/O leaf's call suspends and resumes with the
work's answer or reason, and a job's output arrives before its result.
`tests/integration/job-annotations`, `headless-unframed-text` and
`daemon-statements` cover the record stream, deferred I/O and the daemon's
statement streaming; `tests/e2e/web2-specs/terminal-jobs.spec.ts` the
browser terminal's mode wait, a runaway script and Ctrl-C.

## 7. Known debts

- A coroutine a script creates itself does not suspend for the drain: a
  `scheduler.run` inside one runs as a call that cannot suspend.
- A call that cannot suspend does not wait for its mode in the browser
  (there is no frame runner there).

## 8. See also

- `docs/internals/core/lua/lua.md` -- the engine: tasks, the terminal's
  forms, conditions and templates.
- `docs/internals/core/object/object-model.md` -- `object_eval`, the main
  gateway.
- `docs/guide/web.md` -- the mailbox and the threads of the browser build.
