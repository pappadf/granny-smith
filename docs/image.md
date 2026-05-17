## Image Module

The image module manages floppy and hard-disk containers while delegating all block I/O and persistence to the delta-file storage engine. Images are not loaded entirely into RAM; instead, a lightweight descriptor keeps track of backing files while the storage layer handles reads, writes, and crash recovery.

The image subsystem speaks **paths only**. It does not know about machine ids, slots, drives, or `/opfs/checkpoints/`; the higher layer (`config_t` in `system.c`) decides where to place per-image state and remembers the result across boots.

**Types & Key Values**
- **`image_t`** *(see `src/core/storage/image.h`)* keeps the paths and handles needed by the delta-file storage layer:
	- `storage`: opaque `storage_t*` handle used for every block read/write.
	- `filename`: original path supplied by the user (the immutable base image).
	- `instance_path`: stem `<dir>/<id>` for the per-instance delta+journal pair, where `<id>` is a 16-hex-char opaque id minted by the image layer. `NULL` for read-only mounts.
	- `delta_path`: `<instance_path>.delta`.
	- `journal_path`: `<instance_path>.journal`.
	- `raw_size`: logical size in bytes (`block_count * 512`).
	- `writable`: true when the caller asked for write access.
	- `ghost_instance`: true when delta+journal live in a process-local scratch dir (read-only mounts); they are deleted on `image_close`.
	- `type`: detected category (`image_fd_ds`, `image_hd`, ...).
	- `from_diskcopy`: marks DiskCopy 4.2 sources so their headers can be skipped.

**Module lifecycle**
- **`image_init(checkpoint_t *checkpoint)`** and **`image_delete(void)`** remain no-ops (no global resources).

**Opening images** — three typed entry points

The single old `image_open(filename, writable)` is replaced by three explicit operations matching the three real use cases:

- **`image_open_readonly(const char *base_path)`** — opens a base image with no on-disk delta. The image layer mints a scratch instance under `/tmp/gs-image-ro/`; the delta and journal there are deleted on `image_close`. Use this for probes (`fd validate`, `cdrom validate`, `image partmap`, …) and for read-only mounts (CD-ROM).
- **`image_create(const char *base_path, const char *delta_dir)`** — opens a fresh writable instance. The image layer mints a 16-hex-char opaque id and creates `<delta_dir>/<id>.delta` and `<delta_dir>/<id>.journal`. If `delta_dir` is `NULL`, the directory of the base image is used (legacy adjacent-to-base layout — used by tests with no machine identity).
- **`image_open(const char *base_path, const char *instance_path)`** — reopens an existing writable instance. `instance_path` is the stem returned by `image_path()` when the instance was first created; the image layer appends `.delta` and `.journal` itself. Used by checkpoint restore.

After construction the caller queries the instance stem with **`image_path(const image_t *image)`** and persists it (in a checkpoint or in the higher-layer slot table) so a future `image_open(base, instance_path)` can find the same delta files. Returns `NULL` for read-only mounts.

Common steps (shared by all three openers):

1. `stat()` + a lightweight DiskCopy 4.2 probe distinguish raw images from DC archives. DiskCopy images must have `data_size` aligned to 512 bytes.
2. A `storage_config_t` is built with `base_path=base`, `delta_path`, `journal_path`, and `base_data_offset` (0 for raw, 0x54 for DiskCopy).
3. `storage_new()` opens the base file read-only, opens or creates the delta and journal, and reads existing bitmaps when the delta already exists.

No seeding step is needed — unmodified blocks are read directly from the base file.

**Picking the delta directory** — `system.c` helper

The higher layer in `system.c` chooses the delta directory before each fresh writable mount:

```c
static const char *pick_delta_dir(const char *path) {
    // Volatile bases under /tmp/ (test-uploaded artifacts) keep deltas next
    // to the base — passing NULL falls back to image_create's own derivation.
    if (path && strncmp(path, "/tmp/", 5) == 0)
        return NULL;
    // Otherwise route deltas under the active per-machine directory so they
    // share lifetime with state.checkpoint (see docs/checkpointing.md).
    return checkpoint_machine_dir();
}
```

`fd insert`, `fd create`, and `hd attach` all funnel through this helper.

**Volatile image persistence** — `image_persist_volatile(const char *path)`

When a disk image resides in volatile storage (`/tmp/` or `/fd/`), this function copies it to `/opfs/images/<hash>.img` (OPFS-backed, content-addressed via FNV-1a hash). This runs on the worker thread where OPFS is accessible. The `fd insert` and `hd attach` commands call this automatically before opening the image. Returns a persistent path that the caller must free.

**Reading/Writing image data**
- **`disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size)`** and **`disk_write_data(...)`** enforce 512-byte alignment and forward to `storage_read_block` / `storage_write_block` in a loop.

**Background work**
- **`image_tick_all(config_t *config)`** calls `storage_tick()` for each registered image. With the delta model, `storage_tick()` is a no-op (no consolidation needed).

**Persisting changes / Exporting**
- **`image_save(image_t *image)`** calls `storage_save_state()` to emit a dense raw image back to `image->filename`. DiskCopy sources cannot be exported back to `.dc42`.

**Creating blank floppy images**
- **`image_create_blank_floppy()`** writes a zero-filled 819,200-byte (or 1,474,560-byte HD) raw file that can immediately be opened.

**Checkpointing & metadata**
- **`image_checkpoint()`** writes `{uint32 len, path bytes, writable flag, raw_size, uint32 instance_len, instance_path bytes}` and then calls `storage_checkpoint()`. The `instance_path` field (added in the storage-isolation rewrite) lets the restore path locate the delta+journal pair without relying on adjacent-to-base sidecars. The storage layer writes the current bitmap for quick checkpoints or streams all blocks for consolidated checkpoints.
- During restore the machine init code reads back the same fields and chooses an opener based on `(writable, kind)`:
	- writable + quick → `image_open(base, instance_path)` reopens the same delta files.
	- writable + consolidated → `image_create(base, checkpoint_machine_dir())` mints a fresh instance; the embedded blocks then repopulate it via `storage_restore_from_checkpoint()`.
	- read-only → `image_open_readonly(base)`.
- Old checkpoints written before the format change become unreadable naturally through `checkpoint_validate_build_id`; no migration code is needed.

**Usage notes**
- Writable image deltas now live under `<delta_dir>/<id>.delta` (typically `/opfs/checkpoints/<machine_id>-<created>/<id>.delta`), not next to the base image. Removing the per-machine directory reverts every disk in that machine to its pristine base state in one step.
- DiskCopy images become writable overlays: mutations live in the delta file while the source archive stays untouched.
- Reusing the same base image for a fresh machine no longer replays stale deltas: every `image_create` mints a new random instance id, so two machines mounting the same base get two independent delta files.

---
Last updated for the per-machine checkpoint-isolation rewrite (proposal-checkpoint-storage-isolation.md).
