# Object Model

This document describes the typed object model in `src/core/object/`. The
object model is the single substrate that the shell, the JavaScript / WASM
bridge, scripts, the machine-configuration layer, and the inspector UI all
sit on. Adding a new emulator subsystem, exposing it to scripts, wiring it
into the web frontend, and giving it tab completion is one act, not four.

## Why

Before the object model the emulator had several parallel surfaces with
distinct conventions and divergent failure modes:

- A shell command registry (`(int argc, char **argv) -> uint64_t`) where
  commands manually parsed `argv`, returned a single overloaded integer,
  and wrote prose to stdout.
- An ad-hoc set of `Module.ccall(...)` entry points used by the JS
  frontend to read CPU/memory state and to drive media operations,
  serialised by hand and prone to drift from their C-side counterparts.
- A separate "info" / "print" / "set" path that read or wrote registers
  and Mac globals via the symbol resolver.
- Tab completion that knew only command names — not arguments, not
  subcommand structure, and not state-dependent values.

Each surface re-implemented argument parsing, error reporting, and
response shaping. Adding a new operation meant teaching every surface in
turn; renaming or removing one meant a hunt for callers.

The object model collapses all of that into one tree. Every emulator
subsystem exposes its state and operations through a class descriptor;
every caller — interactive shell, JS bridge, integration script,
inspector panel — walks the same tree, observes the same types, sees
the same errors. There is no shadow API.

## Source Files

| File | Purpose |
|------|---------|
| [object.h](../src/core/object/object.h) | Core contract: classes, members, objects, nodes, path resolution |
| [object.c](../src/core/object/object.c) | Substrate implementation (tree topology, resolver, validation, invalidators) |
| [value.h](../src/core/object/value.h) | Tagged-union `value_t` used at every boundary |
| [value.c](../src/core/object/value.c) | Value lifetime, conversions, formatting helpers |
| [parse.c](../src/core/object/parse.c) | Path tokeniser shared by the resolver and completer |
| [expr.h](../src/core/object/expr.h) | `${...}` expression parser and evaluator |
| [alias.h](../src/core/object/alias.h) | Two-tier `$name` alias table (built-in + user) |
| [api.h](../src/core/object/api.h) | Public C entry point (`gs_eval` — single dispatch for reads, writes, calls, schema, completion, and shell-line input) |
| [meta.h](../src/core/object/meta.h) | The synthetic `Meta` class (`<path>.meta.*` introspection + `meta.complete`) |
| [shell_class.c](../src/core/shell/shell_class.c) | The `Shell` class (`shell.run`, `shell.complete`, `shell.expand`, `shell.script_run`, `shell.alias_set`/`alias_unset`, `shell.interrupt`, `shell.prompt`/`running`/`aliases`/`vars`) |
| [root.c](../src/core/object/root.c) | The `emu` root class plus install/uninstall lifecycle |

## Core concepts

### Values are typed and self-describing

Every value crossing an object-model boundary is a `value_t` — a tagged
union with a discriminator (`V_NONE` / `V_BOOL` / `V_INT` / `V_UINT` /
`V_FLOAT` / `V_STRING` / `V_BYTES` / `V_ENUM` / `V_LIST` / `V_MAP` /
`V_OBJECT` / `V_ERROR`), a width hint for fixed-size integers, and display
flags (`VAL_HEX`, `VAL_RO`, `VAL_VOLATILE`, `VAL_SENSITIVE`, …). Errors are
in-band: a `value_t` of kind `V_ERROR` carries a string message rather
than relying on a separate return channel or out-pointer.

Ownership is single-owner: the receiver of a `value_t` owns it and
must call `value_free` (which is safe on every kind, including the
inline ones). Heap-owning kinds (`V_STRING`, `V_BYTES`, `V_ERROR`,
`V_LIST` and `V_MAP` recursively) `strdup` their inputs at construction
time, so there is no borrowed-string path to confuse callers.

`V_MAP` is the keyed sibling of `V_LIST`: an insertion-ordered sequence
of unique `{key → value}` entries with heap-owned keys and recursively
owned values. Map-shaped method results (`machine.profile`,
`machine.rom.identify`, `debug.frame`, `meta.method_info`, …) return it
directly; the gsEval bridge serialises it as a JSON object exactly once
(the browser receives a native object, never a JSON string to re-parse).
Methods build maps with the `val_map_new` / `val_map_put` /
`val_map_finish` builder and read them with `value_map_get`. In `${…}`
interpolation a map renders as compact canonical JSON, so
`echo "${machine.profile("se30")}"` emits machine-parseable text.

Display flags follow the value: an attribute declared with `VAL_HEX`
emits values that the JSON encoder serialises as `"0x12345678"`; the
shell formatter prints them in hex; the inspector panel renders them
the same way. Each layer reads the intent off the value, not off the
attribute, so passing a `machine.cpu.pc` reading through a function still
formats correctly.

### Classes describe what a node can do

A `class_desc_t` is a static description with a name and a member
table. Each `member_t` is one of three kinds:

- **`M_ATTR`** — a typed attribute with a getter and (optionally) a
  setter. The attribute slot declares the value's kind, optional
  `width` (1/2/4/8 for sized integers), optional `enum_values` table,
  and slot flags (`OBJ_ARG_NONEMPTY`, `OBJ_ARG_STRICT_KIND`). Setters
  whose input fails those constraints are short-circuited by the
  framework with a uniform error before the body runs. Attributes
  that lack a setter or carry the `VAL_RO` flag reject `node_set`
  cleanly. Each member has a per-member `user_data` pointer so
  dispatchers shared across many attributes (the 471-entry
  `mac.globals` is one such case) recover their context from the
  member descriptor instead of via lookup tables.

  `user_data` answers *"which member am I?"*. It is the wrong tool for
  *"which instance am I?"* — that is what `object_new`'s
  `instance_data` is for, and it is per-object. Sibling objects of the
  same class (`scc.a` and `scc.b`, say) should each carry their own
  instance pointer and share one member table; encoding the instance in
  `user_data` forces a second table per sibling, which then has to be
  kept in step by hand.

  A node that holds **per-machine state should be built and destroyed with
  the machine**, not registered once at `shell_init`. `machine` itself is a
  long-lived container whose children come and go, so both shapes appear
  under it, and the distinction is not cosmetic: a process-lifetime node
  cannot own a scheduler source, because the scheduler does not outlive the
  machine. `machine.adb.keyboard` was a facade for that reason and its
  `type()` could only reach a machine that had an `adb_t` to borrow a
  source from; it is per-machine now (`host_input.c`). A container with no
  state of its own — `machine.adb` — can stay a singleton.
- **`M_METHOD`** — a callable taking declared `arg_decl_t` parameters
  and returning a `value_t`. Each parameter declares its kind,
  optional `width`, optional `enum_values`, optional `default_value`,
  a doc string, and per-slot flags (`OBJ_ARG_OPTIONAL`, `OBJ_ARG_REST`,
  `OBJ_ARG_NONEMPTY`, `OBJ_ARG_STRICT_KIND`). The framework validates
  argv against this declaration before invoking the body (see
  *Typed dispatch validation* below) and the completer reads the
  same metadata for argument-position suggestions.
- **`M_CHILD`** — a child object. Children are either *named* (a fixed
  name with its own class) or *indexed* (a sparse, stable-id
  collection: `child.get(i)` answers each index in `[0, child.slots)`,
  NULL for a hole, and the framework walks them; a collection whose ids
  are sparse past any fixed bound gives a `child.next` iterator instead,
  as the debugger's breakpoints do). Indexed children are how
  `debug.breakpoints[7]` and `machine.floppy.drive[0]` work without the
  framework needing to know a collection's storage shape. A class whose
  one indexed child is its collection answers `count` with the live
  entries, unless it declares a `count` of its own.

Member tables are static `const`. The framework walks them linearly
for resolution, completion, and help; no string lookup tables are
maintained at runtime.

### Objects are instances; nodes are addresses

An `object` is a runtime instance of a class. It carries a back-pointer
to module-private state (a `cpu_t *`, a `scsi_t *`, …) that the class's
getters cast through `object_data(self)`. Object trees grow at attach
time and shrink at detach time; the `emu` root persists for the
lifetime of the process.

A `node_t` is the address of *something* in the tree: a triple of
`(object, member, index)`. The shell, the JSON bridge, scripts, and
held breakpoint references all carry the same shape. `node_get` reads;
`node_set` writes; `node_call` invokes a method. The resolver
(`object_resolve`) turns a dotted path string into a node in one walk.

When an object disappears (a machine teardown removes a peripheral, a
breakpoint is deleted), the framework fires per-object invalidator
hooks so listeners that hold cached `node_t` references can drop them.
This keeps held-state consumers (logpoint conditions, watch paths)
from dereferencing freed objects.

## Path forms

Every consumer of the object tree uses the same four path shapes — the
shell, JS, expression interpolation, and integration scripts speak the
same language:

| Form | Meaning | Example |
|------|---------|---------|
| `path` | Read the attribute, format the result, return / print | `machine.cpu.pc` |
| `path = value` | Write the attribute (setter) | `machine.cpu.d0 = 0x1234` |
| `path arg arg …` | Call the method, shell form (whitespace-separated args) | `machine.floppy.drive[0].insert /tmp/fd0 false` |
| `path(arg, arg, …)` | Call the method, expression form (used inside `${…}`) | `${machine.memory.peek.l(0x1000)}` |

Shell form is what users type interactively. Expression form is what
appears inside `${…}` interpolation in scripts and logpoint messages.
Both compile to the same `node_call` underneath.

`${…}` (inside a double-quoted string) evaluates an expression and
splices its formatted value. Inside expression mode, a bare path is a
`node_get` (or, with a trailing call-form argument list, `node_call`);
`$name` reads a binding; literals and operators work the way they do
in C, **with one deliberate exception**: the bitwise operators `&`, `^` and
`|` bind *tighter* than the comparisons, where C binds them looser. So
`${machine.cpu.sr & 0x2000 == 0x2000}` means `(sr & 0x2000) == 0x2000` here
and `sr & (0x2000 == 0x2000)` in C. C's order is a well-known trap and this
is the friendlier reading; the full precedence table is `expr.c`'s grammar
comment, the authority in code. Truthiness is per kind: numbers ≠ 0, non-empty
strings/lists/bytes/maps, `none` never, and errors are not truth values —
an error reaching a condition aborts.

Paths keep resolving *into* structured values (`V_MAP` / `V_LIST`):
when a path prefix names a node whose value is a map or list, the
remaining segments index into that value — dotted `map.key`, bracket
`map["key"]` (any string expression), and numeric `list[N]`. The same
segments work after a call form and through bindings:

    machine.config.vroms[0].card_id            # list → map → value
    machine.profile("se30").capabilities.mmu.kind
    let info = machine.rom.identify("boot.rom")
    echo "${$info.checksum} ${$info["name"]}"
    for k in machine.profile("se30").capabilities { echo "$k" }   # keys

`for … in` over a map iterates its keys (fetch values with `map[$k]`);
`len(map)` counts entries. Maps are read-only through this surface —
there is no `map.key = …` write path.

`$name` is the unified binding surface: scoped
`let` bindings first, then the alias table, whose entries behave as
**reference bindings** — they store path text and re-resolve on every
access, so they survive `machine.boot`:

- **Built-in aliases** are registered by subsystems at `*_init` time.
  `cpu_init` sets up `$pc` → `machine.cpu.pc`, `$d0` → `machine.cpu.d0`, … . Re-
  registering with the same target is a no-op, so repeated machine
  boots are safe.
- **User aliases** are added with the `alias NAME = PATH` statement
  (or `shell.alias.add`). They cannot collide with built-ins or
  reserved words, and they don't persist across the process.

### Reserved words

Statement keywords: `let`, `alias`, `if`, `elif`, `else`, `while`,
`for`, `in`, `break`, `continue`, `return`, `def`, `assert`. Literals:
`true`, `false`, `none`. Held: `do`. These may not be used as member,
alias, or binding names (`object_validate_name`). `on`/`off`/`yes`/`no`
are **not** reserved — they remain accepted as input coercions for
bool-typed argument slots only.

### Library conventions

- **Methods return data; surfaces do the printing.** Search results
  come back as lists (`find.str(...)` → list of addresses; empty =
  not found; `[0]` is the first hit). `*.list` printers are retired:
  an indexed collection read without an index (`debug.breakpoints.entries`)
  returns the entry objects as a list, and the REPL renders a list of
  same-class objects as a table.
- **Success/failure flows as value-or-`V_ERROR`**, never a printed
  message plus a bool. `V_BOOL` returns are reserved for methods whose
  *answer* is a boolean (`storage.path_exists`).
- **Absence is never `none`.** "Not found" is an empty collection;
  failure is `V_ERROR`; `none` is the script-side no-value.
- **Named arguments replace spec strings.** `debug.logpoints.add
  addr=0x16A width=l mode=write level=5 message="…"` — no flag
  grammars inside strings.
- **Deferred evaluation is a parameter type.** `OBJ_ARG_TEMPLATE`
  slots store the raw string body; the subsystem evaluates it at fire
  time with per-fire bindings (`$value`/`$addr`/`$size` for logpoint
  messages).
- **Tree layout:** emulated hardware lives under `machine.*`; tooling
  and session surfaces (`debug`, `find`, `scheduler`, `shell`,
  `storage`) live at the root.

## Typed dispatch validation

`node_call` and `node_set` enforce each method's `arg_decl_t[]` and
each attribute's slot declaration before invoking the body. Bodies
do not re-check kinds, arity, widths, non-emptiness, or enum
membership; the framework rejects mismatches with a uniform
`V_ERROR` and the body never runs.

The same engine drives both surfaces — methods are N-tuples of
typed slots, attributes are 1-tuples — so the validation vocabulary
and error wording are identical regardless of where the input
arrived from.

### What the framework checks

- **Arity.** Required parameters must be present; `OBJ_ARG_OPTIONAL`
  parameters may be omitted; `OBJ_ARG_REST` (last slot only) slurps
  any remaining items into the body's argv. Calls with too many
  arguments are rejected unless the last slot is rest.
- **Kind match.** `argv[i].kind` must equal the slot's declared
  kind (with the coercion exceptions below).
- **Width fit.** `V_INT` / `V_UINT` slots that declare `width=1/2/4/8`
  reject values whose bit pattern doesn't fit. Width `0` (or `10`,
  used by FPU extended-precision attributes) means "no explicit
  bound". Catches the silent truncation problem that used to hide
  in `machine.cpu.pc = 0x100000000` style writes.
- **Non-empty strings.** Slots flagged `OBJ_ARG_NONEMPTY` require a
  non-NULL, non-empty `V_STRING`.
- **Enum membership.** `V_ENUM` slots validate the index is in
  `enum_values`; `V_STRING` input is looked up against the same
  table and rewritten to `V_ENUM` so the body always sees the
  enum form (see coercion below).
- **`V_OBJECT` non-NULL.** `V_OBJECT` slots reject `argv[i].obj == NULL`.
- **Default fill.** Optional parameters that declare a `default_value`
  are synthesised into the rewritten argv when the caller omits them,
  so the body reads `argv[N]` without an `argc` check.

### Coercion policy

Cross-kind input is accepted in a small, deliberate set of cases.
The body always sees a value matching the declared slot kind:

- **`V_INT` ↔ `V_UINT`.** Width fit is checked under the input's
  signedness, then the bit pattern is reinterpreted under the
  declared signedness. So `machine.cpu.pc = -1` succeeds and stores
  `0xFFFFFFFF`; `machine.cpu.d0 = 0xDEADBEEFCAFE` against `width=4` is
  rejected.
- **`V_INT` / `V_UINT` → `V_FLOAT`.** Numeric input widens to
  double. The reverse (`V_FLOAT` to integer slot) is rejected;
  callers needing truncation must cast in the expression.
- **`V_INT` / `V_UINT` 0 / 1 → `V_BOOL`.** Other integers (`2`,
  `-1`, …) are rejected. Strings like `"true"` are the lexer's
  job — the framework does not re-interpret them.
- **`V_STRING` → `V_ENUM`.** A string is matched against the slot's
  `enum_values[]` table and the body receives a `V_ENUM`. Wrong
  names produce a `must be one of {...}` error.
- **`OBJ_ARG_STRICT_KIND` opt-out.** A slot with this flag accepts
  exactly its declared kind and disables the coercions above.

### `V_ANY` slots (and the `V_NONE` spelling on arguments)

A slot declared with `kind = V_ANY` is the explicit "accept any
kind" sentinel — the framework skips kind / width / enum checks
and the body discriminates the input. Used for legitimately
multi-kind attributes and parameters: `machine.rtc.time` accepts either an
ISO-8601 string or a Mac-epoch integer; `machine.adb.keyboard.press` accepts
either a key name or an ADB keycode (and an integer means the same key on
every machine — the Lisa's own wire bytes live on `keyboard.raw`); `machine.memory.dump.addr` accepts
either an address integer or an alias / expression string. Most
slots should declare a concrete kind; the sentinel is reserved for
genuine dual-input shapes.

On an *argument* slot `V_NONE` means the same thing, and predates
`V_ANY`; both spellings work and existing declarations were left
alone. On a method's *result* slot the two are not
interchangeable: there `V_NONE` means "returns nothing" and `V_ANY`
means "polymorphic" — see below.

`V_ANY` is a declaration-only constant, deliberately outside
`value_kind_t`'s enumerated range: it describes what a slot accepts
or promises, never what a live value is, so switches over a value's
kind stay exhaustive without a dead arm.

### Argv rewriting and ownership

Validation copies the caller's argv into a stack-scratch buffer
when any coercion fires or any default needs filling. The caller's
argv is never mutated and the body must not call `value_free` on
items in argv (the existing convention). When the caller passes a
heap-owning value (e.g. `V_STRING`) to `node_set` and the validator
coerces it to a different kind (e.g. `V_ENUM`), `node_set` frees
the orphaned heap memory before calling the setter.

### Class-registration invariants

`object_validate_class` runs at registration time and rejects
malformed declarations before the process can dispatch a single
call. Hard errors abort startup in every build configuration:

- Required parameter follows an optional one.
- More than one `OBJ_ARG_REST` slot, or `OBJ_ARG_REST` not on the
  last slot, or `OBJ_ARG_REST` combined with `OBJ_ARG_OPTIONAL`.
- `default_value` set on a non-optional parameter, or whose kind
  doesn't match the slot.
- `V_ENUM` slot with no `enum_values` table.
- Arg-only flags (`OBJ_ARG_OPTIONAL`, `OBJ_ARG_REST`,
  `default_value`) set on an attribute slot.

This turns class-author mistakes into first-boot failures rather
than first-call surprises.

### Return-kind assertions (debug builds)

In debug builds the framework asserts that getter returns, method
results, and setter returns match their declarations. A getter for
a `V_UINT, width=4` attribute that returns `V_STRING` aborts
immediately; a method declared `result = V_NONE` that returns a
non-error value also aborts. `V_ERROR` is always allowed (in-band
error). Release builds compile the asserts out, so the production
cost is zero. Integration and unit tests run with assertions
enabled so cross-kind regressions surface in CI rather than only
locally.

A method whose result kind genuinely depends on its arguments
declares `result = V_ANY` and is not checked. The one such surface
today is `debug.mac.globals.read`, which hands back a `V_UINT` for a
1/2/4-byte low-memory global and `V_BYTES` for a wider one
(`KeyMap`, `EventQueue`, `FileVars`, …). Declaring that method
`V_UINT` aborted the process on every wide global (issue #106);
`V_NONE` would have aborted on all of them, since a `V_NONE` result
slot asserts that the method returns nothing at all. Reach for
`V_ANY` only when the polymorphism is real — a concrete kind is
still the norm, and it is what makes the assertion useful.

## Foundation for shell and configuration

The same tree is what users navigate interactively, what scripts
configure, and what the JS frontend operates on:

- **Interactive shell.** The terminal tokenises the line and dispatches
  to `node_get` / `node_set` / `node_call` based on the form. Tab
  completion walks the same tree; argument-position completion
  consults the resolved method's `arg_decl_t` table for enum values
  and other hints.
- **Scripts (headless).** Integration tests and reproducible boot
  scripts contain exactly the same path forms users type. `assert`,
  `echo`, and `${…}` interpolation are root methods on `emu`.
  Scripted machine setup (`machine.boot(model=..., rom=...)`, `machine.rom.load(...)`,
  `machine.floppy.drive[0].insert(...)`, `machine.scsi.attach_hd(...)`) is the same
  call sequence whether it runs from a script, from the user's
  terminal, or from the URL-media auto-boot path on the web.
- **JS / WASM bridge.** `gs_eval(path, args_json, out_buf, size)`
  resolves the path, parses arguments from JSON, invokes the right
  read / write / call, and serialises the result back to JSON. JS
  reaches it through a single shared-memory region (`js_bridge_t`,
  declared in [`em.h`](../src/platform/wasm/em.h) and exposed via the
  lone `_get_js_bridge` export); the worker's `shell_poll()` services
  the slot every tick. `Atomics.waitAsync` + `emscripten_atomic_notify`
  carry the completion signal — no polling. The worker-thread guard
  in `worker_thread.h` enforces that `gs_eval` only runs on the worker
  pthread, never via direct `Module.ccall` from the main thread. JS
  callers see numbers, strings, lists, and `{error: "…"}` shapes —
  never raw exit codes. See [`web.md`](web.md) for the wire layout
  and protocol.

  **The result contract** (what `gsEval` in `app/web2/src/bus/emulator.ts`
  resolves to): a value is the result; `null` is **only** a successful
  method that returns nothing (V_NONE); every failure is an
  `{error: "…"}` object — the core's V_ERROR message, or, for a failure of
  the bridge itself (module not ready, a thrown request), the same shape
  with `transport: true`. So `r !== null` is never a success test (an
  `{error}` satisfies it): use `gsOk(r)` for "did it work", `r === true`
  for a V_BOOL method, and a shape check for a read. `gsErrorText(r)`
  gives the reason. A shell statement such as `machine.cpu.d0 = 1` is
  **not** a `gs_eval` path — write an attribute by passing the value as
  the single argument: `gsEval('machine.cpu.d0', [1])`.
- **Inspector UI.** The browser inspector panel reads the tree by
  walking `objects()` / `attributes()` / `methods()` and rendering the
  results. No bespoke inspection protocol; the panel is just another
  caller.

There is no separate "shell command framework" or "JS API" layer. Each
caller is a thin adapter that translates its native input (typed line,
JSON-RPC-shaped message, expression AST) into a node operation.

## Lifecycle

Objects in the tree fall into two camps:

- **Process-singletons.** Stateless or process-global facades — the
  archive extractor, the platform mouse / keyboard / screen / vfs /
  find facades, the rom / vrom / machine / checkpoint orchestrators —
  attach themselves at `shell_init` time via their owning module's
  `*_init` (or `*_class_register`) function and stay attached for the
  process lifetime. Their methods consult the active machine where
  needed but do not hold per-machine state on the object node itself.
- **Cfg-scoped subsystems.** CPU, memory, scheduler, peripherals
  (scc / rtc / via / scsi / floppy / sound / appletalk), and the per-
  entry debug objects (breakpoints / logpoints) are attached when a
  machine is created (`system_create` → `profile->init`) and torn
  down when the machine is destroyed. Their `_init` is the place
  where the object node is allocated and attached to the root, and
  where any built-in aliases the subsystem owns get registered.

The cfg-scoped install path (`root_install`) is idempotent for the
same `cfg` and atomic across cfg changes: a checkpoint reload that
calls `system_create(new)` followed by `system_destroy(old)` does the
right thing because the install path detaches stubs from the previous
cfg before attaching the new ones, and the destroy path no-ops when
the installed cfg is no longer "its" cfg. This invariant lives in
`root.c`.

The result is that paths like `machine.cpu.pc` resolve as soon as a machine is
booted and disappear cleanly when the machine is torn down, without
the caller having to track machine-lifetime explicitly.

## Machine lifecycle

Four operations put a machine in place or restart the one that is running.
They differ in what they keep. Line numbers are as of this writing; the
function names are the stable reference.

| Operation | What it does | Entry point |
|---|---|---|
| `machine.boot(...)` | Builds a **new** machine from a complete boot document ([Boot arguments](#boot-arguments)): validates the whole document, tears the running machine down, constructs, installs the ROM, writes the built-from record `machine.config`. | `machine_method_boot` → `machine_boot_apply` (`src/machines/machine.c:850`) |
| `machine.restart` | **Power-cycle**: rebuilds the machine that `machine.config` describes, takes no arguments, and carries the mounted media across the teardown. Errors if no machine is running. | `machine_method_restart` (`machine.c:1188`) |
| `machine.reset` | **Warm reset** (the reset button): the board's /RESET net, then the CPU back to its reset vector. Nothing is torn down or rebuilt. Errors if no machine is running. | `machine_method_reset` (`machine.c:1177`) → `system_machine_reset` (`src/core/system.c:216`) |
| `checkpoint.load(path)` | Builds a new machine from a checkpoint: it creates the new machine first and destroys the old one afterwards. | `system_checkpoint_load` → `system_restore` (`system.c:1594`) |

`machine.boot`, `machine.restart` and headless startup all go through
`machine_boot_apply`, so they share one sequence: validate, tear down,
construct, record (`machine.c:846-1124`). Every check runs before
`system_destroy`, so a rejected boot leaves the running machine and its
record untouched (`tests/integration/boot-config`).

### What each operation keeps

| State | `machine.boot` | `machine.restart` | `machine.reset` | `checkpoint.load` |
|---|---|---|---|---|
| Model, RAM size, cards | From the document; each omitted field takes the model's default | From the record; per-slot picks replayed only where the user chose them (`machine.c:1202-1235`) | Unchanged | From the checkpoint's model id, RAM size and stored record (`system.c:1612-1665`) |
| ROM bytes | Read from the `rom=` file (`machine.c:1062-1066`) | Read again from the recorded path, so a changed file is picked up. A live `rom.load` rewrites the record first (`rom.c:383`) | Unchanged | From the checkpoint, by content or file reference (`src/core/memory/memory.c:1632`) |
| RAM contents | Zeroed (fresh `calloc`, `memory.c:1584`) | Zeroed | **Kept** | Restored (`memory.c:1628`) |
| PRAM (RTC parameter RAM) | Family construction defaults (`rtc_init` → `pram_defaults_apply`, `src/core/peripherals/rtc.c:463`; tables in `src/machines/runtime/pram_defaults.h`) | Construction defaults again (not carried) | **Kept** | Restored (`rtc.c:485`) |
| TNT NVRAM (Grand Central's Open Firmware store; `pm7500`/`pm8500`/`pm9500`/`ans500`/`ans700`) | Blank: the previous machine's store goes with it | **Carried** (`tnt_teardown`, `src/machines/tnt/tnt.c:768`) | **Kept** | From the checkpoint, which overrides any carried store (`tnt.c:474`) |
| RTC time | Host wall clock at construction (`rtc.c:465`); the Lisa's COPS clock starts at 1 January 1984 (`src/machines/lisa/cops.c:189`) | The same as `machine.boot` | Keeps counting | The saved value (`rtc.c:485`) |
| Mounted media | None. The old machine's images are closed (`system.c:1235`); a CD bay is registered empty (`system.c:1157`) | **Carried**: the same open handles pass through the substrate's `media_detach`/`media_attach`, so the write delta survives | **Kept** (`floppy_reset` keeps media, `system.c:274`) | From the checkpoint |
| Caps Lock latch | Released | **Carried** (`machine.c:1019`, `1101`) | Kept (`adb_reset` preserves it, `src/core/peripherals/adb.c:511`) | From the checkpoint's ADB state (`adb.c:1051`) |
| Scheduler pacing (`scheduler.mode`) | **Carried**: the host harness owns it, not the machine (`machine.c:1000-1008`, `1085`) | **Carried** | Unchanged | The checkpoint's own value (`src/core/scheduler/scheduler.c:145`, `786`) |
| `machine.config.created` | Stamped now | **Preserved** (`machine.c:1244`) | Unchanged | From the checkpoint's record |
| vROM/PROM offer registries | Process-global; survive | Survive | Survive | Survive |
| The explicit `vrom=`/`prom=` pick | The document's, replacing the previous one (none if the document names none); a rejected boot puts the running machine's back (`machine_config_set_explicit_picks`) | The record's | Unchanged | The checkpoint record's |
| Object tree | Machine-scoped nodes rebuilt (`root_install`, `system.c:1169`); process singletons (`machine`, `rom`, `vrom`, `prom`) stay | Rebuilt | Untouched | Rebuilt |

**`machine.boot` inherits nothing from the running machine.** The
document is the whole specification. `model` and `rom` are required, and
every other field falls back to the **model's** defaults, never to the
previous record. That holds across a model change too
(`machine.c:727-734`, `853-856`; `tests/integration/boot-config`). For
this reason the integration runner passes the ROM explicitly: it starts
headless with `rom=` and also exports the same path as `$ROM`, so a
script re-boots with `machine.boot model=... rom="${$ROM}"`
(`scripts/run-integration-test.sh:154-164`). To bring back the machine
you have (including one just restored from a checkpoint), call
`machine.restart`; a bare `machine.boot()` is an error. A new boot does
still see four kinds of process-level state that are not part of any
machine:

- scheduler pacing (the table above);
- the offer registries ([rom.md §10](../memory/rom.md#10-rom-provisioning)),
  though not the previous document's explicit `vrom=`/`prom=` pick;
- per-slot staged picks the caller made before the boot
  (`machine.nubus.slot[N].card_id` / `.video_mode`,
  `machine.pci.slot[N].card_id`), each consumed by that boot;
- the host share, published again for every new machine
  (`provision_default_share`, `system.c:1172`).

**`machine.reset` compared with a guest `RESET`.** The 68k `RESET`
instruction resets the devices on /RESET (`system_reset_devices`,
`system.c:289`) and leaves the CPU alone. `machine.reset` resets those
devices and then resets the CPU to its vector (68040, 68000/68030, or
`ppc_reset`; `system.c:216-231`). Each family binds its own
`substrate->bus_reset`. The Plus and the Lisa have none yet, so they fall
back to the common device set (`system_reset_common_devices`); the code
calls this "a gap, not hardware" (`system.c:292`).

**Differences between families, and known gaps.**

- *Non-volatile state across `machine.restart`.* Only the TNT family
  carries a chip across a power-cycle: its NVRAM store. On every other
  Macintosh, PRAM comes back at the family's construction defaults.
  Hardware keeps battery-backed PRAM through a power-off, so this is a
  gap. The web frontend works around one piece of it by writing the
  startup device again after a restart (`app/web2/src/bus/boot.ts:265`).
  `tnt_nvram_clear` erases the store together with the carry
  (`tnt.c:458`).
- *Media transfer by substrate.* Floppies and `machine.scsi` go through
  the standard pair (`system_media_detach_std` /
  `system_media_attach_std`, `system.c:1398`/`1440`). The TNT family also
  carries `machine.scsi2`, and the Network Servers' second channel keeps
  its bus (`tnt_media_detach`, `tnt.c:925`;
  `tests/integration/ans-machine-restart`). The Lisa carries its Sony
  disk and its ProFile (`lisa_media_detach`,
  `src/machines/lisa/lisa.c:500`). A medium that cannot be re-attached
  is closed and logged (`machine.c:1091-1096`).
- *Failure after teardown.* If `system_create` or ROM staging fails, the
  previous machine is already gone and the process has no machine
  (`machine.c:1055-1080`).
- Host-side state outside the construction configuration (volume,
  camera/microphone capture sources) is not carried by any of these
  operations. The frontend asserts it again (`machine.c:1157-1163`;
  `app/web2/src/bus/boot.ts`, `reconcileUiWithMachine`).

## Boot arguments

`machine.boot` takes only named arguments (`machine_boot_args`,
`src/machines/machine.c:1266`). The shell writes them as `name=value`; the
JS bridge passes them as one JSON object (`initEmulator`,
`app/web2/src/bus/boot.ts:120-138`). An empty string, `0`, or `0xFF` for
`video_sense` means "not given". An explicitly empty value such as `rom=`
is rejected by the grammar before binding (`machine.c:1256-1264`). On
success the call returns `true`; otherwise it returns a `V_ERROR` and the
old machine keeps running.

| Argument | Kind | Default | Meaning and validation |
|---|---|---|---|
| `model` | string | **required** | Machine model id (`machine.profile(id)` describes one). Rejected if missing or not registered (`machine.c:857-862`). |
| `rom` | string | **required** | Path to the ROM file. It must be readable and identify, by checksum, as a ROM whose compatible list contains `model` ([rom.md §10](../memory/rom.md#10-rom-provisioning); `machine.c:873-899`). |
| `ram` | uint (KB) | the model's `ram_default` | Must be one of the model's `ram_options` (`ram_option_allowed`, `machine.c:700`, checked at `863-871`). |
| `rom2` | string | none | The second chip of a two-chip Lisa/XL ROM. It only has to be readable: the chips identify after interleaving, so per-file identification and the compatibility check are skipped (`machine.c:878-884`). |
| `vrom` | string | resolved from the offers | An explicit NuBus declaration-ROM pick. The file must identify as a known declaration ROM (`vrom_identify_card`, `machine.c:964-969`). It then wins the pick order for the card its content provides. |
| `video_card` | string | the slot default | Card id for the machine's **first** NuBus socket. Rejected on a model with no NuBus slots, and for an unknown id (with a "did you mean" hint) (`machine.c:901-912`). A per-slot `machine.nubus.slot[N].card_id` staged before the boot beats it for that slot. |
| `video_sense` | uint | the card's own default | Monitor sense: 0–7 is the passive code; 8–14 is Apple's indexed numbering for monitors that answer the extended probe (only the DAFB models it). Values up to 14 are accepted (`machine.c:933-935`). |
| `video_mode` | string | the card's default | Video-mode id for the first socket. It must be a known mode id (`nubus_video_mode_known`, `machine.c:936`). |
| `custom_mode` | string | none | Custom resolution `WxHxD` for the generic `8_24` kind. Parsed and rejected with the reason (`machine.c:957-961`). |
| `monitor` | string | the model's default | Monitor strapped to the **built-in** video port. The value must be one of the family's monitor ids (see `machine.profile`), and the model must have configurable built-in video. `none` leaves the port unconnected, which hands the screen to a NuBus card. It resolves to a sense code at construction (`machine.c:938-955`, `1047-1051`). |
| `pci_card` | string | the slot default | Card id for the machine's **first** PCI socket. Rejected on a model with no PCI slots, and for an unknown id (`machine.c:913-924`). |
| `prom` | string | resolved from the offers | An explicit PCI expansion-ROM pick. The file must identify as a known Open Firmware expansion ROM (`prom_identify_card`, `machine.c:970-977`). |
| `pci_option` | string | none | `key=value[,key=value]` options for the `pci_card` socket (for example `vram=4m`). A malformed pair is logged and dropped. Whether a key means anything is up to the card's `stage_option` hook (`stage_pci_options`, `machine.c:806`). |

After the per-field checks comes **strict card resolution**. A card the
user *explicitly* picked (a staged per-slot entry, or the wildcard
`video_card`/`pci_card` on the first socket) that needs a declaration ROM
or FCode expansion ROM must resolve from the offer registry. Otherwise the
boot is rejected before teardown (`validate_vrom_resolution` /
`validate_prom_resolution`, `machine.c:745`/`775`). A socket that falls
back to its *default* card degrades to an empty slot with a log instead,
and a soldered-down card such as the SE/30's onboard video synthesises its
own declaration ROM.

The resolved configuration is recorded in `machine.config` (`model`,
`ram`, `rom`, `rom_crc`, `rom2`, `vrom`, `vroms`, `slot_cards`,
`video_card`, `video_sense`, `video_mode`, `custom_mode`, `monitor`,
`pci_card`, `prom`, `pci_option`, `created`, `valid`;
`src/core/machine_config.c`). `machine.restart` and `checkpoint.load`
rebuild from it, the built-in `monitor` strap included.

**Headless command line.** The CLI arguments fill the same document and
call `machine_boot_apply` directly (`src/platform/headless/headless_main.c:1343-1350`):

| CLI | Boot document | Notes |
|---|---|---|
| `rom=<file>` | `rom` | Required. The CLI identifies the file itself first and exits if it does not identify (`headless_main.c:1298`). The directory's `*.vrom` and `*.prom` files are offered before the boot (`offer_sibling_card_roms`, `headless_main.c:931`). |
| `model=<id>` | `model` | Defaults to the first entry of the ROM's compatible list (for the Universal ROM, `se30`). An id outside that list is refused with the list printed (`headless_main.c:1303-1326`). |
| `ram=<kb>` | `ram` | Parsed with `strtoul`. A non-number becomes `0`, which means the model default (`headless_main.c:1156`). |
| `video_card=<id>` | `video_card` | |
| `monitor=<id>` | `monitor` | |
| (none) | `video_sense` | Always `-1` (unset). |

`vrom`, `prom`, `pci_card`, `pci_option`, `video_mode`, `video_sense`,
`custom_mode` and `rom2` have no CLI form. A script that needs them calls
`machine.boot` itself.

## Adding a new class

The pattern is the same regardless of whether the class is a process-
singleton or cfg-scoped:

1. **Declare the class** alongside the subsystem that owns it (e.g.
   `src/core/peripherals/foo.c`). Define a static `member_t` table
   covering attributes, methods, and any child sub-classes; wrap it
   in a `class_desc_t` named after the path segment.
2. **Implement getters / setters / methods** as plain C functions
   matching the framework's signatures. Read instance state through
   `object_data(self)`; emit `value_t` results. Method bodies trust
   the framework's validation contract — for any parameter declared
   with a concrete kind they read `argv[i]` (or `argv[i].u`,
   `argv[i].s`, …) directly without re-checking `argc`, kind, width,
   non-emptiness, or enum membership. Bodies still own *semantic*
   checks — value ranges that depend on runtime state ("HD already
   attached at id %d", "frequency must be a power of two") — and
   discrimination on `V_ANY` / `V_NONE`-kind slots.
3. **Attach the object** at the right lifecycle point — for cfg-scoped
   classes, do it inside the existing `*_init` (next to the
   `cfg->foo = foo_init(...)` call); for process-singletons, add a
   small `foo_class_register` / `foo_class_unregister` pair and call
   `foo_class_register` from `shell_init`.
4. **Register any built-in aliases** the class wants to expose
   (`alias_register_builtin`); idempotent, so safe under repeated
   inits.
5. **That's the whole change.** The shell, the JS bridge, tab
   completion, scripts, and the inspector pick the new class up
   automatically because they all walk the same tree.

A subsystem that wants to expose state and a few operations typically
ends up writing about 50–100 lines of class boilerplate plus the
attribute/method bodies it would have written anyway. Argument
validation that used to dominate the first few lines of every
method body now lives in the slot declaration (one line per
parameter) and is shared by every caller.

## Why the doc avoids enumerating classes

The set of classes, the attributes inside each class, and the exact
method signatures change as subsystems grow features. Listing them in
this document would produce a snapshot that begins rotting on the
first commit. The substrate, the path forms, the lifecycle invariants,
and the bridges are stable; those are what this document covers.

To see what's actually exposed at any given moment, walk the tree:

```
objects()           # children of the root
attributes(cpu)     # all readable/writable attributes on cpu
methods(scsi)       # all callable methods on scsi
help(machine.cpu.pc)        # the doc string declared on the member
```

Those four root methods are themselves part of the object model, so
they reflect the live state of the running emulator, not a doc
written months ago.

## See also

- [shell.md](shell.md) — terminal-side dispatch, tab completion,
  symbol resolution.
- [ARCHITECTURE.md](ARCHITECTURE.md) — overall code organisation.
- `src/core/object/object.h` — the substrate contract, with detailed
  docstrings on every public function.


### Singleton lifetime

A class registered as a process singleton (`<module>_class_register()` from
`shell_init`) stays registered for the life of the process. **There is no
unregister.** Four `*_class_unregister` functions used to exist — `find`,
`mouse`, `screen`, `vfs` — with zero callers between them, and the absence of
a shutdown path is deliberate rather than an omission: nothing in the process
lifetime needs one, and a half-built teardown story is worse than none.

`object_root_reset()` is the test-only path that tears the tree down, and it
routes through `object_delete` so the invalidator contract still holds.

*(If a future embedding needs to build and tear down the emulator repeatedly
in one process, that is the change that should add `shell_shutdown()` — with
all of it, not a piece.)*
