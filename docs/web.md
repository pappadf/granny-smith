# Web Frontend Reference

The Granny Smith web frontend lives in `app/web/index.html`. The page and its module scripts act as the single entry point for initializing the emulator, wiring terminal I/O, and handling user interaction. This reference documents both the user-facing capabilities and the architectural details you need when modifying the UI or embedding it elsewhere.

## Architecture: Pthreads + WasmFS + OPFS

The emulator uses Emscripten's `PROXY_TO_PTHREAD` mode. The C `main()` runs on a dedicated worker thread; the browser main thread handles DOM, input, and compositing.

**Threading model:**
- **Main (browser) thread:** DOM events, xterm.js terminal, UI chrome, file uploads to `/tmp/`
- **Emulator worker thread:** CPU emulation, OPFS file I/O (delta/journal/checkpoint), shell command execution

**Persistence:** A single OPFS mount at `/opfs` provides all persistent storage: `/opfs/images` (ROMs, disk images, content-addressed store), `/opfs/checkpoints`, and `/opfs/upload`. Users can also create arbitrary persistent paths under `/opfs/`. The web app creates subdirectories inside `/opfs/images` (`rom`, `vrom`, `fd`, `fdhd`, `hd`, `cd`) via `mkdir` on the worker thread. `/tmp` uses the memory backend (volatile). The WasmFS root itself is memory-backed (the `wasmfs_create_root_dir` hook cannot use OPFS because it runs on the main browser thread where `wasmfs_create_opfs_backend()` deadlocks). The web app only uses paths under `/opfs/`, so in practice everything except `/tmp` is persistent. All OPFS writes are immediately durable — no explicit sync step needed.

**Core/frontend separation:** The emulator core makes no assumptions about filesystem structure. It accepts paths as command arguments. The web app defines and owns the directory layout, creating directories via shell commands (`mkdir`) and persisting files via `file-copy`.

**Cross-thread communication:** JS on the main thread cannot call WASM functions that access OPFS (different thread). The boundary is a single shared-memory region — `js_bridge_t`, defined in [`src/platform/wasm/em.h`](../src/platform/wasm/em.h) and exported via the lone `_get_js_bridge()` accessor. JS resolves the base pointer once at init and reads/writes fields by offset through `Module.HEAP32` / `Module.HEAPU8`. The struct carries a `version` field that JS verifies against `BRIDGE_VERSION` at startup so layout drift fails loudly.

Every JS→C request rides on a single `pending=1` kind (`gs_eval`). Introspection lives on `<path>.meta.*` ([`proposal-introspection-via-meta-attribute.md`](../local/gs-docs/proposals/proposal-introspection-via-meta-attribute.md)); free-form shell lines and tab completion ride on the `Shell` class's `run` and `complete` methods ([`proposal-shell-as-object-model-citizen.md`](../local/gs-docs/proposals/proposal-shell-as-object-model-citizen.md)). The `pending` field stays as a 32-bit slot for future call kinds, but only kind 1 is currently in use. JS writes `path` / `args`, sets `pending`, and parks on the `done` field via `Atomics.waitAsync`. The worker's `shell_poll()` (called every tick) drains the slot, writes the JSON response into `output`, then issues `__atomic_store_n(&done, 1, SEQ_CST)` followed by `emscripten_atomic_notify` to wake JS — no polling, no `setTimeout` spin. A JS-side `cmdInFlight` lock serialises requests, so the slot is single-buffered by design.

C→JS *state pushes* go through `Module.*` callbacks installed at module construction, not through the bridge slot:

- **`Module.onRunStateChange(running)`** — fired via `MAIN_THREAD_ASYNC_EM_ASM` when the scheduler transitions between running and stopped (and once at the first tick to seed the initial state).
- **`Module.onScreenResize(width, height)`** — fired via `MAIN_THREAD_ASYNC_EM_ASM` from `em_video.c::resize_canvas` whenever the framebuffer's intrinsic dimensions change. Transition-only (guarded against repeated identical sizes). Fires at minimum once per machine boot, again on every video-mode switch (e.g. the JMFB driver flipping a IIcx from 512×342 to 640×480). ASYNC because the worker doesn't block on JS layout. The JS side must update CSS-driven container dimensions on this event; `emscripten_set_canvas_element_size` updates the canvas's *intrinsic* resolution, not the CSS-displayed size.

The two callbacks are the template for any future C→JS event: install on `Module.*`, fire from C with `MAIN_THREAD_*_EM_ASM`. No exports, no SAB plumbing, no JS-side timers. Prompt updates used to ride on a third callback (`onPromptChange`); under [`proposal-shell-as-object-model-citizen.md`](../local/gs-docs/proposals/proposal-shell-as-object-model-citizen.md) the new prompt comes back as `shell.run`'s return value, so the side-channel callback retired.

## Capabilities and Requirements

### Layout and Major Elements
- **Canvas (`#screen`)**: Displays the 512x342 Macintosh Plus framebuffer. The containing `.screen-wrapper` resizes when zoom changes (default 200%, clamped 100-300%). Rendered via OffscreenCanvas on the worker thread.
- **ROM upload dialog**: Modal dialog shown on first visit when no ROM exists in `/opfs/images/rom/`. Provides a file picker button and drag-drop hint.
- **Machine configuration dialog**: After ROM upload (or on fresh start with persisted ROMs), shows model selection, RAM, floppy/HD/CD media dropdowns with upload options.
- **Drop hint (`#drop-hint`)**: Appears when files are dragged into the viewport.
- **Terminal panel (`#terminal-panel`)**: Collapsible xterm.js console wired to the emulator shell. Toggled via the header row or the backtick (`) key.

### Toolbar Controls
- **Run button (`#btn-run`)**: Play/pause toggle. When idle it calls `scheduler.run` via the object-model bridge. When running it calls `scheduler.stop`.
- **Zoom out / level / zoom in**: Adjust the canvas scale in 10% increments, persisted to `localStorage`.
- **Download State**: Saves a temporary checkpoint via `checkpoint.save` and triggers a browser download via `download`.
- **Share**: Copies `location.href` to the clipboard.

### Drag-and-Drop
- **ROM files**: Identified via `rom.identify`, stored in `/opfs/images/rom/<checksum>`. Emulator auto-runs.
- **Floppy images**: Staged to `/tmp/upload/`, probed via `floppy.identify`, then inserted via `floppy.drives[0].insert`. The C-side `image_persist_volatile()` automatically copies the file to `/opfs/images/<hash>.img` (OPFS) before opening.
- **Hard-disk images**: Same persistence flow via `scsi.attach_hd`.
- **Checkpoint files**: Detected via `GSCHKPT` magic. Copied to `/tmp/` and loaded via `checkpoint.load`.
- **ZIP archives**: Extracted client-side via JSZip, contents re-probed for media.
- **Mac archives** (`.sit`, `.hqx`, `.cpt`, `.bin`, `.sea`): Identified via `archive.identify`, extracted via `archive.extract`, contents re-probed for media.

### URL Parameters
- `rom=<url>`: Downloaded and stored in `/opfs/images/rom/`.
- `fdN=<url>` (e.g., `fd0`, `fd1`): Downloaded into `/opfs/images/fd/` and inserted via `fd insert`.
- `hdN=<url>`: Downloaded into `/opfs/images/hd/`. Auto-attached before the first run.
- `vrom=<url>`: Downloaded into `/opfs/images/vrom/` (SE/30 video ROM).
- `speed=max|realtime|hardware`: Passed to the wasm module as `--speed=`.
- `model=<name>`: Override machine type for testing.

### Audio Requirements
- Browsers that block WebAudio until a gesture keep the audio context suspended. The frontend listens to pointer, key, click, and touch events to call `ctx.resume()` once.

### Persistence
- A single OPFS mount at `/opfs` provides all persistent storage. `/opfs/images` (and all subdirectories), `/opfs/checkpoints`, and `/opfs/upload` survive page reloads. Users can create arbitrary persistent paths under `/opfs/`.
- `/tmp` is memory-backed and cleared on reload.
- The WasmFS root is memory-backed (OPFS root hook deadlocks with `PROXY_TO_PTHREAD`), but the web app only uses paths under `/opfs/`.
- All OPFS writes are immediately durable — no sync calls needed.

## Implementation Details

### Module Bootstrapping (`emulator.js`)
- `main.mjs` (generated by Emscripten) is dynamically imported with a cache-busting query parameter. `createModule` is invoked with `canvas`, `arguments`, `locateFile`, `print`/`printErr`, and the two state-push callbacks (`onRunStateChange`, `onScreenResize`).
- After module creation JS calls `Module._get_js_bridge()` once to resolve the base pointer of the `js_bridge_t` struct in shared memory, then verifies the `version` field matches the JS-side `BRIDGE_VERSION`. Field offsets are mirrored as `OFF_*` constants in `emulator.js` and must stay in sync with `src/platform/wasm/em.h`.
- With `PROXY_TO_PTHREAD`, `shell_init()` is called from `main()` on the worker — no `ccall` from JS. JS gates the first `gsEval` on the bridge's `ready` field via `Atomics.waitAsync`; the worker writes that field and calls `emscripten_atomic_notify` once `shell_init()` returns.

### The Bridge Struct
Layout (mirrored in [`emulator.js`](../app/web/js/emulator.js)):

```
offset 0    version       int32     must equal JS_BRIDGE_VERSION
offset 4    ready         int32     1 once worker can dispatch requests
offset 8    pending       int32     request kind (1..4); 0 = idle
offset 12   done          int32     flipped to 1 by worker on completion
offset 16   result        int32     integer result code
offset 20   path[1024]    char[]    JS→C: request path
offset 1044 args[8192]    char[]    JS→C: JSON-encoded arg array
offset 9236 output[16384] char[]    C→JS: JSON-encoded response
total:      25 620 bytes
```

Exactly one request kind is in use, serialised by `cmdInFlight` in JS. Schema introspection rides via `<path>.meta.*`; free-form shell lines and tab completion ride via the `Shell` class's `run` and `complete` methods.

| `pending` | Operation | `path` / `args` carry | `output` carries |
|---|---|---|---|
| `1` | `gs_eval(path, args)` | path + JSON arg array | JSON-encoded `value_t` |

### Request Wakeup (Atomics)
The worker's completion path uses real shared-memory primitives, not polling:

```c
// shell_poll(), after writing result + output:
__atomic_store_n(&g_bridge.done, 1, __ATOMIC_SEQ_CST);
emscripten_atomic_notify((void *)&g_bridge.done, 1);
```

```js
// emulator.js: waitForBridgeDone
const w = Atomics.waitAsync(Module.HEAP32, doneIdx, 0);
if (w.async) await w.value;          // resolves on the notify
Atomics.store(Module.HEAP32, doneIdx, 0);
```

`Atomics.waitAsync` returns synchronously with `not-equal` if the worker beat JS to it; otherwise it yields a Promise that resolves on the notify. Minimum round-trip is one event-loop turn after the worker's tick — no `setTimeout` spin, no main-thread CPU burn. The same pattern gates the initial `ready` flip.

### State Pushes (C→JS Callbacks)
Three pieces of state are pushed via `Module.*` callbacks, set on the config object passed to `createModule`:

- **`Module.onRunStateChange(running)`** — fired via `MAIN_THREAD_ASYNC_EM_ASM` from `em_main_tick` whenever the scheduler running flag changes (and once on the first tick to seed JS). ASYNC because there's no JS code waiting on the value; the worker doesn't block.
- **`Module.onPromptChange(text)`** — fired via `MAIN_THREAD_EM_ASM` (sync) at the end of free-form-line dispatch, before `done` is flipped. Sync because the JS terminal reads the cached prompt right after `gsEvalLine` resolves; ASYNC would race that read.
- **`Module.onScreenResize(width, height)`** — fired via `MAIN_THREAD_ASYNC_EM_ASM` from `em_video.c::resize_canvas` whenever the framebuffer's intrinsic dimensions change (transition-only — repeated identical sizes are suppressed by a `last_w`/`last_h` guard). Fires at minimum once per machine boot, again whenever the active video mode changes — e.g. when the JMFB driver flips a IIcx from the SE/30-default 512×342 to 640×480. ASYNC because the worker doesn't block on JS layout.

JS caches the values in module-locals (`isRunningUI`, `cachedPrompt`, `lastScreenW`/`lastScreenH`) and exposes them through `isRunning()` / `getRuntimePrompt()` / `getLastScreenSize()` for synchronous access from UI code. The screen-resize subscriber API in `app/web/js/emulator.js` (`onScreenResize(cb)`) replays the latest dimensions to new subscribers immediately, so a component mounting after the machine has already booted gets the current size without waiting for the next change.

**Important for the JS side.** Setting the canvas's intrinsic resolution via `emscripten_set_canvas_element_size` does **not** reflow CSS-driven container dimensions. Any wrapper that sizes itself in CSS (zoom wrapper, layout container) must subscribe to `onScreenResize` and re-apply its sizing logic — otherwise a resolution switch leaves the framebuffer stretched or letterboxed at the previous aspect ratio.

### Terminal Integration (xterm.js)
- xterm.js plus the FitAddon power the interactive console. Input handling is fully client-side: the script maintains `{buffer, cursor, history}`. On Enter, the buffer is sent to the bridge as a kind=4 free-form line (`gsEvalLine`); stdout/stderr from the dispatch arrive through `Module.print` and are written into xterm directly. The next prompt arrives via `onPromptChange` before the await resolves.

### Filesystem Layout

```
/                              Memory (default WasmFS root)
├── opfs/                       Single OPFS mount (everything here persists)
│   ├── images/
│   │   ├── rom/               ROM images (named by checksum)
│   │   ├── vrom/              Video ROM images
│   │   ├── fd/                400K/800K floppy images
│   │   ├── fdhd/              1.4MB HD floppy images
│   │   ├── hd/                SCSI hard disk images
│   │   ├── cd/                CD-ROM images (toast/iso)
│   │   ├── <hash>.img         Content-addressed disk images
│   │   ├── <hash>.img.delta   Delta files
│   │   └── <hash>.img.journal Preimage journals
│   ├── checkpoints/
│   │   └── 0000042.checkpoint Quick checkpoint data
│   ├── upload/
│   └── (user can create anything here)
└── tmp/                        Memory mount (volatile)
    ├── upload/                 Drag-and-drop staging
    └── extract/                Archive extraction
```

A single OPFS mount at `/opfs` is created in C `main()` via `wasmfs_create_opfs_backend()`. Subdirectories inside `/opfs/images` are created with `mkdir()` on the worker thread. `/tmp` uses a memory backend and its subdirectories are pre-created in C because `FS.mkdir` from the JS main thread fails cross-thread under WasmFS pthreads.

### Startup Flow

1. Boot WASM module, mount OPFS directories.
2. Check for valid checkpoint → offer "Continue Session" or "Start Fresh".
3. If URL params specify media → download and auto-boot (skip dialogs).
4. Scan `/opfs/images/rom/` for persisted ROMs (probe known checksums).
5. No ROMs found → show ROM upload dialog.
6. ROMs found → show machine configuration dialog.
7. User clicks "Start" → load ROM, mount selected media, run.

### Image Persistence
When `fd insert` or `hd attach` receives a volatile path (`/tmp/`), the C-side `image_persist_volatile()` copies the file to `/opfs/images/<hash>.img` (content-addressed, FNV-1a hash) before the storage engine opens it. This ensures delta and journal files are also on OPFS. The persistent path is stored in checkpoints for restore.

### Drag-and-Drop Flow
1. `processDrop` gathers files via `extractAllDroppedFiles` (supports directories via `webkitGetAsEntry`).
2. Single files are checked for checkpoint signature first.
3. Files are written to `/tmp/upload/` via `FS.writeFile` (memory backend, accessible from main thread).
4. The upload pipeline calls `floppy.identify` / `scsi.identify_hd` / `archive.identify` through `gsEval` to classify the bytes.
5. The worker's `floppy.drives[0].insert` / `scsi.attach_hd` handlers persist volatile images to OPFS automatically.

### URL Media Fetching
- `processUrlMedia` iterates URLSearchParams in three passes (ROM, `fdN`, `hdN`). Each `fetchAndStore(slot, url)` downloads the payload, optionally unzips or auto-extracts a Mac archive via `archive.extract`, and writes the result into the appropriate `/opfs/images/` subdirectory via `storage.cp`.
- After downloads, the page calls `rom.identify` to pick the machine model, then `machine.boot`, `rom.load`, `floppy.drives[0].insert`, and `scsi.attach_hd` in order to bring the system up.

### COOP/COEP Headers
SharedArrayBuffer (required for pthreads) needs Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers. The dev server (`make run`) sends these automatically.

## Extending the Frontend
- Wire new features through the object model (`window.gsEval(path, args)`). The bridge is described in [object-model.md](object-model.md).
- Store long-lived media under `/opfs/images/` subdirectories for OPFS persistence. Reserve `/tmp/` for volatile artifacts.
- Volatile images are automatically persisted to `/opfs/images/` by `image_persist_volatile()` when mounted.
- The core is path-agnostic — all directory structure decisions belong to the web app.
- Any new URL parameter should be handled during `processUrlMedia`.
