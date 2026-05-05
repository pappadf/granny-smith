# Archive (peeler) Integration

This document describes how the emulator unpacks classic Macintosh
archive formats. The underlying library is **peeler**; the
emulator-facing surface is the `archive` namespace on the object tree.

## Overview

**peeler** is a portable C99 library for reading classic Macintosh
archive formats: StuffIt (`.sit`, versions 1.x–5.x), BinHex 4.0
(`.hqx`), Compact Pro (`.cpt`), and MacBinary (`.bin`). It auto-
detects nested formats so a `.sit.hqx` file unpacks in a single call.

The library is developed independently at
<https://github.com/pappadf/peeler> and integrated as a git submodule.
The emulator wraps it as `archive` on the object tree so users and
scripts never see the library name; from their point of view, the
emulator just knows how to identify and extract Mac archives.

## Object-model surface

| Path | Result | Description |
|------|--------|-------------|
| `archive.identify(path)` | `V_STRING` — `"sit"` / `"hqx"` / `"cpt"` / `"bin"` / `"sea"`, or empty string when the file isn't a recognised archive | Format probe; doesn't extract |
| `archive.extract(path, [out_dir])` | `V_BOOL` — `true` on success | Extract every file in the archive to `out_dir` (defaults to the current working directory) |

Empty / missing return values follow the predicate-truthy rule: an
unrecognised file produces an empty string, which scripts can test as
a falsy `${...}`.

`archive.identify` and `archive.extract` are reachable everywhere the
object model is reachable: the interactive shell, headless scripts,
the JavaScript bridge (`gsEval('archive.identify', [path])`), and the
inspector UI.

## Architecture

### Integration

The peeler library is integrated using a **separate library build**:

- **Submodule** at `third-party/peeler/`, tracking the upstream main
  branch.
- **Build** — sources compiled directly alongside the emulator, with
  include paths added in the top-level `Makefile`.
- **Linkage** — peeler objects link into both the WASM module and the
  headless binary.

### API

peeler's processing model is buffer-based:

```
peel_read_file(path) → peel(src, len, &err) → peel_file_list_t
```

The library detects nested formats automatically. The result is a flat
list of files, each with:

- `meta` — file metadata (name, Mac type/creator, Finder flags)
- `data_fork` — main file content
- `resource_fork` — resource fork (when present)

### Wrapper

The emulator-side wrapper lives at
[`src/core/storage/archive.c`](../src/core/storage/archive.c). It:

- Owns the `archive` class descriptor and registers the object as a
  process-singleton from `archive_init` (called by `shell_init`).
- Implements `archive_identify_file(path)` and
  `archive_extract_file(path, out_dir)` — small C wrappers over the
  peeler API that the typed methods bind to.
- Discards resource forks on extraction. The emulator filesystems
  don't model resource forks, and the only callers want the data
  fork (disk images, system files unpacked from `.sit` / `.hqx`).

## Supported formats

- StuffIt archives (`.sit`) — versions 1.x through 5.x
- BinHex 4.0 (`.hqx`) — ASCII-armored format with CRC validation
- Compact Pro (`.cpt`) — compressed archive format
- MacBinary (`.bin`) — file wrapper preserving Mac metadata
- Self-extracting archives (`.sea`)

## Build process

The top-level `Makefile` adds the peeler sources directly:

```makefile
PEELER_DIR := third-party/peeler

PEELER_SRC := $(PEELER_DIR)/lib/peeler.c \
              $(PEELER_DIR)/lib/err.c \
              $(PEELER_DIR)/lib/util.c \
              $(PEELER_DIR)/lib/formats/bin.c \
              $(PEELER_DIR)/lib/formats/cpt.c \
              $(PEELER_DIR)/lib/formats/hqx.c \
              $(PEELER_DIR)/lib/formats/sit.c \
              $(PEELER_DIR)/lib/formats/sit13.c \
              $(PEELER_DIR)/lib/formats/sit15.c
```

Include paths are added via `PEELER_INCLUDES = -I$(PEELER_DIR)/include
-I$(PEELER_DIR)/lib`. peeler adds minimal overhead to clean builds.

## Usage

### Interactive shell

```
> archive.identify /tmp/myarchive.sit.hqx
hqx
> archive.extract /tmp/myarchive.sit.hqx /tmp/out
true
```

### Script form

```
${archive.identify("/tmp/myarchive.sit")}
assert ${archive.extract("/tmp/myarchive.sit", "/tmp/out")}
```

### Web drag-and-drop

The browser frontend probes dropped files in this order: ZIP via
JSZip, Mac archive via `archive.identify`, then media-format probes.
Recognised archives are extracted to a staging directory and their
contents are re-probed for media. See [`web.md`](web.md) for the full
upload pipeline.

## Implementation details

### File extraction flow

1. **Read & peel** — `peel_path()` reads the file and peels all layers
   automatically.
2. **Iterate files** — walk `peel_file_list_t.files[]`.
3. **Write output** — write each file's data fork to the destination
   directory, creating parent directories as needed.
4. **Cleanup** — `peel_file_list_free()`.

### Mac metadata handling

Classic Mac files contain:

- **Data fork** — main file content
- **Resource fork** — additional structured data (icons, code, etc.)
- **Finder info** — type/creator codes, flags

In the emulator's filesystems:

- Data forks are written as regular files.
- Resource forks are discarded (no filesystem support; not needed for
  the disk-image / boot-floppy use cases that drive most extractions).
- Finder metadata is available in `peel_file_meta_t` but not stored.

## Error handling

peeler uses explicit error objects:

- `peel_err_t *` — `NULL` means no error.
- `peel_err_msg(err)` — get human-readable message.
- `peel_err_free(err)` — free the error object.

The wrapper translates these into method-call failures (`V_BOOL false`
for `archive.extract`, empty string for `archive.identify`). Detailed
messages still print to stderr via `fprintf` so scripts get a useful
trace when a recognised archive fails to unpack.

## Updating peeler

```bash
cd third-party/peeler
git pull origin main
cd ../..
make clean
make
git add third-party/peeler
git commit -m "Update peeler to latest version"
```

The submodule tracks specific commits, so updates are explicit and
controlled.

## Testing

Integration coverage lives in `tests/e2e/specs/peeler/peeler.spec.ts`,
which uploads test archives, calls `archive.identify` /
`archive.extract` via `gsEval`, and verifies the extracted files
appear in OPFS. The library itself ships unit tests in
`third-party/peeler/`.

## References

- peeler repository: <https://github.com/pappadf/peeler>
- peeler documentation: `third-party/peeler/docs/`
- [`docs/object-model.md`](object-model.md) — the surface
  `archive.*` participates in.
