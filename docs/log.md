# Logging Framework

This document describes the implemented, lightweight logging framework for Granny Smith. It provides per‑module categories, per‑category log levels, runtime sink controls, and a printf‑style logging API designed to minimize overhead when messages are disabled.


## Goals and non‑goals

- Goals
  - Let modules (e.g., `cpu`, `floppy`, `appletalk`) register a named logging category and emit messages tagged with that category and an integer level.
  - Allow users/tests to list categories and set a level per category at runtime via shell commands.
  - Level 0 means “no output at all” for that category.
  - Make disabled log sites extremely cheap (no string formatting, minimal branches).
  - Keep dependencies small; portable C; compatible with Emscripten.

- Non‑goals (for now)
  - Persistent configuration (e.g., storing levels in localStorage). See “future extensions”.
  - Rich sinks (JSON, structured fields). Current implementation writes formatted text lines to per‑category sinks.
  - Asynchronous or multi-threaded logging. Environment is effectively single-threaded in the browser; we’ll keep the design thread-safe friendly but not add complexity.


## Terminology

- Category: a named source of log messages (e.g., "cpu"). Modules hold a pointer to their category for fast checks.
- Level: an integer where higher typically means more verbose. Level 0 disables all output for that category.
- Site: a specific `LOG(...)` call in code. Sites pass both the category and the level.


## API surface (log.h)

Header is minimal and C‑friendly. All symbols prefixed with `log_` or `LOG_`.

- Types
  - `typedef struct log_category log_category_t;` (opaque to callers)

- Initialization
  - `void log_init(void);`
    - Optional; idempotent. Prepares internal registry and registers shell commands.

- Category management
  - `log_category_t* log_register_category(const char* name);`
    - Registers or returns existing category by name. On first registration, the level defaults to `0` (OFF).
    - Returns non‑NULL pointer on success; `NULL` on OOM or invalid name.
  - `log_category_t* log_get_category(const char* name);`
    - Looks up existing category; returns `NULL` if not found.
  - `const char* log_category_name(const log_category_t* cat);`
  - `int log_get_level(const log_category_t* cat);`
  - `int log_set_level(log_category_t* cat, int level);`
    - Returns previous level, or negative on error.

- Fast path predicate
  - `static inline int log_would_log(const log_category_t* cat, int level);`
    - True when the message should be emitted (i.e., `level <= log_get_level(cat)` and compile‑time filter, see below).

- Emission (formatted)
  - `void log_emit(const log_category_t* cat, int level, const char* fmt, ...)`
    - `printf`‑style, `__attribute__((format(printf,3,4)))` when available.
  - `void log_vemit(const log_category_t* cat, int level, const char* fmt, va_list ap)`

- Macros for zero‑overhead disable
  - Implicit category (preferred, simplest call site): `LOG(level, fmt, ...)` — uses the file’s implicit category (see below).
  - Explicit category (when overriding the file default): `LOG_WITH(cat, level, fmt, ...)`.
  - Both expand to a cheap level check and call `log_emit` only when enabled.

- Compile‑time filtering (optional)
  - `#define LOG_COMPILE_MIN_LEVEL 0` by default.
    - Sites with `level < LOG_COMPILE_MIN_LEVEL` get compiled out (constant‑folded to no‑op) via macro logic.

- Output sinks
  - Per‑category sinks (implemented in `log.c`): each category can emit to stdout and/or an optional file path (append mode). See “Shell command” below for runtime control.
  - Optional global sink: `typedef void (*log_sink_fn)(const char* line, void* user);` and `void log_set_sink(log_sink_fn fn, void* user);`
    - If a global sink is installed, every formatted line is also forwarded to it in addition to the per‑category sinks.
    - If no global sink is installed (default), only per‑category sinks are used.

### Implicit per‑file category

To enable `LOG(level, ...)` without passing a category each time, a translation unit sets its implicit category once near the top of the file:

- One‑liner that both registers and selects the implicit category:
  - `LOG_USE_CATEGORY_NAME("appletalk");`

- Two‑step explicit form when the category pointer is obtained elsewhere:
  - At top of file: `LOG_DECLARE_LOCAL_CATEGORY(cat_var);`
  - In module init: `cat_var = log_register_category("appletalk");`
  - Then: `LOG_USE_CATEGORY(cat_var);`

If `LOG(level, ...)` is used without setting an implicit category in the file, it should cause a compile‑time error to avoid silent misuse.


## Levels and semantics

- Levels are plain integers; smaller means more important or less verbose in this design (so that `level <= category_level` is “emit”).
- The shell never enforces ranges; it accepts any non‑negative integer. Modules may choose their own fine‑grained levels if desired.


## Internal design (log.c)

- Registry
  - Use a singly‑linked list of categories (`log_category` nodes) because the expected number of categories is small (dozens). Simpler, no dynamic map needed.
  - Each node contains:
    - `char* name;` (owned, NUL‑terminated)
    - `int level;` (current threshold; 0 means off)
    - `struct log_category* next;`
    - Optional `uint16_t id;` if we later want stable IDs.

- Lookups
  - `log_get_category(name)` does a case‑sensitive strcmp scan.
  - Category registration reuses existing category (idempotent) and sets level only the first time. Duplicate registrations are common during module init in tests; this avoids conflicts.

- Fast path check
  - `log_would_log(cat, lvl)` does a plain integer compare and also respects `LOG_COMPILE_MIN_LEVEL` if defined.
  - The `LOG` macro expands to:
    - Branch‑predict hint on the negative path (`__builtin_expect` when available) to keep the disabled case cheap.
    - No call, no `va_list`, no `snprintf` when the message is disabled.

- Emission
  - Build a single line per call: `[name] <level> message\n` (example prefix; see formatting)
  - Formatting pipeline:
    - Small fixed‑size stack buffer (e.g., 512 B) for composing the line using `vsnprintf`. If exceeded, truncate with ellipsis.
    - Write directly to the sink (default: stdout). For Emscripten, stdout maps to console; we may optionally add a JS sink later.

- Thread‑safety
  - Not strictly required for now (browser single‑thread). Keep simple:
    - Registry mutation during early init only.
    - `level` reads/writes are `int`; if we later need threads, we can mark as `atomic_int` without API change.

- Memory and lifetime
  - Category pointers are stable for the process lifetime. Modules cache their `log_category_t*` in static file‑scope variables for fast checks.


## Shell command (src/log.c)

The shell exposes a single, unified `log` command (category: "Logging") with argument‑based configuration. It’s registered by `log_init()` and visible in `help`.

- Grammar
  - `log` — list all categories and their settings
  - `log <cat>` — show one category’s settings
  - `log <cat> [<level> | level=N] [stdout=on|off] [file=<path>|file=off] [ts=on|off]`

- Behavior
  - Unknown categories are auto‑created when you set options for them.
  - Bare integer without a key is treated as `level`.
  - `file=<path>` opens/creates the file in append mode; `file=off` disables the file sink.
  - `stdout=on|off` enables/disables writing to stdout for that category (defaults to `on`).
  - `ts=on|off` toggles including a timestamp prefix based on `cpu_instr_count()`.

- Examples
  ```
  log                         # list all
  log cpu                     # show cpu settings
  log cpu 5                   # set level to 5
  log cpu level=7 stdout=off  # quiet stdout for cpu
  log cpu file=/tmp/cpu.log   # append to file as well
  log cpu file=off            # stop writing to file
  log cpu ts=on               # include instruction-count timestamp in prefix
  log cpu level=10 stdout=on file=/tmp/cpu.log ts=on
  ```


## Usage by modules

- Registration (once) and selecting an implicit category
  - In `src/appletalk.c` (and similarly in `cpu.c`, `floppy.c`, etc.):
    ```c
    #include "log.h"

    // One‑liner: register and use an implicit category for this file
  LOG_USE_CATEGORY_NAME("appletalk");
    ```

  - Or the explicit two‑step form when registration happens in a module init function:
    ```c
    #include "log.h"
    static log_category_t* appletalk_cat;
    LOG_USE_CATEGORY(appletalk_cat);  // set the file’s implicit category symbol

  void appletalk_init(void) {
    appletalk_cat = log_register_category("appletalk");
  }
    ```

- Emitting logs
  - Simplest form (implicit category):
    ```c
    LOG(40 /* DEBUG */, "RX frame len=%u src=%02X:%02X", len, a, b);
    ```
  - Explicit category form when desired:
    ```c
    LOG_WITH(appletalk_cat, 40, "...");
    ```
  - When disabled (`log_get_level(category) < 40`), the macro reduces to a quick branch and returns; arguments are not evaluated or formatted.




## Formatting and output

- Default line format:
  - Without timestamp: `[cpu] 7 message text\n`
  - With timestamp enabled: `[cpu] 7 @12345678 message text\n` where `12345678` is `cpu_instr_count()`.
- Rationale: stable, grep‑friendly, inexpensive to compose.
- Newlines: the framework appends one; callers should not include `\n`.


## Performance design

-- Disabled fast path
  - `LOG_WITH(cat, lvl, ...)` expands roughly to:
    ```c
    do { if (__builtin_expect(log_would_log(cat, lvl), 0)) {
             log_emit(cat, lvl, fmt, __VA_ARGS__);
         } } while (0)
    ```
  - `LOG(level, ...)` is identical but uses the file’s implicit category.
  - `log_would_log` is `static inline` so it compiles to a single compare (+ compile‑time check when constant).
  - No `va_list`, no `snprintf`, and argument expressions are evaluated only if enabled.

- Enabled path
  - Single `vsnprintf` into a stack buffer, then one write to the sink.
  - Buffer size chosen to avoid heap allocations and keep stack modest (e.g., 512B). Truncation strategy is predictable.

- Registry costs
  - Category lookups occur only in shell commands. Normal logging uses cached pointers and simple integer reads.


## Error handling

- Category registration:
  - Returns existing category when called repeatedly with the same name.
  - Returns `NULL` on allocation failure; modules may fall back to a static dummy category whose level is 0 (silent) to keep code safe.
- Shell command:
  - Rejects negative levels; prints `log: invalid level`.
  - `log <cat>` (no modifiers) reports `unknown category "name"` when the category does not exist yet.
  - `log <cat> ...options...` auto-creates the category before applying options, so pre-configuring future categories works.


## Integration points

- `src/log.c` — implementation (registry, shell command, sinks, formatting).
- `src/log.h` — public header used by modules and the shell.
- `src/shell.c` — calls `log_init()`; tokenizer already supports quoted args.
- Build: Makefile already includes `src/*.c` via wildcard.

## Level guidelines and recommendations

Levels are plain integers; higher values are more verbose. Level 0 disables all output for a category. To keep logs consistent and useful across modules/devices, use these guidelines:

- 0 — Off. No output.
- 1 — High‑level, user‑visible events and major state transitions.
  - Examples: emulator start/stop, boot milestones, AFP session open/close, floppy insert/eject, SCSI device attach/detach.
- 2 — Important subsystem actions.
  - Examples: CPU reset/interrupt summary, SCSI command start/finish (opcode + status), floppy seek/format start, Appletalk connection open/close, network service announcements.
- 3 — Routine operation summary.
  - Examples: one‑line per request/response (AFP command names, PAP job creation), floppy read/write summary (track/sector + byte count), short packet summaries (src/dst, type, length).
- 4–5 — Verbose details for troubleshooting.
  - Examples: parsed header fields, parameter values, block numbers, retry notices, unusual but recoverable conditions.
- 6–7 — Very verbose internal flow.
  - Examples: per‑packet field dumps (without full data), per‑call state transitions, register changes that matter for debugging.
- 8–9 — Extreme detail suitable for deep debugging.
  - Examples: full hexdumps of frames/blocks, step‑by‑step protocol negotiations, low‑level register I/O traces.
- 10+ — Trace level.
  - Examples: hot‑path tracing, tight loops, highly repetitive logs used temporarily to diagnose tricky issues.

Notes:
- Pick consistent ranges within a module to make it intuitive (e.g., keep all hexdumps at 9, packet summaries at 3).
- Avoid expensive formatting in low levels (1–3) to keep common logging lightweight.
- Prefer `LOG_WOULD_LOG` style checks or `LOG(...)` itself to guard expensive computations.
