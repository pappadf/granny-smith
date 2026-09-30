## Image Module

The image module manages floppy and hard-disk containers while delegating all block I/O and persistence to the delta-file storage engine. Images are not loaded entirely into RAM; instead, a lightweight descriptor keeps track of backing files while the storage layer handles reads, writes, and crash recovery.

The image subsystem speaks **paths only**. It does not know about machine ids, slots, drives, or `/opfs/checkpoints/`; the higher layer (`config_t` in `system.c`) decides where to place per-image state and remembers the result across boots.

**Types & Key Values**
- **`image_t`** *(see `src/core/storage/image.h`)* keeps the paths and handles needed by the delta-file storage layer:
	- `storage`: opaque `storage_t*` handle used for every block read/write.
	- `filename`: original path supplied by the user (the immutable base image); it may run through an image or archive.
	- `source_key`: key of the byte source that path opened ([source.md](source.md)).
	- `format`: wrapper layers peeled to reach the disk, outermost first (`raw`, `dc42`, `bin+ndif`, ...); reported as `files.images[n].format`.
	- `instance_path`: stem `<dir>/<id>` for the per-instance delta+journal pair, where `<id>` is a 16-hex-char opaque id minted by the image layer. `NULL` for read-only mounts.
	- `delta_path`: `<instance_path>.delta`.
	- `journal_path`: `<instance_path>.journal`.
	- `raw_size`: logical size in bytes (`block_count * block_size`).
	- `block_size`: bytes per logical block — 512 for flat disks (the default openers), 532 for a Lisa ProFile (512 data + 20 inline tag).
	- `writable`: true when the caller asked for write access.
	- `ghost_instance`: true when delta+journal live in a process-local scratch dir (read-only mounts); they are deleted on `image_close`.
	- `type`: detected category (`image_fd_ds`, `image_hd`, ...).
	- `from_diskcopy`: marks DiskCopy 4.2 sources so their headers can be skipped.
	- `wrap_prefix` / `wrap_blocks` / `wrap_base` / `wrap_storage_size`: the volume wrapper's synthesised partition map + driver, served in front of an HFS volume when a bare volume or a driverless partitioned disk is attached as a SCSI hard disk ([bare-volume-wrapper.md](bare-volume-wrapper.md)). The volume starts `wrap_base` bytes into `storage` (0 for a bare volume, the `Apple_HFS` partition's start otherwise); `raw_size` is the prefix plus the volume, and `wrap_storage_size` the storage's own size.

**Module lifecycle**
- **`image_init(checkpoint_t *checkpoint)`** and **`image_delete(void)`** remain no-ops (no global resources).

**Opening images** — three typed entry points

The single old `image_open(filename, writable)` is replaced by three explicit operations matching the three real use cases:

- **`image_open_readonly(const char *base_path)`** — opens a base image with no on-disk delta. The image layer mints a scratch instance under `/tmp/gs-image-ro/`; the delta and journal there are deleted on `image_close`. Use this for probes (`fd validate`, `cdrom validate`, `image partmap`, …) and for read-only mounts (CD-ROM).
- **`image_create(const char *base_path, const char *delta_dir)`** — opens a fresh writable instance. The image layer mints a 16-hex-char opaque id and creates `<delta_dir>/<id>.delta` and `<delta_dir>/<id>.journal`. If `delta_dir` is `NULL`, the directory of the base image is used (legacy adjacent-to-base layout — used by tests with no machine identity).
- **`image_open(const char *base_path, const char *instance_path)`** — reopens an existing writable instance. `instance_path` is the stem returned by `image_path()` when the instance was first created; the image layer appends `.delta` and `.journal` itself. Used by checkpoint restore.

After construction the caller queries the instance stem with **`image_path(const image_t *image)`** and persists it (in a checkpoint or in the higher-layer slot table) so a future `image_open(base, instance_path)` can find the same delta files. Returns `NULL` for read-only mounts.

**Non-512 block geometries** — each of the three openers has a `*_with_geometry(..., image_geometry_t geom)` variant that interprets the base as `geom.block_size`-byte blocks (0 ⇒ 512); the default openers are thin wrappers passing `{ .block_size = 512 }`, so existing call sites are unchanged. The Lisa ProFile opens with `{ .block_size = 532 }`. For a disk with **no source file**, **`image_create_blank(uint64_t block_count, image_geometry_t geom)`** makes a writable, all-zero image (no base; unwritten blocks read as zeros) whose scratch delta+journal are removed on `image_close` — used for a blank ProFile, exportable via `image_export_to()`.

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
    // share lifetime with state.checkpoint (see docs/internals/core/checkpointing.md).
    return checkpoint_machine_dir();
}
```

`fd insert`, `fd create`, and `hd attach` all funnel through this helper.

**Image formats** — the format registry

Every opener names a path, and the path may run through an image or an archive (`outer.img/partition1/inner.img`, `disks.zip/System.dsk.gz`): `image_open_path()` opens the path's data fork and resource fork as byte sources through the installed path opener (the VFS — see [source.md](source.md) §3.3), and `image_open_source()` does the rest. The format registry's wrapper loop (`gs_format_unwrap()`, [source.md](source.md) §3.4) peels every encoding layer — UDIF, NDIF, DiskCopy 4.2, MacBinary, BinHex, gzip, in any nesting — and the innermost source is the storage engine's base. Nothing is decoded to a file: a DiskCopy 4.2 payload is a view past the 0x54-byte header, and an NDIF or UDIF image is a chunk-mapped source (`image_chunkmap.c`) whose compressed chunks decode on first touch into the chunk cache. `image->format` records the layers peeled (`"raw"`, `"dc42"`, `"bin+ndif"`, …); `image->filename` is the path the caller named, which is what a checkpoint persists and a restore opens again. A new wrapper format is a new registry row.

| | NDIF (Disk Copy 6.x) | UDIF (`.dmg`) |
|---|---|---|
| Block map | `'bcem'` resource, 12-byte descriptors | `'mish'` tables, 40-byte entries |
| Where it lives | the **resource fork** — needs an AppleDouble sidecar or a containing HFS volume | a **plist inside the file**, found via the 512-byte `'koly'` trailer at EOF |
| Compressors | zero-fill, raw, ADC | zero-fill, ignored, raw, ADC, zlib |
| Parser | `image_ndif.c` | `image_udif.c` |
| Source | `ndif_source_open()` | `udif_source_open()` |

UDIF specifics worth knowing before touching `image_udif.c`:

- **Everything is big-endian**, including on the PowerPC-era images this mostly exists to read.
- **Chunk sectors are relative to their table's `base_sector`.** Absolute position is `table.base_sector + chunk.sector`; one block table per partition, each restarting at zero. This is the classic way to misread the format, and `tests/integration/image-udif/` exists to catch it.
- **`SectorCount` lives at trailer offset 0x1EC**, not where a naive walk of the published struct puts it — the 128-byte checksum blobs shift several fields. Static assertions in `image_udif.c` keep every offset inside the 512-byte trailer.
- **The u32 at `mish` offset 0x24 is the blkx resource ID, not a descriptor count.** The count is at 0xC8.
- **Each block table carries a CRC-32 over its decoded bytes**, and chunks of type `UDIF_CHUNK_IGNORE` are excluded from it. The chunk-mapped source decodes chunk by chunk as the guest reads, so the per-table checksum is not verified; every run is validated against the map when the source opens (inside the image, inside the data fork, a supported codec, under 64 MiB compressed) instead.
- Encrypted (`encrcdsa`), multi-segment (`.dmgpart`), bzip2, LZFSE and LZMA images are **rejected explicitly** rather than partially decoded.

The zlib decompressor both this and the PNG reader use is first-party: `inflate.c`'s entry points wrap peeler's resumable inflate, the one in the tree; the core links no third-party C libraries.

**Where images live** — the image layer opens the path it is given. It does not copy volatile media into persistent storage; the web app does that before attaching (see `docs/guide/web.md`, "Filesystem").

**Reading/Writing image data**
- **`disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size)`** and **`disk_write_data(...)`** enforce `disk->block_size` alignment (512 for flat disks, 532 for a ProFile) and forward to `storage_read_block` / `storage_write_block` in a loop.

**Background work**
- **`image_tick_all(config_t *config)`** calls `storage_tick()` for each registered image. With the delta model, `storage_tick()` is a no-op (no consolidation needed).

**Persisting changes / Exporting**
- **`image_export_to(image_t *image, const char *dest_path)`** calls `storage_save_state()` to write a dense raw copy (base + delta) to a new file. The base image is never written in place.

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
Last updated for the per-machine checkpoint-isolation rewrite.
