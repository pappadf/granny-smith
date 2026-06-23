# Storage

This document describes the **delta-file** storage engine that backs each emulated disk. The previous directory-of-blocks design has been replaced with a simpler model optimized for OPFS, where in-place seeks and writes within a single file are fast.

## 1. Overview

* Every disk image is backed by three files: a **base** (the original image, read-only), a **delta** (all modifications), and a **journal** (preimage crash recovery).
* The delta file contains a fixed header, two bitmaps (current and committed), and a block data area. Modified blocks are written in-place at their LBA offset.
* Reads check a bitmap: bit set → read from delta, bit clear → read from base.
* There is no consolidation, no directory scanning, and no per-block files.

## 2. Filesystem Layout

Delta and journal files are created adjacent to the disk image:

```
/images/
├── a3f7c012.img              # Original disk image (read-only, immutable)
├── a3f7c012.img.delta        # All modifications (header + bitmaps + block data)
└── a3f7c012.img.journal      # Preimage journal (crash recovery, cleared on checkpoint)
```

In the browser, images uploaded via drag-and-drop initially land in volatile `/tmp/` (memory-backed). The C-side `image_persist_volatile()` function copies them to `/images/<hash>.img` (OPFS-backed, content-addressed) before the storage engine opens them. This ensures delta and journal files are also on OPFS and survive page reloads. See `docs/core/storage/checkpointing.md` for details.

## 3. Delta File Format

```
[0 .. 23]                    Fixed header (24 bytes)
  [0..3]   magic: "GSDL"
  [4..7]   version: uint32_t = 1
  [8..15]  block_count: uint64_t
  [16..19] block_size: uint32_t (512 default; 532 for a Lisa ProFile)
  [20..23] reserved: uint32_t

[24 .. 24+bm-1]             Current bitmap (1 bit per block)
[24+bm .. 24+2*bm-1]        Committed bitmap
[24+2*bm .. EOF]             Block data area (block_count × block_size bytes, sparse)
```

The header records `block_size`, so a delta is self-describing: reopen validates
the size it was written with and a future device with a different block geometry
needs no format change. `block_size` is a multiple of 4 in `[512,
STORAGE_MAX_BLOCK_SIZE]` (1024). 512 covers flat disks (Mac SCSI HD, floppy
data); 532 is the Lisa ProFile's block (512 data + 20 inline tag).

Where `bm = ceil(block_count / 8)`. For an 800K floppy: bm = 200 bytes. For a 40MB HD: bm ≈ 10 KB.

The current bitmap tracks which blocks have been modified. The committed bitmap is a snapshot at the last successful checkpoint. Both are kept in memory and flushed to the delta header at checkpoint time.

## 4. Journal Format

The journal is an append-only file of preimage entries:

```
[uint32_t LBA][block_size bytes block data]   # 4 + block_size bytes per entry
```

The entry stride follows the instance's `block_size` (516 bytes for a 512-byte
disk, 536 for a 532-byte ProFile); the header's `block_size` lets a reopen
recompute it.

Before overwriting a committed block in the delta, the storage engine appends the old data to the journal. This enables crash recovery: if the browser closes between checkpoints, the journal can be replayed to restore the delta to its last committed state.

## 5. API Summary

```c
int storage_new(const storage_config_t*, storage_t**);
int storage_delete(storage_t*);
int storage_read_block(storage_t*, size_t byte_offset, void* out_block);  // block_size bytes
int storage_write_block(storage_t*, size_t byte_offset, const void* in_block);
int storage_tick(storage_t*);          // no-op
int storage_checkpoint(storage_t*, checkpoint_t*);
int storage_restore_from_checkpoint(storage_t*, checkpoint_t*);
int storage_apply_rollback(storage_t*);
int storage_clear_rollback(storage_t*);
int storage_save_state(storage_t*, void* ctx, storage_write_callback_t cb);
int storage_load_state(storage_t*, void* ctx, storage_read_callback_t cb);
```

`storage_config_t` fields:

| Field | Meaning |
| ----- | ------- |
| `base_path` | Path to original image file (read-only). |
| `delta_path` | Path to delta file (created if missing). |
| `journal_path` | Path to preimage journal (created if missing). |
| `block_count` | Number of logical blocks. |
| `block_size` | Bytes per block: a multiple of 4 in `[512, STORAGE_MAX_BLOCK_SIZE]` (512 default, 532 for a ProFile). |
| `base_data_offset` | Byte offset to data in base file (e.g. DiskCopy header skip). |

## 6. Reads & Writes

**Read:** Validate alignment, compute the LBA. If the bitmap bit is set, seek into the delta's data area and read one block (`block_size` bytes). Otherwise, seek into the base file and read. If no base file exists, return zeros.

**Write:**
1. If the block is committed (bit set in committed bitmap) and not yet journaled, read the old data from the delta and append it to the journal.
2. Seek into the delta's data area and write one block (`block_size` bytes).
3. Set the bitmap bit (in memory only — flushed at checkpoint time).

Common case (no preimage needed): one seek + one write.

## 7. Checkpoint Integration

**Quick checkpoints:** `storage_checkpoint()` writes the current bitmap to the checkpoint stream (in-memory, fast). Then `storage_clear_rollback()` copies the current bitmap to committed, flushes both bitmaps to the delta header, and truncates the journal. If no blocks were modified since the last checkpoint, the flush is skipped entirely (zero OPFS I/O).

**Consolidated checkpoints:** `storage_save_state()` streams every block (from delta where bitmap is set, from base otherwise) into the checkpoint.

**Restore from quick checkpoint:** Read the bitmap from the checkpoint stream, set it as current and committed, truncate the journal. The delta's block data is already correct (OPFS auto-persisted every write).

**Restore from consolidated checkpoint:** `storage_load_state()` reads all blocks into the delta, sets all bitmap bits, and commits.

## 8. Crash Recovery (Rollback)

If the browser closes between checkpoints, the delta may contain uncommitted modifications. The journal captures preimages of committed blocks that were overwritten.

`storage_apply_rollback()`:
1. Read each journal entry (LBA + `block_size`-byte preimage).
2. Write the preimage back to the delta at the corresponding offset.
3. Set current bitmap = committed bitmap.
4. Flush bitmaps to delta header.
5. Truncate journal.

This restores the delta to its last committed state. The operation is idempotent.

**When to call:** If no checkpoint will be loaded (fresh boot with existing delta), call `storage_apply_rollback()` before normal operation. If a checkpoint will be loaded, skip rollback — the checkpoint's bitmap is authoritative and the delta data is correct.

## 9. Recovery

On startup, `storage_new()` opens the existing delta file (if present), reads the header and bitmaps, and scans the journal to build the in-memory index. No directory scanning or file enumeration is needed. If the delta doesn't exist, it is created with empty bitmaps.

## 10. Unit Tests

Unit tests live in `tests/unit/suites/storage/test.c` and exercise:
- Invalid argument handling
- Basic read/write with base image verification
- State save/load round-trip
- Delta persistence across close/reopen
- Rollback (preimage journal replay)

## 11. Resource forks as VFS paths

The image-VFS backend (`src/core/vfs/image_vfs.c`) exposes the two-fork
nature of classic-Mac HFS files in the path itself. For any HFS file
`<file>` with a non-empty resource fork, the following synthetic paths
resolve:

| Path | Kind | Bytes |
|---|---|---|
| `<file>` | file | data fork |
| `<file>/finf` | file (32 B) | Finder info blob (16 B FInfo + 16 B FXInfo) |
| `<file>/rsrc` | directory | enumerates resource types and the `_raw` escape hatch |
| `<file>/rsrc/_raw` | file | raw fork bytes (the entire resource fork) |
| `<file>/rsrc/<TYPE>` | directory | enumerates resource IDs under a type |
| `<file>/rsrc/<TYPE>/<id>` | file | the resource's bytes (verbatim, no length prefix) |
| `<file>/rsrc/<TYPE>/<id>.info` | file | sidecar JSON: `{name, attrs[], size}` |

`<TYPE>` is the four-byte resource type, MacRoman-transcoded to UTF-8 (so
`CODE`, `vers`, `STR ` with trailing space, etc.). `<id>` is the signed
int16 resource ID as base-10, including a leading `-` for negative values
(common for system-reserved resources, e.g. `DRVR/-16` is `.Sony`).

Example shell session:

```
> vfs.ls "/images/sys.img/System Folder/Finder/rsrc/"
CODE
MENU
vers
STR#
…
_raw

> vfs.cat "/images/sys.img/System Folder/Finder/rsrc/vers/1.info"
{"name":"","attrs":["purgeable"],"size":50}

> storage.cp -r "/images/sys.img/System Folder/Finder/rsrc/" "/tmp/finder-rsrc/"
```

Eligibility rules:

- **Files only.** HFS folders never gain a synthetic `/rsrc/` subtree.
- **Non-empty fork only.** Files with `rsrc_fork.logical_size == 0`
  resolve `<file>/rsrc` and any deeper path as ENOENT.
- **HFS-backed only.** UFS partitions, host paths, and ISO 9660 images
  have no resource forks; `<path>/rsrc/...` cleanly returns ENOENT on
  those.
- **Real HFS filenames named `rsrc` or `finf`.** Disambiguated by a
  retry path in `image_vfs.c`: the synthetic interpretation is tried
  first; on miss the full literal component list is retried.

For binary data over the headless TCP shell or JS bridge, prefer
`storage.cp <path> <dst>` over `vfs.cat <path>` — the latter streams
non-printable bytes into the response stream, which is fine for small
text resources but unsafe for arbitrary code/PICT/SND bytes.

The parser lives in `src/core/storage/resource_fork.{c,h}` and exposes
a small C API (`rfork_parse`, `rfork_num_types`, `rfork_id_at`,
`rfork_lookup`, …) that downstream consumers (e.g. the `re`
orchestrator under `src/core/re/`) call directly without routing
through the VFS. Parsed maps are cached LRU-style under `image_vfs.c`
keyed on `(mount, hfs_cnid)` with capacity 8; the cache is invalidated
when the parent mount is destroyed.
