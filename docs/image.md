## Image Module

The image module manages floppy and hard-disk containers while delegating all block I/O and persistence to the delta-file storage engine. Images are not loaded entirely into RAM; instead, a lightweight descriptor keeps track of backing files while the storage layer handles reads, writes, and crash recovery.

**Types & Key Values**
- **`image_t`** *(see `src/core/storage/image.h`)* keeps the paths and handles needed by the delta-file storage layer:
	- `storage`: opaque `storage_t*` handle used for every block read/write.
	- `filename`: original path supplied by the user (used by checkpoints and UI).
	- `delta_path`: path to the delta file (`<filename>.delta`).
	- `journal_path`: path to the preimage journal (`<filename>.journal`).
	- `raw_size`: logical size in bytes (`block_count * 512`).
	- `writable`: true when caller requested write access and the host path allows it.
	- `type`: detected category (`image_fd_ds`, `image_hd`, ...).
	- `from_diskcopy`: marks DiskCopy 4.2 sources so their headers can be skipped.

**Module lifecycle**
- **`image_init(checkpoint_t *checkpoint)`** and **`image_delete(void)`** remain no-ops (no global resources).

**Opening images** — `image_open(const char *filename, bool writable)`
1. Validates access: if `writable==true` the code probes `fopen(path, "r+b")` to honor host permissions.
2. Uses `stat()` and a lightweight header read to distinguish raw images from DiskCopy 4.2 archives. DiskCopy images must have `data_size` aligned to 512 bytes.
3. Builds delta and journal paths adjacent to the image (`<filename>.delta`, `<filename>.journal`).
4. Creates a `storage_config_t` with `base_path=filename`, `delta_path`, `journal_path`, and `base_data_offset` (0 for raw, 0x54 for DiskCopy).
5. Calls `storage_new()` which opens the base file read-only, opens/creates the delta and journal, and reads existing bitmaps if the delta already exists.

No seeding step is needed — unmodified blocks are read directly from the base file.

**Volatile image persistence** — `image_persist_volatile(const char *path)`

When a disk image resides in volatile storage (`/tmp/` or `/fd/`), this function copies it to `/images/<hash>.img` (OPFS-backed, content-addressed via FNV-1a hash). This runs on the worker thread where OPFS is accessible. The `fd insert` and `hd attach` commands call this automatically before opening the image. Returns a persistent path that the caller must free.

**Reading/Writing image data**
- **`disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size)`** and **`disk_write_data(...)`** enforce 512-byte alignment and forward to `storage_read_block` / `storage_write_block` in a loop.

**Background work**
- **`image_tick_all(config_t *config)`** calls `storage_tick()` for each registered image. With the delta model, `storage_tick()` is a no-op (no consolidation needed).

**Persisting changes / Exporting**
- **`image_save(image_t *image)`** calls `storage_save_state()` to emit a dense raw image back to `image->filename`. DiskCopy sources cannot be exported back to `.dc42`.

**Creating blank floppy images**
- **`image_create_blank_floppy()`** writes a zero-filled 819,200-byte raw file that can immediately be opened.

**Checkpointing & metadata**
- **`image_checkpoint()`** writes `{uint32 len, path bytes, writable flag, raw_size}` and then calls `storage_checkpoint()`. The storage layer writes the current bitmap for quick checkpoints or streams all blocks for consolidated checkpoints.
- During restore, `system.c` re-opens the image via `image_open` to rebuild handles, then calls `storage_restore_from_checkpoint()`. Quick checkpoints set the bitmap from the checkpoint stream; consolidated checkpoints repopulate the delta from embedded data.

**Usage notes**
- Delta and journal files accumulate beside each image. Removing `<filename>.delta` and `<filename>.journal` reverts the disk to its pristine base state.
- DiskCopy images become writable overlays: mutations live in the delta file while the source archive stays untouched.

---
Last updated for the delta-file storage rewrite.
