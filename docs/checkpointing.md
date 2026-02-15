
# Checkpointing

## Overview

Checkpointing allows the emulator to save its entire state to persistent storage and later restore it, ensuring seamless continuity across sessions or devices. Each subsystem that maintains state requiring preservation must implement a `<subsystem>_checkpoint(...)` function to serialize its internal state to a binary stream. Restoration is handled by the corresponding `<subsystem>_init(...)` function, which deserializes state from the stream. The "system" subsystem orchestrates the overall checkpointing process, coordinating all subsystems.

There are two main types of checkpoints:

- **Quick checkpoint:** Automatically triggered by the emulator at regular intervals or in response to events (such as a browser page reload or closure). Quick checkpoints are optimized for speed and may skip serializing state already present in persistent storage (e.g., disk image blocks), relying on the underlying file system to persist those changes. This enables fast, frequent state saves without redundant data.

- **Consolidated checkpoint:** Explicitly created by the user to export the complete machine state into a single file or stream. In this mode, *all* emulator state—including data normally left in persistent storage—is fully serialized. Consolidated checkpoints are slower to create but provide a complete, portable snapshot suitable for backup, transfer, or archival.

Supporting both checkpoint types balances performance and reliability, enabling robust state persistence for both automated recovery and user-driven export.


## Background Checkpoint Completion Tracking

Background checkpoints (quick checkpoints saved automatically) use a marker-based system to ensure only fully-persisted checkpoints are loaded after a page reload:

### File Naming Convention

Checkpoint files use a 7-digit sequence number:
- `0000001.checkpoint` - The actual checkpoint data
- `0000001.pending` - Marker indicating checkpoint write is in progress
- `0000001.complete` - Marker indicating checkpoint is fully persisted to IndexedDB

### Checkpoint Save Flow

1. **Create `.pending` marker** before writing any checkpoint data
2. **Write checkpoint data** to the `.checkpoint` file
3. **Trigger IndexedDB sync** and wait for completion callback
4. **On sync success:**
   - Remove `.pending` marker
   - Create `.complete` marker
   - Clean up older checkpoint files (seq < current)
5. **On sync failure:**
   - Leave `.pending` marker in place
   - Do NOT create `.complete` marker

### Checkpoint Load Flow (on page load)

1. **Clean up incomplete checkpoints:**
   - Remove any `.checkpoint` files without corresponding `.complete` markers
   - Remove orphaned `.pending` markers
2. **List only complete checkpoints** (those with both `.checkpoint` AND `.complete`)
3. **Offer the latest complete checkpoint** to the user
4. **Clean up older checkpoints** after user selection

### Page Reload Safety

The marker system handles page reload edge cases:
- If reload interrupts during checkpoint write: `.pending` exists but no `.complete`
- If reload interrupts during sync: `.pending` exists but no `.complete`
- If reload after sync but before cleanup: Both markers exist; considered complete

Incomplete checkpoints are automatically cleaned up on next page load.


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
    - **v2 (`GSCHKPT2`)** — Used for consolidated (full-export) checkpoints. Per-block RLE compression with file/line metadata for diagnostics. Data blocks ≥ 64 bytes are RLE-compressed individually.
    - **v3 (`GSCHKPT3`)** — Used for quick (background auto-save) checkpoints. All data is accumulated into a pre-allocated memory buffer, then the entire buffer is RLE-compressed in a single pass and written to disk in one `fwrite` call. No per-block metadata (filenames, line numbers) is stored.
  - The v3 format structure: `GSCHKPT3` (8 bytes) + uncompressed_size (8 bytes) + compressed_size (8 bytes) + RLE-compressed payload.
  - File references in v3 quick checkpoints: files at persistent paths (`/persist/`) are stored as path-only references; files at volatile paths are embedded with content to survive page reload.
  - The reader auto-detects the format by inspecting the 8-byte magic signature.


## Image Persistence for Quick Checkpoints

Quick checkpoints assume that disk image backing files and their `.blocks/` directories already exist in persistent storage at restore time. In the browser, images uploaded via drag-and-drop or test injection initially land in volatile MEMFS (`/tmp/`), which is wiped on page reload. The `media-persist.js` module fixes this by automatically copying volatile images to IDBFS before the C core opens them.

### How It Works

When an `insert-fd` or `attach-hd` command targets a volatile path (`/tmp/` or `/fd/`), the JS-side command preprocessor (registered in `emulator.js`):

1. Reads the image file from volatile storage.
2. Computes a content hash (FNV-1a over first 64 KB + total file size) → 8-char hex.
3. Copies the file to `/persist/images/<hash>.img` (skipped if the hash file already exists).
4. Triggers an IDBFS sync to ensure the copy reaches IndexedDB.
5. Rewrites the mount command to use the persistent path.

The C core then opens the image at its persistent location, and the storage engine creates `.blocks/` adjacent to it under `/persist/images/`. All subsequent quick checkpoints reference this persistent path. After a page reload, the image and its `.blocks/` directory are available from IDBFS and the checkpoint restores successfully.

### Content-Addressed Naming

Images under `/persist/images/` are named by their content hash (`<hash>.img`). This provides:

- **Deduplication:** The same image mounted multiple times is stored only once.
- **Skip-if-present:** If the hash file exists, the copy is skipped entirely — no wasted I/O.
- **No name collisions:** Different images with the same original filename get unique hashes.

Images loaded via URL parameters (`url-media.js`) already land in `/persist/boot/` and are not affected by this mechanism — `isVolatile()` returns false for persistent paths.

### Filesystem Layout

```
/persist/
├── boot/
│   ├── rom           # ROM binary
│   ├── fd0           # Boot floppy (URL-parameter path)
│   └── hd0           # Boot HD (URL-parameter path)
├── images/
│   ├── a3f7c012.img         # Content-addressed persisted image
│   ├── a3f7c012.img.blocks/ # Storage engine directory
│   └── ...
└── checkpoint/
    ├── 0000042.checkpoint
    └── 0000042.complete
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
  - For example, the floppy controller snapshot saves controller/drive position, but does not reopen disk images; image association is handled at a higher level.

- Disk images and storage backends
  - `image_checkpoint` writes the on-disk filename, writable flag, raw image size (`raw_size`), and then delegates to `storage_checkpoint`.
  - `storage_checkpoint` inspects `checkpoint_get_kind(checkpoint)` to decide whether to write only metadata (quick checkpoints) or stream the entire backing store (consolidated checkpoints).
  - During restore of a **consolidated checkpoint**, `system.c` unconditionally recreates each backing file from `raw_size` before opening it. This guarantees the file matches the checkpoint regardless of whether it was missing (e.g. MEMFS cleared after page reload), stale, or the wrong size. The subsequent `storage_restore_from_checkpoint` call then populates all storage blocks from the embedded data.
  - During restore of a **quick checkpoint**, backing files are expected to already exist with correct data in persistent storage. `storage_restore_from_checkpoint` applies the rollback journal on top of the existing base data.

- Error handling
  - `checkpoint_has_error(cp)` can be checked after a subsystem's read/write; `setup_plus_*` reports and aborts on failure.
  - subsystem should return early on `NULL` or errored checkpoint handles.

- Compatibility considerations
  - The size+file+line header helps detect struct layout drift; if a struct changes, the reader emits a helpful diagnostic indicating the write-site.
  - For non-breaking evolution, consider versioned payloads or write smaller sub-structures with stable layouts.