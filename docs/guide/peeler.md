# Archive (peeler) Integration

This document describes how the emulator reads classic Macintosh
archive formats and the common cross-platform ones. The underlying
library is **peeler**; the emulator-facing surfaces are the VFS (an
archive is a directory tree you can list, `cd` into and attach from)
and the `files.archive` node on the object tree.

## Overview

**peeler** is a portable C99 library for reading archive and wrapper
formats:

| Name | Kind | Format |
|------|------|--------|
| `hqx` | wrapper | BinHex 4.0 (CRC-checked, both forks) |
| `bin` | wrapper | MacBinary I/II/III (both forks) |
| `gz`  | wrapper | gzip (multi-member; BGZF is indexed for random access) |
| `sit` | archive | StuffIt 1.x–5.x (methods 0, 1, 2, 3, 5, 13, 15) |
| `cpt` | archive | Compact Pro |
| `zip` | archive | Zip (stored and deflate; Zip64; self-extracting prefixes; `__MACOSX/` AppleDouble folded back into resource forks and Finder info) |
| `tar` | archive | tar (ustar, GNU long names, pax `path`/`size`, base-256 sizes, hard links; macOS `._` AppleDouble folded back; every member a view) |

A *wrapper* holds exactly one file; an *archive* holds a tree. Nesting
is handled by the caller opening an entry's fork as a new source, so a
`.sit.hqx`, a `.dsk.gz` or a zip of zips each unpack layer by layer.

The library originated as a standalone project at
<https://github.com/pappadf/peeler>. It now lives in-tree under
[`src/peeler/`](../../src/peeler/), and the in-tree copy is canonical:
the upstream repository is refreshed *from* this tree, never the other
way round.

## API

peeler is structure-first. Opening an archive reads its headers and
directory only; fork payloads are decoded when, and as far as, someone
reads them.

```c
peel_source_t  *src = peel_source_file(path, &err);        // or memory, view, or your own ops
peel_archive_t *a   = peel_open(src, NULL, NULL, &err);    // detect + parse structure
for (int i = 0; i < peel_count(a); i++) {
    const peel_entry_t *e = peel_entry(a, i);               // path, sizes, type/creator, tiers
    peel_source_t *data = peel_open_fork(a, i, PEEL_FORK_DATA, &err);
    ...                                                     // read it like any source
    peel_source_release(data);
}
peel_close(a);
peel_source_release(src);
```

- **Sources** (`peel_source_t`) — reference-counted byte sources with
  `read`, `size`, `key` and `tier`. The tier says how reads cost:
  `RANDOM` (a view or a memory buffer), `INDEXED` (BGZF), `EARNED`
  (decoded once, then random), `STREAM` or `WHOLE`. The emulator's
  core uses the same type as `gs_source_t`
  (see [`source.md`](../internals/core/storage/source.md)).
- **Probe** — `peel_probe_init` reads a bounded head and tail of a
  source; `peel_identify` and `peel_formats` run the detectors over it
  without opening anything.
- **Forks** — a stored fork is a view onto the archive's source; a
  compressed fork is a decode-through source that fills a *sink*
  (heap by default; the emulator passes a scratch-backed one) only as
  far as reads require (tier `EARNED`: what has been decoded stays in the
  sink, so backward reads are free). Every decoder is resumable -- deflate,
  StuffIt's RLE90, LZW, Huffman (3), LZSS+Huffman (13) and Arsenic (15),
  Compact Pro's LZH and RLE -- stopping wherever the caller's buffer fills,
  so a reader never re-decodes from the start and never waits for more of
  a fork than it asked for. The fork CRC is checked once the fork is
  complete. BinHex alone decodes a fork whole (tier `WHOLE`): its files are
  small. The buffer API drains the same producers, so both decode one way. A plain gzip stream
  states no reliable size -- its tail ISIZE names only the last member,
  modulo 4 GiB -- so the listing shows that as a hint (as `gzip -l` does)
  and the fork's source earns its true size with one decoding pass; the
  sink is then created with `PEEL_SIZE_UNKNOWN`.
- **Buffer API** — `peel()`, `peel_path()` and the per-format
  `peel_hqx`/`peel_bin`/`peel_sit`/`peel_cpt`/`peel_zip`/`peel_gz`
  remain, as sugar over the structure-first API, for callers that want
  every file decoded into memory at once.

Errors are `peel_err_t *` objects (`NULL` means success;
`peel_err_msg`, `peel_err_free`).

### Command-line tool

`src/peeler/cmd/main.c` builds a standalone `peeler` binary:

```
peeler <archive> [<output-dir>]          # peel every layer, write the files
peeler list <archive>                    # one line per entry: sizes, method, tier
peeler extract <archive> <member> [<dir>] # one member (both forks) into <dir>
```

## Emulator integration

### Format registry

The core has a single table of formats a byte source can be in
([`format_registry.h`](../../src/core/storage/format_registry.h)).
peeler's wrappers (`hqx`, `bin`, `gz`) sit there beside the disk-image
wrappers (UDIF, NDIF, DiskCopy 4.2) and are unwrapped in a loop; its
archives (`sit`, `cpt`, `zip`, `tar`) are namespace formats beside `disk`
(partition map or bare volume). So the same detection decides what a
file is whether it is opened as a disk, listed in the Filesystem tab,
probed or attached to a SCSI slot.

### VFS

Every archive is a directory tree in the VFS
([`namespace.md`](../internals/core/vfs/namespace.md)): `ls`, `cd`,
`cat` and `cp` work inside it, and a disk image inside an archive can
be attached directly by its path, e.g.
`MacTest_Disk.image_.sit_.hqx/MacTest Disk.image`. Nothing is extracted
to a temporary file; the image reads through the archive's sources and
the chunk cache.

A file's resource fork and Finder info are reachable as
`<file>/rsrc/_raw` and the Finder-info leaf, the same as on an HFS
volume.

### `files.archive`

| Path | Result | Description |
|------|--------|-------------|
| `files.archive.identify(path)` | `VK_STRING` — `"sit"` / `"cpt"` / `"zip"` / `"tar"` / `"hqx"` / `"bin"` / `"gz"`, or empty string when the file isn't a recognised archive | Bounded format probe; doesn't extract |
| `files.archive.extract(path, [out_dir])` | `VK_BOOL` — `true` on success | Copy every file in the archive to `out_dir` (defaults to the current working directory) |

Both go through the VFS, so `path` may itself be inside an image or
another archive. `extract` is a recursive copy of the archive's tree:
data forks become regular files, and a file with a resource fork or
non-trivial Finder info also gets an AppleDouble `._<name>` sidecar.
Entry names are confined: none may escape `out_dir`.

Empty / missing return values follow the predicate-truthy rule: an
unrecognised file produces an empty string, which scripts can test as
a falsy `${...}`.

Both are reachable everywhere the object model is: the interactive
shell, headless scripts, the JavaScript bridge
(`gsEval('files.archive.identify', [path])`) and the inspector UI.
`extract` is dispatched as an I/O-job leaf (`MM_IO`), so decoding and
writing a large archive never blocks the emulator thread.

The wrapper lives at
[`src/core/storage/archive.c`](../../src/core/storage/archive.c) and
attaches under `files` from `archive_init`.

## Usage

### Interactive shell

```
> files.archive.identify /tmp/myarchive.sit.hqx
hqx
> ls /tmp/myarchive.sit.hqx
> files.archive.extract /tmp/myarchive.sit.hqx /tmp/out
true
```

### Script form

```
${files.archive.identify("/tmp/myarchive.sit")}
assert ${files.archive.extract("/tmp/myarchive.sit", "/tmp/out")}
```

### Web drag-and-drop

The browser frontend asks the core (`files.archive.identify`) whether a
dropped file is an archive. A recognised archive is not extracted: its
members are probed for media through the archive's VFS path, and only the
medium found is copied out. See [`web.md`](web.md) for the full upload
pipeline.

### Web Filesystem tab

The Filesystem panel expands any file the core marks `expandable` in
its `files.list` result — disk images and archives alike — so archive
contents can be browsed in place. Right-clicking an expandable archive
adds an **Unpack** entry, which calls `files.archive.extract` into a
sibling `<name>_unpacked/` directory and refreshes the tree. A file inside
an image or archive that the core identifies as a medium offers **Insert
into floppy drive**, **Attach as hard disk** or **Insert into CD-ROM drive**
while a machine exists; the core attaches it by its in-archive path. No file
extension is consulted anywhere; see
[`diskImage.ts`](../../app/web2/src/lib/diskImage.ts) and
[`FilesystemView.svelte`](../../app/web2/src/components/panel-views/filesystem/FilesystemView.svelte).

## Build

`src/sources.mk` compiles every `src/peeler/lib/*.c` and
`src/peeler/lib/formats/*.c` into both the WASM module and the headless
binary, with `-Isrc/peeler/include -Isrc/peeler/lib`. peeler also keeps
its own `Makefile` (`make -C src/peeler`, `make -C src/peeler test`) so
it builds and tests on its own. It follows the repository's
`.clang-format` like any other source.

## Updating upstream

Changes are made under `src/peeler/` and committed like any other
source. To publish them to the standalone repository, copy the tree
out:

```bash
# from a granny-smith checkout, into a checkout of https://github.com/pappadf/peeler
rsync -a --delete --exclude .git --exclude .github --exclude build \
    src/peeler/ /path/to/peeler/
cd /path/to/peeler && make && make test
git add -A && git commit -m "Sync from granny-smith"
```

## Testing

- `make -C src/peeler test` runs the corpus in `src/peeler/test/`
  against its recorded checksums. The zip, gzip and tar cases
  (`zip_*`, `gz_*`, `tar_*`: Info-ZIP mixed, all-stored and Zip64
  archives, a Python Zip64 one, self-extractors with and without adjusted
  offsets, a Finder zip with `__MACOSX` companions, single- and
  multi-member gzip, BGZF; GNU, ustar and pax tars, a hard link, a tar in
  gzip, a macOS tar with `._` companions) are
  built by `src/peeler/test/make_archive_fixtures.py` with independent
  writers, and their checksums come from the source files, not from
  peeler.
- `tests/unit/suites/peeler` runs under AddressSanitizer, UBSan and
  LeakSanitizer, and again on wasm32. `zipgz.c` holds the zip, gzip, tar
  and inflate tests: an in-test DEFLATE encoder (stored, fixed and dynamic
  blocks) drives round trips, resumption at every output boundary, and
  refusal of truncated, over-subscribed and out-of-window streams; a
  counting source checks the partial-access contract (a 50 MB zip opens
  in at most three reads, none of its members' data; a stored member's
  bytes are read only when asked for; a BGZF read inflates one block).
- `tests/unit/suites/source` and `tests/unit/suites/image_vfs` cover the
  core's side: decode-through sources, the chunk cache and nested
  archive/image descent.
- Integration rows `archive-fork-unpack` and `archive-confined` cover
  `files.archive.extract`; the web2 e2e specs `filesystem-tab.spec.ts`,
  `url-archive-boot.spec.ts` and `display-drop.spec.ts` cover the UI.

## References

- peeler upstream repository: <https://github.com/pappadf/peeler>
- peeler format notes: `src/peeler/docs/`
- [`source.md`](../internals/core/storage/source.md) — byte sources,
  tiers and the chunk cache.
- [`namespace.md`](../internals/core/vfs/namespace.md) — namespaces and
  the VFS.
