# Namespaces: disks, filesystems and archives as trees

Owning files: `src/core/vfs/namespace.h`, `src/core/vfs/ns_disk.c`,
`src/core/vfs/ns_archive.c`, `src/core/vfs/image_vfs.c` (the path glue),
`src/core/vfs/vfs.c` (the resolver).

## 1. Responsibilities & design

A namespace lists paths and opens their forks as byte sources
([../storage/source.md](../storage/source.md)). A disk image, a filesystem
volume and an archive are each a namespace over a source, and a file a
namespace opens is a source another namespace can be opened on. That is all
nesting is:

```
ls roms.zip/System 7.sit/Disk Tools.img/partition1/System Folder
```

walks four namespaces, each opened on a source the previous one handed out.

The user-visible path grammar is unchanged:

- A path continues through any file the format registry recognises as a
  namespace. `cat foo.img` still dumps raw bytes (`vfs_resolve`); `ls foo.img`
  descends (`vfs_resolve_descend`). The same rule holds for `foo.zip`.
- Inside a disk image: `partitionN/…`, with a synthetic `partition1` for a
  bare volume.
- Inside an archive: the member tree directly.
- Any file with a resource fork or Finder info has the synthetic leaves
  `…/file/rsrc` (a directory of resource types, with `_raw` for the whole
  fork) and `…/file/finf` (32 bytes of Finder info) — HFS files and two-fork
  archive members alike.
- Wrapper formats with exactly one file (MacBinary, BinHex, DiskCopy 4.2,
  gzip) are transparent: `foo.img.bin/partition1` works, and `foo.img.bin`
  lists partitions.
- HFS names containing `/` are spelled with `:`.

## 2. Key types & files

| Type / function | File | Purpose |
|---|---|---|
| `gs_namespace_ops_t` | `namespace.h` | `list`, `stat`, `open(path, fork)`, `close` |
| `gs_dirent_t` | `namespace.h` | name, kind, fork sizes, Mac type/creator/flags, tier |
| `gs_ns_open_disk` | `ns_disk.c` | APM or bare volume → `partitionN` → HFS/HFS+/UFS |
| `gs_ns_open_archive` | `ns_archive.c` | any peeler format → its member tree |
| `image_mount_t` | `image_vfs.c` | a mounted namespace, keyed by its source's key |

## 3. Behaviour/algorithms

### 3.1 Resolution

`vfs.c` walks a path left to right. On the host, the first regular file with
more path after it is opened as a source and handed to the format registry;
if it is a namespace, it is mounted and the rest of the path is resolved
inside it. Inside a namespace the same rule repeats: the first *file* with
more path after it (and whose next component is not `rsrc` or `finf`) is
opened as a source — a view of the parent, or a decode-through fork — and
mounted in turn. Nothing is copied to a scratch file on the way.

### 3.2 Mounts

A mount holds a namespace and the source it was opened on. The mount table
is keyed by the source's key, so the same file reached twice shares one
mount. A mount refuses service (`-EBUSY`) while an image whose key contains
its own is attached writable (`image_key_is_open_writable`): the guest's
writes go to that image's delta, which a read-only mount cannot see.

### 3.3 The synthetic resource tree

`image_vfs.c` adds `rsrc` and `finf` to any namespace whose `stat` reports a
resource fork or Finder info. Parsed resource maps are cached (eight
entries, LRU, pinned while a handle borrows them), keyed by mount and path.

### 3.4 Opening files as sources

`vfs_open_source(path, fork)` resolves a path and returns the fork as a
source: a host file, a namespace member, its resource fork, or its Finder
info. It is installed as the storage engine's path opener
(`gs_source_set_path_opener`), which is how a floppy, a SCSI disk or a CD
image can be a file inside another image or an archive.

## 4. Object-model / shell surface

`files.ls` entries carry `expandable: true` for a file the registry
recognises as a namespace; the web UI routes on that flag rather than on the
file's extension. `files.mounts[n]` lists live mounts with their format.

## 5. Checkpointing

Mounts are not checkpointed; they are rebuilt on demand.

## 6. Testing

- Unit: `image_vfs`, `vfs`.
- Integration: `image-hfs-traverse`, `image-partmap`, `vfs-rsrc`,
  `image-attach-busy`, `image-export-raw`, `archive-vfs`.

## 7. Known debts

- Zip names are matched case-sensitively and HFS names case-insensitively;
  each adapter matches its own way.

## 8. See also

[../storage/source.md](../storage/source.md),
[../storage/image.md](../storage/image.md),
[../storage/target-filesystems.md](../storage/target-filesystems.md).
