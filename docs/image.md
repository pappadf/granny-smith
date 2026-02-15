## Image Module

The image module manages floppy and hard-disk containers while delegating all block I/O, caching, and persistence to the `storage.*` engine. Images are no longer loaded entirely into RAM; instead, a light-weight descriptor keeps track of backing files and storage directories while the storage layer handles compression, append-only logs, and background checkpoints.

**Types & Key Values**
- **`image_t`** *(see `src/image.h`)* keeps only the paths and handles needed by the new directory-of-blocks storage layer:
	- `storage`: opaque `storage_t*` handle used for every block read/write.
	- `filename`: original path supplied by the user (used by checkpoints and UI).
	- `storage_root`: directory that owns the `.dat` range files (`<filename>.blocks/`).
	- `raw_size`: logical size in bytes (`block_count * 512`).
	- `writable`: true when caller requested write access and the host path allows it.
	- `type`: detected category (`image_fd_ds`, `image_hd`, ...).
	- `from_diskcopy`: marks DiskCopy 4.2 sources so their headers can be skipped during import.

**Module lifecycle**
- **`image_init(checkpoint_t *checkpoint)`** and **`image_delete(void)`** remain no-ops (no global resources yet).

**Opening images** — `image_open(const char *filename, bool writable)`
1. Validates access: if `writable==true` the code probes `fopen(path, "r+b")` to honor host permissions.
2. Uses `stat()` and a lightweight header read to distinguish raw images from DiskCopy 4.2 archives. DiskCopy images must have `data_size` aligned to 512 bytes and the header must fit within the file size.
3. Builds a per-image storage root located next to the source file (`<filename>.blocks`).
4. Creates the new storage directory if needed and inspects it for existing `.dat` files. On first launch the directory is empty, so the module streams the base image (raw or DiskCopy after its header) through `storage_load_state()` to seed the directory-of-blocks representation.
5. Initializes `storage_t` with the simple `storage_config_t { path_dir, block_count, block_size=512 }` describing the directory.

Every disk access after this point goes through the storage engine; no module keeps long-lived raw buffers in memory.

**Reading/Writing image data**
- **`disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size)`** and **`disk_write_data(...)`** now enforce 512-byte alignment and forward to `storage_read_block` / `storage_write_block` in a loop. The helpers assert that `offset + size <= disk_size(image)` to guard callers.

**Background work**
- **`image_tick_all(config_t *config)`** runs once per VBL (invoked from `trigger_vbl`). It iterates registered images and calls `storage_tick()` so the storage layer can perform bounded consolidation passes (merging 16 sibling blocks into higher-level files).

**Persisting changes / Exporting**
- **`image_save(image_t *image)`** calls `storage_save_state()` to emit a dense raw image back to `image->filename`. DiskCopy sources cannot currently be exported back to `.dc42`; attempting to save one returns `-1` and logs a warning. Mutations continue to live inside `<filename>.blocks/`.

**Creating blank floppy images**
- **`image_create_blank_floppy()`** is unchanged: it writes a zero-filled 819 200-byte raw file that can immediately be opened (which in turn sets up an empty `.blocks` overlay).

**Integration helpers**
- **`add_image(struct config *sim, image_t *image)`** still maintains `config->images`.
- Shell commands remain commented out, but the stubs (`cmd_images`, `cmd_save`) persist for future CLI wiring.

**Checkpointing & metadata**
- **`image_get_filename()`** returns the original path (matching what checkpoints serialize).
- **`image_checkpoint()`** writes `{uint32 len, path bytes, writable flag}` and then calls `storage_checkpoint()`. The storage layer emits a compact header for quick checkpoints (`has_data = 0`) or streams an entire directory-of-blocks snapshot when the caller requested a consolidated checkpoint (`has_data = 1`).
- During restore, `system.c` re-opens the image (`image_open`) to rebuild handles, then immediately calls `storage_restore_from_checkpoint()`. Quick checkpoints simply validate metadata and keep using the `.blocks` directory on disk; consolidated checkpoints repopulate the directory from the embedded snapshot.

**Usage notes**
- Storage directories accumulate beside each image to cache dirty data efficiently. Removing `<filename>.blocks` reverts the disk to its pristine state (base raw + original file).
- DiskCopy images become writable overlays: mutations live in the `.blocks` directory even though the source archive stays untouched.
- Because `image_save()` only exports raw images, use host-side utilities to repackage DiskCopy files if needed.

---
Last updated for the storage-backed rewrite (`src/image.c`).

