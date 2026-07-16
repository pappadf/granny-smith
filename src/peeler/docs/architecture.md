# libpeeler — Architecture

## 1  Introduction

**libpeeler** is a small C99 library for peeling apart classic Macintosh
archive formats: StuffIt (`.sit`), Compact Pro (`.cpt`), BinHex (`.hqx`), and
MacBinary (`.bin`).  Formats can be arbitrarily nested (e.g. `.sit.hqx`) and
libpeeler discovers and peels the layers automatically.

The name follows the metaphor: classic Mac archives are the tough skin wrapped
around fragile Mac files (protecting those Resource and Data forks) for
transport over early networks.  libpeeler is the tool that strips away each
`.sit` or `.hqx` encoding layer to expose the raw contents inside.  The `lib`
prefix follows the standard UNIX C-library convention (`libpng`, `libcurl`).

### 1.1  Goals

- **Simplicity above all.**  The library should be easy to understand, easy
  to embed, and easy to extend with new formats.
- **Correctness.**  Produce bit-identical output for every supported archive.
- **Minimal dependencies.**  C99 standard library only.  No external packages.

### 1.2  Non-Goals

- Real-time streaming decompression.
- Thread safety (callers serialize externally if needed).
- Compression / archive creation.

---

## 2  Design Principles

1. **Buffers, not streams.**  Every processing stage takes a complete input
   buffer and produces complete output buffer(s).  No partial reads, no state
   machines, no resume logic.

2. **Two kinds of operations.**  A *transform* converts one buffer into one
   buffer (e.g. BinHex decode, MacBinary unwrap).  An *extract* converts one
   buffer into a list of named files (e.g. StuffIt archive).  These are
   separate concepts with separate types.

3. **Explicit ownership.**  Every buffer returned by the library is
   `malloc`-allocated and owned by the caller.  The caller frees it.
   No hidden shared state, no reference counting.

4. **Composition over inheritance.**  Nesting (`.sit.hqx`) is handled by
   calling transforms in sequence — the output buffer of one becomes the
   input of the next.  No layer-wrapping machinery is needed.

5. **Fail fast, fail loud.**  Errors are reported immediately with a
   descriptive message.  No mid-operation recovery.  Functions return a
   simple success/error status; on error the caller inspects a message.

6. **No threading concerns in the core.**  The library is single-threaded.
   If applications need thread safety they can serialize calls externally.
   This eliminates thread-local storage and synchronization complexity.

---

## 3  Core Data Types

### 3.1  Byte Buffer

```c
// A contiguous, owned byte buffer.
// Created by the library, freed by the caller via peel_free().
typedef struct {
    uint8_t *data;     // Pointer to buffer contents
    size_t   size;     // Number of valid bytes
} peel_buf_t;
```

The library provides `peel_free(peel_buf_t *buf)` which calls `free()` on
the data pointer and zeros the struct.  Callers may also free `buf->data`
directly if they prefer.

### 3.2  File Metadata

```c
// Metadata for a single file extracted from an archive.
typedef struct {
    char     name[256];       // Original filename (null-terminated)
    uint32_t mac_type;        // Classic Mac file type  (e.g. 'TEXT')
    uint32_t mac_creator;     // Classic Mac creator    (e.g. 'ttxt')
    uint16_t finder_flags;    // Finder flags
} peel_file_meta_t;
```

Metadata fields are best-effort: not all formats populate all fields.
Unpopulated fields are zeroed.

### 3.3  Extracted File

```c
// A single file produced by archive extraction.
// Both forks are always present; unused forks have size == 0.
typedef struct {
    peel_file_meta_t meta;
    peel_buf_t       data_fork;       // May be empty (size 0)
    peel_buf_t       resource_fork;   // May be empty (size 0)
} peel_file_t;
```

### 3.4  File List

```c
// A flat list of files produced by archive extraction.
typedef struct {
    peel_file_t *files;    // Array of extracted files
    int            count;    // Number of entries
} peel_file_list_t;
```

The library provides `peel_file_list_free(peel_file_list_t *list)` which
frees every buffer inside every file, then frees the array itself.

### 3.5  Error Context

```c
// Opaque error state.  NULL means no error.
// Obtain the message with peel_err_msg().
typedef struct peel_err peel_err_t;

const char *peel_err_msg(const peel_err_t *err);
void        peel_err_free(peel_err_t *err);
```

Every fallible function takes `peel_err_t **err` as its last parameter.
On failure, the function allocates an error, writes the pointer through
`*err`, and returns an obvious sentinel value (NULL pointer or 0 count).
On success, `*err` is set to NULL.  This pattern avoids global state
entirely:

```c
peel_err_t *err = NULL;
peel_buf_t out = peel_hqx(input.data, input.size, &err);
if (err) {
    fprintf(stderr, "HQX failed: %s\n", peel_err_msg(err));
    peel_err_free(err);
    return 1;
}
```

---

## 4  API Surface

### 4.1  Input Helpers

```c
// Read an entire file into a buffer.
peel_buf_t peel_read_file(const char *path, peel_err_t **err);

// Wrap an existing pointer (copies the data, caller keeps ownership of src).
peel_buf_t peel_buf_copy(const void *src, size_t len, peel_err_t **err);

// Wrap an existing pointer WITHOUT copying.  Caller guarantees the pointer
// remains valid until the buffer is no longer used.  peel_free() on this
// buffer is a no-op for the data pointer.
peel_buf_t peel_buf_wrap(const void *src, size_t len);
```

### 4.2  Transform Functions (one buffer in → one buffer out)

Each transform peels a single encoding layer.  Input is a raw byte pointer
and length (not a `peel_buf_t`) so callers aren't forced to create structs:

```c
// BinHex 4.0:  .hqx → decoded binary (header + data fork + resource fork
// concatenated, or just the data fork — see §4.4 for multi-fork handling).
peel_buf_t peel_hqx(const uint8_t *src, size_t len,
                               peel_err_t **err);

// MacBinary:  .bin → inner payload.
peel_buf_t peel_bin(const uint8_t *src, size_t len,
                               peel_err_t **err);
```

Transforms that produce a single file with both forks (HQX, MacBinary) return
a `peel_file_t` instead of a bare buffer, so both forks and metadata are
available:

```c
// Richer variant that preserves fork separation and metadata.
peel_file_t peel_hqx_file(const uint8_t *src, size_t len,
                                     peel_err_t **err);

peel_file_t peel_bin_file(const uint8_t *src, size_t len,
                                     peel_err_t **err);
```

### 4.3  Extract Functions (one buffer in → list of files out)

Archive formats produce multiple files:

```c
// StuffIt classic / SIT5:
peel_file_list_t peel_sit(const uint8_t *src, size_t len,
                                      peel_err_t **err);

// Compact Pro:
peel_file_list_t peel_cpt(const uint8_t *src, size_t len,
                                      peel_err_t **err);
```

### 4.4  Unified "Just Peel Everything" Entry Point

The main convenience function handles detection, chaining, and extraction
in one call:

```c
// Detect format(s), peel all layers, return extracted files.
// Handles arbitrarily nested formats (e.g. .sit.hqx, .sit.bin.hqx).
// Returns all peeled files, or an empty list with an error.
peel_file_list_t peel(const uint8_t *src, size_t len,
                                 peel_err_t **err);

// Same, but reads from a file path.
peel_file_list_t peel_path(const char *path,
                                      peel_err_t **err);
```

### 4.5  Format Detection

```c
// Identify the outermost format without peeling.
// Returns a format name string ("hqx", "bin", "sit", "cpt", etc.)
// or NULL if unrecognized.
const char *peel_detect(const uint8_t *src, size_t len);
```

---

## 5  How Nesting Works

The key insight: nested formats like `.sit.hqx` are just function
composition.  Each encoding layer is a skin to peel — once peeled, the next
layer is exposed and the process repeats.

### 5.1  `peel` Implementation Sketch


```c
peel_file_list_t peel(const uint8_t *src, size_t len,
                                 peel_err_t **err) {
    uint8_t *cur = NULL;         // current working buffer (owned)
    size_t   cur_len = len;
    const uint8_t *cur_ptr = src;
    bool owns_cur = false;

    // Repeatedly peel off transform layers.
    for (;;) {
        const char *fmt = peel_detect(cur_ptr, cur_len);
        if (!fmt) break;

        if (strcmp(fmt, "hqx") == 0) {
            peel_buf_t decoded = peel_hqx(cur_ptr, cur_len, err);
            if (*err) goto fail;
            if (owns_cur) free(cur);
            cur = decoded.data;  cur_len = decoded.size;  owns_cur = true;
            cur_ptr = cur;
            continue;
        }
        if (strcmp(fmt, "bin") == 0) {
            peel_buf_t decoded = peel_bin(cur_ptr, cur_len, err);
            if (*err) goto fail;
            if (owns_cur) free(cur);
            cur = decoded.data;  cur_len = decoded.size;  owns_cur = true;
            cur_ptr = cur;
            continue;
        }

        // Terminal formats: peel and return.
        peel_file_list_t result;
        if (strcmp(fmt, "sit") == 0) {
            result = peel_sit(cur_ptr, cur_len, err);
        } else if (strcmp(fmt, "cpt") == 0) {
            result = peel_cpt(cur_ptr, cur_len, err);
        } else {
            break;  // Unknown terminal format
        }
        if (owns_cur) free(cur);
        return result;
    }

    // No archive found — wrap the (possibly transformed) buffer as a
    // single-file result so the caller always gets a file list.
    peel_file_list_t result = wrap_as_single_file(cur_ptr, cur_len, err);
    if (owns_cur) free(cur);
    return result;

fail:
    if (owns_cur) free(cur);
    return (peel_file_list_t){0};
}
```

This is the *entire* format chaining logic.  Just a loop that peels
layers one at a time until the contents are fully exposed.

### 5.2  Detection Order

Detection is a simple priority list of magic-byte checks:

| Priority | Format | Probe |
|----------|--------|-------|
| 1 | BinHex 4.0 | `"(This file must be converted with BinHex"` anywhere in first 256 bytes |
| 2 | MacBinary | Header CRC check on 128-byte header |
| 3 | StuffIt classic | `"SIT!"` / `"ST46"` / etc. at offset 0, `"rLau"` at offset 10 |
| 4 | StuffIt 5 | `"StuffIt (c)1997-"` at offset 0 |
| 5 | Compact Pro | Magic byte `0x01` + volume byte `0x01` + structure validation |

Transform formats (HQX, MacBinary) are checked first because they wrap
archive formats.  Archive formats are checked last because they are
terminal (they produce file lists, not buffers to further decode).

---

## 6  Memory Management

### 6.1  Allocation Strategy

Every `peel_*` function does its own allocation:

- **Transforms** allocate a single output buffer sized to the known or
  estimated uncompressed length, growing with `realloc` if needed.
- **Archive peelers** allocate per-file buffers.  For formats where file
  sizes are known from directory headers (SIT, CPT), each buffer is
  allocated to exact size.  For unknown sizes, start at a reasonable
  estimate and grow.
- **The archive blob itself** may be kept in memory while peeling is
  in progress (since archive peelers need random access to it).  It is
  not copied — the peeler works from the caller's input pointer directly.

### 6.2  Peak Memory

For a `.sit.hqx` file of size *S*:

| Phase | Allocated | Notes |
|-------|-----------|-------|
| Read file | *S* | Original file contents |
| Peel HQX | ~0.75×*S* | Decoded binary (smaller than HQX text) |
| After HQX | ~0.75×*S* | Original freed; peeled buffer kept |
| Peel SIT | Σ file sizes | Each file allocated individually |
| After SIT | Σ file sizes | Decoded SIT blob freed |

Peak memory ≈ *S* + 0.75×*S* during the HQX peeling phase, then drops.  For
a 10 MB `.sit.hqx`, peak is ~17.5 MB.

For the largest realistic classic Mac archives (~50 MB), peak memory stays
under 100 MB — entirely acceptable on any modern machine.  The buffer-based
approach uses more memory than a streaming architecture would, but classic Mac
archives are small by today's standards, and the simplicity gain is enormous.

### 6.3  Ownership Rules

1. Input data is **borrowed** (const pointer).  The library never modifies or
   frees the input.
2. Output data is **owned by the caller**.  The library allocates; the caller
   must free (via `peel_free`, `peel_file_list_free`, or plain `free`).
3. Intermediate buffers (between chained transforms) are managed internally
   by `peel` and freed before it returns.
4. Error objects are owned by the caller and freed via `peel_err_free`.

No shared ownership.  No reference counting.  No hidden aliases.

---

## 7  Error Handling

### 7.1  Error Object

```c
struct peel_err {
    char message[512];
};
```

Allocated with `malloc`, populated with `snprintf`, returned through an
output parameter.  The caller checks for non-NULL and reads the message.

The output-parameter pattern is deliberate:
- A global error string silently overwrites the previous error, which is a
  debugging hazard.
- A global couples every function to hidden state.
- The output-parameter approach is equally simple and avoids both problems.

### 7.2  Internal Error Creation

```c
// Internal helper — not part of the public API.
static peel_err_t *make_err(const char *fmt, ...) {
    peel_err_t *e = malloc(sizeof(*e));
    if (!e) return NULL;  // OOM while reporting an error — tough luck
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
    return e;
}
```

Each peel function uses early-return on error:

```c
if (src_len < 128) {
    *err = make_err("MacBinary header too short (%zu bytes)", src_len);
    return (peel_buf_t){0};
}
```

### 7.3  setjmp/longjmp for Deep Errors

Some decompressors (StuffIt method 15's arithmetic coder, LZW decoders)
detect errors deep inside nested call stacks.  Propagating error codes
through every return is tedious.  For these cases, use `setjmp`/`longjmp`:

```c
typedef struct {
    jmp_buf  jmp;
    char     errmsg[256];
} decode_ctx_t;

static void decode_abort(decode_ctx_t *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->errmsg, sizeof(ctx->errmsg), fmt, ap);
    va_end(ap);
    longjmp(ctx->jmp, 1);
}

peel_buf_t peel_sit15(const uint8_t *src, size_t len,
                                     peel_err_t **err) {
    decode_ctx_t ctx = {0};
    if (setjmp(ctx.jmp)) {
        *err = make_err("SIT15: %s", ctx.errmsg);
        return (peel_buf_t){0};
    }
    // ... deep decompression code can call decode_abort(&ctx, ...) ...
}
```

This keeps the hot path free of error-checking boilerplate while still
surfacing errors cleanly through the public API.

---

## 8  Format Handler Registration

The `peel` entry point needs to try formats automatically.  This is
driven by a small static handler table.

### 8.1  Handler Table

```c
typedef enum {
    PEEL_FMT_WRAPPER,      // one buffer in → one buffer out
    PEEL_FMT_ARCHIVE,      // one buffer in → file list out
} peel_fmt_kind_t;

typedef struct {
    const char         *name;
    peel_fmt_kind_t   kind;
    // Returns true if this format's magic bytes match.
    bool (*detect)(const uint8_t *src, size_t len);
    // Only one of these is non-NULL, depending on kind:
    peel_buf_t       (*peel)(const uint8_t *src, size_t len,
                               peel_err_t **err);
    peel_file_list_t (*peel_archive)(const uint8_t *src, size_t len,
                                       peel_err_t **err);
} peel_format_t;
```

### 8.2  Static Registration

```c
static const peel_format_t g_formats[] = {
    // Transforms first (they peel wrappers to reveal archives)
    { "hqx", PEEL_FMT_WRAPPER, hqx_detect, peel_hqx, NULL },
    { "bin", PEEL_FMT_WRAPPER, bin_detect, peel_bin,  NULL },
    // Archive peelers last (they are terminal)
    { "sit", PEEL_FMT_ARCHIVE, sit_detect, NULL, peel_sit },
    { "cpt", PEEL_FMT_ARCHIVE, cpt_detect, NULL, peel_cpt },
};
```

Adding a new format means: (1) write a `detect` function, (2) write a
`peel_<fmt>` function, (3) add one line to the table.

---

## 9  Multi-Fork Handling

Classic Macintosh files have two forks: data and resource.  libpeeler handles
this by always including both forks in the result structure.

### 9.1  Both Forks in the Result

Every `peel_file_t` has both `data_fork` and `resource_fork` fields.
Extractors populate whichever forks exist; unused forks have `size == 0`.

This means:
- No iterator state.
- No "which fork am I reading?" ambiguity.
- The caller can inspect both forks directly.
- Writing an AppleDouble file is straightforward: check if
  `resource_fork.size > 0` and emit accordingly.

### 9.2  Single-File Transforms

For wrapper formats that contain a single file (HQX, MacBinary), the
`_peel_hqx_file()` variant returns a `peel_file_t` with metadata and both
forks.  The simpler `_peel_hqx()` variant returns only the data fork as a
raw buffer, which is what `peel`'s chaining loop uses.

---

## 10  CLI Design

The `peeler` command-line tool is a thin wrapper around the library:

```c
int main(int argc, char **argv) {
    // Parse args to get input_path, output_dir, flags...

    peel_err_t *err = NULL;
    peel_file_list_t files = peel_path(input_path, &err);
    if (err) {
        fprintf(stderr, "Error: %s\n", peel_err_msg(err));
        peel_err_free(err);
        return 1;
    }

    for (int i = 0; i < files.count; i++) {
        peel_file_t *f = &files.files[i];
        // Write data fork
        if (f->data_fork.size > 0) {
            write_file(output_dir, f->meta.name, f->data_fork);
        }
        // Write resource fork (AppleDouble, etc.)
        if (f->resource_fork.size > 0) {
            write_appledouble(output_dir, f->meta.name,
                              f->resource_fork, f->meta);
        }
    }

    peel_file_list_free(&files);
    return 0;
}
```

No iteration state, no streaming read loop, no fork-tracking bookkeeping.

---

## 11  Design Rationale

### 11.1  Why Buffers Instead of Streams

A streaming (pull-based) architecture where each decoder is a state machine
that yields bytes on demand would use less peak memory.  However:

- Most archive formats (StuffIt, Compact Pro) require random access to
  internal directory structures, so the archive must be in memory anyway.
- State machines that can yield partial results, resume, and track position
  are the single biggest source of complexity in a decompression library.
- Classic Macintosh archives are small by modern standards — rarely larger
  than 50 MB.  Even 100 MB of peak RAM is negligible.

The buffer-based model trades a modest amount of extra memory for dramatically
simpler decoders.  Each decoder is a plain function: bytes in, bytes out.
No state machines, no partial-read tracking, no "have I been initialized?"
flags.

### 11.2  Why Wrappers and Archives Are Separate Types

Encoding wrappers (BinHex, MacBinary) peel one layer and produce a single
buffer.  Archive formats (StuffIt, Compact Pro) produce a list of named
files.  Conflating these behind a single interface forces every format to
implement capabilities it doesn't need and obscures what each function
actually does.

Keeping them separate makes the API self-documenting:
- `peel_hqx()` — peels one wrapper layer, returns a buffer.
- `peel_sit()` — peels an archive, returns a list of files.

### 11.3  Why Error Objects Instead of Global State

A thread-local error string (set by the library, read by the caller) is
simple but has two problems: it silently overwrites previous errors, and it
couples every function to hidden global state.  The output-parameter pattern
(`peel_err_t **err`) is equally concise and avoids both issues.

---

## 12  Repository Layout

```
include/peeler.h             Public API (types + all function declarations)
lib/
  peeler.c                   peel(), detection, helpers
  err.c                      Error object creation and formatting
  formats/
    hqx.c                    BinHex 4.0 decoder
    bin.c                    MacBinary decoder
    sit.c                    StuffIt classic + SIT5 peeler
    sit13.c                  SIT method 13 decompressor (helper)
    sit15.c                  SIT method 15 decompressor (helper)
    cpt.c                    Compact Pro peeler
cmd/
  main.c                     CLI entry point (`peeler` binary)
test/
  test_hqx.c                 Per-format unit tests
  test_bin.c
  test_sit.c
  test_cpt.c
  test_peel.c                Integration tests (nested formats)
  testfiles/                 Sample archives and expected checksums
docs/
  internals/                 Format specifications (one .md per format)
```

Each file under `lib/formats/` is self-contained: it includes `peeler.h` for
types and exposes its `detect` + `peel_<fmt>` function.  No internal
header is needed for cross-format concerns because there are none — each
format is independent.

A small `lib/internal.h` may exist for shared helpers (big-endian reads,
CRC routines, buffer growth), but the emphasis is on keeping cross-cutting
dependencies minimal.

---

## 13  Adding a New Format

1. Write a format spec in `docs/internals/<name>.md`.
2. Create `lib/formats/<name>.c` with:
   - A `bool <name>_detect(src, len)` function.
   - A `peel_<name>()` function (returns a buffer or file list
     depending on whether the format is a wrapper or an archive).
3. Add one entry to `g_formats[]` in `lib/peeler.c`.
4. Declare the public function in `include/peeler.h`.
5. Add test cases under `test/`.

No vtable to implement, no lifecycle contract to satisfy, no layer lifetime
management to get right.

---

## 14  Future Considerations

### 14.1  Very Large Files

If a future format requires handling files too large to buffer (>100 MB),
a targeted streaming API can be added for that specific format without
changing the overall architecture:

```c
// Optional streaming interface for very large archives.
typedef struct peel_stream peel_stream_t;
peel_stream_t *peel_stream_open(const char *path, peel_err_t **err);
int              peel_stream_next(peel_stream_t *s, peel_file_t *out,
                                    peel_err_t **err);
void             peel_stream_close(peel_stream_t *s);
```

This should only be added if a concrete need arises.  YAGNI.

### 14.2  Progress Callbacks

For GUI integration, a simple progress callback can be threaded through
the extract functions:

```c
typedef void (*peel_progress_fn)(int files_done, int files_total,
                                   void *user_data);
```

This is trivial to add since the extractor knows the total file count up
front (from the archive directory).

### 14.3  Cancellation

A cancellation mechanism can use the same callback: if it returns non-zero,
the extractor aborts and returns a partial result or error.  This is clean
in the buffer model because there is no streaming state to unwind.

---

## 15  Summary

The core idea is: **treat decompression as pure functions on byte buffers.**

- `bytes → bytes` for wrappers (HQX, MacBinary).
- `bytes → [files]` for archives (StuffIt, Compact Pro).
- Nesting = peeling layers via sequential function calls.
- All allocation is explicit.  All ownership is caller-side.
- Errors are values, not global state.

This trades a modest amount of extra memory for dramatically simpler
implementation, testing, and mental model.  For the realistic scale of classic
Macintosh archives, this tradeoff is overwhelmingly favorable.
