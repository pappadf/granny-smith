# Web Frontend Reference

The Granny Smith web frontend lives in `app/web/index.html`. The page and its module scripts act as the single entry point for initializing the emulator, wiring terminal I/O, and handling user interaction. This reference documents both the user-facing capabilities and the architectural details you need when modifying the UI or embedding it elsewhere.

## Architecture: Pthreads + WasmFS + OPFS

The emulator uses Emscripten's `PROXY_TO_PTHREAD` mode. The C `main()` runs on a dedicated worker thread; the browser main thread handles DOM, input, and compositing.

**Threading model:**
- **Main (browser) thread:** DOM events, xterm.js terminal, UI chrome, file uploads to `/tmp/`
- **Emulator worker thread:** CPU emulation, OPFS file I/O (delta/journal/checkpoint), shell command execution

**Persistence:** A single OPFS mount at `/opfs` provides all persistent storage: `/opfs/images` (ROMs, disk images, content-addressed store), `/opfs/checkpoints`, and `/opfs/upload`. Users can also create arbitrary persistent paths under `/opfs/`. The web app creates subdirectories inside `/opfs/images` (`rom`, `vrom`, `fd`, `fdhd`, `hd`, `cd`) via `mkdir` on the worker thread. `/tmp` uses the memory backend (volatile). The WasmFS root itself is memory-backed (the `wasmfs_create_root_dir` hook cannot use OPFS because it runs on the main browser thread where `wasmfs_create_opfs_backend()` deadlocks). The web app only uses paths under `/opfs/`, so in practice everything except `/tmp` is persistent. All OPFS writes are immediately durable — no explicit sync step needed.

**Core/frontend separation:** The emulator core makes no assumptions about filesystem structure. It accepts paths as command arguments. The web app defines and owns the directory layout, creating directories via shell commands (`mkdir`) and persisting files via `file-copy`.

**Cross-thread communication:** JS on the main thread cannot call WASM functions that access OPFS (different thread). Instead, calls are dispatched via a shared-heap queue (SharedArrayBuffer). The worker's `shell_poll()` dequeues and executes them each tick. Two queue kinds matter to JS:

- **Object-model calls** — `window.gsEval(path, args?)` writes the path and JSON-encoded arguments into the shared buffer, the worker resolves the path against the object tree, runs the operation, and writes a JSON-encoded `value_t` back. This is the path every typed call goes through (see [object-model.md](object-model.md)).
- **Free-form line input** — the terminal's `Enter` key sends the raw line through the same queue with a different kind tag, and the worker dispatches it through the shell's path-form parser. JS code never uses this directly; only the xterm.js terminal does.

Run-state and prompt are also communicated via shared-heap flags.

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
- `main.mjs` (generated by Emscripten) is dynamically imported with a cache-busting query parameter. `createModule` is invoked with canvas, arguments, and print callbacks.
- After module creation, shared-heap pointers are resolved: object-model call buffers (path, args, result), pending/done flags, prompt buffer, and running-state flag. These are C globals exported via getter functions, accessible through `Module.HEAP32` on the SharedArrayBuffer.
- With `PROXY_TO_PTHREAD`, `shell_init()` is called from `main()` on the worker — no `ccall` from JS.

### Shared-Heap Bridge (`gsEval`)
JS cannot call WASM functions that access OPFS from the main thread. Instead, every typed call goes through a shared-heap queue:
1. JS writes the path string and JSON-encoded args into shared-heap buffers (`Module.stringToUTF8`).
2. JS sets `g_gs_pending = 1` via `Module.HEAP32`.
3. The worker's `shell_poll()` (called every tick) reads the buffers, calls `gs_eval(path, args_json, out_buf, out_size)`, and sets `g_gs_done = 1`.
4. JS polls `g_gs_done` with `setTimeout(check, 1)`, parses the JSON result, and resolves the Promise.

Calls are serialised (one at a time) via an async mutex in JS. The free-form-line input from xterm.js uses the same queue with a different kind tag — the worker dispatches it through the shell's path-form parser instead of directly to `gs_eval`.

### Run-State Polling
The worker updates `g_shared_is_running` every tick. JS polls it via `setInterval(pollRunState, 100)` and fires registered callbacks when the state changes. This replaces the old `Module.onRunStateChange` EM_ASM callback which couldn't work cross-thread.

### Prompt Buffer
The worker writes the current prompt (disassembly at PC) into `g_prompt_buffer` after each command. JS reads it via `Module.UTF8ToString()` when the terminal needs to display the prompt.

### Terminal Integration (xterm.js)
- xterm.js plus the FitAddon power the interactive console. Input handling is fully client-side: the script maintains `{buffer, cursor, history}`. On Enter, the buffer is dispatched through the shared-heap command queue.

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
