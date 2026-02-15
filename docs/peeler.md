# Peeler Integration

This document describes the **peeler** library integration in the Granny Smith emulator, which provides support for unpacking classic Macintosh archive formats.

## 1. Overview

**peeler** is a modern, portable C99 library for peeling apart classic Macintosh archive formats such as StuffIt (.sit), BinHex (.hqx), Compact Pro (.cpt), and MacBinary (.bin). It is integrated as a git submodule and provides a `peeler` shell command for extracting archives within the emulator's MEMFS environment.

The library is developed independently at https://github.com/pappadf/peeler and integrated into this project to enable users to extract vintage Macintosh software and data files directly within the emulated environment.

## 2. Architecture

### 2.1. Integration Method

peeler is integrated using the **separate library build** approach:

* **Git Submodule:** Located at `third-party/peeler/`, tracks the main branch
* **Build System:** Source files compiled directly alongside the emulator
* **Linkage:** Compiled as part of the main WASM module during build

### 2.2. Buffer-Based API

peeler uses a **buffer-based processing model** where archives are read into memory and extracted in a single pass:

```
peel_read_file(path) → peel(src, len, &err) → peel_file_list_t
```

The library automatically detects and handles nested formats (e.g. `.sit.hqx`). The extracted result is a flat list of files, each with:

* `meta` - File metadata (name, Mac type/creator, Finder flags)
* `data_fork` - Main file content as a buffer
* `resource_fork` - Resource fork as a buffer (if present)

## 3. Shell Integration

### 3.1. Command Interface

The peeler library is exposed through a shell command registered in `src/core/shell/shell.c`:

```
peeler [-o <dir>] [-v] <archive1> [<archive2> ...]
```

**Options:**
* `-o <dir>` - Extract files to specified directory (default: current directory)
* `-v, --verbose` - Enable verbose output showing each extracted file
* `-p, --probe` - Test format detection without extracting
* `-h, --help` - Display help message

**Supported Formats:**
* StuffIt archives (.sit) - versions 1.x through 5.x
* BinHex 4.0 (.hqx) - ASCII-armored format with CRC validation
* Compact Pro (.cpt) - compressed archive format
* MacBinary (.bin) - file wrapper preserving Mac metadata

### 3.2. Implementation Files

| File                  | Purpose                                           |
| --------------------- | ------------------------------------------------- |
| `src/core/shell/peeler_shell.c`  | Shell command implementation and peeler API usage |
| `src/core/shell/peeler_shell.h`  | Public interface for shell integration            |
| `third-party/peeler/` | peeler library submodule                          |

The shell integration (`peeler_shell.c`) wraps the peeler library API and provides:

* Command-line parsing compatible with shell tokenizer
* File extraction to MEMFS with directory creation
* Progress reporting and error handling
* Resource fork handling (currently no-op for MEMFS)

## 4. Build Process

### 4.1. Makefile Integration

The `Makefile` includes peeler source files directly:

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

Include paths are added via `PEELER_INCLUDES = -I$(PEELER_DIR)/include -I$(PEELER_DIR)/lib`.

### 4.2. Build Time Impact

Peeler adds minimal overhead to clean builds as it compiles directly with the emulator sources.

## 5. Usage Examples

### 5.1. Extracting a BinHex-encoded StuffIt Archive

```
peeler myarchive.sit.hqx
```

This automatically:
1. Reads the file into memory
2. Detects BinHex encoding and nested StuffIt archive
3. Peels all layers and extracts files
4. Writes extracted files to current directory

### 5.2. Extracting Multiple Archives to Specific Directory

```
mkdir /extracted
peeler -o /extracted -v archive1.sit archive2.cpt archive3.bin
```

### 5.3. Typical Workflow

```
# Upload archive to MEMFS (via web UI file upload)
ls
# Shows: myapp.sit.hqx

# Extract archive
peeler myapp.sit.hqx
# Output: Extracting: MyApp
#         Successfully extracted 'myapp.sit.hqx' (1 file)

# Verify extraction
ls
# Shows: myapp.sit.hqx  MyApp
```

## 6. Implementation Details

### 6.1. File Extraction Process

The extraction process in `peeler_shell.c` follows this sequence:

1. **Read & Peel:** `peel_path()` reads the file and peels all layers automatically
2. **Iterate Files:** Loop over `peel_file_list_t.files[]` array
3. **Write Output:** Write each file's data fork to MEMFS, skip resource forks
4. **Cleanup:** Free the file list via `peel_file_list_free()`

### 6.2. Mac Metadata Handling

Classic Mac files contain:
* **Data Fork:** Main file content
* **Resource Fork:** Additional structured data (icons, code, etc.)
* **Finder Info:** Type/creator codes, flags

In the MEMFS environment:
* Data forks are written as regular files
* Resource forks are available but not persisted (not needed for most use cases)
* Finder metadata is available in `peel_file_meta_t` but not stored (no filesystem support)

## 7. Updating peeler

To update to a newer version of peeler:

```bash
cd third-party/peeler
git pull origin main
cd ../..
make clean
make
git add third-party/peeler
git commit -m "Update peeler to latest version"
```

The submodule tracks specific commits, so updates are explicit and controlled.

## 8. Development Notes

### 8.1. Error Handling

peeler uses explicit error objects:
* `peel_err_t *` - NULL means no error
* `peel_err_msg(err)` - Get human-readable error message
* `peel_err_free(err)` - Free error object

The shell integration reports errors via `fprintf(stderr, ...)` which appears in the emulator's console output.

### 8.2. Memory Management

* The library allocates buffers internally
* Caller frees via `peel_free()` for buffers, `peel_file_list_free()` for file lists
* No persistent memory caching — each operation is independent

### 8.3. Format Detection

`peel_detect(src, len)` identifies the outermost format without extracting:
* Returns a short name ("hqx", "bin", "sit", "cpt") or NULL if unknown
* Used by the `--probe` shell option

## 9. Testing

To test peeler integration:

1. Build project: `make`
2. Start emulator with test archives in MEMFS
3. Use shell command: `peeler test.sit`
4. Verify extracted files: `ls`

End-to-end tests can be added to `tests/e2e/` using Playwright to:
* Upload test archives
* Execute peeler commands via shell
* Verify extracted file presence

## 10. Future Enhancements

Potential improvements:
* Support for AppleDouble format in MEMFS (store resource forks separately)
* Progress callbacks for large archives
* Integration with file upload UI (auto-detect and extract archives)
* Command completion for archive filenames

## 11. References

* peeler repository: https://github.com/pappadf/peeler
* peeler documentation: `third-party/peeler/docs/`
