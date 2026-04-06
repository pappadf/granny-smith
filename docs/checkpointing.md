
# Checkpointing

## Overview

Checkpointing allows the emulator to save its entire state to persistent storage and later restore it, ensuring seamless continuity across sessions or devices. Each subsystem that maintains state requiring preservation must implement a `<subsystem>_checkpoint(...)` function to serialize its internal state to a binary stream. Restoration is handled by the corresponding `<subsystem>_init(...)` function, which deserializes state from the stream. The "system" subsystem orchestrates the overall checkpointing process, coordinating all subsystems.

There are two main types of checkpoints:

- **Quick checkpoint:** Automatically triggered by the emulator at regular intervals or in response to events (such as a browser tab becoming hidden). Quick checkpoints are optimized for speed and may skip serializing state already present in persistent storage (e.g., disk image blocks in the delta file), relying on OPFS auto-persistence. This enables fast, frequent state saves without redundant data.

- **Consolidated checkpoint:** Explicitly created by the user to export the complete machine state into a single file or stream. In this mode, *all* emulator state—including data normally left in persistent storage—is fully serialized. Consolidated checkpoints are slower to create but provide a complete, portable snapshot suitable for backup, transfer, or archival.

Supporting both checkpoint types balances performance and reliability, enabling robust state persistence for both automated recovery and user-driven export.


## Background Checkpoints

Background checkpoints (quick checkpoints saved automatically) are written directly to OPFS-backed storage. With OPFS + pthreads, every `fclose()` is immediately durable — no async sync step or marker protocol is needed.

### File Naming Convention

Checkpoint files use a 7-digit sequence number:
- `0000001.checkpoint` - The checkpoint data (durable on write)

### Checkpoint Save Flow

1. **Write checkpoint data** to the `.checkpoint` file (synchronous, auto-persisted by OPFS)
2. **Clean up older checkpoints** (remove files with sequence numbers < current)

### Checkpoint Load Flow (on page load)

1. **Scan for `.checkpoint` files** and find the highest sequence number
2. **Validate build ID** to reject checkpoints from incompatible builds
3. **Offer the latest checkpoint** to the user (continue or start fresh)

No marker files are needed because OPFS writes are durable the moment the file is closed. There is no window where a partially-written checkpoint could be mistaken for a valid one — `system_checkpoint` writes the complete file or not at all.


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
  - `save-state <file>`: Iterates all subsystems and writes a complete machine snapshot to the specified file.
  - `load-state <file>`: Constructs a new `config_t` by calling `setup_plus(checkpoint)`, restoring each subsystem from the stream.

- **File format & signature:**
  - Two on-disk formats are used:
    - **v2 (`GSCHKPT2`)** — Used for consolidated (full-export) checkpoints. Per-block RLE compression with file/line metadata for diagnostics. Data blocks >= 64 bytes are RLE-compressed individually.
    - **v3 (`GSCHKPT3`)** — Used for quick (background auto-save) checkpoints. All data is accumulated into a pre-allocated memory buffer, then the entire buffer is RLE-compressed in a single pass and written to disk in one `fwrite` call. No per-block metadata (filenames, line numbers) is stored.
  - The v3 format structure: `GSCHKPT3` (8 bytes) + uncompressed_size (8 bytes) + compressed_size (8 bytes) + RLE-compressed payload.
  - The reader auto-detects the format by inspecting the 8-byte magic signature.


## Image Persistence for Quick Checkpoints

Quick checkpoints assume that disk image backing files and their delta/journal files exist in persistent storage at restore time. In the browser, images uploaded via drag-and-drop initially land in volatile `/tmp/` (memory-backed), which is wiped on page reload. The C-side `image_persist_volatile()` function fixes this by copying volatile images to OPFS before the storage engine opens them.

### How It Works

When an `insert-fd` or `attach-hd` command targets a volatile path (`/tmp/` or `/fd/`), `image_persist_volatile()` (called from the worker thread where OPFS is accessible):

1. Reads the image file from volatile storage.
2. Computes a content hash (FNV-1a over first 64 KB + total file size) -> 8-char hex.
3. Copies the file to `/images/<hash>.img` (skipped if the hash file already exists).
4. Returns the persistent path.

The command then opens the image at its persistent location. The storage engine creates `.delta` and `.journal` files adjacent to it under `/images/` — all on OPFS, surviving page reloads. Quick checkpoints reference the persistent path, so checkpoint restore succeeds after reload.

### Content-Addressed Naming

Images under `/images/` are named by their content hash (`<hash>.img`). This provides:

- **Deduplication:** The same image mounted multiple times is stored only once.
- **Skip-if-present:** If the hash file exists, the copy is skipped entirely — no wasted I/O.
- **No name collisions:** Different images with the same original filename get unique hashes.

Images loaded via URL parameters (`url-media.js`) land in `/boot/` and are not affected by this mechanism.

### Filesystem Layout

```
/                              Memory (default WasmFS root)
├── boot/                       OPFS (persistent)
│   ├── roms/
│   │   ├── <CHECKSUM>         ROM by checksum
│   │   └── latest             Active ROM
│   ├── fd0, fd1, ...          Boot floppy slots
│   └── hd0, hd1, ...          Boot HD slots
├── images/                     OPFS (persistent)
│   ├── <hash>.img             Content-addressed disk images
│   ├── <hash>.img.delta       Delta files (modifications)
│   └── <hash>.img.journal     Preimage journals
├── checkpoint/                 OPFS (persistent)
│   └── 0000042.checkpoint     Quick checkpoint data
└── tmp/                        Memory (volatile scratch)
    ├── upload/                 File upload staging
    └── extract/                Archive extraction
```


### Details and Best Practices

- **Goals:**
  - Minimize the number of read/write calls for performance (ideally one contiguous write for the plain-data portion of a subsystem).
  - Preserve robustness and debuggability with size validation and source location tagging.

- **Macros and debug metadata:**
  - `system_write_checkpoint_data(cp, data, size)` and `system_read_checkpoint_data(cp, data, size)` are macros that expand to `x_system_*` and automatically capture `__FILE__` and `__LINE__`.
  - The written stream stores a size header, the originating file path and line, and then the raw bytes. On read, mismatches are reported with the saved source anchors.

- **Struct layout guideline:**
  - Place POD (plain old data) fields first, pointers and non-POD fields last. This allows a single block I/O for the contiguous POD region, then serializes any pointed-to buffers separately. Use `offsetof(struct <type>, first_pointer_field)` to bound the POD region when helpful.

- **Orchestration and ordering:**
  - `setup_plus_checkpoint(file, kind)` opens a write handle and invokes each subsystem's `<subsystem>_checkpoint` in a well-defined order (RAM, CPU, scheduler, RTC, SCC, sound, VIA, mouse, SCSI, keyboard, floppy, etc.).
  - `setup_plus(checkpoint)` constructs all subsystems with the same stream handle so each subsystem can restore directly during init.

- **Cross-subsystem state:**
  - If a command or state spans multiple subsystems (e.g., floppy + image paths), keep the serialization logic in `system.c` or at an appropriate orchestration layer to avoid tight coupling inside a device subsystem.

- Disk images and storage backends
  - `image_checkpoint` writes the on-disk filename, writable flag, raw image size (`raw_size`), and then delegates to `storage_checkpoint`.
  - `storage_checkpoint` inspects `checkpoint_get_kind(checkpoint)` to decide whether to write only the bitmap (quick checkpoints) or stream the entire delta (consolidated checkpoints).
  - During restore of a **consolidated checkpoint**, `system.c` unconditionally recreates each backing file from `raw_size` before opening it. The subsequent `storage_restore_from_checkpoint` call populates all delta blocks from the embedded data.
  - During restore of a **quick checkpoint**, backing files and their deltas are expected to already exist in OPFS with correct data. `storage_restore_from_checkpoint` reads the bitmap from the checkpoint stream and sets it as the current state. The delta's block data is already correct (OPFS auto-persisted every write).

- Error handling
  - `checkpoint_has_error(cp)` can be checked after a subsystem's read/write; `setup_plus_*` reports and aborts on failure.
  - Subsystems should return early on `NULL` or errored checkpoint handles.

- Compatibility considerations
  - The size+file+line header helps detect struct layout drift; if a struct changes, the reader emits a helpful diagnostic indicating the write-site.
  - For non-breaking evolution, consider versioned payloads or write smaller sub-structures with stable layouts.
