
# Checkpointing

## Overview

Checkpointing allows the emulator to save its entire state to persistent storage and later restore it, ensuring seamless continuity across sessions or devices. Each subsystem that maintains state requiring preservation must implement a `<subsystem>_checkpoint(...)` function to serialize its internal state to a binary stream. Restoration is handled by the corresponding `<subsystem>_init(...)` function, which deserializes state from the stream. The "system" subsystem orchestrates the overall checkpointing process, coordinating all subsystems.

There are two main types of checkpoints:

- **Quick checkpoint:** Automatically triggered by the emulator at regular intervals or in response to events (such as a browser tab becoming hidden). Quick checkpoints are optimized for speed and may skip serializing state already present in persistent storage (e.g., disk image blocks in the delta file), relying on OPFS auto-persistence. This enables fast, frequent state saves without redundant data.

- **Consolidated checkpoint:** Explicitly created by the user to export the complete machine state into a single file or stream. In this mode, *all* emulator state—including data normally left in persistent storage—is fully serialized. Consolidated checkpoints are slower to create but provide a complete, portable snapshot suitable for backup, transfer, or archival.

Supporting both checkpoint types balances performance and reliability, enabling robust state persistence for both automated recovery and user-driven export.


## Background Checkpoints

Background checkpoints (quick checkpoints saved automatically) are serialised into a buffer on the emulator thread and written to OPFS-backed storage by the I/O worker (the save flow below). With OPFS + pthreads, every `fclose()` is immediately durable — no async sync step or marker protocol is needed. Each machine owns a directory under `/opfs/checkpoints/`; the quick-checkpoint slot, the writable image deltas, and the manifest all live together under that directory and are treated as one atomic unit.

### Per-Machine Directory

```
/opfs/checkpoints/<machine_id>-<created>/
  state.checkpoint        ← machine state (CPU, RAM, peripherals, per-image bitmap)
  state.checkpoint.tmp    (briefly, during a checkpoint write)
  <id>.delta              ← writable image delta (one pair per writable image)
  <id>.journal
  manifest.json           ← informational: build id, machine model, mounted images
```

- `<machine_id>` is a 16-hex-char opaque token (8 random bytes) held in `localStorage` under `gs.checkpoint.machine`. Generated on first boot, rotated only on explicit "new machine" actions.
- `<created>` is a UTC timestamp in compact ISO 8601 (`YYYYMMDDTHHMMSSZ`) — purely for human legibility in `ls /opfs/checkpoints/`. Code never parses it.
- `<id>` (per-image instance id) is also 16 hex chars, minted by the image layer in `image_create`. Each writable image gets a fresh one — reusing the same base image for an unrelated machine no longer replays stale deltas.

The C side is told about the active machine via `machine.register(<id>, <created>)`, which the frontend (`app/web2/src/bus/emulator.ts`, from the identity minted in `app/web2/src/lib/machineId.ts`) issues exactly once on startup before any image is opened. The handler calls `checkpoint_machine_set` and then `checkpoint_machine_sweep_others` to drop stale machine directories left behind by previous sessions.

`checkpoint_machine_set` is called **at most once per process lifetime**. Rotation is a JS-driven page reload; the C side does not support changing machine identity in-place.

### Checkpoint Save Flow

1. **Serialise** on the emulator thread into the quick buffer (one contiguous copy of RAM, the VRAM blocks, the device state, the disk bitmaps), with the v3 header laid down at the front at `checkpoint_close`.
2. **Publish** on the I/O worker (`src/core/io/io_worker.h`): write the buffer to `<machine_dir>/state.checkpoint.tmp` in 1 MB chunks (yielding between them so the guest's own disk reads are not queued behind one long write on the filesystem's proxy thread), `fclose` (OPFS auto-persists), then **atomic rename** to `<machine_dir>/state.checkpoint`. The rename is the swap; readers always see a complete file. The emulator thread never waits for it: the tick that serialised goes on with the next frame, and the completion arrives at a later drain as the `checkpoint_saved` event.
3. **One buffer.** While a publish is in flight the buffer is the worker's; an auto-save that comes due meanwhile is skipped and counted (the 750 ms rate limit already says "not yet"). `checkpoint.snapshot` waits for a publish in flight, saves, and waits for its own — a snapshot promises a complete file when it returns. `checkpoint.load`, `checkpoint.clear`, `storage.rm` and `storage.mv` wait for a publish in flight before touching the directory, so the rename never lands under them. A build without the worker (`--io=sync`, the unit suites) runs the same write inline.

There is no sequence-numbered file scheme any more. A monotonic `generation` counter inside the checkpoint header replaces it for diagnostics; on disk there is only one file.

### Checkpoint Load Flow (on page load)

1. **Compute** the path: `<machine_dir>/state.checkpoint`.
2. **Validate build ID** to reject checkpoints written by an incompatible build.
3. **Offer** the checkpoint to the user (continue or start fresh).

No marker files are needed: OPFS writes are durable the moment the file is closed, and the `tmp`+rename pattern guarantees that `state.checkpoint` is either complete or absent.

### Headless Variant

The headless target has no `localStorage` and no machine-id concept. Pass `--checkpoint-dir=<path>` to point the machine layer at an explicit directory; deltas land there alongside `state.checkpoint`. Tests typically use a per-test temp directory. With no flag, the machine directory stays unset and `image_create` falls back to placing deltas next to the base image (legacy headless behavior).


## Checkpointing Design

- **Opaque handle:**
  - `checkpoint_t` is an opaque pointer to a checkpoint handle. Its internals are defined in `system.c`.

- **Stream helpers:**
  - `system_read_checkpoint_data(checkpoint, data, size)` and `system_write_checkpoint_data(checkpoint, data, size)` are macros that wrap typed I/O with a header containing the size, source filename, and line number for robust validation and diagnostics.

- **Subsystem serialization:**
  - Each subsystem that persists state provides a `<subsystem>_checkpoint(self, cp)` function. The typical pattern is to write the contiguous POD (plain old data) region in one block, then serialize dynamic buffers separately.

- **API declaration:**
  - Each subsystem declares its checkpoint function in its own header, alongside `*_init` and `delete_*`. The central `system.h` does not list all subsystem checkpoint prototypes; consumers include the relevant subsystem headers as needed.

- **Save/Load commands:**
  - `machine.register(<id>, <created>)` (legacy alias: `checkpoint --machine <id> <created>`): Activates the per-machine directory `<machine_id>-<created>` under `/opfs/checkpoints/` and sweeps stale sibling dirs. **Must be the first checkpoint operation in the process** (called once by the web2 bus at startup, before any image is opened); subsequent calls in the same process are rejected.
  - `checkpoint --save <file>`: Iterates all subsystems and writes a consolidated machine snapshot to the specified file. Consolidated checkpoints are self-contained and live wherever the user chooses — they do **not** go under `/opfs/checkpoints/<machine_id>-...`, which is reserved for the single quick-checkpoint slot.
  - `checkpoint --load <file>`: Constructs a new `config_t` via the active machine profile, restoring each subsystem from the stream.
  - `checkpoint --validate <path>`: Checks if the file contains a valid checkpoint (magic bytes).
  - `checkpoint --probe`: Returns 0 if a valid `state.checkpoint` exists in the current machine directory.
  - `checkpoint load`: Rebuilds the machine from the stream (the new machine is constructed before the old one is destroyed). The debug object is part of the machine, so the previous machine's breakpoints and logpoints do not survive a load; add them again afterwards. Memory logpoints added after a load fire as they did before it (#172).
  - `checkpoint clear`: Deletes `state.checkpoint` (and any leftover `*.tmp`) inside the current machine directory, together with the image deltas and journals there that no open image holds — the discarded state's deltas can never be reached again, and would otherwise accumulate one per session; the directory itself is left in place.

- **File format & signature:**
  - Two on-disk formats are used:
    - **v2 (`GSCHKPT2`)** — Used for consolidated (full-export) checkpoints. Per-block RLE compression with file/line metadata for diagnostics. Data blocks >= 64 bytes are RLE-compressed individually.
    - **v3 (`GSCHKPT3`)** — Used for quick (background auto-save) checkpoints. All data is accumulated into a pre-allocated memory buffer behind a header-sized gap; at close the header is filled in and the whole buffer is one file the I/O worker writes and publishes. No RLE (the payload is mostly uncompressible RAM: `compressed_size == uncompressed_size` marks it raw) and no per-block metadata (filenames, line numbers).
  - The v3 format structure: `GSCHKPT3` (8 bytes) + build id + model id + ram_size_kb + uncompressed_size (8 bytes) + compressed_size (8 bytes) + raw payload.
  - The reader auto-detects the format by inspecting the 8-byte magic signature.


## Image Persistence for Quick Checkpoints

Quick checkpoints assume that disk image base files and their delta/journal pairs exist in persistent storage at restore time. In the browser that is arranged by the web app, not the core: an uploaded or URL-fetched image is copied into `/opfs/images/<category>/` (`app/web2/src/bus/upload.ts::persist`) *before* it is attached, so the path the machine opens — and a checkpoint records — is already on OPFS. The core opens the path it is given and does not copy media anywhere.

`/opfs/images/` is **strictly read-only base content**. The writable side — delta and journal — is rooted under the per-machine checkpoint directory (`/opfs/checkpoints/<machine_id>-<created>/<id>.delta` and `<id>.journal`), not next to the base. This is the key bug fix from the storage-isolation rewrite: reusing the same base image for an unrelated machine no longer replays stale deltas, because every `image_create` mints a fresh random instance id (see `docs/core/storage/image.md`).

### How It Works

`fd insert` / `hd attach` open the image at the given path via `image_create(base, pick_delta_dir(base))`. `pick_delta_dir` returns `checkpoint_machine_dir()` for OPFS-backed bases, so the delta and journal are created under `/opfs/checkpoints/<machine_id>-<created>/`. Quick checkpoints record the per-image `instance_path` so a future restore can reopen the same files via `image_open(base, instance_path)`.

A volatile path (`/tmp/…`) attached from the shell or API stays volatile: the image, its delta and the checkpoint's reference to it are gone after a reload. Copy it under `/opfs/` first to keep it.

Until 2026-09 the core copied every non-OPFS image to a flat, hash-named `/opfs/images/<8-hex>.img` before opening it (`image_persist_volatile`). That code is gone. Files it wrote are left in place: a checkpoint manifest records its base by path, so an old checkpoint still reopens its image.

### Filesystem Layout

```
/                              Memory (default WasmFS root)
├── opfs/                                       Single OPFS mount (persistent)
│   ├── images/                                 Read-only base images
│   │   ├── rom/                                ROM images (named by content id)
│   │   ├── vrom/                               Video ROM images
│   │   ├── fd/                                 Floppy images (400K / 800K / 1.4MB)
│   │   ├── hd/                                 SCSI hard disk images
│   │   └── cd/                                 CD-ROM images
│   ├── checkpoints/                            Per-machine state lives here
│   │   └── <machine_id>-<created>/             One directory per machine
│   │       ├── state.checkpoint                Quick checkpoint (atomic via tmp+rename)
│   │       ├── state.checkpoint.tmp            (briefly, during a write)
│   │       ├── <id>.delta                      Writable image delta (per image)
│   │       ├── <id>.journal                    Writable image preimage journal
│   │       └── manifest.json                   Build/setup metadata (informational)
│   ├── upload/
│   └── (user can create anything here)
└── tmp/                                        Memory mount (volatile)
    ├── gs-image-ro/                            Scratch deltas for read-only mounts
    ├── upload/                                 File upload staging
    └── extract/                                Archive extraction
```

A single OPFS mount at `/opfs` is created in C `main()`. Subdirectories inside `/opfs/images` are created via `mkdir()` on the worker thread; the `<machine_id>-<created>` directory is created lazily by `checkpoint_machine_set()`.

Read-only image opens (`image_open_readonly`) park their throwaway delta and journal under `/tmp/gs-image-ro/<random>.{delta,journal}` and delete those on `image_close`. They never touch `/opfs/images/`.


### Details and Best Practices

- **Goals:**
  - Minimize the number of read/write calls for performance (ideally one contiguous write for the plain-data portion of a subsystem).
  - Preserve robustness and debuggability with size validation and source location tagging.

- **Macros and debug metadata:**
  - `system_write_checkpoint_data(cp, data, size)` and `system_read_checkpoint_data(cp, data, size)` are macros that expand to `x_system_*` and automatically capture `__FILE__` and `__LINE__`.
  - The written stream stores a size header, the originating file path and line, and then the raw bytes. On read, mismatches are reported with the saved source anchors.

- **Struct layout guideline:**
  - Place POD (plain old data) fields first, pointers and non-POD fields last. This allows a single block I/O for the contiguous POD region, then serializes any pointed-to buffers separately. Use `offsetof(struct <type>, first_pointer_field)` to bound the POD region when helpful.
  - Restated for device authors, alongside the scheduler-lifetime and assert rules, in [`../../guide/STYLE_GUIDE.md`](../../guide/STYLE_GUIDE.md) § "Device module conventions".
  - **Layout changes are free.** The stream is positional with no version field and a build-ID mismatch is rejected outright (`checkpoint.c`; the ID is `__DATE__ " " __TIME__`, force-recompiled every build), so a checkpoint can only ever be restored by the exact binary that wrote it. There is no old format to support. The one real constraint is that a save and its restore must change **together, in the same commit** — a swapped pair does not fail at the swap, it cross-loads and dies later at whichever block first disagrees on size.

- **Orchestration and ordering:**
  - `setup_plus_checkpoint(file, kind)` opens a write handle and invokes each subsystem's `<subsystem>_checkpoint` in a well-defined order (RAM, CPU, scheduler, RTC, SCC, sound, VIA, mouse, SCSI, keyboard, floppy, etc.).
  - `setup_plus(checkpoint)` constructs all subsystems with the same stream handle so each subsystem can restore directly during init.

- **Cross-subsystem state:**
  - If a command or state spans multiple subsystems (e.g., floppy + image paths), keep the serialization logic in `system.c` or at an appropriate orchestration layer to avoid tight coupling inside a device subsystem.

- Disk images and storage backends
  - `image_checkpoint` writes the on-disk filename, writable flag, raw image size (`raw_size`), the per-image `instance_path` stem, and then delegates to `storage_checkpoint`. The `instance_path` field is what lets a future restore reopen the same delta+journal pair without relying on adjacent-to-base sidecars.
  - `storage_checkpoint` inspects `checkpoint_get_kind(checkpoint)` to decide whether to write only the bitmap (quick checkpoints) or stream the entire delta (consolidated checkpoints). It is unchanged by the storage-isolation rewrite — the bitmap and block streams are still file-format-compatible at the storage layer.
  - During restore the machine init code (e.g. `plus_init`, `se30_init`) reads `(name, writable, raw_size, instance_path)` and picks an opener based on `(writable, kind)`:
    - **Consolidated + writable** → `image_create_empty(name, raw_size)` recreates the base file, then `image_create(name, checkpoint_machine_dir())` mints a fresh writable instance; `storage_restore_from_checkpoint` populates all delta blocks from the embedded data.
    - **Quick + writable** → `image_open(name, instance_path)` reopens the same delta+journal that were live at save time. `storage_restore_from_checkpoint` reads the bitmap from the checkpoint stream and sets it as the current state. The delta's block data is already correct (OPFS auto-persisted every write).
    - **Read-only** → `image_open_readonly(name)`. Per-instance scratch under `/tmp/gs-image-ro/` is fresh.
  - Old checkpoints written before the format change become unreadable naturally through `checkpoint_validate_build_id`; no migration code exists.

- Error handling
  - `checkpoint_has_error(cp)` can be checked after a subsystem's read/write; `setup_plus_*` reports and aborts on failure.
  - Subsystems should return early on `NULL` or errored checkpoint handles.

- Compatibility considerations
  - The size+file+line header helps detect struct layout drift; if a struct changes, the reader emits a helpful diagnostic indicating the write-site.
  - For non-breaking evolution, consider versioned payloads or write smaller sub-structures with stable layouts.
