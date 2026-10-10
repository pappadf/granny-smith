# Logging Framework

This document describes the implemented, lightweight logging framework for Granny Smith. It provides per‑module categories, per‑category log levels, runtime sink controls, and a printf‑style logging API designed to minimize overhead when messages are disabled.


## Goals and non‑goals

- Goals
  - Let modules (e.g., `cpu`, `floppy`, `appletalk`) register a named logging category and emit messages tagged with that category and an integer level.
  - Allow users/tests to list categories and set a level per category at runtime via shell commands.
  - Setting a category to level 0 suppresses every `LOG(n, …)` site with
    `n >= 1`. It does **not** silence `LOG(0, …)` — see “Level 0 is the
    always-on level” below, which is a deliberate convention with ~200 sites,
    not an accident.
  - Make disabled log sites extremely cheap (no string formatting, minimal branches).
  - Keep dependencies small; portable C; compatible with Emscripten.

- Non‑goals (for now)
  - Persistent configuration (e.g., storing levels in localStorage). See “future extensions”.
  - Rich sinks (JSON, structured fields). Current implementation writes formatted text lines to per‑category sinks.
  - Asynchronous logging. `LOG(...)` is called from the emulator thread (the tick, every leaf) and from the job thread (the interpreter); the sink is a synchronous write in the caller's thread. A line from a leaf serving a request travels to that client through the output sink (`out.h`) like any other printed text; the per-line `log` event (`gs_event_emit`) is the structured stream the browser's Logs view reads.


## Terminology

- Category: a named source of log messages (e.g., "cpu"). Modules hold a pointer to their category for fast checks.
- Level: an integer where higher typically means more verbose. Setting a
  category to 0 is the “off” position for ordinary sites (level 1 and up);
  level-0 SITES still emit, by design — see below.
- Site: a specific `LOG(...)` call in code. Sites pass both the category and the level.


## API surface (log.h)

Header is minimal and C‑friendly. All symbols prefixed with `log_` or `LOG_`.

- Types
  - `typedef struct log_category log_category_t;` (opaque to callers)

- Initialization
  - None needed: the registry is static and categories are created on first registration.
  - `void log_set_context_hooks(const log_context_hooks_t* hooks);`
    - The logger is a leaf module (libc only). What it knows about the running machine arrives through three optional hooks: `instr_count` (the `@count` timestamp), `format_pc` (the `PC=` decoration) and `observe_line` (sees every emitted line — the debug trace's capture). `src/core/debug/log_context.c` supplies them; `core_init` installs them with `log_context_install()`. Without hooks, the decorations read `@0` and `PC=00000000`.

- Category management
  - `log_category_t* log_register_category(const char* name);`
    - Registers or returns existing category by name. On first registration, the level defaults to `0` (OFF).
    - The name must be declared in the category manifest (`GS_LOG_CATEGORIES` in `src/core/debug/log_categories.h`) — a name not in the manifest is a typo and is refused, not created.
    - Returns non‑NULL pointer on success; `NULL` on OOM or invalid name.
  - `log_category_t* log_get_category(const char* name);`
    - Looks up existing category; returns `NULL` if not found.
  - `const char* log_category_name(const log_category_t* cat);`
  - `int log_get_level(const log_category_t* cat);`
  - `int log_set_level(log_category_t* cat, int level);`
    - Returns previous level, or negative on error.

- Fast path predicate
  - `static inline int log_would_log(const log_category_t* cat, int level);`
    - True when the message should be emitted (i.e., `level <= log_get_level(cat)` and compile‑time filter, see below). `log_emit`/`log_vemit` apply the same gate themselves, so a direct call honours the level too.

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
  - A file that cannot be opened makes `log_set_category_file` return -1 with `errno` from `fopen` (its other failures set `errno` too: `EINVAL` for an unknown category, `ENOMEM`); the caller (`log.set`, `log.category[...].file`) puts the reason in its error value (`cannot set log file '<path>': <reason>`) — nothing is printed to stderr.
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

The macros define a file-local `log_local_category()` accessor (and, for the one-liner, a `log_local_category_ptr` cache); `LOG` calls it. The variadic macros rely on the GNU `, ##__VA_ARGS__` comma swallow — the project is GCC/Clang-only (`docs/guide/STYLE_GUIDE.md`, "Compiler Extensions").


## Levels and semantics

- Levels are plain integers; smaller means more important or less verbose in this design (so that `level <= category_level` is “emit”).
- The shell never enforces ranges; it accepts any non‑negative integer. Modules may choose their own fine‑grained levels if desired.

### Conventional bands (guidance, not a rule)

Because ranges are not enforced, `log.set scc 4` and `log.set adb 4` mean
different things — `scc.c` uses levels up to 11, `adb.c` stops at 3. That is
deliberate, but it means a user cannot transfer intuition between two modules
without reading them. New code should follow these bands so that intuition
starts to hold:

| level | meaning | rough frequency |
|---|---|---|
| 1 | warning, error, or an unmodelled request we had to refuse | rare |
| 2 | state change — mode set, device attached, reset | occasional |
| 3 | one line per transaction, command or host event | per interaction |
| 4+ | per byte, per sample, per scanline | firehose |

Modules with genuinely more structure may go further — `scc.c`'s 6 and 11 and
`llap`'s 8 and 11 (in `appletalk.c`) are deliberately paired for LocalTalk tracing, so a
single level selects a matched view across both. Exceeding the bands is fine;
doing it *by accident* is what the table is here to prevent.

### Level 0 is the always-on level

A category's level starts at 0 and 0 means "off", so a `LOG(0, ...)` site
emits whenever the category has not been turned up — that is, always, in the
default configuration. This falls out of the `level <= category_level` rule
rather than being a separate mechanism, and it is **used deliberately**: there
are ~200 such sites in `src/`, and the ones sampled are all unrecoverable or
degraded conditions where silence is the wrong default — out of memory during
machine construction, a window table overflowing, an address decode falling
back to a linear walk.

Use level 0 only for those. If a message should be suppressible, it is a
level-1 warning, not a level-0 one. The only way to silence a level-0 site is
the compile-time `LOG_COMPILE_MIN_LEVEL`.


## Internal design (log.c)

- Registry
  - Categories (`log_category` nodes) are indexed by name in a fixed open-addressed table (FNV-1a hash, linear probing; 256 slots, statically asserted to be at least twice the manifest) and also kept on a singly-linked list, newest first, for enumeration (`log_foreach_category`).
  - Each node contains:
    - `char* name;` (owned, NUL‑terminated)
    - `int level;` (current threshold; 0 is the off position for level-1-and-up sites)
    - `struct log_category* next;`
    - Optional `uint16_t id;` if we later want stable IDs.

- Lookups
  - `log_get_category(name)` is a case‑sensitive hash-table lookup.
  - Category registration reuses existing category (idempotent) and sets level only the first time. Duplicate registrations are common during module init in tests; this avoids conflicts.

- Fast path check
  - `log_would_log(cat, lvl)` does a plain integer compare and also respects `LOG_COMPILE_MIN_LEVEL` if defined.
  - The `LOG` macro expands to:
    - Branch‑predict hint on the negative path (`__builtin_expect` when available) to keep the disabled case cheap.
    - No call, no `va_list`, no `snprintf` when the message is disabled.

- Emission
  - Build a single line per call: `[name] <level> message\n` (example prefix; see formatting)
  - Formatting pipeline:
    - Prefix, indent and body are formatted straight into one buffer: a 768-byte stack buffer for the common case, moving to the heap (exact size, second `vsnprintf`) when a line outgrows it. Lines are never clipped; only if that allocation fails is the line cut short and ended with `...`.
    - The indent is a slice of a constant 64-space string (no per-line fill).
    - Write directly to the sink (default: stdout). For Emscripten, stdout maps to console; we may optionally add a JS sink later.

- Thread‑safety
  - No locking. `LOG` runs on the emulator thread and the job thread, so this rests on two facts:
    - Registry mutation happens at setup only: `log_register_manifest` creates every category, so later `log_register_category` calls (the lazy `LOG_USE_CATEGORY_NAME` first use included) are read-only lookups.
    - Configuration (`level`, sinks, the indent, the global sink) is written by `log.set` and the host at quiet points; a racing reader sees the old or the new `int`/pointer. Should that stop being enough, those fields become `atomic_int`/atomic pointers without an API change — open debt.

- Memory and lifetime
  - Category pointers are stable for the process lifetime. Modules cache their `log_category_t*` in static file‑scope variables for fast checks.


## Shell surface (`log.set`)

The shell exposes the configuration as a typed method on the root `log` object: `log.set(category, level=, stdout=, file=, ts=, pc=)`, with real named arguments (the legacy flat `log` command and the earlier `debug.log` method are retired). The category is a VK_ENUM over the manifest, so completion offers every declared name and a typo is rejected at the call, not silently configured.

- Grammar
  - `log.levels` — every registered category and its current level, as a map
  - `log.set(<cat>)` — show one category's settings
  - `log.set(<cat>, level=<n>, stdout=<bool>, file=<path>, ts=<bool>, pc=<bool>)` — any subset of the named arguments; each given one is applied, then the settings print

- Behavior
  - Categories come from the manifest; an unknown name is an error, never auto-created.
  - `file=<path>` opens/creates the file in append mode; `file=off` disables the file sink.
  - `stdout=<bool>` enables/disables writing to stdout for that category (defaults to `on`).
  - `ts=<bool>` toggles including a timestamp prefix based on `cpu_instr_count()`.
  - `pc=<bool>` toggles including the PC register in the prefix.

- Examples
  ```
  log.levels                                 # list all categories and levels
  log.set("cpu")                            # show cpu settings
  log.set("cpu", level=5)                   # set level to 5
  log.set("cpu", level=7, stdout=false)      # quiet stdout for cpu
  log.set("cpu", file="tmp/cpu.log")         # append to file as well
  log.set("cpu", ts=true)                    # include instruction-count timestamp in prefix
  ```


## Usage by modules

- Registration (once) and selecting an implicit category
  - In `src/core/network/appletalk.c` (and similarly in `cpu.c`, `floppy.c`, etc.):
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

  void appletalk_network_init(void) {
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
  - Single `vsnprintf` into a stack buffer, then one write to each sink.
  - The 768-byte stack buffer avoids heap allocation for ordinary lines; a longer line costs one allocation instead of being truncated.

- Registry costs
  - Category lookups occur only in shell commands. Normal logging uses cached pointers and simple integer reads.


## Error handling

- Category registration:
  - Returns existing category when called repeatedly with the same name.
  - Returns `NULL` on allocation failure; modules may fall back to a static dummy category whose level is 0 (silent) to keep code safe.
- Shell surface:
  - Rejects negative levels; `log.set` returns an error ("level must be a non-negative integer").
  - `log.set(<cat>)` (no modifiers) reports `unknown category "name"` when the category is not in the manifest.


## Integration points

- `src/core/debug/log.c` — implementation (registry, sinks, formatting) and the category manifest loader.
- `src/core/debug/log_categories.h` — the category manifest (`GS_LOG_CATEGORIES`), the one place a new category is declared.
- `src/core/debug/log.h` — public header used by modules and the shell.
- `src/core/debug/log_context.c` — the context hooks (instruction count, PC decoration, debug-trace capture); `src/core/core_init.c` installs them (`log_context_install()`).
- `src/core/debug/log_class.c` — the root `log` object: the `log.set` method (which prints the category's settings through the output sink), `log.levels` and `log.category[...]`.

## Level guidelines and recommendations

Levels are plain integers; higher values are more verbose. Setting a category
to 0 is the off position for level-1-and-up sites; `LOG(0, …)` sites still
emit, deliberately (see “Level 0 is the always-on level”). To keep logs consistent and useful across modules/devices, use these guidelines:

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

## Built-in debug categories

Three pre-defined categories pair with debug shell commands and feed into the
standard log pipeline:

- **`logpoint`** — emitted by PC logpoints (`debug.logpoints.add addr=<addr> [message="…"]`).
  Default level 0 (silent). Enable with `log.set("logpoint", level=1)` to see each hit.
- **`memory`** — emitted by memory read/write logpoints
  (`debug.logpoints.add addr=<addr> mode=read|write|rw [width=b|w|l] [message="…"]`). Each event reports `addr`, `size`,
  `value`, `pc`, and optionally a substituted user message: `${expr}` splices
  any expression (`${machine.cpu.pc}`), while `$value`, `$addr` and `$size`
  are per-fire bindings that exist only at the hit.
  See `docs/internals/core/memory/memory.md` for the fast-path-preserving mechanism that backs these.
- **`exceptions`** — emitted by the CPU exception trace ring
  (`debug.exceptions [filter]` dumps the ring; `log.set("exceptions", level=1)` streams every event).
  Each line includes vector, frame format, faulting/stacked PC, fault address,
  R/W direction, SR, VBR, and a marker for double-fault detection. Replaces
  ad-hoc `fprintf` instrumentation in `cpu_internal.h` for MMU/bus-error
  debugging sessions.

These categories are auto-registered the first time their feature is used; they
also appear in `log` with no arguments once registered.
