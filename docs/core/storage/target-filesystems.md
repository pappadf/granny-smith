# Target Filesystem Access (APM, HFS, UFS)

Granny Smith can read the **contents of a guest disk image** — its partition
map and the files inside its HFS or UFS volumes — without booting the guest or
SCSI-attaching the disk. This is a host-side, **read-only** facility built on
plain block reads against an `image_t`; it never runs guest driver code, parses
no guest page tables, and touches no live machine state.

It exists so tooling and the shell can answer questions like "what partitions
does this image have?" and "give me the bytes of `/etc/motd` (or a resource
fork) out of this volume" cheaply and deterministically.

```
shell / web terminal     image partmap | image probe | vfs.ls | vfs.list | vfs.cat | storage.cp
        │                  web Filesystem tree → vfs.list (image descent)
        ▼
  VFS resolver  ───────  path normalise + descent + auto-mount cache   (src/core/vfs/vfs.c)
        │
   ┌────┴───────┐
 host backend   image backend  ── one mount per image; routes per-partition
 (host_vfs.c)   (image_vfs.c)
                    │
        ┌───────────┼───────────────┐
   APM parser    HFS walker      UFS walker
 (image_apm.c)  (image_hfs.c)   (image_ufs.c)
        └───────────┬───────────────┘
              storage / image.c  ── disk_read_data / disk_read_bytes (512-byte blocks)
```

Everything under the block-read line is byte-offset parsing; the filesystem
walkers never see anything but bytes from the image.

---

## 1. Architecture & layering

The **VFS layer** (`src/core/vfs/vfs.h`) gives the shell's filesystem commands a
small backend interface instead of calling libc directly. Two backends ship:

- **host** (`host_vfs.c`) — ordinary POSIX paths (the browser's OPFS, or the
  native filesystem in headless builds). Stateless; its `ctx` is always `NULL`.
- **image** (`image_vfs.c`) — paths that descend *into* a disk image. Its `ctx`
  is an `image_mount_t*` from the auto-mount cache.

A path resolver (`vfs_resolve` / `vfs_resolve_descend`) routes between them
transparently, so a single path can cross the host→image boundary:

```
ls /opfs/disks/aux.img/partition4/etc/motd
       └──── host path ────┘└── in-image tail ──┘
```

The backend vtable (`vfs_backend_t`) is the whole contract:

```c
typedef struct vfs_backend {
    const char *scheme;                 // "host" or "image"
    int  (*stat)(void *ctx, const char *path, vfs_stat_t *out);
    int  (*opendir)(void *ctx, const char *path, vfs_dir_t **out);
    int  (*readdir)(vfs_dir_t *d, vfs_dirent_t *out);   // 0=eof,1=entry,<0 err
    void (*closedir)(vfs_dir_t *d);
    int  (*open)(void *ctx, const char *path, vfs_file_t **out);
    int  (*read)(vfs_file_t *f, uint64_t off, void *buf, size_t n, size_t *nread);
    void (*close)(vfs_file_t *f);
    int  (*mkdir)(void *ctx, const char *path);         // image: static -EROFS
    int  (*unlink)(void *ctx, const char *path);        // image: static -EROFS
    int  (*rename)(void *ctx, const char *src, const char *dst); // image: -EROFS
} vfs_backend_t;
```

`vfs_stat`/`vfs_opendir`/`vfs_open`/`vfs_mkdir`/`vfs_unlink`/`vfs_rename` are the
convenience wrappers that combine resolution with a backend call and are the
primary entry points for shell commands.

---

## 2. Partition tables — Apple Partition Map (APM)

**Source:** `src/core/storage/image_apm.{c,h}` (pure parser) and
`image_apm_io.c` (image-backed entry point, split out so unit tests can link the
parser without the storage stack).

APM media uses **512-byte logical blocks** (`APM_BLOCK_SIZE`). The on-disk
layout the parser understands:

| Block | Contents |
|-------|----------|
| 0     | Driver Descriptor Map (DDM). May declare a non-512 block size; we do not honour that — every APM medium we care about uses 512. Not parsed beyond being skipped. |
| 1..N  | Partition map entries, one per 512-byte block. Entry 1 (block 1) carries `pmMapBlkCnt`, the count of entries. |

Each entry is parsed big-endian (`be16`/`be32` done by hand, no host
`htonl` dependency) at these offsets:

| Offset | Field | Meaning |
|--------|-------|---------|
| `0x00` | `pmSig` | signature `"PM"` (`0x504D`) |
| `0x04` | `pmMapBlkCnt` | number of partition-map blocks (entry 1 only is read for the count) |
| `0x08` | `pmPyPartStart` | partition start, in 512-byte blocks |
| `0x0C` | `pmPartBlkCnt` | partition size, in 512-byte blocks |
| `0x10` | `pmPartName` | 32-byte name |
| `0x30` | `pmParType` | 32-byte type string |
| `0x58` | `pmPartStatus` | status flags |

**Filesystem classification** (`image_apm_classify_type`) maps the type string
to an `apm_fs_kind`, case-insensitively:

| `pmParType` | `apm_fs_kind` |
|-------------|---------------|
| `Apple_HFS`, `Apple_HFSX` | `APM_FS_HFS` |
| `Apple_UNIX_SVR2` | `APM_FS_UFS` (A/UX) |
| `Apple_partition_map` | `APM_FS_PARTITION_MAP` |
| `Apple_Driver*` | `APM_FS_DRIVER` |
| `Apple_Free` | `APM_FS_FREE` |
| `Apple_Patches` | `APM_FS_PATCHES` |
| (anything else) | `APM_FS_UNKNOWN` |

**Safety / robustness:**

- `image_apm_probe_magic(block1)` is the cheap sniff used before committing to a
  full parse: signature `"PM"` plus a plausible `pmMapBlkCnt` in `1..256`.
- The map length is capped at `APM_MAX_PARTITIONS = 256` to protect against a
  corrupt `pmMapBlkCnt` driving a huge allocation. Real Apple media never
  exceeds ~64 entries.
- A per-entry signature mismatch or buffer exhaustion stops the walk and returns
  the **partial** table rather than rejecting the whole map.
- `image_apm_parse` reads up to **257 blocks (~128 KB)** in a single
  `disk_read_data`, capped to the image size so small fixtures still work.

**Key types** (`image_apm.h`):

```c
typedef struct apm_partition {
    uint32_t index;          // 1-based, matches Apple convention
    uint64_t start_block;    // pmPyPartStart (512-byte blocks)
    uint64_t size_blocks;    // pmPartBlkCnt
    char     name[33];       // pmPartName, NUL-terminated
    char     type[33];       // pmParType, NUL-terminated
    uint32_t status;         // pmPartStatus
    enum apm_fs_kind fs_kind;
} apm_partition_t;

typedef struct apm_table {
    uint32_t map_block_count;       // pmMapBlkCnt from entry 1
    uint32_t n_partitions;
    apm_partition_t *partitions;    // heap array, owned by caller
} apm_table_t;
```

Images **without** an APM (a bare 400K/800K/1.4M HFS floppy, or a raw HD volume)
are handled by the mount layer, which synthesises a single `partition1` covering
the whole image (see §5).

---

## 3. HFS reader

**Source:** `src/core/storage/image_hfs.{c,h}`. Read-only catalog walker plus an
Extents Overflow walker.

### Opening a volume

`hfs_open(image_t *img, uint64_t partition_byte_offset, uint64_t partition_byte_size)`:

- `partition_byte_offset` is `0` for a bare floppy, or `start_block * 512` for an
  APM partition. `partition_byte_size` caps every read.
- Reads the **Master Directory Block (MDB)** at byte offset **1024** within the
  partition; signature must be `"BD"` (`0x4244`) at MDB offset 0.

Geometry pulled from the MDB:

| MDB offset | Field | Use |
|------------|-------|-----|
| `0`   | `drSigWord` | `"BD"` signature |
| `18`  | `drNmAlBlks` | number of allocation blocks |
| `20`  | `drAlBlkSiz` | allocation block size (bytes) |
| `28`  | `drAlBlSt` | first allocation block, in 512-byte units → `alloc_block0_byte_off = partition_off + drAlBlSt*512` |
| `36`  | `drVN` | volume name (Pascal string, ≤ 28 bytes) |
| `130` | `drXTFlSize` | Extents Overflow file size |
| `134` | `drXTExtRec` | EO file's 3 inline extents |
| `146` | `drCTFlSize` | Catalog file size |
| `150` | `drCTExtRec` | Catalog file's 3 inline extents |

### Catalog: loaded into RAM, B-tree leaf scan

On open, the **catalog file** is read fully into memory using its three inline
extents (`drCTExtRec`), then the B-tree **leaf chain** is walked
(`fLink`-linked, node kind `0xFF`) and every folder/file record is flattened
into `vol->records[]`. Lookups then scan that list; root directory is
`HFS_ROOT_CNID = 2`. File records yield: CNID, valence (folders), the data and
resource fork descriptors, and the 32-byte Finder info (16 `FInfo` + 16
`FXInfo`).

### Forks and the Extents Overflow file

A `hfs_fork_t` carries the logical size, three inline extents, and the
`(file_id, fork_type)` key needed to chase overflow extents:

```c
typedef struct hfs_fork {
    uint64_t logical_size;
    uint32_t file_id;     // CNID of the owning file (0 for ad-hoc forks)
    uint8_t  fork_type;   // 0x00 = data fork, 0xFF = resource fork
    struct { uint32_t start_ablock; uint32_t num_ablocks; } extents[HFS_INLINE_EXTENTS]; // 3
} hfs_fork_t;
```

`hfs_read_fork` reads a fork at any logical offset:

1. **Inline extents** — the three extents from the catalog record are applied
   first (an `APPLY_EXTENT` helper reads the overlapping slice of each).
2. **Extents Overflow (EO) file** — if the request spills past the inline
   coverage, the reader chases EO records. At `hfs_open` time the EO file is
   loaded from `drXTFlSize`/`drXTExtRec` and its leaf chain is parsed into a flat
   sorted list `vol->xt_records[]`. Each EO leaf record is keyed by a 7-byte key
   `forkType(1) + fileNumber(4) + startBlock(2)` and supplies 3 more extents.
   `find_xt_record(fork_type, file_id, need_block)` returns the highest-
   `start_block` record at or before the current logical block; records chain
   together until the request is satisfied. A "no forward progress" guard
   prevents an infinite loop on a malformed record.
3. **Tail** — anything still uncovered (e.g. the EO file is itself fragmented
   past its own 3 inline extents, or the volume is corrupt) zero-fills to honour
   the `logical_size` contract.

> Consulting the Extents Overflow file means **fragmented forks now read in
> full**. Previously the reader used only the 3 inline extents and silently
> zero-filled the remainder, which corrupted any structure living deep in a fork
> — most visibly a resource fork's map area at the high end of the data. That
> limitation is gone.

### Filenames: MacRoman → UTF-8 and the `/`↔`:` swap

- Catalog names are transcoded from **MacRoman to UTF-8** via
  `macroman_to_utf8()` in `src/core/storage/macroman.{c,h}` (a standalone
  translation unit so a future resource-fork parser or HFS+ walker can share the
  128-entry table without depending on `image_hfs.c`).
- HFS uses `:` as its on-disk path separator and allows `/` **freely inside
  filenames**; the VFS uses `/` as the separator. To make slashed names
  addressable, the reader swaps the two characters on the boundary:
  - **Outbound** (`fill_dirent`): `/` bytes in an HFS name surface through the
    VFS as `:` — so a file named `MacTest cx/ci` on disk lists as
    `MacTest cx:ci`.
  - **Inbound** (`hfs_fold_byte`): catalog-name comparison folds `/` and `:` to
    the same canonical byte (alongside the ASCII case fold), applied
    symmetrically in both the B-tree comparison and the UTF-8 fallback. A path
    component containing `:` therefore matches the on-disk name containing `/`.

  There is no aliasing risk: `:` cannot legally appear in an on-disk HFS name, so
  the mapping is one-way unambiguous, and names without `/` are unaffected.

### HFS API (`image_hfs.h`)

```c
hfs_volume_t *hfs_open(image_t *img, uint64_t part_off, uint64_t part_size);
void          hfs_close(hfs_volume_t *vol);
const char   *hfs_volume_name(const hfs_volume_t *vol);

int hfs_lookup(hfs_volume_t *vol, const char *const *components, size_t nc,
               hfs_dirent_t *out);   // nc==0 → root; 0 hit, -ENOENT miss

hfs_dir_iter_t *hfs_opendir_cnid(hfs_volume_t *vol, uint32_t parent_cnid);
int             hfs_readdir_next(hfs_dir_iter_t *it, hfs_dirent_t *out); // 1/0/<0
void            hfs_closedir_iter(hfs_dir_iter_t *it);

int hfs_read_fork(hfs_volume_t *vol, const hfs_fork_t *fork,
                  uint64_t off, void *buf, size_t n, size_t *nread);
```

### HFS scope & limitations

- Covers 400K/800K/1.4M floppies with no partition map, and HFS partitions
  inside an APM image.
- Data fork, resource fork, and the 32-byte Finder info are all readable.
- HFS classic only — **HFS+ is not supported**.
- The catalog and EO files are themselves loaded via their *own* three inline
  extents. If one of those special files is fragmented past 3 extents, only the
  captured portion is parsed (a pragmatic compromise; realistic volumes keep
  these files small).

---

## 4. UFS reader (A/UX)

**Source:** `src/core/storage/image_ufs.{c,h}`. Read-only UFS-1 walker targeting
A/UX 3.0.x.

- `ufs_open(img, partition_byte_offset, partition_byte_size)` reads the **BSD FFS
  superblock at offset 8192** (`UFS_SBOFF`) and validates `FS_MAGIC`
  (`0x011954`). A/UX writes big-endian on 68k, but either byte order is accepted
  (tolerating an occasional LE-rewritten image). `ufs_probe` is the cheap
  single-sector pre-check.
- Targets the `Apple_UNIX_SVR2` APM entries a period A/UX install writes.
- **Inode layout is 4.3BSD-Tahoe**: the 32-bit `di_size` lives at inode offset
  **12** (not 8) — Tahoe used `quad_t val[0]` for `di_rdev` and `val[1]` for
  size, and A/UX inherited that. Files > 4 GiB are unsupported.
- Block addressing follows **direct (12) + single-indirect + double-indirect**
  pointers. Triple-indirect is unimplemented (no current fixture needs it).
- Root inode is `UFS_ROOT_INO = 2`. Inodes are read **on demand** (no full
  in-RAM snapshot, unlike HFS) — suited to large volumes with sparse access.
- Names are 8-bit clean and passed through unchanged. Symbolic links are
  *reported* (`is_symlink`) but **not followed** — callers see the link itself.

### UFS API (`image_ufs.h`)

```c
bool          ufs_probe(image_t *img, uint64_t part_off, uint64_t part_size);
ufs_volume_t *ufs_open (image_t *img, uint64_t part_off, uint64_t part_size);
void          ufs_close(ufs_volume_t *vol);

int ufs_lookup(ufs_volume_t *vol, const char *const *components, size_t nc,
               ufs_dirent_t *out);   // nc==0 → root; -ENOTDIR on non-dir mid-path

ufs_dir_iter_t *ufs_opendir_ino(ufs_volume_t *vol, uint32_t ino);
int             ufs_readdir_next(ufs_dir_iter_t *it, ufs_dirent_t *out); // skips "." ".."
void            ufs_closedir_iter(ufs_dir_iter_t *it);

int ufs_read_file(ufs_volume_t *vol, uint32_t ino,
                  uint64_t off, void *buf, size_t n, size_t *nread);
```

---

## 5. The glue — descent resolver + auto-mount cache

**Source:** `src/core/vfs/vfs.c` (resolver) and `src/core/vfs/image_vfs.{c,h}`
(auto-mount cache + image backend).

### Path descent

`walk_for_descent` (in `vfs.c`) normalises the input path (resolving `.`/`..`
against the shell cwd), then walks it left-to-right. The **first intermediate
segment that `stat`s as a regular file** is probed as an image; on success the
remaining path routes into the image backend. The byte length of the image-file
prefix becomes the split point, and the tail (`/partitionN/...`) is what the
image backend consumes.

Two resolver variants:

- `vfs_resolve` — strict. A path that *terminates* exactly at an image file is
  treated as the raw file (so `cat foo.img` dumps the raw blob).
- `vfs_resolve_descend` — used by `ls`/`cd`: a bare image path descends into the
  image's **partition-list root**, so `ls foo.img` lists `partition1…N` instead
  of erroring.

### In-image path grammar

`parse_image_path` (in `image_vfs.c`) interprets the tail:

```
/                              → mount root (lists partitionN)
/partitionN                    → partition root (a directory)
/partitionN/dir/file           → file/dir inside the volume
/partitionN/.../file/rsrc      → HFS resource fork of that file
/partitionN/.../file/finf      → 32-byte Finder-info blob of that file
```

- The first component must be `partitionN` (1-based; case-insensitive
  `partition` prefix).
- `rsrc` / `finf` are recognised only as a **trailing** suffix on an HFS file. If
  the suffix lookup fails, the reader retries treating the suffix as part of the
  filename, so a real file literally named `rsrc` still resolves. UFS partitions
  have no forks — the suffix logic is skipped for them.

### Auto-mount cache

`image_vfs_acquire_mount` opens an image on first descent and caches it:

- Cache holds up to **`IMAGE_VFS_MAX_MOUNTS = 8`** mounts, keyed on the canonical
  absolute host path (via `realpath`). A recorded `dev`/`inode`/`mtime` triggers
  reprobe + eviction if the file is swapped underneath us.
- On open it probes **APM first, then bare HFS at offset 0**. A bare HFS floppy
  gets a synthetic `partition1` (`Apple_HFS`, start 0, whole-image size) so the
  rest of the code treats it uniformly. If neither matches → `-ENOTDIR` ("not a
  recognised image").
- Per-partition filesystem state is opened **lazily** on first access
  (`get_partition_hfs` / `get_partition_ufs`, which compute `start_block*512`).
- **Read-only**: the image backend's `mkdir`/`unlink`/`rename` slots are static
  `-EROFS` rejecters — never conditionally writable.
- **SCSI-attach conflict**: if the same file is attached to the guest via
  `hd`/`cdrom`, `image_vfs_notify_attached` marks the mount *conflicted* and
  backend calls return `-EBUSY`; `image_vfs_notify_detached` clears it. This
  prevents reading an image the guest is mutating.
- A reference count tracks live dir/file handles so a mount is not torn down
  underneath them.

Partition enumeration at the mount root lists **all** partitions — including
ones that can't be descended into (`map`, `driver`, `free`, `patches`). Those
stat as empty read-only directories; `opendir` on them returns `-ENOTDIR`. Use
`image partmap` for the full typed layout.

---

## 6. Using it

### Shell / object-model commands

Partition inspection (`src/core/shell/cmd_image.c`):

| Command | Effect |
|---------|--------|
| `image partmap <path> [--json]` | Parse and print the partition map (text table or JSON array). |
| `image probe <path>` | Print the detected format without descending — APM, ISO 9660 (`CD001` @ 0x8000), APM+ISO hybrid, bare HFS, or raw. |
| `image list [--json]` | Enumerate currently-cached auto-mounts (format, partition count, refs, busy/ok). |
| `image unmount <path>` | Force-close a cached auto-mount. |

Content access (`src/core/vfs/vfs_class.c`, `src/core/shell/cmd_cp.c`):

| Command | Effect |
|---------|--------|
| `vfs.ls [path]` | List a directory (names to stdout) — descends into images, partitions, and HFS/UFS directories. Defaults to the cwd. |
| `vfs.list [path]` | Like `vfs.ls`, but returns a **JSON array** `[{name, kind, size}]` instead of printing. Same descent rules. This is what the web Filesystem tree calls to expand a disk image. |
| `vfs.cat <path>` | Dump a file's bytes — data fork, or `…/rsrc` resource fork, or `…/finf` Finder info. |
| `vfs.mkdir <path>` | Create a directory — **host paths only** (image paths return `-EROFS`). |
| `cp <src> <dst>` | Copy a file/tree, including *out of* an image into OPFS. |

Example session:

```
> image partmap /opfs/disks/aux.img
format: APM (512B blocks, 81920 total)
  #  Name              Type             Start   Size  FS
  1  Apple            Apple_partition_map   1     63  map
  2  Macintosh        Apple_Driver43       64     32  drvr
  3  MacOS            Apple_HFS            96  20480  HFS
  4  Root             Apple_UNIX_SVR2   20576  61344  UFS

> vfs.ls /opfs/disks/aux.img/partition4/etc
motd
passwd
...

> vfs.cat /opfs/disks/aux.img/partition4/etc/motd
Welcome to A/UX

> vfs.cat "/opfs/disks/sys7.img/partition3/System Folder/Finder/rsrc"   # resource fork
```

A file whose on-disk HFS name contains `/` is addressed by substituting `:`:

```
> vfs.ls /opfs/disks/MacTest.image/partition1
MacTest cx:ci
> vfs.cat "/opfs/disks/MacTest.image/partition1/MacTest cx:ci"
```

### From the browser

The web frontend runs these through the **terminal pane**
(`app/web2/src/components/panel-views/terminal/TerminalPane.svelte`) via
`gsEvalLine`, and programmatically via `gsEval` (`app/web2/src/bus/emulator.ts`).

> **The Filesystem panel descends into disk images.** The Filesystem tree
> (`app/web2/src/components/panel-views/filesystem/FilesystemView.svelte`)
> browses OPFS through `opfs.ts`, and a row whose name is a disk image
> (`.img` / `.dsk` / `.dc42` / `.iso` / `.toast` / `.cdr` / `.hda` / `.image`)
> is **expandable**: expanding it lists the image's partitions, and each
> partition expands into its HFS/UFS contents. The tree routes those listings
> through `vfs.list` (`app/web2/src/bus/vfs.ts`) instead of OPFS; the
> classification helpers live in `app/web2/src/lib/diskImage.ts`. Everything
> inside an image is **read-only** — the tree omits rename/delete/unpack from
> the context menu and refuses drops *onto* an image (the image backend is
> `-EROFS`). Two ways to get data *out* of an image, both via the VFS-backed
> `storage.cp` (recursive for folders):
>
> - **Download** a file — copies its data fork to a scratch OPFS path, hands
>   the bytes to the browser, then deletes the scratch file.
> - **Drag** a file or folder onto an OPFS folder — copies it there (a normal
>   OPFS-to-OPFS drag still *moves*; an image source can't be moved, so it
>   copies). The drag's drop-effect shows "copy" vs "move" accordingly.
>
> (Resource forks aren't surfaced in the tree; use `vfs.cat …/rsrc` for those.)

---

## 7. Capability matrix

| Area | Supported | Not supported |
|------|-----------|---------------|
| Partition map | APM (512-byte blocks, big-endian); synthetic single-partition for bare/raw HFS | GPT; DDM-declared non-512 block sizes |
| HFS | MDB + catalog B-tree; 3 inline extents **plus Extents Overflow file** (fragmented forks read fully); data/resource forks; Finder info; MacRoman→UTF-8; `/`↔`:` name addressing | HFS+; EO/catalog file fragmented past their *own* 3 inline extents |
| UFS | UFS-1 / 4.3BSD-Tahoe, big-endian; direct + single + double indirect; root traversal; symlink reporting | triple-indirect; files > 4 GiB; symlink following |
| Mutability | read-only everywhere (`-EROFS`) | any write path into an image |
| Concurrency | refuses descent into a file the guest has SCSI-attached (`-EBUSY`) | — |
| GUI | terminal commands (`vfs.*`, `image …`); **read-only image descent in the Filesystem tree** (expand image → partitions → HFS/UFS contents); **Download** or **drag out** a file/folder from an image (copied via `storage.cp`) | writing into an image from the tree (read-only); resource-fork extraction from the tree (use `vfs.cat …/rsrc`) |

---

## 8. Testing

- **Unit** — `tests/unit/suites/vfs/` exercises the APM parser
  (`image_apm_parse_buffer`, `image_apm_probe_magic`,
  `image_apm_classify_type`) with synthetic byte buffers, plus the path
  resolver. Run `make -C tests/unit run`.
- **Integration** — `tests/integration/image-hfs-traverse` and
  `image-ufs-traverse` drive real fixtures end-to-end. Run
  `make integration-test-image-hfs-traverse` etc., or the full
  `make integration-test`.

---

## 9. Source map

| Path | Role |
|------|------|
| `src/core/vfs/vfs.{c,h}` | VFS backend interface, path resolver, convenience wrappers, cwd |
| `src/core/vfs/host_vfs.c` | Host (POSIX/OPFS) backend |
| `src/core/vfs/image_vfs.{c,h}` | Auto-mount cache + image backend; in-image path parsing; partition routing |
| `src/core/vfs/vfs_class.c` | Object-model `vfs.ls` / `vfs.cat` / `vfs.mkdir` bindings |
| `src/core/storage/image_apm.{c,h}` | Pure APM parser |
| `src/core/storage/image_apm_io.c` | Image-backed APM entry point |
| `src/core/storage/image_hfs.{c,h}` | HFS catalog + Extents Overflow walker, fork reader |
| `src/core/storage/image_ufs.{c,h}` | UFS-1 superblock + inode walker, file reader |
| `src/core/storage/macroman.{c,h}` | MacRoman → UTF-8 transcoder (shared) |
| `src/core/shell/cmd_image.c` | `image partmap/probe/list/unmount` |
| `src/core/shell/cmd_cp.c` | `cp` (VFS-backed, supports image→host) |
| `src/core/storage/image.{c,h}` | Underlying disk image + `disk_read_data` / `disk_read_bytes` |

See also: `docs/core/storage/image.md` (image container & delta storage), `docs/core/peripherals/scsi.md`
(attaching images to the guest), and `docs/core/shell/shell.md` (the object-model shell).
