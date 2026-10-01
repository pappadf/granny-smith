# Byte sources, the chunk cache and the format registry

Owning files: `src/core/storage/source.{h,c}`, `src/core/storage/source_cache.c`,
`src/core/storage/chunk_cache.{h,c}`, `src/core/storage/format_registry.{h,c}`,
`src/core/storage/image_chunkmap.{h,c}`, and the type's definition in
`src/peeler/include/peeler.h`.

## 1. Responsibilities & design

Everything the emulator reads that is not guest RAM — a disk image on the
host, the payload of a DiskCopy 4.2 file, the decoded disk inside an NDIF or
UDIF image, a file on an HFS volume, a member of a zip — is a **byte source**:
`read(offset, len)` plus a size, a stable identity (the *key*) and a cost
*tier*. Formats are adapters between sources and *namespaces*
([../vfs/namespace.md](../vfs/namespace.md)), and nesting is alternating the
two:

```
host file roms.zip                              source (random)
  └─ zip namespace                              list members
       └─ open("System 7.sit")                  source (random: stored, or stream: deflated)
            └─ sit namespace                    list entries
                 └─ open("Disk Tools.img")      source (whole: decoded on first read)
                      └─ DC42 view              source
                           └─ disk namespace    partition1
                                └─ open("System") source
```

The storage engine ([storage.md](storage.md)) reads its *base* through a
source, so any read-only source is a writable disk: the delta takes the
writes, whatever the base is.

Peeler (`src/peeler/`) is the lowest layer and must build on its own, so it
defines the type (`peel_source_t`); the core adopts it unchanged
(`typedef peel_source_t gs_source_t`). An archive member peeler opens is
therefore a source the storage engine can mount with no adapter.

## 2. Key types & files

| Type / function | File | Purpose |
|---|---|---|
| `gs_source_t`, `gs_source_ops_t` | `source.h` (= `peel_source_t`) | read / size / key / tier / close; reference counted |
| `gs_tier_t` | `source.h` | `RANDOM`, `INDEXED`, `EARNED`, `STREAM`, `WHOLE` |
| `gs_source_host` | `source.c` | a host file, `pread`, thread-safe |
| `gs_source_view`, `gs_source_memory` | `source.c` | offset window; bytes in memory |
| `gs_source_open_path` | `source.c` | open a path's data / resource fork / Finder info through the installed opener |
| `gs_source_decode_through` | `source_cache.c` | cache-backed wrapper for forward-only or expensive sources |
| `gs_source_locked` | `source_cache.c` | serialise reads of a source that is not thread-safe |
| `gs_scratch_sink` | `source_cache.c` | where peeler's compressed forks decode to |
| `gs_chunk_cache_t` | `chunk_cache.c` | LRU of decoded chunks with coalescing and spill |
| `gs_format_t`, `gs_format_unwrap` | `format_registry.c` | the one table of formats |
| `ndif_source_open`, `udif_source_open` | `image_chunkmap.c` | NDIF / UDIF as chunk-mapped sources |

## 3. Behaviour/algorithms

### 3.1 Identity

A key names the bytes a source serves, so caches and the writable-image
check can compare sources that were reached by different routes.

- A host file: its `realpath` plus size and mtime (`/m/disk.img@1474560:1727712000`).
  A changed file is a different key, so nothing cached from the old one is served.
  In the browser, WasmFS gives a file in OPFS the time it was loaded as its
  mtime, so across a reload only the path and size are identity:
  `gs_key_same_source` compares keys that way there and exactly natively.
- A view: its parent's key and the range, or a key the adapter chooses
  (a DiskCopy payload is `<key>#dc42`, a decoded NDIF `<key>#ndif`).
- A member of a namespace: the parent's key and the member path
  (`<key>/partition1/Disk.img`, `<key>/System 7.sit/rsrc`).

`image_key_is_open_writable(key)` answers *true* for a key equal to, or
inside, the key of an image attached writable, which is what keeps the VFS
from serving a stale base while the guest writes to its delta.

### 3.2 Tiers

The tier is advisory but honest; callers make policy from it. A view has its
parent's tier. A stored archive member is `RANDOM`; an NDIF/UDIF chunk map
and a BGZF gzip are `INDEXED`; a deflated zip member, a plain gzip and a
compressed StuffIt or Compact Pro fork are `EARNED` -- decoded a buffer at a
time as reads reach them, into a sink that keeps what was decoded -- and
`RANDOM` once the sink holds all of it; a BinHex fork is `WHOLE` (decoded in
full on first read: its files are small). A decode-through wrapper is
`EARNED`. `vfs_is_expandable` probes only what is cheap to read now
(`INDEXED` or better), so listing a directory never decodes a member.

### 3.3 Path opening

The storage engine and the ROM loader open what the user named through
`gs_source_open_path(path, fork)`. The VFS installs itself as the opener at
start-up, so a path may continue through an image or an archive
(`outer.img/partition1/inner.img`, `roms.zip/Plus.rom`). With no opener
installed — a unit test — a path is a host file, and its resource fork and
Finder info come from an AppleDouble companion (`._NAME`, or the legacy
`%NAME`) or a raw `NAME.rsrc`.

### 3.4 The format registry

One table replaces the image opener's, the VFS mount probe's and SCSI media
validation's probe orders. Wrappers (source → source) are built in and tried
first, in a loop, so any nesting unwraps: UDIF (by its trailer), NDIF (by the
resource fork's `bcem` map), DiskCopy 4.2 (by its header), and peeler's
BinHex, MacBinary and gzip. Namespace formats (a disk, an archive) register
at start-up from the VFS. Detection reads a bounded probe: 64 KiB of head,
64 KiB of tail and, for NDIF, the resource fork.

`gs_format_open_namespace` unwraps, detects a namespace format and opens it;
when the payload of a peeler wrapper is no tree (an application in a `.bin`),
the wrapper itself is shown as a one-file namespace.

### 3.5 NDIF and UDIF

Both formats describe the decoded disk as runs of sectors: zero-fill, a raw
copy of a data-fork range, or a compressed range (ADC; zlib for UDIF). The
chunk-mapped source validates every run when it opens (inside the image,
inside the data fork, a codec we implement, under 64 MiB), then serves a
read run by run: zeros, a direct read, or a decode on first touch into the
chunk cache. Nothing is written to a scratch file and the decoded disk never
exists whole. The UDIF per-table CRC-32 is not checked (it covers a whole
table, and tables are decoded piecemeal); zlib chunks carry their own
integrity through the deflate stream's structure.

### 3.6 The chunk cache

A bounded LRU (64 MiB by default, `GS_CHUNK_CACHE_MB`) of variable-sized
chunks keyed by (source key, chunk index). When two threads ask for the same
absent chunk — the guest reading a disk while the I/O worker exports it —
one fetches and the other waits for it. With a spill directory
(`image_scratch_dir()/chunks`), an evicted chunk is appended to a per-key
spill file rather than dropped; the spill budget is `GS_CHUNK_SPILL_MB`
(512 MiB on WASM, unbounded natively). Both budgets are settings at run time
(`files.cache.memory_mb`, `files.cache.spill_mb`; §4): a smaller memory
budget evicts at once, and a spill area over its new budget is emptied --
spilled chunks are only ever a faster way to fetch them again.

### 3.7 Decode-through and sinks

`gs_source_decode_through(src)` caches another source in 128 KiB chunks.
Reading ahead of a forward-only source's cursor stores every chunk passed on
the way, so a backward read later is a hit rather than a restart.

Peeler never allocates scratch space for a compressed fork itself: it fills
the sink the caller supplies. The core's sink keeps a fork of up to 8 MiB in
memory and a larger one in an unlinked-on-close file under
`image_scratch_dir()`.

### 3.8 "Not yet"

A source whose bytes are not all at hand -- a remote file still downloading
-- answers a read with `GS_EAGAIN` and implements the optional `poll` op,
which waits until a read may make progress. `gs_source_poll` asks the
nearest source in the parent chain that has one (a view of a remote file
polls the file; the locked wrapper forwards to what it wraps), and answers
0 at once for sources that never say "not yet". `gs_source_read_exact`
waits `GS_EAGAIN` out with it, so every reader built on it -- the storage
engine, the chunk cache, peeler's decoders -- works over such a source
unchanged; a source that keeps refusing without progress is given up on
rather than spun on.

### 3.9 Threads

A host source may be read from any thread. Other sources are not
thread-safe: the storage engine wraps its base in `gs_source_locked`, as the
registry does for every peeler payload, since an export streams the base on
the I/O worker while the guest reads it.

## 4. Object-model / shell surface

- `files.cache` — the chunk cache: `memory_mb` and `spill_mb` (read-write;
  `spill_mb` 0 is unbounded), and the counters `memory_bytes`,
  `spill_bytes`, `hits`, `misses`, `evictions`.
- `files.images[n].format` reports each image's wrapper chain (`raw`,
  `dc42`, `bin+ndif`, …); the VFS listing's `expandable` flag comes from
  `gs_format_is_namespace`.

## 5. Checkpointing

An image persists the path its caller named (not a decoded copy's), and a
restore opens that path again through the resolver, so an image inside an
archive or another image restores the same way. It persists its source's key
too. A quick checkpoint holds only the image's delta, so its disk is the base
plus that delta: the restore compares the reopened base's key with the saved
one (`gs_key_same_source`) and refuses a base that is no longer the same
bytes, naming both keys. A consolidated checkpoint carries every block, so
its base is not compared.

## 6. Testing

- Unit: `peeler` (the source type, archives, resumable decoders), `source`
  (views, decode-through, chunk-cache coalescing, spill and run-time
  budgets, the detection budget, `GS_EAGAIN` and poll, key comparison),
  `udif`, `ndif`, `storage` (the base as a source).
- Integration: `image-udif` (with `files.cache`), `image-export-raw`,
  `image-hfs-traverse`, `vfs-rsrc`, `archive-fork-unpack`,
  `checkpoint-base-identity` (a replaced base is refused).
- e2e: `checkpoint-resume.spec.ts` resumes a machine with a floppy attached
  across a reload (the OPFS time stamp case).

## 7. Known debts

- A source's key is not length-bounded (the proposal suggested 128 bytes);
  deep nesting makes long keys.
- The UDIF per-table checksum is not verified (§3.5).

## 8. See also

[../vfs/namespace.md](../vfs/namespace.md), [storage.md](storage.md),
[image.md](image.md), [../../../guide/peeler.md](../../../guide/peeler.md).
