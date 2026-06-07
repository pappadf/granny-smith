# Web Frontend Reference

The current Granny Smith web frontend lives in `app/web2/` (Svelte 5 +
Vite + TypeScript) — that's what `make run` serves. The legacy vanilla-
DOM frontend in `app/web-legacy/` is kept reachable via `make run-legacy`
during a soak period; it shares the same Emscripten / bridge / OPFS
contract documented below. New code goes into `app/web2/`.

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
offset 9236   output[16384] char[] C→JS: JSON-encoded response
total       25620 bytes
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
     a checkpoint exists for the URL-encoded machine.
   - `processUrlMedia()` — handles `?rom=` / `?fd0=` etc. URL params.

Module-construction call ([`bus/emulator.ts::bootstrap`](../app/web2/src/bus/emulator.ts)):

```ts
Module = await createModule({
  canvas,
  arguments: wasmArgs,
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
  — machine state, drive activity, in-flight upload progress.
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
   validation; rejects mismatched files with a toast.
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
   Filesystem view is the low-level OPFS browser.
4. **Drag-and-drop onto an Images-tab category** —
   [`ImageCategorySection.svelte`](../app/web2/src/components/panel-views/images/ImageCategorySection.svelte)
   wraps each section in a drop host. Drop calls
   `acceptFilesAsCategory(files, mediaIdFor(cat))`. Same strict
   per-category validation as path 1.

All four paths run through `startUpload` / `finishUpload`
([`state/uploads.svelte.ts`](../app/web2/src/state/uploads.svelte.ts))
so the status bar shows a "Uploading: \<name\>" pulse during long
writes. Confirmation toasts are centralised in
[`state/toasts.svelte.ts`](../app/web2/src/state/toasts.svelte.ts).

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
  `.bin` / `.sea`.
- **`machine.profile(id)`** → JSON profile with `name`, `needs_vrom`,
  `ram_options[]`, `ram_default`, `floppy_slots[]`, `scsi_slots[]`,
  `has_cdrom`, … — drives the slot-specific rows in the New Machine
  dialog (Video ROM hidden when `needs_vrom: false`, RAM dropdown built
  from `ram_options`, floppy rows = `floppy_slots.length`).
- **`machine.boot(id, ram_kb)`** — destroys any current machine and
  creates a fresh one of the named model.
- **`rom.load(path)`** — loads a ROM into the booted machine.
- **`debug.frame([addr], [count])`** — bundled JSON snapshot for the
  Debug tab: 16 GPRs + control regs, disasm rows with per-row MMU
  translation (`phys`/`valid`), optional `fpu` block. One round-trip
  instead of ~21 per pause.
- **`debug.disasm([addr], [count])`** — pretty-prints to stdout,
  returns `V_BOOL` (truthy for shell `assert ${…}` use). The web2
  Disasm pane uses `debug.frame` instead.
- **`debug.breakpoints.add(addr [, condition])`** /
  **`debug.breakpoints.add(addr, "--remove")`** — set / clear.
- **`memory.peek.{b,w,l}(addr)`** — single-byte / word / long read.
- **`memory.peek.bytes(addr, count)`** — bulk read, `V_BYTES`, capped
  at 4 KB. The Memory pane uses this so a 128-byte refresh is one
  bridge call.
- **`floppy.drives[i].insert(path, writable)` / `.eject` / `.present`**
- **`scsi.attach_hd(path, id)` / `scsi.attach_cdrom(path, id)` /
  `scsi.detach_hd(id)` / `scsi.detach_cdrom(id)`**
- **`checkpoint.save(path)` / `checkpoint.load(path)`**

## Filesystem Layout

```
/                              Memory (default WasmFS root)
├── opfs/                       Single OPFS mount — persistent
│   ├── images/
│   │   ├── rom/                ROM images, named by checksum
│   │   ├── vrom/               Video ROM images
│   │   ├── fd/                 400K/800K floppy images
│   │   ├── fdhd/               1.44 MB HD floppy images
│   │   ├── hd/                 SCSI hard-disk images
│   │   ├── cd/                 CD-ROM images (.iso / .toast / .cdr)
│   │   ├── <hash>.img          Content-addressed disk images
│   │   ├── <hash>.img.delta    Delta files
│   │   └── <hash>.img.journal  Pre-image journals
│   ├── checkpoints/
│   │   └── <machine-id>-<ts>/  Per-machine checkpoint dirs
│   │       └── state.checkpoint
│   ├── upload/                 Drag-and-drop staging
│   └── config/                 e.g. recent.json
└── tmp/                        Memory mount (volatile)
```

`/opfs` is created in C `main()` via `wasmfs_create_opfs_backend()`.
Subdirectories inside `/opfs/images` are created with `mkdir()` on the
worker thread. `/tmp/` uses a memory backend; its subdirectories are
pre-created in C because `FS.mkdir` from the JS main thread fails
cross-thread under WasmFS pthreads.

When `fd insert` or `scsi.attach_hd` receives a volatile path
(`/tmp/…`), the C-side `image_persist_volatile()` copies the file to
`/opfs/images/<hash>.img` (content-addressed, FNV-1a hash) before the
storage engine opens it. This ensures delta and journal files are also
on OPFS. The persistent path is stored in checkpoints for restore.

## URL Parameters

Handled in [`bus/urlMedia.ts`](../app/web2/src/bus/urlMedia.ts) and
invoked from `main.ts` after `whenModuleReady()` resolves:

- `rom=<url>` — downloaded into `/opfs/images/rom/`, auto-identified,
  auto-boots.
- `fdN=<url>` (`fd0`, `fd1`) — downloaded into `/opfs/images/fd/`,
  inserted into `floppy.drives[N]`.
- `hdN=<url>` — downloaded into `/opfs/images/hd/`, attached via
  `scsi.attach_hd`.
- `vrom=<url>` — downloaded into `/opfs/images/vrom/` (SE/30 / IIcx /
  IIfx).
- `speed=max|realtime|hardware` — forwarded to the wasm module as
  `--speed=`.
- `model=<id>` — preferred machine id (must be in the ROM's compatible
  list).

Each fetch supports transparent `.zip` unzipping plus auto-extraction
of Mac archives via `archive.extract`.

## Startup Flow

1. Boot WASM module (`createModule`), transfer canvas, wire callbacks.
2. WebGL2 probe — abort to full-page error if unavailable.
3. Resolve `js_bridge_t` base pointer, verify version.
4. Run `machine.register(<machine-id>, <created>)` to set the per-
   machine checkpoint dir.
5. `whenModuleReady()` resolves; `__gsReady = true`.
6. `maybeOfferBackgroundCheckpoint()` — surfaces a resume modal if
   a `state.checkpoint.tmp` exists.
7. If URL has media params → `processUrlMedia()` downloads + auto-boots.
8. Otherwise the Welcome view sits on top of the canvas. The user
   either picks a Recent machine, drops a ROM on the Display, or opens
   the New Machine dialog.
9. New Machine dialog scans `/opfs/images/rom/`, identifies each via
   `rom.identify`, builds the Model dropdown from `compatible[]` model
   ids, and fetches profiles via `machine.profile(id)` to drive RAM /
   VROM / floppy-slot UI.
10. `Start Machine` → `machine.boot`, `rom.load`, optional VROM /
    floppy / HD / CD attach, `scheduler.run`. The Welcome layer fades
    out; the canvas takes over.

## Terminal Integration (xterm.js)

[`TerminalPane.svelte`](../app/web2/src/components/panel-views/terminal/TerminalPane.svelte)
dynamically imports `@xterm/xterm` and `@xterm/addon-fit` on first
mount so they're code-split out of the main bundle. The terminal's
input state machine (`{buffer, cursor, history}`) mirrors the legacy
`app/web-legacy/js/terminal.js`. On Enter it calls
`gsEvalLine(line)`, which routes to the Shell class's `run` method;
the next prompt is returned from `shell.run` and cached for the next
`showPrompt()`. Stdout / stderr from `Module.print` lands in the same
pane via [`bus/logSink.ts`](../app/web2/src/bus/logSink.ts).

Tab completion uses the typed `shell.complete(line, cursor)` method.
Ctrl-C calls `shell.interrupt`.

xterm's theme is fed from the design tokens `--gs-terminal-bg` /
`--gs-terminal-fg` / `--gs-terminal-cursor`; an `$effect` watching
`theme.mode` pushes the resolved palette into `xterm.options.theme`
on toggle so light/dark switches re-skin live.

## Audio

Browsers gate WebAudio behind a user gesture. The audio worklet init
runs lazily after the first pointer/key/click/touch event; the
emulator can run silently before that without errors.

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
- Volatile media images are auto-persisted to `/opfs/images/` by
  `image_persist_volatile()` when mounted.
- The core is path-agnostic — all directory-structure decisions belong
  to the web app.
- Any new URL parameter is handled in
  [`bus/urlMedia.ts::parseUrlMediaParams`](../app/web2/src/bus/urlMedia.ts).
- The diagnostic harness at
  [`scripts/ui2-diag.mjs`](../scripts/ui2-diag.mjs) drives Chromium
  via Playwright, captures console / pageerror / xterm contents, and
  prints a JSON report. Run with `make ui2-diag`.
