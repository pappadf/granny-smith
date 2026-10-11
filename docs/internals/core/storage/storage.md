# Storage

This document describes the **delta-file** storage engine that backs each emulated disk. The previous directory-of-blocks design has been replaced with a simpler model optimized for OPFS, where in-place seeks and writes within a single file are fast.

## 1. Overview

* Every disk image is backed by three files: a **base** (the original image, read-only), a **delta** (all modifications), and a **journal** (preimage crash recovery).
* The delta file contains a fixed header, two bitmaps (current and committed), two cluster tables (current and committed), and a data area of slots. The first write to any block of a 32 KB cluster gives that cluster the next slot at the end of the file, so the delta grows with what the guest wrote, not with the highest block it wrote.
* Reads check a bitmap: bit set → read from delta, bit clear → read from base.
* There is no consolidation, no directory scanning, and no per-block files.

## 2. Filesystem Layout

Delta and journal files are created in the delta directory the caller
chooses — under the per-machine checkpoint directory by default, adjacent
to the disk image in the legacy layout:

```
/images/
├── a3f7c012.img              # Original disk image (read-only, immutable)
├── 8f2a41c09b3d7e15.delta    # All modifications (header + bitmaps + block data)
└── 8f2a41c09b3d7e15.journal  # Preimage journal (crash recovery, cleared on checkpoint)
```

In the browser, the web app copies uploaded and URL-fetched images into `/opfs/images/<category>/` before attaching them, so the base image is OPFS-backed and survives a reload; the core itself opens whatever path it is given. See `docs/internals/core/checkpointing.md` for details.

## 3. Delta File Format

The browser charges a file's logical length against the origin's quota, holes included, so a delta that placed block N at `N × block_size` cost the size of the disk the first time the guest wrote its last sector (HFS writes its alternate MDB there when a volume is initialised). Version 2, what every new delta is, places blocks by cluster:

```
[0 .. 63]                    Header (64 bytes, little-endian)
  [0..3]   magic: "GSDL"
  [4..7]   version: uint32_t = 2
  [8..15]  block_count: uint64_t
  [16..19] block_size: uint32_t (512 default; 532 for a Lisa ProFile)
  [20..23] cluster_blocks: uint32_t (blocks per cluster; 64 = 32 KB at 512 B)
  [24..31] cluster_count: uint64_t (= ceil(block_count / cluster_blocks))
  [32..39] slots_committed: uint64_t (slots in use at the last commit)
  [40..63] reserved

[64 .. +bm]                  Current bitmap (1 bit per block)
[.. +bm]                     Committed bitmap
[.. +4·C]                    Current cluster table (uint32_t per cluster: 0 = no slot, k = slot k−1)
[.. +4·C]                    Committed cluster table
[data_offset ..]             Slots (cluster_blocks × block_size bytes each), in allocation order
```

`data_offset` is the metadata's size rounded up to a sector. Where `bm = ceil(block_count / 8)` and `C = cluster_count`: for an 800K floppy the metadata is under 1 KB; for a 2 GB disk it is about 1.5 MB, fixed, and the data area grows by one 32 KB slot per cluster first written.

- **Read** block N: bitmap bit clear → base. Else its data is at `data_offset + ((table[N / cb] − 1) × cb + N % cb) × block_size`.
- **Write** block N: a cluster with no slot gets `slots_used + 1`; a block that is set in the committed bitmap has its preimage journaled first, read from the slot it will be replayed into (a committed cluster's slot never moves). The blocks of a slot that were never written keep their bitmap bit clear and still read from the base.

The header records `block_size`, so a delta is self-describing: reopen validates the size it was written with. `block_size` is a multiple of 4 in `[512, STORAGE_MAX_BLOCK_SIZE]` (1024). 512 covers flat disks (Mac SCSI HD, floppy data); 532 is the Lisa ProFile's block (512 data + 20 inline tag).

The current bitmap and table track what has been written; the committed copies are a snapshot at the last successful checkpoint. Both are kept in memory and flushed to the delta at checkpoint time, after the data they point at.

**Byte order.** Every multi-byte field is little-endian: the header (written field by field), the journal's LBAs, and the cluster tables (arrays of `uint32_t` stored in place, which `storage.c` asserts at build time is little-endian on the host — every target is). A bitmap is a byte stream with no byte order: block N is bit `N & 7` (least significant first) of byte `N >> 3`.

**Version 1** (still opened, never created) is a 24-byte header (magic, version 1, `block_count`, `block_size`, reserved), the two bitmaps, and a block area with block N at `24 + 2·bm + N × block_size`.

## 4. Journal Format

The journal is an append-only file of preimage entries:

```
[uint32_t LBA, little-endian][block_size bytes block data]   # 4 + block_size bytes per entry
```

The entry stride follows the instance's `block_size` (516 bytes for a 512-byte
disk, 536 for a 532-byte ProFile); the header's `block_size` lets a reopen
recompute it.

Before overwriting a committed block in the delta, the storage engine appends the old data to the journal and flushes it, so the preimage is out of the journal's stdio buffer before the overwrite can leave the delta's (write-ahead). This enables crash recovery: if the browser closes between checkpoints, the journal can be replayed to restore the delta to its last committed state. A block is journaled once per commit: an in-memory bitmap (one bit per block) records which blocks already have a preimage, so the check on each write is O(1), and a commit clears it with the journal.

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
| `base` | The original image as a read-only byte source (`source.h`), or NULL for a blank disk. A DiskCopy header, an NDIF chunk map or an archive member is a source of its own, so no offset is needed. |
| `delta_path` | Path to delta file (created if missing). |
| `journal_path` | Path to preimage journal (created if missing). |
| `block_count` | Number of logical blocks. |
| `block_size` | Bytes per block: a multiple of 4 in `[512, STORAGE_MAX_BLOCK_SIZE]` (512 default, 532 for a ProFile). |

`storage_checkpoint` needs a checkpoint stream (NULL is an error); a commit without one is `storage_clear_rollback`.

## 6. Reads & Writes

**Read:** Validate alignment, compute the LBA. If the bitmap bit is set, seek into the delta's data area and read one block (`block_size` bytes). Otherwise, read it from the base. If no base exists, or the block lies past the end of a base shorter than the geometry, return zeros. A block that is there and cannot be read — from the delta or the base — is an error (`STATUS_E_IO`, buffer zeroed), never silent zeros.

**Write:**
1. If the block is committed (bit set in committed bitmap) and not yet journaled, read the old data from the delta and append it to the journal.
2. Seek into the delta's data area and write one block (`block_size` bytes).
3. Set the bitmap bit (in memory only — flushed at checkpoint time, after the block data, so the on-disk bitmap never names a block still in the delta's stdio buffer).

Common case (no preimage needed): one seek + one write.

## 7. Checkpoint Integration

Every storage snapshot in a checkpoint starts with a 24-byte little-endian header, written field by field: `version` (u32), `has_data` (u8, 1 for consolidated), 3 reserved bytes, `block_count` (u64), `block_size` (u32), 4 reserved bytes. The quick payload's `cluster_blocks` (u32) and slot count (u64) are little-endian too.

**Quick checkpoints:** `storage_checkpoint()` writes the current bitmap to the checkpoint stream (in-memory, fast), then the delta's layout: `cluster_blocks` (0 for a v1 delta), the slots in use, and the cluster table. Then `storage_clear_rollback()` copies the current bitmap and table to committed, records the slot high-water mark, flushes the metadata to the delta, and truncates the journal. If no blocks were modified since the last checkpoint, the flush is skipped entirely (zero OPFS I/O).

**Consolidated checkpoints:** `storage_save_state()` streams every block (from delta where bitmap is set, from base otherwise) into the checkpoint. The stream reads as `storage_read_block` does: a block past the end of a short base streams as zeros, and a block the base holds and cannot read fails the stream with `STATUS_E_IO` — an export or a consolidated checkpoint never embeds zeros in its place. `image_checkpoint` then marks the checkpoint failed, so its `.tmp` is never renamed over a good one.

A restore that has no disk to load a snapshot into skips it. It checks the snapshot's geometry first, as `storage_new` would: a block size outside `[512, STORAGE_MAX_BLOCK_SIZE]` or a block count past `UINT32_MAX` is refused (`STATUS_E_INVAL`), so a crafted checkpoint cannot overrun the skip's stack buffer.

**Restore from quick checkpoint:** Roll back first (journal replay, post-commit slots truncated away), then read the bitmap and layout from the checkpoint stream, check the layout matches the delta's, set them as current and committed, truncate the journal. The rollback comes first because the emulator may have kept running after the checkpoint was saved: the blocks it overwrote since are restored from their preimages and the slots it allocated are cut away, which leaves the delta's data exactly as it was at the commit the checkpoint made. The checkpoint's bitmap and table then name that data.

**Restore from consolidated checkpoint:** `storage_load_state()` reads all blocks into the delta, sets all bitmap bits, and commits. The stream yields a block at a time, but the delta is written a run at a time (blocks whose delta positions are contiguous, up to the 4 MB streaming chunk): a seek and write per block was a filesystem call per 512 bytes, which under WasmFS/OPFS made opening a Save State with a hard disk take minutes.

## 8. Crash Recovery (Rollback)

If the browser closes between checkpoints, the delta may contain uncommitted modifications. The journal captures preimages of committed blocks that were overwritten.

`storage_apply_rollback()`:
1. Read each journal entry (LBA + `block_size`-byte preimage).
2. Write the preimage back to the delta where the block lives (its cluster's slot, which a committed cluster never changes).
3. Set current bitmap and table = committed; slots in use = `slots_committed`.
4. Truncate the delta to the end of the committed slots: the slots allocated since hold only blocks the committed bitmap does not name, so they go wholesale, with no journal needed for them.
5. Flush the metadata; truncate the journal.

This restores the delta to its last committed state. The operation is idempotent.

**When to call:** If no checkpoint will be loaded (fresh boot with existing delta), call `storage_apply_rollback()` before normal operation. If a quick checkpoint will be loaded, there is no need: `storage_restore_from_checkpoint()` rolls back itself before applying the checkpoint's bitmap.

## 9. Recovery

On startup, `storage_new()` opens the existing delta file (if present), reads the header, bitmaps and tables, and scans the journal to build the in-memory index. A v2 delta reopens at its last commit: anything past `slots_committed` is truncated away, as a rollback would, and a current table that names an uncommitted slot falls back to the committed one. No directory scanning or file enumeration is needed. If the delta doesn't exist, it is created with empty bitmaps.

## 10. Unit Tests

Unit tests live in `tests/unit/suites/storage/test.c` and exercise:
- Invalid argument handling
- Basic read/write with base image verification
- State save/load round-trip
- Delta persistence across close/reopen
- Rollback (preimage journal replay); one preimage per block per commit
- Delta v2: the last block of a 2 GiB disk costs the metadata and one slot; rollback truncates the slots allocated since the commit; a reopen without one lands at the commit; a v1 delta still opens, reads, and rolls back

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
> files.ls "/images/sys.img/System Folder/Finder/rsrc/"
CODE
MENU
vers
STR#
…
_raw

> files.cat "/images/sys.img/System Folder/Finder/rsrc/vers/1.info"
{"name":"","attrs":["purgeable"],"size":50}

> files.cp "/images/sys.img/System Folder/Finder/rsrc/" "/tmp/finder-rsrc/" recursive=true
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
`files.cp <path> <dst>` over `files.cat <path>` — the latter streams
non-printable bytes into the response stream, which is fine for small
text resources but unsafe for arbitrary code/PICT/SND bytes.

The parser lives in `src/core/storage/resource_fork.{c,h}` and exposes
a small C API (`rfork_parse`, `rfork_num_types`, `rfork_id_at`,
`rfork_lookup`, …) that downstream consumers (e.g. the `re`
orchestrator under `src/core/re/`) call directly without routing
through the VFS. Parsed maps are cached LRU-style under `image_vfs.c`
keyed on `(mount, hfs_cnid)` with capacity 8; the cache is invalidated
when the parent mount is destroyed.
