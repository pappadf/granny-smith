# Web Frontend Reference

The Granny Smith web frontend lives in `app/web2/` (Svelte 5 + Vite +
TypeScript) — that's what `make run` serves. It talks to the emulator
through the Emscripten / bridge / OPFS contract documented below.

> A legacy vanilla-DOM frontend (`app/web-legacy/`) preceded web2 and was
> removed once web2 reached parity; some comments below reference it as the
> historical origin of a given pattern.

## Architecture: Pthreads + WasmFS + OPFS

The emulator uses Emscripten's `PROXY_TO_PTHREAD` mode. The C `main()`
runs on a dedicated worker thread; the browser main thread handles DOM,
input, and compositing.

**Threading model**
- **Main (browser) thread:** DOM events, the Terminal console, UI chrome,
  OPFS reads via the browser API, file uploads staged to `/opfs/upload/.scratch/`.
- **Emulator worker thread:** CPU emulation, OPFS file I/O via WasmFS
  (delta/journal/checkpoint), shell command execution, all object-model
  dispatch.
- **Canvas:** transferred to the worker via `OffscreenCanvas` at boot —
  `transferControlToOffscreen` is handled by Emscripten under
  `OFFSCREENCANVASES_TO_PTHREAD='#screen'`. WebGL2 rendering happens on
  the worker side. After the transfer, the main thread must NOT write
  to the canvas's `width`/`height` attributes (Svelte reactive bindings
  would throw `InvalidStateError`).
- **Input:** mouse / keyboard events are picked up by Emscripten's
  built-in proxied callbacks on the worker side. The web2 UI does not
  install JS-side handlers on the canvas — each event would otherwise
  cost a bridge round-trip.

**Persistence.** A single OPFS mount at `/opfs` provides all persistent
storage. `/tmp/` is memory-backed (volatile). The WasmFS root is
memory-backed because `wasmfs_create_opfs_backend()` deadlocks on the
browser main thread; the web app only writes under `/opfs/`. All OPFS
writes are immediately durable — no explicit sync step needed.

**Two views of OPFS, kept coherent.** The browser main thread reaches OPFS
directly through `navigator.storage` (reads, and the upload staging writes),
while the emulator worker reaches it through WasmFS. WasmFS keeps its own
inode cache layered over OPFS and does **not** observe out-of-band changes:
it lazily *sees* a newly created file on first access, but a file the worker
created and then has cached goes stale if the main thread deletes it — so a
later worker-side create at that path fails (`I/O error`). To stay coherent,
the Filesystem tab routes its **mutations through the worker**
(`files.rm` / `files.mv` / `files.cp`), reserving `navigator.storage`
for reads. (This is why `BrowserOpfs.delete` / `.move` in
[`bus/opfs.ts`](../../app/web2/src/bus/opfs.ts) call `gsEval` rather than
`removeEntry` directly.)

**Core / frontend separation.** The emulator core is path-agnostic: it
accepts paths as command arguments. The web app owns the directory
layout under `/opfs/`. The C side creates `/opfs/images/{rom,vrom,fd,
fdhd,hd,cd}`, `/opfs/{checkpoints,upload}` and an emptied
`/opfs/upload/.scratch` at boot via `mkdir`-on-worker; the web app reads
them via the browser's OPFS API.

**Cross-thread communication.** JS on the main thread cannot directly
call WASM functions that touch OPFS (different thread). The boundary is
the **mailbox** — a control block and two record rings in shared memory
(`src/core/mailbox/mailbox.h`, exported via the lone `_get_gs_mailbox()`
accessor; see "The Mailbox" below). JS binds to it once at init, checks
its MAGIC and VERSION so layout drift fails loudly, and from then on
writes request records and reads result records through `Module.HEAPU8`
and Atomics.

Every JS→C request is a `REQ_EVAL` record (`gs_eval`) carrying an id.
Introspection rides on `<path>.meta.*`; free-form shell lines and tab
completion ride on the `Shell` class's `run` / `complete` methods. The
worker's `shell_poll()` (called every tick, and from the idle wait on a
stopped machine) drains every pending request and writes one
`EVT_RESULT` per request; the page's reader loop wakes on `EVT_HEAD` via
`Atomics.waitAsync` — no polling, no `setTimeout` spin — and resolves the
promise whose id the result carries. Requests are not serialised on the
page: any number may be in flight.

**The result contract.** `gsEval(path, args)` resolves to one of three
shapes, and callers must tell them apart:

- a **value** — the attribute's or method's result (a V_MAP arrives as an
  object, a V_LIST as an array, a V_BOOL as `true`/`false`);
- **`null`** — only a *successful* method that returns nothing (V_NONE);
- **`{ error }`** — failure.  A V_ERROR from the core carries its message;
  a failure of the bridge itself (the module not ready, a request too
  large, a dead worker) also sets `transport: true`.

So `r !== null` is never a success test — `{ error }` passes it.  Use
`gsOk(r)` for "did it work" (neither an error nor a V_BOOL `false`),
`r === true` for a V_BOOL method, and a shape check for a read;
`gsErrorText(r)` gives the reason
([`bus/emulator.ts`](../../app/web2/src/bus/emulator.ts)).

What the core says about the machine arrives as **events on the mailbox's
event ring** (see "Events from the core" below): the run state, the
effective speed, the screen geometry, the floppy drives, the activity lights, the perf
samples, checkpoint saves, log lines, breakpoint hits and assertion
failures. The `Module.*` callbacks that remain are the platform
transports, installed at module construction:

- **`Module.print` / `Module.printErr`** — Emscripten's stdout/stderr
  pipes. `logSink` routes these to the Terminal console.
- **`Module.onAbort(what)`** — the glue's `abort()`: the worker trapped.
  The bridge is marked dead and every request fails at once.
- **`Module.onVideoInReady(ptr)`** / **`Module.onAudioInReady(ptr)`** —
  fired once at startup with the address of the webcam frame transport
  (`em_camera.c`) or the microphone ring (`em_audio_in.c`) in the shared
  heap; see "Shared-heap transports" below. JS reads the layout from the
  block's header on first use; everything after that is direct heap
  access, not callbacks.
- **`Module.onVideoInState(active)`** — fired when the guest gates the
  AV digitizer's VDC clock, i.e. when capture actually starts and stops.
  [`state/camera.svelte.ts`](../../app/web2/src/state/camera.svelte.ts)
  attaches or stops the `MediaStreamTrack` on it, so the camera light
  is on only while the guest is capturing.
- **`Module.onAudioInState(active)`** / **`Module.onAudioInInjected(path)`**
  — the Singer's capture gate, and the file `machine.audioin.inject` just
  fed the guest (the page plays it aloud too); see
  [../machines/av/singer.md](../internals/machines/av/singer.md).
- **`Module.onVoodooGpuOverlay(visible)`** — whether the Voodoo2
  takeover's overlay canvas should show.

- **`Module.onVoodooGpuAttach(ctrl, bytes)` / `onVoodooGpuDetach(ctrl)`**
  — the Voodoo2 WebGPU takeover (`raster=webgpu`,
  [`src/platform/wasm/em_gpu.c`](../../src/platform/wasm/em_gpu.c)): the
  emulator's raster pthread allocated a control block + op ring +
  readback area at `ctrl` in the shared heap and wants the page's GPU
  worker attached to it.  [`gpu/voodoo2Gpu.svelte.ts`](../../app/web2/src/gpu/voodoo2Gpu.svelte.ts)
  posts the wasm memory and the address to the worker
  ([`gpu/voodoo2Gpu.worker.ts`](../../app/web2/src/gpu/voodoo2Gpu.worker.ts)),
  which then talks to the emulator through shared memory only —
  `Atomics.waitAsync` on the ring's head, `Atomics.notify` on its tail
  and the acknowledge word — while the C side waits with futexes.  The
  worker is started once at page load with the `#screen3d` overlay
  canvas (transferred), and the page writes whether a WebGPU device
  exists into the bridge's `gpu_available` word before it reports ready
  (`bootstrap` awaits the answer; one not in within 2 s counts as no
  adapter and is logged), so no boot and no `catalog.profile` read can
  come first: the card catalog's `voodoo2_webgpu` offer and the core's
  backend choice at machine creation are honest.  The page shows the overlay exactly
  while GPU mode is engaged (the worker relays the MODE records it
  consumes).  The protocol is
  [`voodoo2_gpu_protocol.h`](../../src/core/peripherals/pci/cards/voodoo2_gpu_protocol.h)
  / `voodoo2Protocol.ts`.

- **`Module.onPrinterAttach(ctrl, version)`** — the emulated LaserWriter's
  interpreter (`src/platform/wasm/em_main.c`,
  `laserwriter_ring_attach_requested`): the printer bridge allocated a
  control block + two byte rings at `ctrl` in the shared heap on the
  first print job and wants the page's platen worker attached.
  [`printer/platen.ts`](../../app/web2/src/printer/platen.ts) starts
  [`printer/platen.worker.ts`](../../app/web2/src/printer/platen.worker.ts)
  then (lazily: the worker fetches its own non-threaded module,
  `platen-<version>.js` beside `main.mjs`, built by `make platen-module`),
  and posts the wasm memory and the address; the worker keeps one
  interpreter per emulated machine's printer (created on its first job,
  freed by the ring's PRINTER_FREE when the machine goes), so what a job
  makes permanent with `exitserver` lasts until then.  It parks in
  `Atomics.waitAsync` on the outbound ring's head while the C side wakes
  it with `emscripten_futex_wake`.  Each finished PDF comes back to the
  page as a transferable and opens in a viewer dialog (the browser's own
  PDF viewer in a frame, with Download and Open-in-a-tab), named
  `<job>-<title>.pdf`, releasing the pointer lock first so the cursor
  is free to use it; a browser without an inline viewer
  (`navigator.pdfViewerEnabled` false, e.g. Chrome on Android) downloads
  it at once instead.  The status bar shows the printer's activity from
  the `printer_status` event and reopens the last document.  The protocol is
  [`laserwriter_ring_protocol.h`](../../src/core/network/laserwriter_ring_protocol.h)
  / `printer/platenProtocol.ts`; the whole path is
  [`docs/reference/protocols/laserwriter-session.md`](../reference/protocols/laserwriter-session.md) §5.5.

One input rides the module config the other way: **`gsAudioWorkletUrl`**,
the bundled audio-out worklet, which `em_audio.c` loads when sound
starts.

These callbacks are for one thing only: a platform transport handing the
page a handle or a buffer (screen geometry, a ring's control block, a
worklet URL). Anything the core has to *say* — run state, a media change,
a log line, progress, a download's chunk — is an event on the mailbox's
event ring (`gs_event_emit`, "Events from the core" below), not a new
`Module.on*` callback: an event never blocks the emulator thread, is
ordered with the results, and reaches headless clients the same way.

## The Mailbox

Every JS↔C request travels through the mailbox: a control block and two
record rings in the wasm heap (`src/core/mailbox/mailbox.h`; mirrored in
[`app/web2/src/bus/mailbox.ts`](../../app/web2/src/bus/mailbox.ts), on the
record-ring primitive `mailbox_ring.h` / `bus/mailboxRing.ts` the platen and
Voodoo2 GPU transports share). The page writes `REQ_EVAL` records into the
request ring and wakes the worker; the emulator thread drains them at every
tick — all of them, under a 2 ms budget — and writes an `EVT_RESULT` per
request into the event ring; a reader loop on the page resolves the promise
whose id it carries. Any number of requests may be in flight; a late answer
can never be mistaken for another call's.

```
control block, 32 × uint32, 64-byte aligned (`_get_gs_mailbox()`)
  [0]  MAGIC 'GSMB'   [1] VERSION 9
  [2]  REQ_OFF  [3] REQ_SIZE  256 KB   request ring, page → core
  [4]  EVT_OFF  [5] EVT_SIZE  1 MB     event ring,   core → page
  [6]  REQ_HEAD (page)  [7] REQ_TAIL (core)   free-running byte counts
  [8]  EVT_HEAD (core)  [9] EVT_TAIL (page)
  [10] STATUS  [11] HEARTBEAT  [12] READY  [13] GPU_AVAILABLE
  [14..19] statistics: requests, events, stalls, longest drain µs, refused,
           events dropped
records: {u32 kind, u32 len} + payload; len a multiple of 8; PAD to the end
  REQ_EVAL      {id, client, deadline_ms, path_len, args_len} + path + args
  REQ_SCRIPT    {id, client, deadline_ms, src_len} + src          (a job)
  REQ_CANCEL    {id, client, target_id}     REQ_MODE_STOP {id, client, owner}
  REQ_ACK_BUF   {id, client, handle}        (a transfer buffer was consumed)
  EVT_RESULT    {id, ok, json_len, out_len} + json + output
  EVT_PROGRESS  {json_len} + {"id":request,"done":n,"total":n}
  EVT_STATE / EVT_NOTIFY / EVT_LOG {json_len} + json      (events from the core)
```

Limits: a path of up to 1023 bytes and an arguments document of up to
128 KB (the page refuses larger ones before writing); a result of up to
256 KB (`GS_MBX_RESULT_MAX`), on every client. A larger result is an error
that names its size and the limit — never truncated, never routed some
other way; data that can be larger is a download (below) or is asked for
in bounded pieces, as `memory.peek.bytes` is. A result the event ring has
no room for is held back and delivered once the page has read; the core
never blocks on the page.

**Output.** What a leaf prints while it runs (every stdout site in the core
goes through the sink `gs_out.h`) travels with its answer: `EVT_RESULT`'s
`output` text, which the page hands to the terminal, so a page leaf's
printout reads as it did when stdout reached the terminal directly. A
job's output (below) arrives as `EVT_LOG {"event":"output","id":request,
"client":c,"text":...}` records in the order the job produced it, before
the job's result. Outside any request — boot messages, a breakpoint hit —
text still goes to stdout and `Module.print`.

**Transfer buffers.** Bulk data reaches the page through a buffer in the
core's heap that an I/O job owns: the job publishes it under a handle,
an event names it as `{handle, ptr, len}`, the page reads the bytes
through the memory as it is at that moment (`Module.wasmMemory.buffer`,
never a cached view: the heap grows) and acknowledges it with
`REQ_ACK_BUF {handle}`, and the core hands it back to the job to refill.
Every buffer has an owner, the job, which releases its handle when it
ends. Downloads (below) take this road; uploads go through the transfer
window (`files.xfer_buffer`, 2 MB, static), a shared buffer the page
fills, whose `xfer_write` / `xfer_read` run as I/O jobs.

### Events from the core

The core also speaks first. `gs_event_emit` (`src/core/event/gs_event.h`)
takes a kind and a small JSON object and, in the browser, writes it as an
`EVT_STATE` / `EVT_NOTIFY` / `EVT_LOG` record on the event ring at once,
waking the page — from a leaf, from the tick, from anywhere on the emulator
thread; an event never blocks (no room: dropped and counted). The page's
reader loop runs for as long as anyone listens and hands each event to
`onCoreEvent` subscribers in `bus/emulator.ts` as `{kind, event, data}`;
the last 64 are on `window.__gsCoreEvents` for automation.

The scheduler is the first emitter. Every run is a **mode** with an owner
(the client whose request started it) and, once it stops, a reason:

```
EVT_STATE {"event":"mode_started","mode":N,"owner":C,"budget":I}
EVT_STATE {"event":"mode_ended","mode":N,"owner":C,"reason":R,"pc":P,"instr_count":I}
  R ∈ budget | breakpoint | stop_request | cancelled | assert
```

`mode_ended` goes out from `scheduler_run_frame` at the point where the
run stops, whichever path stopped it — the instruction budget, a
breakpoint, `scheduler.stop`, an assertion — so a `debug.step` emits it
before its own result. The page's run/paused state follows these events.

The rest of what the tick used to diff and push through `Module.on*`
callbacks is emitted at its source too; the page routes each in
`routeCoreEvent` (`bus/emulator.ts`):

| Event | Emitted by | Payload |
|---|---|---|
| `state:speed` | the scheduler, whenever the effective speed changes (governor step, pin, mode switch) | `{x256}` |
| `state:breakpoint_hit` | the debugger, at the hit | `{pc, addr}` |
| `state:assert_failed`, `state:assert_expr` | the failure hook | `{where}`, `{expr}` |
| `state:perf` | the tick, ~1 Hz; the sample that would straddle a machine change is skipped | `{mips, tps, tick_max_ms, tick_p50_ms, poll_max_ms}` |
| `state:screen` | the renderer, where it consumes a shape change, and once when a machine is attached (boot or restore, before any frame) | `{width, height, par_w, par_h}` |
| `state:machine_booted` | the end of `system_create`: `machine.boot`, `checkpoint.load` (not `machine.restart`, which builds nothing) | `{model, restored}` |
| `notify:floppy` | the floppy controller, on insert, eject (guest or host) and restore | `{drive, present}` |
| `notify:media` | the SCSI bus, when a device's medium is inserted or ejected (guest or host) | `{bus, id, present}` |
| `notify:drive_activity` | the tick, on a light's edge; a machine change turns lit lights off and re-bases | `{kind, state}` |
| `notify:checkpoint_saved` | `system_quick_checkpoint` | `{elapsed_ms}` |
| `notify:printer_status` | the PAP layer, when the LaserWriter's status string changes | `{status}` |
| `notify:download_chunk` | the download job, per 4 MB chunk | `{id, handle, ptr, len, last, name}` |
| `log:log` | the log sink, every line | `{line}` |
| `log:output` | the job layer, a job's printed text | `{id, client, text}` |

What still crosses as a `Module.on*` callback is a platform transport
handing the page a handle or a buffer (the Voodoo2 and
printer rings, camera and microphone rings): not an event about the
machine.

### Jobs: scripts off the emulator thread

A terminal line is not a `shell.run` call any more. The page posts it as a
`REQ_SCRIPT` record (`{id, client, deadline_ms, src_len} + src`) and the
core queues it as a **job** for the one **job thread** (`src/core/job/job.h`),
created at boot right after `READY`. The interpreter runs there, on its
own stack, and touches nothing but its own memory: every read or write of
the object tree (`node_get` / `node_set` / `node_call`, and the REPL's
printing of a result) is handed to the emulator thread through the
**seam**, `job_on_emulator()`, and served in the same drain that serves
the page — at the next frame boundary while the machine runs, within a
millisecond while it is stopped. A leaf that starts a *bounded* mode
(`scheduler.run N`, `debug.step N`) holds the job until that mode ends, so
`scheduler.run N` inside a script means "run N"; a bare `scheduler.run`
from the terminal returns at once (the machine runs on; Ctrl-C stops it),
while a headless script's bare `scheduler.run` waits for the machine to
stop (`job_glue_unbounded_waits`). The job's answer is an `EVT_RESULT`
carrying the shell's new prompt (what `shell.run` returned), or
`{"error"}` when the script failed or was cancelled. A build without a
job thread (headless today, the unit suites) runs the script inline in
the drain, with the same interface.

Scripts are FIFO — one runs at a time per process; `REQ_EVAL` leaves from
any client are served alongside. The scope stack, alias table and
function table are shared behind one lock (`job_tables_lock`), taken per
operation; function bodies are reference-counted, so a redefinition or
removal from another client cannot free a body a job is executing.

`REQ_CANCEL {id, client, target_id}` cancels a job of that client: a
queued one finishes at once, a running one unwinds at its next statement
(`E_CANCELLED` → `{"error":"cancelled"}`), and the mode it started, if
one is running, is stopped. `REQ_MODE_STOP {id, client, owner}` stops a
running mode by owner (0: any); both answer `true` / `false`.

**I/O jobs.** A leaf whose cost is the size of a file rather than of the
machine — `files.cp`, `files.import`, `files.export_raw`,
`files.hd_create` / `fd_create` / `profile_create`, `files.xfer_write`
/ `xfer_read`, `files.archive.extract`, a SCSI `image.export` and the Lisa
`profile.save`, `files.download`, and the quick checkpoint's publish — runs on
the **I/O worker** (`src/core/io/io_worker.h`), a second thread created
at boot. `meta.method_info` reports such a method with `io: true`
(`MM_IO`). The leaf takes its request off the drain's answer path
(`gs_result_defer`) and returns at once; the worker does the work in
1 MB chunks (`GS_IO_CHUNK_KB`), yielding between them and reporting
progress (`io_progress` → `EVT_PROGRESS {id, done, total}`; `gsEvalWithProgress`
on the page); the completion, reported at a later drain, writes the
request's `EVT_RESULT` (`gs_result_complete`), so the page's promise
settles when the file is done and the emulator thread served frames
throughout. A script's call is held the same way and a failure is the
call's error. `REQ_CANCEL` of the request — or of the script whose call it
is — cancels the job at its next chunk (`io_cancelled`); a cancelled copy
leaves no partial destination. Writing over, moving or removing a path a
device has open answers `E_BUSY`. An `image.export` snapshots the disk's
read side (its own handles, a copy of the modification bitmap) on the
emulator thread and write-locks the device until the file is written: a
guest write to it fails meanwhile, as a drive being copied does. Without
a worker (`--io=sync`) the same work runs inline, with the same hooks.

**Downloads.** `files.download path` is an I/O job that reads the file 4 MB at
a time into a transfer buffer and announces each chunk as
`notify:download_chunk`; the page copies the bytes into a Blob part,
acknowledges the buffer (the worker refills it), and on the last chunk
saves the Blob through a transient anchor (`bus/download.ts`). Neither
thread waits on the other; a page that never acknowledges times the job
out after 30 s, not the machine. The LaserWriter's PostScript capture
(`appletalk.printer.capture`) takes the same road.

**Ctrl-C, exactly.** The terminal is client 2 (the rest of the page is
client 1). Ctrl-C cancels the terminal's foreground job if it has one;
else stops a run *the terminal* started (`REQ_MODE_STOP {owner: 2}`);
else prints `^C  (nothing to interrupt; Pause stops the machine)` — a
machine running because the toolbar or a resume started it is not the
terminal's to stop. `shell.interrupt` remains as a leaf with the same
meaning for the client that calls it.

### Wake-ups and the idle wait

The page stores `REQ_HEAD` and `Atomics.notify`s it; the core stores
`EVT_HEAD` and wakes the page's `Atomics.waitAsync`. While the machine runs,
a request is served at the next tick (≤ one frame). While it is **stopped**
the tick parks in a bounded futex wait on `REQ_HEAD` — 4 ms slices, 12 ms in
all, draining the pthread's proxied input events between slices — so a
request on an idle machine is served in about a millisecond.

### Liveness

`HEARTBEAT` is bumped once per tick and once per idle slice. The page samples
it every second while visible: three seconds without a change while requests
are pending marks the emulator dead (every pending request fails with a
transport error, the crash banner shows). A wasm trap or `abort()` does the
same through `Module.onAbort`. An ordinary request past 120 s fails for its
own caller only; known-long requests (`checkpoint.*`, `files.cp`, …) have
no deadline.

## Module Bootstrapping

Entry point: [`app/web2/src/main.ts`](../../app/web2/src/main.ts).

1. Synchronous pre-mount work:
   - Load persisted state from `localStorage` (skin, panel pos+size,
     debug pane state, …).
   - Apply the appearance (`applyAppearance`, see "Styling and skins"):
     `index.html`'s pre-paint script has already set `<html data-skin>`
     before any stylesheet, so there is no flash; this corrects it
     against the skin registry.
   - Auto-pick panel orientation from viewport size if no persisted
     value.
2. **WebGL2 probe.** [`lib/webglCheck.ts`](../../app/web2/src/lib/webglCheck.ts)
   creates an off-DOM canvas and asks for a `webgl2` context. If
   missing (e.g. Chrome GPU process dead, hardware acceleration
   disabled), the app renders a full-page error overlay via
   [`lib/webglErrorPage.ts`](../../app/web2/src/lib/webglErrorPage.ts) and
   does **not** mount Svelte. The error page is vanilla DOM so it
   survives a degraded framework runtime.
3. Mount the Svelte tree.
4. Post-mount async (after `App.svelte`'s effects have run):
   - `await whenModuleReady()` (resolved by `bus/emulator.ts::bootstrap`
     once the bridge's `ready` flag flips). Exposes `window.__gsReady =
     true` for headless automation
     ([`scripts/ui2-diag.mjs`](../../scripts/ui2-diag.mjs)).
   - `maybeOfferBackgroundCheckpoint()` — surfaces a resume prompt if
     this browser's machine has a saved checkpoint.  Skipped when the URL
     boots a machine (it has a `rom=`): the saved checkpoint is left
     unoffered.
   - `processUrlMedia()` — handles the URL's media parameters (any of
     them starts it).

Module-construction call ([`bus/emulator.ts::bootstrap`](../../app/web2/src/bus/emulator.ts)):

```ts
const url = new URL(`main.mjs?v=${bust}`, document.baseURI).href;
Module = await createModule({
  canvas,
  // Pthread workers must load this exact URL, cache-buster included.
  mainScriptUrlOrBlob: url,
  locateFile: (p) =>
    p.endsWith('.wasm') ? new URL(`main.wasm?v=${bust}`, document.baseURI).href : p,
  print: routePrintLine,
  printErr: routeErrLine, // also recognises a worker crash
  onAbort: (what) => markBridgeDead(`Aborted(${String(what ?? '')})`),
  onScreenResize: handleScreenResize,
  // ...the other transport callbacks listed above...
  gsAudioWorkletUrl,
});
```

Both URLs resolve against the document, not the site root, so the build
works under any deploy path.

The canvas reference is passed once; Emscripten transfers it to the
worker via `transferControlToOffscreen` and resolves the `#screen` DOM
id from `OFFSCREENCANVASES_TO_PTHREAD`. After `createModule` returns,
JS calls `Module._get_gs_mailbox()` to resolve the mailbox's control
block, verifies its MAGIC and VERSION, waits for `READY`, then
`await gsEval('machine.register', …)` to activate the per-machine
checkpoint directory.

## Major UI Surfaces

The Svelte app is organised under
[`app/web2/src/components/`](../../app/web2/src/components/):

- **Display** ([`display/`](../../app/web2/src/components/display/)) —
  ScreenView (the canvas), DisplayToolbar (zoom, pause/run, save,
  theme), DropOverlay (drag state machine §8.5), WelcomeView with
  Home / Configuration slides for new-machine setup.
- **Workbench** ([`workbench/`](../../app/web2/src/components/workbench/))
  — flex container with the Display + a resizable Panel docked
  bottom / left / right.
- **Panel views** ([`panel-views/`](../../app/web2/src/components/panel-views/)):
  Terminal (console + command browser), SYSTEM (see below), Logs,
  Filesystem tree, Images, Checkpoints, Debug (Disassembly + Registers +
  FPU + Memory + MMU + Breakpoints + Watchpoints + Call Stack).
- **Status bar** ([`status-bar/`](../../app/web2/src/components/status-bar/))
  — machine state, drive activity, in-flight upload progress. The HD /
  FD / CD lights are real: the core counts every drive read and write on
  the image (`files.images[i].reads` / `.writes`), the worker tick sums
  them per kind and emits a `drive_activity` event only when
  a light changes, holding each on at least 100 ms
  ([`drive_activity.c`](../../src/core/storage/drive_activity.c)). A model
  shows only the lights its profile has drives for.
- **Common** ([`common/`](../../app/web2/src/components/common/)) —
  CollapsibleSection, Tree, Table, PaneSplit, Modal, Toast, ContextMenu,
  ValueEditor (a value's editor by its type descriptor), PathField, Icon
  (codicon sprite at [`src/icons/sprite.svg`](../../app/web2/src/icons/sprite.svg)).
- **Primitives** ([`ui/`](../../app/web2/src/components/ui/)) — the
  styled building blocks every view composes: buttons, inputs and form
  fields; Tabs, Toolbar, Separator; Disclosure, TreeItem (the one row
  look of the Files, SYSTEM and command-browser trees), ListRow;
  SectionHeading, Hint, Sash, Switch; Badge, ProgressBar, Spinner,
  ActivityDot, StatusDot, DriveLight, Card, Hero, Callout. Each draws only
  from component tokens, so a skin restyles it without touching its markup.
- **Dialogs** ([`dialogs/`](../../app/web2/src/components/dialogs/)) —
  ConfirmDialog and PromptDialog, plus the app-wide questions that
  [`state/dialogs.svelte.ts`](../../app/web2/src/state/dialogs.svelte.ts)
  asks (`askText`, `askConfirm`) in place of the browser's `prompt()` and
  `confirm()`, which cannot be styled (a lint forbids them).

State lives under [`app/web2/src/state/`](../../app/web2/src/state/) —
each `*.svelte.ts` file owns a `$state` slice (`machine`, `layout`,
`debug`, `appearance`, `logs`, `images`, `uploads`, `toasts`, …). The bus
layer at [`app/web2/src/bus/`](../../app/web2/src/bus/) wraps every
`gsEval` call site.

## SYSTEM tab

[`SystemView.svelte`](../../app/web2/src/components/panel-views/machine/SystemView.svelte)
shows the model's state and edits it. Its rows come from
[`lib/systemRows.ts`](../../app/web2/src/lib/systemRows.ts), which builds
them from `meta.members` with values.

**Shared member data.** The SYSTEM tab and the command browser read the
model through [`bus/memberStore.ts`](../../app/web2/src/bus/memberStore.ts):
- one cache of each node's structure (`meta.members` without values),
  keyed by path; values are never cached;
- one table of the core events that change what the views show, and the
  structure each drops (`changeFor`); a finished console job drops the
  cached levels that list a collection;
- `onMembersChanged` tells the views, after the cache has dropped what
  changed.

Both views keep their tree in a
[`TreeState`](../../app/web2/src/lib/treeState.svelte.ts): the open rows
(by key, which is the path for a model row), the loaded levels, the rows
on screen, and a refresh that re-reads the root and every open level
(sibling subtrees in parallel, one refresh at a time). A level opened while
a refresh runs is kept when the refresh lands.

**Rows.**
- The root's children sit under their domain dividers.
- A node's attributes and children follow in model order.
- A collection expands to its live entries (`path[i]` / `path["k"]`).
- Internal members are never shown; advanced ones only with the Advanced
  toggle.
- A node that has only methods (no attributes, no children) is not a
  folder: its methods are a submenu of its parent's context menu.

**Values** are shown as the REPL prints them
([`lib/typeDescriptor.ts`](../../app/web2/src/lib/typeDescriptor.ts)).
What is an enum, object, error or container is decided in one place,
[`lib/taggedValue.ts`](../../app/web2/src/lib/taggedValue.ts): a tag is a
plain object with exactly the tag's keys (`{enum, index}`,
`{object, name, path}`, `{error}`); the console's value tree uses the same
rule and text.

| Value | Shown as |
|---|---|
| hex integer | `0x408986` |
| other integer | decimal |
| enum | its name |
| bool | a toggle |
| float | `%g` |
| sensitive value | `••••` |
| list, map | compact JSON, expandable |

Read-only values are dimmed and show a lock on hover.

**Editing.** Double-click a value, or press Enter / F2 on the selected row.
A bool toggles with one click; an enum with values edits in a dropdown.
Enter commits and Esc cancels. A commit takes one of two paths:
- **Literal** (an integer in hex, decimal or binary, `true` / `false`, an
  enum name, or any text for a string): written with `gsEval(path,
  [value])`, then echoed to the console as the statement it equals (e.g.
  `machine.cpu.d0 = 0x1234`). An error shows under the field and the value
  reverts.
- **Anything else**, and integers above 2^53: runs as the console statement
  `<path> = <text>`.

**Context menu.**
- Runs the node's methods. A destructive one asks for confirmation first.
- A few methods the tab runs its own way, listed in its handler map by the
  method's path or name: `export` writes the image to `/opfs/exports` and
  downloads it.
- A method with arguments opens a form generated from the arguments' type
  descriptors: `path` arguments get a file browser over `files.list`.
- A call is echoed as its statement in argument mode.
- Every row also has **Copy value** and **Copy path**.

**Refresh.** Open levels re-read:
- on the core's state events (`mode_started` / `mode_ended`,
  `breakpoint_hit`, `speed`, `checkpoint_saved`, `floppy`,
  `drive_activity`, `media`);
- after every console job and SYSTEM action;
- every 2 s while the machine runs and the page is in the foreground.

On `state:machine_booted` the whole tree reloads, keeping the open levels
by path.

**Revealing a node.** Ctrl/Cmd-click on a console object link opens SYSTEM
at that node (`revealInSystem`).

## Upload Pipeline

Four deliberate ways to get a media image into OPFS, all routing
through [`app/web2/src/bus/upload.ts`](../../app/web2/src/bus/upload.ts).
Every byte goes through the core's **transfer window**
([`bus/xfer.ts`](../../app/web2/src/bus/xfer.ts)): the page copies a chunk
into a fixed buffer in wasm memory and `files.xfer_write` writes it on
the emulator thread (`files.xfer_read` is the reverse).  The page never
calls `Module.FS`: under WasmFS that runs on the page's thread and
busy-waits for the OPFS thread, and in Safari — where WebKit serves a
worker's OPFS request through the page's thread — it deadlocked the page.

1. **New Machine dialog dropdowns** — picking "Upload image…" in a
   floppy / HD / CD / ROM / VROM slot calls
   `pickAndUploadAs(mediaId)` →
   `acceptFilesAsCategory(files, mediaId)`. Strict per-category
   validation; rejects mismatched files with a toast. The slot then
   selects what was stored, and a cancelled or rejected upload keeps the
   previous pick. The floppy / HD
   slots also offer "Create blank image…", which opens
   [`CreateImageDialog.svelte`](../../app/web2/src/components/display/CreateImageDialog.svelte)
   and creates a blank image directly in OPFS via `files.fd_create`
   (800 KB / 1.4 MB) or `files.hd_create` (size from
   `machine.scsi.hd_models`; named `.dmg`, so a blank 2 GB disk is a
   4 KB UDIF).
2. **Drag-and-drop onto the Display** —
   [`DropOverlay.svelte`](../../app/web2/src/components/display/DropOverlay.svelte)
   captures drops, calls `processDataTransfer` →
   `acceptFiles(files)`. Auto-detects type by probing each
   `MediaTypeDescriptor` in order; a file the core identifies as an archive
   (`files.archive.identify`: StuffIt, Compact Pro, Zip, BinHex, MacBinary,
   gzip — by content, not name) is not extracted: its members are probed in
   place through the archive's VFS path, and only the first medium that
   validates is copied out into its store. A floppy goes into the first empty drive the
   model has, a CD into the model's CD bay (`bus/media.ts`; an occupied
   bay is refused, not overwritten); ROMs trigger a full cold boot via
   `maybeBootFromRom`.  **Several files in one drop** each run that
   single-file flow on their own (`acceptOne`: stage, probe, store or
   reject, discard; a large one streamed as below), and the drop ends with
   one summary instead of a message per file — "3 stored (2 floppies,
   1 ROM), 1 rejected: 'notes.txt' doesn't look like …".  What follows is
   what single drops would do, and no more: a ROM boots only when it is the
   drop's only ROM, and per category only the first file goes into an empty
   drive.  A checkpoint is loaded only when dropped on its own; in a drop
   with other files it is rejected ("drop a checkpoint on its own"), since
   loading it would replace the machine they were meant for.
3. **Drag-and-drop onto the Filesystem tab** —
   [`FilesystemView.svelte`](../../app/web2/src/components/panel-views/filesystem/FilesystemView.svelte)
   accepts external file drops on folder rows, calls
   `acceptFilesRaw(files, targetDir)`. **No validation** — the
   Filesystem view is the low-level OPFS browser — except into
   `/opfs/images/hd` and `/opfs/images/cd`, the emulator's own stores, where
   a large image is imported as one (below). The same tab also does
   *internal* drags — move within OPFS, and **copy a file/folder out of a
   disk image** to an OPFS folder — through the operations in
   [`bus/fsOps.ts`](../../app/web2/src/bus/fsOps.ts).
4. **Drag-and-drop onto an Images-tab category** —
   [`ImageCategorySection.svelte`](../../app/web2/src/components/panel-views/images/ImageCategorySection.svelte)
   wraps each section in a drop host. Drop calls
   `acceptFilesAsCategory(files, mediaIdFor(cat))`. Same strict
   per-category validation as path 1.

**Large disk images are imported streamed, never staged expanded.** The
browser charges a file's logical length against the origin's quota, zeros
included, so a hard-disk or CD image stored as a raw file costs its full
size — and staging it before copying it cost that twice.  A file larger than
`LARGE_IMPORT_BYTES` (16 MiB, [`lib/media.ts`](../../app/web2/src/lib/media.ts))
dropped on the Display, picked or dropped as an HD/CD, or dropped into
`/opfs/images/hd` or `/opfs/images/cd` in the Filesystem tab goes through
[`bus/importImage.ts`](../../app/web2/src/bus/importImage.ts): its decoded
bytes stream through the transfer window into the core's UDIF writer
(`files.udif_open` / `udif_append` / `udif_finish`), which drops zero runs and
deflates the rest in 64 KB chunks, into a `<name>.dmg.part` of its own in the
scratch area (below); that is validated as a CD or hard disk (the image layer reads UDIF in place) and
moved into `/opfs/images/<category>/<name>.dmg`.  A zip is unpacked
forward-only on the way ([`lib/zipStream.ts`](../../app/web2/src/lib/zipStream.ts):
local headers, `DecompressionStream('deflate-raw')`, CRC-32 checked, data
descriptors found by signature), a gzip by `DecompressionStream('gzip')`; a
bare zip's members are tried in order until one stores.  A `.dmg` the
emulator can read in place is stored as it is (`files.udif_info`), one with
chunks too large is re-chunked (`files.convert`).  Mac archives (StuffIt,
Compact Pro, BinHex, MacBinary) still go through staging and
`files.archive.extract`.  The status bar shows bytes read and stored and a
cancel button, for an `HD=` / `CD=` download as for an upload; every exit
— stored, failed or cancelled — removes the `.part`.  Smaller files keep the
staged flow, now moved (`files.mv`) into place rather than copied.

**The scratch area.** Every file the page writes on its way somewhere else
lives in `/opfs/upload/.scratch/` (`lib/opfsPaths.ts`: `SCRATCH_DIR`,
`scratchPath`): an upload being probed, a dropped checkpoint being loaded, a
URL download, a streamed import's `.dmg.part`, the Save State file while it
is downloaded, a file copied out of an image to be downloaded.  Each
operation writes under a name no other uses (`<nonce>-<name>`), so two
uploads of one name, or two URL boots of one slot, never touch each other's
file, and removes its file on every exit (`try`/`finally`).  Nothing is ever
attached from the scratch area: what is kept is moved into
`/opfs/images/<category>/` first.  The core empties the directory at startup
(`em_main.c`), which covers an operation a closed tab or a crash cut short.
The rest of `/opfs/upload` is the user's (the Filesystem tab may put files
there) and is never touched.

All four paths run through `startActivity` / `endActivity`
([`state/activity.svelte.ts`](../../app/web2/src/state/activity.svelte.ts)) so
the status bar shows a spinner with a "\<verb>: \<name>" label during long
operations. The verb is general — uploads show "Uploading", and the
Filesystem-tab worker ops reuse the same indicator ("Copying", "Moving",
"Deleting", "Unpacking", "Downloading"). Confirmation toasts are centralised
in [`state/toasts.svelte.ts`](../../app/web2/src/state/toasts.svelte.ts).

## C-side surfaces the UI consumes

Highlights — see [object-model.md](../internals/core/object/object-model.md) for the
typed-dispatch and introspection surface.

- **`machine.rom.identify(path)`** → `{recognised, checksum, name,
  compatible[], size}`. Drives the Model dropdown in the New Machine
  dialog.
- **`catalog.vroms.identify(path)`** / **`catalog.proms.identify(path)`** →
  the card a video ROM / PCI expansion ROM belongs to, or `null`.
- **`machine.floppy.identify(path)`** → density string (`400K` / `800K` /
  `1.4MB`); empty if not a floppy.
- **`machine.scsi.identify_hd(path)` / `machine.scsi.identify_cdrom(path)`**
  → bool.
- **`files.archive.identify(path)`** → the archive format (`sit`, `cpt`,
  `zip`, `hqx`, `bin`, `gz`) or an empty string.
  **`files.archive.extract(path, out_dir)`** → bool; powers the
  Filesystem-tab "Unpack" action (an upload probes the archive in place). See [peeler.md](peeler.md).
- **`files.list(path)`** → `[{name, kind, size, expandable}]`, descending into
  disk images (partitions, then HFS/UFS contents) and archives, nested to any
  depth. `expandable` marks a file the core can open as a tree; the Filesystem
  tree expands exactly those; see [`target-filesystems.md`](../internals/core/storage/target-filesystems.md).
- **`files.cp(src, dst, [recursive])`** — copy, including *out of* an image into
  OPFS (backs copy-out and Download). **`files.rm(path)`** /
  **`files.mv(src, dst)`** — recursive remove / move, run worker-side so
  WasmFS stays coherent (see Persistence above).
- **`files.hd_create(path, size)`** / **`files.fd_create(path,
  high_density)`** — create a blank HD / floppy image; **`machine.scsi.hd_models`** →
  drive-size catalog. These drive the New Machine dialog's "Create blank
  image…" option.
- **`catalog.profile(id)`** → the model's profile: `name`,
  `ram_options[]`, `ram_default`, `floppy_slots[]`, `hd_bays[]` (each
  `{bus, id, label}`, the firmware's boot bay first, on whatever bus it
  is), `cdrom` (the CD bay, or `null`), `video_slots[]`, `capabilities`
  (MMU kind, FPU, `video_in`, `audio_in`, `aux_cpus`), … — drives the
  New Machine dialog's rows (RAM from `ram_options`, one floppy row per
  slot, a Bay selector when there is more than one hard-disk bay, a CD row
  only with a CD bay), the Debug view's panels and the status bar's
  drive lights.  `bus/profile.ts` reads it once per model.
- **`machine.videoin.source`** (`none`/`pattern`/`file`/`host`) plus the
  read-only `connected` / `fields` — the AV video digitizer's host source.
  The camera toolbar button sets `host`; the button itself is gated on
  `capabilities.video_in` from `catalog.profile`. See
  [../machines/av/vdc.md](../internals/machines/av/vdc.md).
- **`machine.boot(model=..., rom=..., ...)`** — destroys any current
  machine and creates a fresh one from a complete configuration document
  (model and rom required; nothing is inherited from the previous
  machine).
- **`machine.restart`** — power-cycles the running machine: a reset with
  the RAM cold. Nothing is torn down, so PRAM/NVRAM, the clock, the
  mounted media, the Caps Lock latch and the LaserWriter survive; the
  Restart button re-asserts nothing afterwards. A changed configuration
  is a new `machine.boot` document.
- **`debug.frame([addr], [count], [before])`** — bundled snapshot for
  the Debug tab, the same call as **`machine.cpu.frame`**: `{arch, pc,
  regs, rows, fpu?}` — the core's own register names, disasm rows with
  per-row MMU translation (`phys`/`valid`), optional `fpu` block.  One
  round-trip per pause.  Every CPU-like object answers the same contract:
  an auxiliary core listed in `capabilities.aux_cpus` has
  `machine.<name>.frame` (the AV DSP3210's `machine.dsp.frame`).
- **`debug.disasm([addr], [count])`** — pretty-prints to stdout,
  returns `V_BOOL` (truthy for shell `assert ${…}` use). The web2
  Disasm pane uses `debug.frame` instead.
- **`debug.breakpoints.add(addr [, condition])`** — set (a second add at
  the same address returns the existing entry);
  **`debug.breakpoints.meta.indices("entries")`** — the live ids (ids are
  never reused, so never walk by `count`);
  **`debug.breakpoints.entries[id].{addr,enabled,condition,hit_count}`** and
  **`.remove()`** — read, toggle, and clear one entry.
- **`machine.memory.peek.{b,w,l}(addr)`** — single-byte / word / long read.
- **`machine.memory.peek.bytes(addr, count)`** — bulk read, `V_BYTES`,
  capped at 4 KB. The Memory pane uses this so a 128-byte refresh is one
  bridge call.
- **`machine.floppy.drive[i].insert(path, writable)` / `.eject()` /
  `.present`** — only the drives the model has.
- **`<path>.meta.members([values])`** — every member of a node in one
  call: `{name, kind, category, label, doc}` plus, per kind, `readonly`
  and `value`, `indexed` and `indices`, or the method's UI metadata. The
  Machine tree, its context menu and the command browser read it.
- **`files.images[i].reads` / `.writes`** — the drive I/O counters
  behind the activity lights.
- **`machine.attach_hd(path, [bay])` / `machine.attach_cdrom(path)` /
  `machine.eject_media(bus, [id])`** — media by bay, on whatever bus the
  bay is (`machine.scsi`, `machine.scsi2`, the Lisa's ProFile).  `bay`
  indexes `catalog.profile(id).hd_bays` (0, the default, is the boot bay);
  the CD goes to `profile.cdrom`.  Each attach answers the bay it used,
  `{bus, id, label}`, which is what `eject_media` takes; an occupied bay,
  a bay the model does not have, and a CD on a model with no CD bay are
  refused.  The per-bus `machine.scsi.attach_hd(path, id)` /
  `attach_cdrom(path, id)` and `machine.scsi.device[id].eject()` remain
  for scripts that mean one SCSI id on one bus.
- **`checkpoint.save(path)` / `checkpoint.load(path)`**

## Filesystem Layout

```
/                              Memory (default WasmFS root)
├── opfs/                       Single OPFS mount — persistent
│   ├── images/
│   │   ├── rom/                ROM images, named by content id
│   │   ├── vrom/               Video ROM images
│   │   ├── fd/                 Floppy images (400K / 800K / 1.44 MB)
│   │   ├── hd/                 SCSI hard-disk images (imported as UDIF .dmg)
│   │   └── cd/                 CD-ROM images (imported as UDIF .dmg)
│   ├── checkpoints/
│   │   └── <machine-id>-<ts>/  Per-machine checkpoint dirs
│   │       ├── state.checkpoint
│   │       └── <id>.delta / <id>.journal   Writable image state
│   └── upload/                 The user's; the page writes only in .scratch/
│       └── .scratch/           Files on their way into a store; emptied at startup
└── tmp/                        Memory mount (volatile)
```

`/opfs` is created in C `main()` via `wasmfs_create_opfs_backend()`.
Subdirectories inside `/opfs/images` are created with `mkdir()` on the
worker thread. `/tmp/` uses a memory backend; its subdirectories are
pre-created in C because `FS.mkdir` from the JS main thread fails
cross-thread under WasmFS pthreads.

The core opens media at whatever path it is given and never copies it
elsewhere. Persistence is the web app's job: an upload, and a URL-parameter
download, is stored into `/opfs/images/<category>/` before it is attached
(`bus/upload.ts::persist`, `bus/importImage.ts` for large disks), so the base image lives on OPFS and the path
recorded in checkpoints still resolves after a reload. A volatile path
(`/tmp/…`) attached from the shell stays volatile: the image, its delta and
any checkpoint's reference to it do not survive a reload. Copy it under
`/opfs/` first (`files.import <src> <dst>`) to keep it.

## URL Parameters

Handled in [`bus/urlMedia.ts`](../../app/web2/src/bus/urlMedia.ts) (the
addressing rules are in [`lib/mediaUrl.ts`](../../app/web2/src/lib/mediaUrl.ts))
and invoked from `main.ts` after `whenModuleReady()` resolves.  A URL with a
`rom=` boots straight into a running machine: no Welcome view, no
configuration dialog.

**Nothing is asked on such a load.** `main.ts` marks it before mount
([`state/urlBoot.svelte.ts`](../../app/web2/src/state/urlBoot.svelte.ts)
`beginUrlBoot`), which keeps the "preview build" notice closed (it waits for
a visit to the start screen) and skips the resume-from-checkpoint prompt.
In Welcome's place the display shows
[`UrlBootView`](../../app/web2/src/components/display/UrlBootView.svelte):
the Granny Smith headline, "Downloading the machine's ROM and disks…", and one
row per file with its name and a progress bar.  The bar fills from the
response's `Content-Length`; without one (a streamed zip member, a compressed
response) it is indeterminate, with the bytes received so far.  Rows go
Waiting → downloading → Unpacking… (a container member) → ✓ size, then
"Starting the machine…" until the core reports it running, when the view
goes (a later Shut Down shows the ordinary Welcome).  The ROM is fetched
first; if it cannot be had, or is not a ROM, the disks are not fetched ("Not
needed"), and the view says why and offers **Go to the start screen**.  A
failed disk does not stop the boot: one that cannot be downloaded, or is not
valid as its slot's kind (an 800 KB floppy image given as `hd0=`), is
rejected exactly as the same file dropped on its category is — its row and a
toast give the validator's reason — and the machine boots without it.
Nothing is attached from the scratch copy.  Per-file "fetched" toasts are left to pages not showing the
view; errors still toast.

- `rom=<url>` — downloaded into `/opfs/images/rom/`, auto-identified,
  auto-boots.  **Given twice** (`rom=<chip>&rom=<chip>`) it names the two
  byte-wide chips of a ROM dumped as halves — the Lisa's `341-0175`/`341-0176`,
  the Macintosh XL's `341-0346`/`341-0347`: the page interleaves them, trying
  both orders and keeping the one whose own checksum verifies
  (`machine.rom.identify`), so the order in the URL does not matter.
- `fdN=<url>` (`fd0`, `fd1`) — downloaded into `/opfs/images/fd/`,
  inserted into floppy drive N, when the model has that drive.
- `hdN=<url>` — streamed into `/opfs/images/hd/` as a compact UDIF
  (`<name>.dmg`; a zip member or a gzip body is unpacked on the way, so
  neither the download nor the expanded disk is ever stored whole), attached to the
  model's N-th hard-disk bay (`machine.attach_hd(path, N)`; `hd0` is the
  boot bay, on whatever bus it is — SCSI, a Network Server's second
  channel, the Lisa's ProFile).  `hd0` is also named the startup device, as
  the New Machine dialog and Restart do (`bus/boot.ts::setStartupDisk`): on
  SCSI, PRAM's default device; on a Lisa's ProFile, `BootVol = 2` with the
  parameter-memory checksum left invalid (`machine.hd.pram_init(2, false)`),
  so the boot ROM goes to the ProFile instead of its startup-device screen and
  the OS restores its device table from the disk's own snapshot.  A bare HFS volume
  (no partition map — the shape of most of the archive.org Macintosh
  library), or a partitioned
  disk with no driver (the Disk Copy 4.2 image shape), is attached
  through the volume wrapper and boots
  ([bare-volume-wrapper.md](../internals/core/storage/bare-volume-wrapper.md)).
- `cd=<url>` — streamed into `/opfs/images/cd/` as a UDIF, inserted into the
  model's CD bay (`machine.attach_cdrom`), on a model that has one.
- `vrom=<url>` — downloaded into `/opfs/images/vrom/` and passed in the
  boot document (`machine.boot … vrom=`), so it is this boot's pick for its
  card's declaration ROM, ahead of any other revision of it already stored
  (SE/30 / IIcx / IIfx).  One that is not a vROM is rejected like any other
  medium (above) and the machine boots without it, saying so.  Storing it
  also offers it to the core's ROM catalog, for later boots.
- `speed=paced|accelerated|turbo` — the toolbar's pacing mode from the
  start, set once on the page's run loop (`scheduler.mode`); pacing is host
  state, so every machine the page boots or restores runs under it (legacy
  `max`/`realtime`/`hardware` are accepted as aliases).  The wasm module takes no command line.
- `model=<id>` — preferred machine id (must be in the ROM's compatible
  list).
- `skin=<id>` — show this load in another skin (an id from
  [`skins/registry.ts`](../../app/web2/src/skins/registry.ts)); not saved,
  and an unknown id is ignored.  Read by `state/appearance.svelte.ts` and
  `index.html`'s pre-paint script, not by `urlMedia`, and matched exactly.

**Names are case-insensitive**: `ROM=`, `Rom=` and `rom=` are one
parameter, `HD0=` is `hd0=`, and a bare `HD=` / `FD=` means `hd0` / `fd0`.
The first occurrence of a name wins; a second spelling of it is ignored
with a console warning.

**A value may continue through a container.**  The first path segment
with a container extension (`.zip`, `.sit`, `.sea`, `.cpt`, `.hqx`,
`.bin`) that has more path after it ends the container's URL; the rest,
percent-decoded, is the member to take out of it:

```
ROM=https://host/path/roms.zip/Mac%20IIci/iici.rom
FD1=https://host/disks/Games.sit/Dark%20Castle.img
```

The container is fetched whole; a zip member is found by exact path, then
ignoring case, then by a unique base name; a Mac archive is unpacked by
`files.archive.extract` and searched the same way.  A container named with no
member keeps the old behaviour (a zip's first file, a Mac archive's
`files.find_media` pick).  A missing member is reported with the first
few names the container does hold.

**Encoding.**  Write a value `encodeURIComponent`-encoded.  Browsers let
`:` and `/` through unencoded, so a hand-typed URL works — *unless* it
contains `&` (splits the query), `#` (ends it), `+` (becomes a space) or
`%` (starts an escape).  Member names in the archive.org ROM archive
contain `&` (`9630C68B - Power Mac 7200&7500&8500&9500 v2.ROM`): write it
`%26`.  Spaces may be typed or written `%20`.

**archive.org.**  Its file servers send no CORS headers, so a page on
another origin cannot read `archive.org/download/<item>/<file>` at all.
The page rewrites archive.org URLs to the endpoints that do allow it
(verified 2026-09-28 with `Origin: https://pappadf.github.io`):

| The value names | Fetched from | Notes |
|---|---|---|
| `/download/<item>/<file>` | `/cors/<item>/<file>` | whole file; `/cors/` ignores `Range` |
| `/download/<item>/<x.zip>/<member>` | unchanged | archive.org extracts the member server-side (`view_archive.php`); whole member |
| `/details/<item>/<file>` | as `/download/…` | |
| `/details/<item>` | the item's one original media file | found through `archive.org/metadata/<item>`; an item with several asks you to name one |

Neither endpoint serves partial content, so remote media is always
downloaded whole (then kept in OPFS like any upload).

**Mixed content.**  An `http://` value on an `https://` page is refused
before fetching (the browser would block it), with a message saying so;
network, CORS, 404 and member-not-found failures are each reported as
such.

**Worked example** — a Macintosh IIci booting System 7.5.3 off a bare
archive.org volume, with the ROM taken out of archive.org's ROM archive:

```
https://pappadf.github.io/gs-pages/staging/
  ?ROM=https://archive.org/download/mac_rom_archive_-_as_of_8-19-2011/mac_rom_archive_-_as_of_8-19-2011.zip/368CADFE%20-%20Mac%20IIci.ROM
  &HD0=https://archive.org/download/AppleMacintoshSystem753/System7_5_3.img
```

(one line, no spaces).  A **Lisa 2** booting the Office System 3.1, all from
archive.org — the Rev H boot ROM as its two chip dumps from the `lisa-software`
item, and an IDLE ProFile image out of a zip:

```
https://pappadf.github.io/gs-pages/staging/
  ?ROM=https://archive.org/download/lisa-software/Lisa%20Software.zip/Lisa%20Software/firmware/341-0175-H.BIN
  &ROM=https://archive.org/download/lisa-software/Lisa%20Software.zip/Lisa%20Software/firmware/341-0176-H.BIN
  &HD0=https://archive.org/download/apple-lisa-profile-hd-disk-images-for-lisaem-and-idle-lisa-office-system-3.1-lis/IDLE_LOS3.1-after1stBoot.zip/profile.raw
```

The IDLE images in that item are raw 532-byte-block ProFile disks; the
Office System 3.1, Workshop 3.0 and Xenix ones boot (verified 2026-09-28);
`IDLE_MacWorksXL30` stops with boot-ROM error 23, and the `LisaEM_*` ones <!-- lint-allow: LisaEm -->
(DiskCopy 4.2 files with tags) have not been tried as ProFile disks.  The
`apple-lisa-h-1983` item's chip dumps do not verify (scattered single-bit
differences from the known Rev H ROM) and are refused.

`AppleMacintoshSystem701/System7_0_1.img` is a
Plus-only minimal install: pair it with
`4D1F8172%20-%20MacPlus%20v3.ROM`.  The e2e spec
`tests/e2e/web2-specs/url-archive-boot.spec.ts` replays this URL with
archive.org's endpoints routed to the gs-test-data copies.

Downloads run one at a time and stream to the scratch area through the
same chunked writer as uploads (`bus/upload.ts::streamToOpfs`), so an
image is never held whole in memory — except a zip, which is read back
whole to unzip.

## Startup Flow

The same sequence as Module Bootstrapping above, end to end:

1. WebGL2 probe — abort to a full-page error if unavailable (Svelte is
   never mounted).
2. Mount Svelte; `ScreenView` calls `bootstrap(canvas)`: load the
   module (`createModule`, no command line), wire the callbacks, resolve
   the mailbox's control block and verify its MAGIC and VERSION; wait
   for the emulator worker and for the WebGPU adapter's answer, which is
   written to the core (`gpu_available`; bounded, see above).
3. Run `machine.register(<machine-id>, <created>)` to set the per-
   machine checkpoint dir.
4. `whenModuleReady()` resolves; `__gsReady = true`.
5. `maybeOfferBackgroundCheckpoint()` — surfaces a resume prompt if a
   `state.checkpoint` exists (not when the URL has a `rom=`).
6. If the URL has any media parameter → `processUrlMedia()` downloads
   and auto-boots; with a `rom=`, the download progress view stands in for
   Welcome meanwhile.
7. Otherwise the Welcome view sits on top of the canvas. The user drops
   a ROM on the Display or opens the New Machine dialog.
8. The New Machine dialog scans `/opfs/images/rom/`, identifies each via
   `rom.identify`, builds the Model dropdown from `compatible[]` model
   ids, and reads profiles (`bus/profile.ts`) to drive the RAM, video,
   floppy, hard-disk bay and CD rows.
9. `Start Machine` → one `machine.boot` document (the ROM included),
   the media into their bays (`bus/media.ts`), the post-boot
   reconciliation, `scheduler.run`. The Welcome layer fades out; the
   canvas takes over.

## Terminal console

The Terminal tab's left pane is a console: DOM-rendered output entries and
a CodeMirror 6 input, in
[`ConsoleView.svelte`](../../app/web2/src/components/panel-views/terminal/ConsoleView.svelte)
and [`ConsoleInput.ts`](../../app/web2/src/components/panel-views/terminal/ConsoleInput.ts).
CodeMirror is code-split (dynamically imported when the console first
mounts). The view is assembled from:

| Module | Does |
|---|---|
| [`lib/stickToBottom.svelte.ts`](../../app/web2/src/lib/stickToBottom.svelte.ts) | auto-scroll (an attachment on the output) |
| [`FindBar.svelte`](../../app/web2/src/components/panel-views/terminal/FindBar.svelte) | the find bar and its state; matching is [`lib/find.ts`](../../app/web2/src/lib/find.ts) |
| [`inputAssist.svelte.ts`](../../app/web2/src/components/panel-views/terminal/inputAssist.svelte.ts) | what the input asks the shell while typing: highlighting, completion, the signature hint |
| [`lib/usage.ts`](../../app/web2/src/lib/usage.ts) | `shell.usage` answers, kept per path until the machine changes (shared with the command browser); an argument's span in UTF-16 |
| [`ValueTree.svelte`](../../app/web2/src/components/panel-views/terminal/ValueTree.svelte) | a value entry's links and expandable lists / maps |

The entries, the input queue and the running job live in
[`state/console.svelte.ts`](../../app/web2/src/state/console.svelte.ts), so
scrollback survives the pane being remounted. `createConsole()` makes one
(`{state, model, submit, interrupt, …, dispose}`); the app's is created
when the module loads, and tests build their own and dispose of them.

**Output.** [`bus/logSink.ts`](../../app/web2/src/bus/logSink.ts) turns what
the core sends into console records: `Module.print` / `Module.printErr`
lines, a job's output pieces (`log:output`), its annotation records
(`log:value_begin`, `log:value`, `log:error`), and the start and end of
the console's own job. It pushes them straight into the app's console
model, which exists from page load, so boot output is there when the
Terminal first opens.
[`lib/consoleModel.ts`](../../app/web2/src/lib/consoleModel.ts) makes
entries from those records:

| Entry | From |
|---|---|
| `command` | the submitted input (shown after a `›` glyph) |
| `text` | a job's printed lines, or a `Module.print` line outside a job |
| `stderr` | a `printErr` line, or another client's `error` annotation |
| `value` | the text between `value_begin` and `value`; with the value's tagged JSON, an object renders as a link to its node in the command browser and a list or map expands (nested items as SYSTEM prints them) |
| `error` | an `error` annotation of the console's job, at its place in the output |
| `echo` | a statement another surface ran for the user (dimmed) |

The core writes a job's error once, as its `error` annotation, so the entry
needs no matching against stderr. The one exception is an error too large
for a record: its annotation is the shortened `truncated` form, which the
console skips, and the full text arrives on stderr instead.

New entries are appended once per animation frame. At most 5 000 are kept.
Off-screen entries skip layout (`content-visibility: auto`). Auto-scroll
follows only while the view is at the bottom. While a job runs longer than
1 s, a "running… N s" line shows below the output.

**Input.**

| Key | Action |
|---|---|
| Enter | submit, unless `shell.needs_continuation(text)` says the block continues (then a newline) |
| Shift+Enter | newline |
| ↑ / ↓ | history, on the first / last line |
| Tab | completion: a lone candidate or a longer common prefix at once, else a popup of `shell.complete(line, cursor, true)` candidates coloured by kind, with their doc (the answer the command browser's sync asked for is reused while the text and cursor are unchanged) |
| Ctrl+C | copy a selection (input or output); otherwise interrupt |
| Ctrl+L | clear the output |
| Mod+F | find in the output |
| Ctrl+Shift+Space | show the signature hint |
| Esc | close the popup, else the signature hint |

Completion offsets are converted between UTF-16 and the core's UTF-8 bytes
([`lib/utf8.ts`](../../app/web2/src/lib/utf8.ts)).

**Highlighting.** 30 ms after the input's text last changed, the console
asks `shell.highlight(text)` and colours the input with the answer
([`lib/highlight.ts`](../../app/web2/src/lib/highlight.ts)); an answer for
an older text is dropped. A `command` entry keeps the colours its text had
when it was submitted. The command browser colours the signature and
example lines of a usage block the same way. Unresolved path segments get
the `unknown` colour with a wavy underline. With the machine running in
turbo, the round trip's 95th percentile over 200 requests is about 17 ms
(one frame of the mailbox's polling), within the 30 ms budget, which
`highlight.spec.ts` checks.

**Running input.**
- Submitted text is queued; the queue runs one script job at a time via
  `gsEvalLine` (`REQ_SCRIPT`, "Jobs" above), so type-ahead runs in order.
- A pasted block (CRLF → LF, trailing blanks and a leading `› ` / `> ` per
  line removed) is reviewed in the input and runs as **one** job on Enter.
- The job's result is the shell's new prompt, shown beside the input.
- History is the last 500 submissions, in `localStorage`
  (`gs.console.history`).

**Interrupting.** Ctrl+C without a selection:
- drops the queue;
- cancels the console's job, else stops a run the console started ("Ctrl-C,
  exactly" above);
- if there is neither, says there is nothing to interrupt.

Cmd+C on macOS is the browser's copy.

**Menus and find.**
- The output's context menu offers **Copy**, **Copy as commands** (the
  statements of the command entries in the selection), **Copy output**
  (the clicked entry's job), **Copy value as JSON**, **Paste**, **Select
  all** and **Clear**.
- Mod+F opens a find bar (next / previous, match case).

**Signature hint.** While the cursor is in a method's arguments
(`shell.complete`'s `context.method`), a hint above the input shows the
method's signature (`shell.usage`) with the current argument underlined
(`arg_spans[context.arg_index]`, also for a `name=value` argument).

### Command browser ↔ console

The command browser
([`CommandBrowser.svelte`](../../app/web2/src/components/panel-views/terminal/CommandBrowser.svelte),
rows from [`lib/commandsTree.ts`](../../app/web2/src/lib/commandsTree.ts),
the tree, selection and marks in
[`state/commandTree.svelte.ts`](../../app/web2/src/state/commandTree.svelte.ts),
the details pane in
[`UsagePane.svelte`](../../app/web2/src/components/panel-views/terminal/UsagePane.svelte))
and the console follow each other through
[`terminalBridge.ts`](../../app/web2/src/components/panel-views/terminal/terminalBridge.ts)
and [`state/terminalSync.svelte.ts`](../../app/web2/src/state/terminalSync.svelte.ts).
The browser's top level is a row of expandable section headlines:
Commands (the root's own methods, then the commands whose target exists —
`ls`, `cd`, `run`, … — each typed bare), one per domain the root's children
declare (Machine, Emulator, Network), then Aliases and Language.  A
section's rows sit at its own indent; the domain sections start open,
Aliases and Language closed.  Each section is a provider in
`commandsTree.ts` (key `section:*`, label, doc, whether it starts open, and
where its rows come from).  The browser lists basic and advanced members
alike (internal ones are left out as a level is read), so any path typed in
the console has a row to follow; following it opens the section the path
lives in.  Opening a path (`openPath`) walks the rows by key, so a command
row, which carries its target's path, is never taken for the member.

**Browsing previews.** Selecting a row (a click, ↑/↓, type-to-find) only
previews it: a method's or attribute's usage text shows in the details pane
under the tree, and the console is left alone.  The pane closes with its ×,
with Esc, or with a second click on the same row.  It also closes when the
console's input empties, e.g. after a command runs.  It grows to fit its
text up to 60% of the browser; a longer text scrolls inside it.

**Browser → console.** Inserting is explicit — a double-click, Enter on a
leaf, or the pane's Insert button — and replaces the path token at the
console's cursor, then hands focus to the console:

| Row | Written |
|---|---|
| object | `path.` |
| indexed (or hybrid) collection | `path[` |
| keyed collection | `path["` |
| collection entry | `path[i].` / `path["key"].` |
| method | `path ` |
| attribute | `path` |
| alias / keyword | `$name` / `keyword ` |

- Expanding or collapsing (twistie, ←/→) writes nothing; ↑/↓, Home, End,
  PageUp and PageDown move the selection.
- With the pane closed, Esc returns focus to the console, putting the input
  back as it was if the browser inserted since it took focus.
- Tab hands focus to the console with the cursor at the end.

**Console → browser.** Each change of the input asks
`shell.complete(line, cursor, true)` once typing pauses. The browser then
([`lib/pathToken.ts`](../../app/web2/src/lib/pathToken.ts)):
- opens the levels of the path token;
- marks the children matching the partial segment and dims the rest of
  that level, selecting the match (not when the change was the browser's
  own write);
- with the cursor in a method's arguments, selects that method and marks
  the current argument in its usage;
- for a `$…` token, selects the alias.

Typing in the console never moves focus. A finished console job drops the
cached levels that list collections, so their entries are re-read.

Colours come from the `--gs-syntax-*` palette, defined per skin (the
Workbench skins carry VS Code's Dark+ and Light+, in
[`skins/workbench/tokens.css`](../../app/web2/src/skins/workbench/tokens.css)
and [`skins/workbench-light/tokens.css`](../../app/web2/src/skins/workbench-light/tokens.css)),
and from the `--gs-console-*` tokens. The `hl-*`
syntax classes are one global set,
[`styles/syntax.css`](../../app/web2/src/styles/syntax.css), used by the
console's entries and input, the completion popup and the command browser's
usage blocks. They are CSS variables, so a theme switch restyles everything
already shown.

## Audio

Browsers gate WebAudio behind a user gesture. The audio worklet init
runs lazily after the first pointer/key/click/touch event; the
emulator can run silently before that without errors.

The worklet is
[`audio/gsAudio.worklet.ts`](../../app/web2/src/audio/gsAudio.worklet.ts),
bundled by Vite and handed to the core as `Module.gsAudioWorkletUrl`;
its ring logic is the class in
[`audio/audioRing.ts`](../../app/web2/src/audio/audioRing.ts), which the
unit tests drive directly. It reads int16 frames straight from
`em_audio.c`'s ring in the shared heap. Each index has one writer: the
emulator advances `write`, the worklet advances `read`, both
free-running. The producer overwrites a full ring and the consumer
resyncs when it has been lapped; a new stream is a `reset_gen` bump the
consumer carries out, and only the worklet whose id is in `owner`
consumes, so a replaced node cannot race its successor.

## Shared-heap transports

Four paths move data through the wasm heap instead of the bridge: the
camera, the microphone, audio out and the Voodoo2/printer command
rings. Each starts with a control block whose first two words are a
magic and a version, followed by the `(offset, size)` pairs of what it
carries; the C side fills the block before it announces the pointer,
and JS derives every offset from it and refuses, with a toast, a block
it does not recognise. The word indices live in
[`em_shm_layout.h`](../../src/platform/wasm/em_shm_layout.h) and are
mirrored in [`bus/shmLayout.ts`](../../app/web2/src/bus/shmLayout.ts); a
unit test compares every mirrored header with its TS twin.

## Camera (AV video input)

The AV machines' video digitizer can take its frames from the host
webcam. `em_camera.c` owns a static double-buffered frame slot pair
behind its control block in the shared heap (static storage, so the
address survives `ALLOW_MEMORY_GROWTH`) and announces its address once
via `Module.onVideoInReady`. The **main thread** decodes each camera
frame onto a 640×480 canvas, writes it into the *non-active* slot
through `Module.HEAPU8`, flips the active index and bumps `seq`; the
**worker** copies out of the active slot at field cadence through the
`gs_video_in_frame` seam. Writing only the non-active slot does not by
itself rule out a tear — a reader still copying slot A can see the
writer finish B, flip, and start on A — so the reader checks `seq`
after its copy and retries. Staleness is at most one frame and no locks
cross the thread boundary. The bridge is deliberately not involved: it
caps at ~4 KB per call, and a frame is 1.2 MB.

The camera button in the display toolbar is the master toggle (its click
is also the user gesture `getUserMedia` needs) and is shown only on
machines whose profile reports `capabilities.video_in`. The physical
device is attached only while that toggle *and* the guest's capture
engine are both on — see `Module.onVideoInState` above and
[../machines/av/vdc.md](../internals/machines/av/vdc.md).

## COOP/COEP Headers

`SharedArrayBuffer` (required for pthreads + Atomics) needs Cross-
Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers. The dev
server [`scripts/dev_server.py`](../../scripts/dev_server.py) sends both
unconditionally; serving `index.html` directly (no redirect) keeps the
headers intact through Codespaces' port-forwarding proxy.

## Styling and skins

Every visual value is a design token, a `--gs-*` CSS custom property, and
[`styles/contract.ts`](../../app/web2/src/styles/contract.ts) lists them all
(name, kind, layer, default, purpose). There are three layers:

- **Semantic tokens** (surfaces, text, borders, intents, machine states,
  the syntax and code palettes) are defined by a skin in
  `skins/<id>/tokens.css`.
- **Scale tokens** (type, space, radius, metrics, z-order, motion) are in
  [`styles/scale.css`](../../app/web2/src/styles/scale.css).
- **Component tokens** (`--gs-button-*`, `--gs-tab-*`, …) default to
  semantic ones in [`styles/components.css`](../../app/web2/src/styles/components.css).

Components are built from the primitives in `components/ui/`, which read
only component tokens. A **skin** is a folder of token values, plus an
optional icon sprite, webfonts and an override stylesheet. Each skin is one
look, light or dark (its `--gs-color-scheme` token): Midnight (the default)
and Starlight share a glass-card layout, Platinum is Mac OS 8, Aqua is
Mac OS X 10.0, and Workbench and Workbench Light are the VS Code look. The
display toolbar's Appearance button (a paintbrush) lists the skins;
`?skin=<id>` selects one for one page load. [`src/skins/README.md`](../../app/web2/src/skins/README.md)
is the authoring guide.

**Appearance.** [`state/appearance.svelte.ts`](../../app/web2/src/state/appearance.svelte.ts)
holds the chosen skin and what it resolves to. `applyAppearance()` is the
only writer of `<html data-skin>` and the `color-scheme` / `theme-color`
meta tags, which follow the skin's `--gs-color-scheme`. The one other
writer is the pre-paint script in `index.html`, which runs before any
stylesheet so a persisted choice never flashes the default. The choice
persists as `gs-skin` (absent means Midnight); `?skin=` overrides it for
one load. `lib/tokens.ts` reads token values from JavaScript (`readToken`,
`readMetric`), and `onAppearanceChange` lets code re-read them after a
switch, which is how the console's CodeMirror input re-measures.

**Cascade layers.** [`styles/layers.css`](../../app/web2/src/styles/layers.css)
fixes the order `gs.reset`, `gs.base`, `gs.tokens`, `gs.components`,
`gs.skin`, `gs.overrides`. Each global stylesheet wraps itself in its layer.
A preprocess step in `svelte.config.js` puts every component `<style>` in
`gs.components`, so a skin's overrides win without specificity tricks.

**Rules** (enforced by `tests/lint/tokens.test.ts`, `contrast.test.ts` and
`sprite.test.ts`):
- every `var(--gs-*)` names a contract token, with no fallback;
- every contract token is read somewhere (CSS `var()` or `readToken`);
- no literal colours, scale values or opacities in components;
- focus is never hidden;
- every stylesheet is in its layer;
- only `state/appearance` writes the appearance attributes;
- nothing styles the emulated screen's canvases;
- no native `prompt()`, `confirm()` or `alert()`;
- every skin's sprite defines exactly the registry's icon ids;
- text meets the declared contrast pairs.

The **UI gallery** (`?gallery` on the dev server) renders every primitive
in every state, and `make ui2-gallery` screenshots it in every skin
([`tests/e2e/README.md`](../../tests/e2e/README.md), "UI screenshots").

## Extending the Frontend

- Wire new features through the object model
  (`bus/emulator.ts::gsEval(path, args)`). The bridge contract is
  documented in [object-model.md](../internals/core/object/object-model.md).
- New panel views drop into
  [`app/web2/src/components/panel-views/`](../../app/web2/src/components/panel-views/)
  and get registered in `PANEL_TABS` ([`state/layout.svelte.ts`](../../app/web2/src/state/layout.svelte.ts))
  and `PanelContent`.
- New persistent UI state goes into a `state/<slice>.svelte.ts` file
  with `$state(...)`. Wire localStorage persistence in
  [`state/persist.svelte.ts`](../../app/web2/src/state/persist.svelte.ts).
- Media is persisted by the web app (`bus/upload.ts::persist`), not by
  the core; the core opens the path it is given.
- The core is path-agnostic — all directory-structure decisions belong
  to the web app.
- Any new URL parameter is handled in
  [`bus/urlMedia.ts::parseUrlMediaParams`](../../app/web2/src/bus/urlMedia.ts),
  and its name added to `canonicalParamName` in
  [`lib/mediaUrl.ts`](../../app/web2/src/lib/mediaUrl.ts).
- The diagnostic harness at
  [`scripts/ui2-diag.mjs`](../../scripts/ui2-diag.mjs) drives Chromium
  via Playwright, captures console / pageerror / Terminal console contents, and
  prints a JSON report. Run with `make ui2-diag`.
