# Storage

This document describes the **directory-of-blocks** storage engine that backs each emulated disk. The previous multi-layer RAW/Delta/AOL design has been replaced with a simpler model that mirrors the ideas captured in `new-ideas/new-storage.md`.

## 1. Overview

* Every disk image is represented by **one directory** of files.
* Each file contains one or more contiguous 512-byte blocks. The filename encodes the logical block range it covers.
* Writes are persisted immediately: the engine writes a temporary file, renames it into place, and updates an in-memory filename index.
* There is no in-memory cache of block contents, no append-only log, and no replay step during startup.

This favors environments such as IDBFS/OPFS where creating many small files is cheap, while large file appends are expensive.

## 2. Filesystem Layout

By default, storage directories are created adjacent to the disk image:

```
/path/to/disk.img
/path/to/disk.img.blocks/
    meta.json
    0000000X.dat
    00000002.dat
    ...
```

In the browser environment, the web frontend ensures disk images are stored under persistent paths before the C core opens them. Images uploaded via drag-and-drop or test injection are copied from volatile `/tmp/` to `/persist/images/<hash>.img` (content-addressed), so the storage directory becomes `/persist/images/<hash>.img.blocks/` — which is backed by IDBFS and survives page reloads. See `docs/checkpointing.md` for details.

### 2.1. Storage Cache (GS_STORAGE_CACHE)

The `GS_STORAGE_CACHE` environment variable redirects all `.blocks/` directories to a separate location. This keeps source data directories clean—particularly useful for integration tests and CI environments where generated files should not intermingle with committed assets.

When set, the disk path is resolved to an absolute path (via `realpath`), and storage directories are created under the cache root:

```
# Without GS_STORAGE_CACHE:
tests/data/systems/System_6_0_8.dsk.blocks/

# With GS_STORAGE_CACHE=/tmp/cache and disk at /home/user/project/tests/data/systems/System_6_0_8.dsk:
/tmp/cache/home/user/project/tests/data/systems/System_6_0_8.dsk.blocks/
```

The full absolute path ensures uniqueness when multiple projects use the same cache root.

**Usage examples:**

```bash
# Shell
export GS_STORAGE_CACHE=/tmp/gs-cache
./gs-headless rom=... fd=...

# Makefile
GS_STORAGE_CACHE=$(BUILD)/storage-cache $(HEADLESS) rom=... fd=...
```

If the environment variable is unset or empty, the legacy behavior (adjacent `.blocks/` directory) is used.

### 2.2. Directory Contents

`meta.json` records `block_count` and `block_size` (currently fixed at 512). Every other file ends in `.dat` and represents block data.

### 2.3. Patterns & Levels

Each `.dat` file name is an 8-character hexadecimal pattern followed by `.dat`. Trailing characters may be literal `X`s:

| Name        | Level | Blocks Covered              |
| ----------- | :---: | --------------------------- |
| `00000002`  |   0   | Block `0x00000002`          |
| `0000000X`  |   1   | Blocks `0x0`–`0xF`          |
| `000000XX`  |   2   | Blocks `0x0`–`0xFF`         |
| `00000XXX`  |   3   | Blocks `0x0`–`0xFFF`        |

Level `L` covers `16^L` blocks. The filename encodes the base LBA; trailing `X`s replace the lower `4*L` bits. The highest level used for a disk is the largest `L` where `16^L <= block_count` (up to 8 because LBAs are 32-bit).

### 2.4. Precedence

When reading block `B`, the engine probes files from level 0 upward. The first matching file wins:

1. `0000000B.dat`
2. `000000BX.dat`
3. `000000XX.dat`
4. ... up to `max_level`

If no file covers the block, `storage_read_block()` returns 512 zero bytes.

## 3. In-Memory Index

`storage_new()` scans the directory once and builds per-level sorted vectors of `range_entry { base_lba, level }`. No block data is cached—only filenames and their coverage ranges. The index can be rebuilt at any time by rescanning the directory.

## 4. API Summary

```
int storage_new(const storage_config_t*, storage_t**);
int storage_delete(storage_t*);
int storage_read_block(storage_t*, size_t byte_offset, void* out512);
int storage_write_block(storage_t*, size_t byte_offset, const void* in512);
int storage_tick(storage_t*);
int storage_checkpoint(storage_t*);
int storage_restore_from_checkpoint(storage_t*, checkpoint_t*);
int storage_apply_rollback(storage_t*);
int storage_clear_rollback(storage_t*);
int storage_save_state(storage_t*, void* ctx, storage_write_callback_t cb);
int storage_load_state(storage_t*, void* ctx, storage_read_callback_t cb);
```

`storage_config_t` now only needs:

| Field | Meaning |
| ----- | ------- |
| `path_dir` | Directory that owns all `.dat` files. |
| `block_count` | Number of exposed 512-byte blocks. |
| `block_size` | Must be 512 (reserved for future flexibility). |
| `consolidations_per_tick` | Max range merges performed per `storage_tick()`. Set `<= 0` to disable automatic merges. |

## 5. Reads & Writes

**Read:** Validate alignment, compute the LBA, then walk levels 0→`max_level`. When a matching entry exists, open the corresponding file, seek to the block offset, and `fread` 512 bytes. Missing blocks return zeros.

**Write:** Align + bounds check, compute the level-0 filename, write 512 bytes to `<file>.tmp`, close, then `rename()` it to `<file>.dat`. The level-0 entry is inserted into the in-memory index if it did not exist previously. Because the rename is atomic, the block is durable immediately.

`storage_checkpoint()` now routes through the checkpoint stream so loaders can reconstruct `.blocks` directories when consolidated checkpoints are requested; for quick checkpoints only a geometry header is emitted.

## 6. Consolidation

Having one file per block is simple but produces many directory entries. `storage_tick()` incrementally collapses groups of sixteen siblings:

```
00000000.dat .. 0000000F.dat  =>  0000000X.dat
0000000X.dat .. 000000FX.dat  =>  000000XX.dat
```

Conditions for a merge:

1. Sixteen consecutive entries exist at level `L`.
2. Their bases are aligned to the parent span (`16^(L+1)`) and ordered.
3. No parent file currently exists (or it can be replaced).

The merge process streams the child files into a temporary parent file, renames it, inserts the `(base, L+1)` entry, deletes the children, and removes them from the index. A crash during the merge is safe because either the parent does not appear or the more specific children continue to win.

## 7. Full-State Save/Load

* `storage_save_state()` iterates every LBA from 0 to `block_count-1`, reading each block (falling back to zeros) and streaming it to the provided callback.
* `storage_load_state()` deletes all existing `.dat` files, then writes new range files by streaming data from the callback, choosing the largest aligned level for each contiguous section. This is used both for importing a raw disk image and for restoring emulator save states.

## 8. Checkpoint Snapshots

`storage_checkpoint()` produces an inline snapshot that lives inside the emulator’s checkpoint stream. The payload begins with a compact header (`version`, `block_count`, `block_size`, `has_data`). The header’s `has_data` bit is derived from the checkpoint kind:

- **Quick checkpoints** (default) set `has_data = 0`. Only the header is written, so restores merely validate that the disk geometry matches what is already on disk. No block data is transmitted, keeping snapshots tiny.
- **Consolidated checkpoints** set `has_data = 1`, and the storage layer streams every block (via `storage_save_state`) directly into the checkpoint. This allows save files to fully describe each disk even when the user later moves the `_state` file to a different machine.

`storage_restore_from_checkpoint()` reads the header, verifies geometry, and either skips the payload (`has_data = 0`) or rebuilds the `.blocks` directory (`has_data = 1`) by forwarding the embedded blocks to `storage_load_state()`. `system.c` invokes this immediately after reopening each image so disk overlays are recreated before other devices resume.

## 9. Rollback Overlay

Quick checkpoints intentionally omit disk payloads: only geometry is serialized, so the on-disk `.blocks/` tree must already reflect the checkpointed CPU/RAM state. To make that safe even if the browser closes the tab before the next checkpoint, each storage directory maintains a **rollback overlay**:

* The overlay lives under `<image>.blocks/rollback/` and stores one 512-byte *preimage* file (`0000000N.pre`) for every logical block that changed after the most recent checkpoint commit.
* When `storage_write_block()` sees the first write to LBA `N` during the current checkpoint epoch, it snapshots the previous contents into the overlay (using the same temp+rename strategy) before completing the write. Subsequent writes to the same block reuse the existing preimage.
* After any checkpoint succeeds—quick *or* consolidated—`storage_clear_rollback()` removes every `.pre` file and treats the current contents as the new baseline.
* If the previous session ended abruptly, `storage_apply_rollback()` replays the overlay by streaming each `.pre` file back into the main `.dat` tree before device state is restored. `storage_restore_from_checkpoint()` invokes this automatically whenever the checkpoint stream omits disk data (`has_data = 0`).

Because every preimage is self-contained, persisting the overlay aligns with IDBFS/OPFS constraints (many small files are cheap, while appending to a single log is expensive). The apply step is idempotent—re-running it after a crash simply rewrites identical data and deletes the processed overlay files.

## 10. Recovery

Because every write is a complete file, recovery is trivial:

1. On startup, scan the directory and rebuild the range index. Ignore malformed filenames or files whose coverage exceeds `block_count`.
2. Optionally validate that file sizes match `16^level * 512`.
3. Resume normal operation. Consolidation will eventually remove redundant files if the last shutdown occurred mid-merge.

## 11. Migration Notes

* Existing `.lacs/delta`/`aol` hierarchies are no longer used. New images are stored in `<image>.blocks/`.
* Importing from a flat raw image is performed by calling `storage_load_state()` exactly once after `storage_new()` returns. The image module does this automatically when it detects that a storage directory has no `.dat` files yet.
* Unit tests live in `tests/unit/suites/storage/test.c` and exercise reads, writes, consolidation, and full-state save/load.