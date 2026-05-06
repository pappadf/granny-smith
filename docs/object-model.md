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
| [api.h](../src/core/object/api.h) | Public C entry points (`gs_eval`, `gs_inspect`, `gs_complete`) |
| [root.c](../src/core/object/root.c) | The `emu` root class plus install/uninstall lifecycle |

## Core concepts

### Values are typed and self-describing

Every value crossing an object-model boundary is a `value_t` — a tagged
union with a discriminator (`V_NONE` / `V_BOOL` / `V_INT` / `V_UINT` /
`V_FLOAT` / `V_STRING` / `V_BYTES` / `V_ENUM` / `V_LIST` / `V_OBJECT` /
`V_ERROR`), a width hint for fixed-size integers, and display flags
(`VAL_HEX`, `VAL_RO`, `VAL_VOLATILE`, `VAL_SENSITIVE`, …). Errors are
in-band: a `value_t` of kind `V_ERROR` carries a string message rather
than relying on a separate return channel or out-pointer.

Ownership is single-owner: the receiver of a `value_t` owns it and
must call `value_free` (which is safe on every kind, including the
inline ones). Heap-owning kinds (`V_STRING`, `V_BYTES`, `V_ERROR`,
`V_LIST` recursively) `strdup` their inputs at construction time, so
there is no borrowed-string path to confuse callers.

Display flags follow the value: an attribute declared with `VAL_HEX`
emits values that the JSON encoder serialises as `"0x12345678"`; the
shell formatter prints them in hex; the inspector panel renders them
the same way. Each layer reads the intent off the value, not off the
attribute, so passing a `cpu.pc` reading through a function still
formats correctly.

### Classes describe what a node can do

A `class_desc_t` is a static description with a name and a member
table. Each `member_t` is one of three kinds:

- **`M_ATTR`** — a typed attribute with a getter and (optionally) a
  setter. Attributes that lack a setter or carry the `VAL_RO` flag
  reject `node_set` cleanly. Each member has a per-member `user_data`
  pointer so dispatchers shared across many attributes (the 471-entry
  `mac.globals` is one such case) recover their context from the
  member descriptor instead of via lookup tables.
- **`M_METHOD`** — a callable taking declared `arg_decl_t` parameters
  and returning a `value_t`. Argument declarations carry a kind, a
  doc string, and an optional `OBJ_ARG_OPTIONAL` / `OBJ_ARG_REST`
  flag, which is enough metadata for the framework to validate and
  for the completer to suggest sensible argument values.
- **`M_CHILD`** — a child object. Children are either *named* (a fixed
  name with its own class) or *indexed* (a sparse, stable-id
  collection accessed via `child.get(i)` / `child.count` / `child.next`
  callbacks). Indexed children are how `debug.breakpoints[7]` and
  `floppy.drives[0]` work without the framework needing to know a
  collection's storage shape.

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
| `path` | Read the attribute, format the result, return / print | `cpu.pc` |
| `path = value` | Write the attribute (setter) | `cpu.d0 = 0x1234` |
| `path arg arg …` | Call the method, shell form (whitespace-separated args) | `floppy.drives[0].insert /tmp/fd0 false` |
| `path(arg, arg, …)` | Call the method, expression form (used inside `${…}`) | `${memory.peek.l(0x1000)}` |

Shell form is what users type interactively. Expression form is what
appears inside `${…}` interpolation in scripts and logpoint messages.
Both compile to the same `node_call` underneath.

`${…}` evaluates an expression and substitutes its formatted value
into the surrounding string. Inside, `$name` is an alias lookup (see
below); a bare path is a `node_get` (or, with one trailing argument
list in expression form, a `node_call`); literals and operators
work the way they do in C, with truthiness following the proposal's
falsy rules (empty string, "false", "0", any error).

`$name` is the alias surface. Two tiers:

- **Built-in aliases** are registered by subsystems at `*_init` time.
  `cpu_init` sets up `$pc` → `cpu.pc`, `$d0` → `cpu.d0`, … . Re-
  registering with the same target is a no-op, so repeated machine
  boots are safe.
- **User aliases** are added at runtime via `shell.alias.add`. They
  cannot collide with built-ins or reserved words, and they don't
  persist across the process.

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
  Scripted machine setup (`machine.boot('plus')`, `rom.load(...)`,
  `floppy.drives[0].insert(...)`, `scsi.attach_hd(...)`) is the same
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

The result is that paths like `cpu.pc` resolve as soon as a machine is
booted and disappear cleanly when the machine is torn down, without
the caller having to track machine-lifetime explicitly.

## Adding a new class

The pattern is the same regardless of whether the class is a process-
singleton or cfg-scoped:

1. **Declare the class** alongside the subsystem that owns it (e.g.
   `src/core/peripherals/foo.c`). Define a static `member_t` table
   covering attributes, methods, and any child sub-classes; wrap it
   in a `class_desc_t` named after the path segment.
2. **Implement getters / setters / methods** as plain C functions
   matching the framework's signatures. Read instance state through
   `object_data(self)`; emit `value_t` results.
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
attribute/method bodies it would have written anyway.

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
help(cpu.pc)        # the doc string declared on the member
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
