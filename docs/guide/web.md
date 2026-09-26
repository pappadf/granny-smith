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
- **Main (browser) thread:** DOM events, xterm.js terminal, UI chrome,
  OPFS reads via the browser API, file uploads staged to `/opfs/upload/`.
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
(`storage.rm` / `storage.mv` / `storage.cp`), reserving `navigator.storage`
for reads. (This is why `BrowserOpfs.delete` / `.move` in
[`bus/opfs.ts`](../app/web2/src/bus/opfs.ts) call `gsEval` rather than
`removeEntry` directly.)

**Core / frontend separation.** The emulator core is path-agnostic: it
accepts paths as command arguments. The web app owns the directory
layout under `/opfs/`. The C side creates `/opfs/images/{rom,vrom,fd,
fdhd,hd,cd}` and `/opfs/{checkpoints,upload}` at boot via
`mkdir`-on-worker; the web app reads them via the browser's OPFS API.

**Cross-thread communication.** JS on the main thread cannot directly
call WASM functions that touch OPFS (different thread). The boundary is
a single shared-memory region — `js_bridge_t`, defined in
[`src/platform/wasm/em.h`](../src/platform/wasm/em.h) and exported via
the lone `_get_js_bridge()` accessor. JS resolves the base pointer once
at init and reads/writes fields by offset through `Module.HEAP32` /
`Module.HEAPU8`. The struct carries a `version` field that JS verifies
against `BRIDGE_VERSION` at startup so layout drift fails loudly.

Every JS→C request rides on the single `pending=1` kind (`gs_eval`).
Introspection rides on `<path>.meta.*`; free-form shell lines and tab
completion ride on the `Shell` class's `run` / `complete` methods.
The `pending` slot is sized as a 32-bit field for future kinds, but
only kind 1 is currently in use. JS writes `path` / `args`, sets
`pending`, and parks on the `done` field via `Atomics.waitAsync`. The
worker's `shell_poll()` (called every tick) drains the slot, writes
the JSON response into `output`, then issues `__atomic_store_n(&done,
1, SEQ_CST)` followed by `emscripten_atomic_notify` to wake JS — no
polling, no `setTimeout` spin. A JS-side `cmdInFlight` lock serialises
requests, so the slot is single-buffered by design.

C→JS *state pushes* go through `Module.*` callbacks installed at module
construction, not through the bridge slot:

- **`Module.onRunStateChange(running)`** — fired via
  `MAIN_THREAD_ASYNC_EM_ASM` from `em_main_tick` when the scheduler
  transitions between running and stopped, plus once at the first tick
  to seed JS.
- **`Module.onScreenResize(width, height)`** — fired via
  `MAIN_THREAD_ASYNC_EM_ASM` from `em_video.c::resize_canvas` whenever
  the framebuffer's intrinsic dimensions change. Transition-only
  (guarded against repeated identical sizes). Fires at minimum once per
  machine boot, again on every video-mode switch (e.g. the JMFB driver
  flipping a IIcx from 512×342 to 640×480). ASYNC because the worker
  doesn't block on JS layout.
- **`Module.onLogEmit(line)`** — fired via `MAIN_THREAD_ASYNC_EM_ASM`
  per emitted log line, gated by `log_would_log()` so the worker pays
  the cross-thread cost only when a category's level is above zero.
  Routed in [`app/web2/src/bus/logSink.ts`](../app/web2/src/bus/logSink.ts)
  into the reactive `logs.entries` buffer (rAF-coalesced).
- **`Module.print` / `Module.printErr`** — Emscripten's stdout/stderr
  pipes. The same `logSink` writes these to the xterm pane.
- **`Module.onVideoInReady(ptr, w, h)`** — fired once at startup from
  `em_camera.c::em_camera_init` with the address of the webcam frame
  transport in the shared heap (see below). JS keeps the pointer;
  everything after that is direct heap access, not callbacks.
- **`Module.onVideoInState(active)`** — fired when the guest gates the
  AV digitizer's VDC clock, i.e. when capture actually starts and stops.
  [`state/camera.svelte.ts`](../app/web2/src/state/camera.svelte.ts)
  attaches or stops the `MediaStreamTrack` on it, so the camera light
  is on only while the guest is capturing.

- **`Module.onVoodooGpuAttach(ctrl, bytes)` / `onVoodooGpuDetach(ctrl)`**
  — the Voodoo2 WebGPU takeover (`raster=webgpu`,
  [`src/platform/wasm/em_gpu.c`](../src/platform/wasm/em_gpu.c)): the
  emulator's raster pthread allocated a control block + op ring +
  readback area at `ctrl` in the shared heap and wants the page's GPU
  worker attached to it.  [`gpu/voodoo2Gpu.svelte.ts`](../app/web2/src/gpu/voodoo2Gpu.svelte.ts)
  posts the wasm memory and the address to the worker
  ([`gpu/voodoo2Gpu.worker.ts`](../app/web2/src/gpu/voodoo2Gpu.worker.ts)),
  which then talks to the emulator through shared memory only —
  `Atomics.waitAsync` on the ring's head, `Atomics.notify` on its tail
  and the acknowledge word — while the C side waits with futexes.  The
  worker is started once at page load with the `#screen3d` overlay
  canvas (transferred), and writes whether a WebGPU device exists into
  the bridge's `gpu_available` word so the core's backend choice is
  honest at machine creation.  The page shows the overlay exactly
  while GPU mode is engaged (the worker relays the MODE records it
  consumes).  The protocol is
  [`voodoo2_gpu_protocol.h`](../src/core/peripherals/pci/cards/voodoo2_gpu_protocol.h)
  / `voodoo2Protocol.ts`.

- **`Module.onPrinterAttach(ctrl, version)`** — the emulated LaserWriter's
  interpreter (`src/platform/wasm/em_main.c`,
  `laserwriter_ring_attach_requested`): the printer bridge allocated a
  control block + two byte rings at `ctrl` in the shared heap on the
  first print job and wants the page's platen worker attached.
  [`printer/platen.ts`](../app/web2/src/printer/platen.ts) starts
  [`printer/platen.worker.ts`](../app/web2/src/printer/platen.worker.ts)
  then (lazily: the worker fetches its own non-threaded module,
  `platen-<version>.js` beside `main.mjs`, built by `make platen-module`),
  and posts the wasm memory and the address; the worker parks in
  `Atomics.waitAsync` on the outbound ring's head while the C side wakes
  it with `emscripten_futex_wake`.  Each finished PDF comes back to the
  page as a transferable and is downloaded at once as
  `<job>-<title>.pdf`.  The protocol is
  [`laserwriter_ring_protocol.h`](../src/core/network/laserwriter_ring_protocol.h)
  / `printer/platenProtocol.ts`; the whole path is
  [`docs/core/network/laserwriter.md`](../core/network/laserwriter.md) §5.5.

These callbacks are the template for any future C→JS event: install on
`Module.*`, fire from C with `MAIN_THREAD_*_EM_ASM`. No exports, no
SAB plumbing, no JS-side timers. (A previous `onPromptChange` callback
retired when the new prompt started coming back as `shell.run`'s return
value under proposal-shell-as-object-model-citizen.)

## The Bridge Struct

Layout (mirrored as `OFF_*` constants in
[`app/web2/src/bus/emulator.ts`](../app/web2/src/bus/emulator.ts)):

```
offset    0   version    int32     must equal JS_BRIDGE_VERSION
offset    4   ready      int32     1 once worker can dispatch requests
offset    8   pending    int32     request kind (1 = gs_eval); 0 = idle
offset   12   done       int32     flipped to 1 by worker on completion
offset   16   result     int32     integer result code
offset   20   path[1024] char[]    JS→C: request path
offset 1044   args[8192] char[]    JS→C: JSON-encoded arg array
offset 9236   output[262144] char[] C→JS: JSON-encoded response
offset 271380 gpu_available int32   JS→C: 1 once the page has a WebGPU device (Voodoo2 takeover)
total       271384 bytes
```

Only `pending=1` is in use. JS writes `path` / `args`, sets `pending`,
parks on `done`; the worker writes `result` + `output`, then flips
`done`. `cmdInFlight` on the JS side serialises requests so the
single-buffered slot is safe.

### Request Wakeup (Atomics)

```c
// shell_poll(), after writing result + output:
__atomic_store_n(&g_bridge.done, 1, __ATOMIC_SEQ_CST);
emscripten_atomic_notify((void *)&g_bridge.done, 1);
```

```ts
// bus/emulator.ts: waitForBridgeDone
const w = Atomics.waitAsync(Module.HEAP32, doneIdx, 0);
if (w.async) await w.value;          // resolves on the notify
Atomics.store(Module.HEAP32, doneIdx, 0);
```

`Atomics.waitAsync` returns synchronously with `not-equal` if the
worker beat JS to it; otherwise it yields a Promise that resolves on
the notify. Minimum round-trip is one event-loop turn after the
worker's tick — no `setTimeout` spin, no main-thread CPU burn. The same
pattern gates the initial `ready` flip.

## Module Bootstrapping

Entry point: [`app/web2/src/main.ts`](../app/web2/src/main.ts).

1. Synchronous pre-mount work:
   - Load persisted state from `localStorage` (theme, panel pos+size,
     debug pane state, …).
   - Apply theme to `<html data-theme>` to avoid a flash.
   - Auto-pick panel orientation from viewport size if no persisted
     value.
2. **WebGL2 probe.** [`lib/webglCheck.ts`](../app/web2/src/lib/webglCheck.ts)
   creates an off-DOM canvas and asks for a `webgl2` context. If
   missing (e.g. Chrome GPU process dead, hardware acceleration
   disabled), the app renders a full-page error overlay via
   [`lib/webglErrorPage.ts`](../app/web2/src/lib/webglErrorPage.ts) and
   does **not** mount Svelte. The error page is vanilla DOM so it
   survives a degraded framework runtime.
3. Mount the Svelte tree.
4. Post-mount async (after `App.svelte`'s effects have run):
   - `await whenModuleReady()` (resolved by `bus/emulator.ts::bootstrap`
     once the bridge's `ready` flag flips). Exposes `window.__gsReady =
     true` for headless automation
     ([`scripts/ui2-diag.mjs`](../scripts/ui2-diag.mjs)).
   - `maybeOfferBackgroundCheckpoint()` — surfaces a resume prompt if
     this browser's machine has a saved checkpoint.
   - `processUrlMedia()` — handles the URL's media parameters (any of
     them starts it).

Module-construction call ([`bus/emulator.ts::bootstrap`](../app/web2/src/bus/emulator.ts)):

```ts
Module = await createModule({
  canvas,
  locateFile: (p) => (p.endsWith('.wasm') ? `/main.wasm?v=${bust}` : p),
  print:       routePrintLine,
  printErr:    routePrintLine,
  onRunStateChange: handleRunStateChange,
  onScreenResize:   handleScreenResize,
  onLogEmit:        routeLogEmit,
});
```

The canvas reference is passed once; Emscripten transfers it to the
worker via `transferControlToOffscreen` and resolves the `#screen` DOM
id from `OFFSCREENCANVASES_TO_PTHREAD`. After `createModule` returns,
JS calls `Module._get_js_bridge()` to resolve the bridge base pointer,
verifies the version, then `await gsEval('machine.register', …)` to
activate the per-machine checkpoint directory.

## Major UI Surfaces

The Svelte app is organised under
[`app/web2/src/components/`](../app/web2/src/components/):

- **Display** ([`display/`](../app/web2/src/components/display/)) —
  ScreenView (the canvas), DisplayToolbar (zoom, pause/run, save,
  theme), DropOverlay (drag state machine §8.5), WelcomeView with
  Home / Configuration slides for new-machine setup.
- **Workbench** ([`workbench/`](../app/web2/src/components/workbench/))
  — flex container with the Display + a resizable Panel docked
  bottom / left / right.
- **Panel views** ([`panel-views/`](../app/web2/src/components/panel-views/)):
  Terminal, Logs, Machine tree, Filesystem tree, Images, Checkpoints,
  Debug (Disassembly + Registers + FPU + Memory + MMU + Breakpoints +
  Watchpoints + Call Stack).
- **Status bar** ([`status-bar/`](../app/web2/src/components/status-bar/))
  — machine state, drive activity, in-flight upload progress. The HD /
  FD / CD lights are real: the core counts every drive read and write on
  the image (`storage.images[i].reads` / `.writes`), the worker tick sums
  them per kind and pushes `Module.onDriveActivity(kind, state)` only when
  a light changes, holding each on at least 100 ms
  ([`drive_activity.c`](../src/core/storage/drive_activity.c)). A model
  shows only the lights its profile has drives for.
- **Common** ([`common/`](../app/web2/src/components/common/)) —
  CollapsibleSection, Tree, TabStrip, Modal, Toast, ContextMenu, Icon
  (codicon sprite at [`public/icons/sprite.svg`](../app/web2/public/icons/sprite.svg)).

State lives under [`app/web2/src/state/`](../app/web2/src/state/) —
each `*.svelte.ts` file owns a `$state` slice (`machine`, `layout`,
`debug`, `theme`, `logs`, `images`, `uploads`, `toasts`, …). The bus
layer at [`app/web2/src/bus/`](../app/web2/src/bus/) wraps every
`gsEval` call site.

## Upload Pipeline

Four deliberate ways to get a media image into OPFS, all routing
through [`app/web2/src/bus/upload.ts`](../app/web2/src/bus/upload.ts):

1. **New Machine dialog dropdowns** — picking "Upload image…" in a
   floppy / HD / CD / ROM / VROM slot calls
   `pickAndUploadAs(mediaId)` →
   `acceptFilesAsCategory(files, mediaId)`. Strict per-category
   validation; rejects mismatched files with a toast. The floppy / HD
   slots also offer "Create blank image…", which opens
   [`CreateImageDialog.svelte`](../app/web2/src/components/display/CreateImageDialog.svelte)
   and creates a blank image directly in OPFS via `storage.fd_create`
   (800 KB / 1.4 MB) or `storage.hd_create` (size from
   `scsi.hd_models`).
2. **Drag-and-drop onto the Display** —
   [`DropOverlay.svelte`](../app/web2/src/components/display/DropOverlay.svelte)
   captures drops, calls `processDataTransfer` →
   `acceptFiles(files)`. Auto-detects type by probing each
   `MediaTypeDescriptor` in order; archives (`.zip`, `.sit`, `.hqx`,
   `.cpt`, `.bin`, `.sea`) are extracted via `archive.extract` and the
   inner image re-probed. Floppy / CD images auto-mount into an empty
   drive (`floppy.drives[i].present` is checked iteratively, SCSI ID 3
   for CD); ROMs trigger a full cold boot via `maybeBootFromRom`.
3. **Drag-and-drop onto the Filesystem tab** —
   [`FilesystemView.svelte`](../app/web2/src/components/panel-views/filesystem/FilesystemView.svelte)
   accepts external file drops on folder rows, calls
   `acceptFilesRaw(files, targetDir)`. **No validation** — the
   Filesystem view is the low-level OPFS browser. The same tab also does
   *internal* drags — move within OPFS, and **copy a file/folder out of a
   disk image** to an OPFS folder — through the operations in
   [`bus/fsOps.ts`](../app/web2/src/bus/fsOps.ts).
4. **Drag-and-drop onto an Images-tab category** —
   [`ImageCategorySection.svelte`](../app/web2/src/components/panel-views/images/ImageCategorySection.svelte)
   wraps each section in a drop host. Drop calls
   `acceptFilesAsCategory(files, mediaIdFor(cat))`. Same strict
   per-category validation as path 1.

All four paths run through `startActivity` / `endActivity`
([`state/activity.svelte.ts`](../app/web2/src/state/activity.svelte.ts)) so
the status bar shows a spinner with a "\<verb>: \<name>" label during long
operations. The verb is general — uploads show "Uploading", and the
Filesystem-tab worker ops reuse the same indicator ("Copying", "Moving",
"Deleting", "Unpacking", "Downloading"). Confirmation toasts are centralised
in [`state/toasts.svelte.ts`](../app/web2/src/state/toasts.svelte.ts).

## C-side surfaces the UI consumes

Highlights — see the typed-dispatch / introspection proposals for the
full surface.

- **`rom.identify(path)`** → JSON `{recognised, checksum, name,
  compatible[], size}`. Drives the Model dropdown in the New Machine
  dialog.
- **`vrom.identify(path)`** → bool (32-KB check).
- **`floppy.identify(path)`** → density string (`400K` / `800K` /
  `1.4MB`); empty if not a floppy.
- **`scsi.identify_hd(path)` / `scsi.identify_cdrom(path)`** → bool.
- **`archive.identify(path)`** → JSON for `.sit` / `.hqx` / `.cpt` /
  `.bin` / `.sea`. **`archive.extract(path, out_dir)`** → bool; powers the
  Filesystem-tab "Unpack" action.
- **`vfs.list(path)`** → JSON `[{name, kind, size}]`, descending into a disk
  image (partitions, then HFS/UFS contents). The Filesystem tree calls this to
  browse inside images; see [`target-filesystems.md`](target-filesystems.md).
- **`storage.cp([-r], src, dst)`** — copy, including *out of* an image into
  OPFS (backs copy-out and Download). **`storage.rm(path)`** /
  **`storage.mv(src, dst)`** — recursive remove / move, run worker-side so
  WasmFS stays coherent (see Persistence above).
- **`storage.hd_create(path, size)`** / **`storage.fd_create(path,
  high_density)`** — create a blank HD / floppy image; **`scsi.hd_models`** →
  drive-size catalog. These drive the New Machine dialog's "Create blank
  image…" option.
- **`machine.profile(id)`** → JSON profile with `name`, `needs_vrom`,
  `ram_options[]`, `ram_default`, `floppy_slots[]`, `scsi_slots[]`
  (`{label, id, boot}` — `boot` marks the bay the firmware boots from
  when it is not the first listed), `has_cdrom`, … — drives the
  slot-specific rows in the New Machine dialog (Video ROM hidden when
  `needs_vrom: false`, RAM dropdown built from `ram_options`, floppy rows
  = `floppy_slots.length`, a Bay selector when `scsi_slots` has more
  than one entry, preselecting the `boot` bay; the hard disk attaches at
  that id, never at an assumed 0).
- **`machine.videoin.source`** (`none`/`pattern`/`file`/`host`) plus the
  read-only `connected` / `fields` — the AV video digitizer's host source.
  The camera toolbar button sets `host`; the button itself is gated on
  `capabilities.video_in` from `machine.profile`. See
  [../machines/av/vdc.md](../machines/av/vdc.md).
- **`machine.boot(model=..., rom=..., ...)`** — destroys any current
  machine and creates a fresh one from a complete configuration document
  (model and rom required; nothing is inherited from the previous
  machine).
- **`machine.restart`** — power-cycles the running machine: rebuilds it
  from its built-from record with the mounted media still attached.
- **`rom.load(path)`** — loads a ROM into the booted machine.
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
- **`memory.peek.{b,w,l}(addr)`** — single-byte / word / long read.
- **`memory.peek.bytes(addr, count)`** — bulk read, `V_BYTES`, capped
  at 4 KB. The Memory pane uses this so a 128-byte refresh is one
  bridge call.
- **`floppy.drives[i].insert(path, writable)` / `.eject` / `.present`**
- **`machine.attach_hd(path, [bay])` / `machine.attach_cdrom(path)` /
  `machine.eject_media(bus, [id])`** — media by bay, on whatever bus the
  bay is (`machine.scsi`, `machine.scsi2`, the Lisa's ProFile).  `bay`
  indexes `machine.profile(id).hd_bays` (0, the default, is the boot bay);
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
│   │   ├── rom/                ROM images, named by checksum
│   │   ├── vrom/               Video ROM images
│   │   ├── fd/                 Floppy images (400K / 800K / 1.44 MB)
│   │   ├── hd/                 SCSI hard-disk images
│   │   └── cd/                 CD-ROM images (.iso / .toast / .cdr)
│   ├── checkpoints/
│   │   └── <machine-id>-<ts>/  Per-machine checkpoint dirs
│   │       ├── state.checkpoint
│   │       └── <id>.delta / <id>.journal   Writable image state
│   └── upload/                 Drag-and-drop staging
└── tmp/                        Memory mount (volatile)
```

`/opfs` is created in C `main()` via `wasmfs_create_opfs_backend()`.
Subdirectories inside `/opfs/images` are created with `mkdir()` on the
worker thread. `/tmp/` uses a memory backend; its subdirectories are
pre-created in C because `FS.mkdir` from the JS main thread fails
cross-thread under WasmFS pthreads.

The core opens media at whatever path it is given and never copies it
elsewhere. Persistence is the web app's job: an upload, and a URL-parameter
download, is copied into `/opfs/images/<category>/` before it is attached
(`bus/upload.ts::persist`), so the base image lives on OPFS and the path
recorded in checkpoints still resolves after a reload. A volatile path
(`/tmp/…`) attached from the shell stays volatile: the image, its delta and
any checkpoint's reference to it do not survive a reload. Copy it under
`/opfs/` first (`storage.import <src> <dst>`) to keep it.

## URL Parameters

Handled in [`bus/urlMedia.ts`](../app/web2/src/bus/urlMedia.ts) and
invoked from `main.ts` after `whenModuleReady()` resolves:

- `rom=<url>` — downloaded into `/opfs/images/rom/`, auto-identified,
  auto-boots.
- `fdN=<url>` (`fd0`, `fd1`) — downloaded into `/opfs/images/fd/`,
  inserted into floppy drive N, when the model has that drive.
- `hdN=<url>` — downloaded into `/opfs/images/hd/`, attached to the
  model's N-th hard-disk bay (`machine.attach_hd(path, N)`; `hd0` is the
  boot bay, on whatever bus it is — SCSI, a Network Server's second
  channel, the Lisa's ProFile).
- `cd=<url>` — downloaded into `/opfs/images/cd/`, inserted into the
  model's CD bay (`machine.attach_cdrom`), on a model that has one.
- `vrom=<url>` — downloaded into `/opfs/images/vrom/` (SE/30 / IIcx /
  IIfx).
- `speed=paced|accelerated|turbo` — the toolbar's pacing mode from the
  start: a boot pushes it to the core (`scheduler.mode`), and a resumed
  machine is switched to it (legacy `max`/`realtime`/`hardware` are
  accepted as aliases).  The wasm module takes no command line.
- `model=<id>` — preferred machine id (must be in the ROM's compatible
  list).

Downloads run one at a time and stream to `/opfs/upload/` through the
same chunked writer as uploads (`bus/upload.ts::streamToOpfs`), so an
image is never held whole in memory — except a `.zip`, which is read
back whole to unzip.  Mac archives are auto-extracted via
`archive.extract`.

## Startup Flow

The same sequence as Module Bootstrapping above, end to end:

1. WebGL2 probe — abort to a full-page error if unavailable (Svelte is
   never mounted).
2. Mount Svelte; `ScreenView` calls `bootstrap(canvas)`: load the
   module (`createModule`, no command line), wire the callbacks, resolve
   the `js_bridge_t` base pointer and verify its version.
3. Run `machine.register(<machine-id>, <created>)` to set the per-
   machine checkpoint dir.
4. `whenModuleReady()` resolves; `__gsReady = true`.
5. `maybeOfferBackgroundCheckpoint()` — surfaces a resume prompt if a
   `state.checkpoint` exists.
6. If the URL has any media parameter → `processUrlMedia()` downloads
   and auto-boots.
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

## Terminal Integration (xterm.js)

[`TerminalPane.svelte`](../app/web2/src/components/panel-views/terminal/TerminalPane.svelte)
dynamically imports `@xterm/xterm` and `@xterm/addon-fit` on first
mount so they're code-split out of the main bundle, and it stays mounted
(hidden) once opened, so scrollback survives a tab switch. The terminal's
input state machine (`{buffer, cursor, history}`) lives in the component.
All input arrives through xterm's `onData` — keys, pastes, IME text —
and [`lineDiscipline.ts`](../app/web2/src/components/panel-views/terminal/lineDiscipline.ts)
turns it into editing actions, which apply one at a time while the input
line is live: whatever is typed while a command runs waits for the next
prompt, and a multi-line paste runs line by line. On Enter the pane calls
`gsEvalLine(line)`, which routes to the Shell class's `run` method;
the next prompt is returned from `shell.run` and cached for the next
`showPrompt()`. Stdout / stderr from `Module.print` lands in the same
pane via [`bus/logSink.ts`](../app/web2/src/bus/logSink.ts), which holds
what is printed before the terminal first opens and replays it then.

Tab completion uses the typed `shell.complete(line, cursor)` method.
Ctrl-C calls `shell.interrupt` and drops the type-ahead; Cmd-C on macOS
is the browser's copy.

xterm's theme is fed from the design tokens `--gs-terminal-bg` /
`--gs-terminal-fg` / `--gs-terminal-cursor`; an `$effect` watching
`theme.mode` pushes the resolved palette into `xterm.options.theme`
on toggle so light/dark switches re-skin live.

## Audio

Browsers gate WebAudio behind a user gesture. The audio worklet init
runs lazily after the first pointer/key/click/touch event; the
emulator can run silently before that without errors.

The worklet is
[`audio/gsAudio.worklet.ts`](../app/web2/src/audio/gsAudio.worklet.ts),
bundled by Vite and handed to the core as `Module.gsAudioWorkletUrl`;
its ring logic is the class in
[`audio/audioRing.ts`](../app/web2/src/audio/audioRing.ts), which the
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
[`em_shm_layout.h`](../src/platform/wasm/em_shm_layout.h) and are
mirrored in [`bus/shmLayout.ts`](../app/web2/src/bus/shmLayout.ts); a
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
[../machines/av/vdc.md](../machines/av/vdc.md).

## COOP/COEP Headers

`SharedArrayBuffer` (required for pthreads + Atomics) needs Cross-
Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers. The dev
server [`scripts/dev_server.py`](../scripts/dev_server.py) sends both
unconditionally; serving `index.html` directly (no redirect) keeps the
headers intact through Codespaces' port-forwarding proxy.

## Extending the Frontend

- Wire new features through the object model
  (`bus/emulator.ts::gsEval(path, args)`). The bridge contract is
  documented in [object-model.md](object-model.md).
- New panel views drop into
  [`app/web2/src/components/panel-views/`](../app/web2/src/components/panel-views/)
  and get registered in `PanelTab` / `PanelContent`.
- New persistent UI state goes into a `state/<slice>.svelte.ts` file
  with `$state(...)`. Wire localStorage persistence in
  [`state/persist.svelte.ts`](../app/web2/src/state/persist.svelte.ts).
- Media is persisted by the web app (`bus/upload.ts::persist`), not by
  the core; the core opens the path it is given.
- The core is path-agnostic — all directory-structure decisions belong
  to the web app.
- Any new URL parameter is handled in
  [`bus/urlMedia.ts::parseUrlMediaParams`](../app/web2/src/bus/urlMedia.ts).
- The diagnostic harness at
  [`scripts/ui2-diag.mjs`](../scripts/ui2-diag.mjs) drives Chromium
  via Playwright, captures console / pageerror / xterm contents, and
  prints a JSON report. Run with `make ui2-diag`.
