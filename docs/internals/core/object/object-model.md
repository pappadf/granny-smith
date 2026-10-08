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
| [object.h](../../../../src/core/object/object.h) | Core contract: classes, members, objects, nodes, path resolution |
| [object.c](../../../../src/core/object/object.c) | Substrate implementation (tree topology, resolver, validation, invalidators) |
| [value.h](../../../../src/core/object/value.h) | Tagged-union `value_t` used at every boundary |
| [value.c](../../../../src/core/object/value.c) | Value lifetime, conversions, formatting helpers |
| [parse.c](../../../../src/core/object/parse.c) | Path tokeniser shared by the resolver and completer |
| [expr.h](../../../../src/core/object/expr.h) | `${...}` expression parser and evaluator |
| [alias.h](../../../../src/core/object/alias.h) | Two-tier `$name` alias table (built-in + user) |
| [api.h](../../../../src/core/object/api.h) | Public C entry point (`gs_eval` — single dispatch for reads, writes, calls, schema, completion, and shell-line input) |
| [meta.h](../../../../src/core/object/meta.h) | The synthetic `Meta` class (`<path>.meta.*` introspection + `meta.complete`) |
| [usage.h](../../../../src/core/object/usage.h) | Usage text for any path (`help`, `shell.usage`) |
| [lint.h](../../../../src/core/object/lint.h) | The member-doc lint behind `shell.lint_members` |
| [shell_class.c](../../../../src/core/shell/shell_class.c) | The `Shell` class (`shell.run`, `shell.complete`, `shell.expand`, `shell.script_run`, `shell.interrupt`, `shell.prompt`/`running`/`vars`; aliases live on its `shell.alias` child: `add`/`remove`/`list`) |
| [root.c](../../../../src/core/object/root.c) | The `emu` root class plus install/uninstall lifecycle |

## Core concepts

### Values are typed and self-describing

Every value crossing an object-model boundary is a `value_t` — a tagged
union with a discriminator (`V_NONE` / `V_BOOL` / `V_INT` / `V_UINT` /
`V_FLOAT` / `V_STRING` / `V_BYTES` / `V_ENUM` / `V_LIST` / `V_MAP` /
`V_OBJECT` / `V_ERROR`), a width hint for fixed-size integers, and display
flags (`VAL_HEX`, `VAL_VOLATILE`, `VAL_SENSITIVE`, `VAL_PATH`, …). Errors are
in-band: a `value_t` of kind `V_ERROR` carries a string message rather
than relying on a separate return channel or out-pointer.

Ownership is single-owner: the receiver of a `value_t` owns it and
must call `value_free` (which is safe on every kind, including the
inline ones). Heap-owning kinds (`V_STRING`, `V_BYTES`, `V_ERROR`,
`V_LIST` and `V_MAP` recursively) `strdup` their inputs at construction
time, so there is no borrowed-string path to confuse callers.

`V_MAP` is the keyed sibling of `V_LIST`: an insertion-ordered sequence
of unique `{key → value}` entries with heap-owned keys and recursively
owned values. Map-shaped method results (`catalog.profile`,
`machine.rom.identify`, `debug.frame`, `meta.method_info`, …) return it
directly; the gsEval bridge serialises it as a JSON object exactly once
(the browser receives a native object, never a JSON string to re-parse).
Methods build maps with the `val_map_new` / `val_map_put` /
`val_map_finish` builder and read them with `value_map_get`. In `${…}`
interpolation a map renders as compact canonical JSON, so
`echo "${catalog.profile("se30")}"` emits machine-parseable text.

A method's UI metadata (`meta.method_info(name)`, and each method entry of
`meta.members()`) carries four flags from `ui_flags`: `destructive`,
`mutate`, `hidden` and `io`. `io` (`MM_IO`) marks an I/O job — a method
whose cost is the size of a file rather than of the machine (`files.cp`,
`files.archive.extract`, `image.export`, `files.download`, …): it answers when the work
ends, reports progress (`EVT_PROGRESS`), and a cancel of its request stops
it between chunks (see [`../../guide/web.md`](../../../guide/web.md), "I/O
jobs").

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
  framework with a uniform error before the body runs. An attribute
  is read-only exactly when it has no setter (`member_is_readonly`);
  `node_set` rejects it cleanly. A member's `flags` hold only its
  visibility category (`M_CAT_*`). Each member has a per-member `user_data` pointer so
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
  optional `width`, optional `enum_values`, optional `default_value`
  (or, for a default that is computed, a `default_doc`), a doc string,
  and per-slot flags (`OBJ_ARG_OPTIONAL`, `OBJ_ARG_REST`,
  `OBJ_ARG_NONEMPTY`, `OBJ_ARG_STRICT_KIND`). The framework validates
  argv against this declaration before invoking the body (see
  *Typed dispatch validation* below) and the completer reads the
  same metadata for argument-position suggestions.
- **`M_CHILD`** — a child object. Children are either *named* (a fixed
  name with its own class, attached or handed out by `lookup`) or a
  *collection's entries* (`child.collection`, see below). Collections are
  how `debug.breakpoints[7]` and `machine.floppy.drive[0]` work without
  the framework needing to know a collection's storage shape.

Callback bodies are written with `DEF_GETTER(name)`, `DEF_SETTER(name)`
and `DEF_METHOD(name)`, which spell the three signatures with the
parameters a body may leave unused marked so.

Member tables are static `const`. The framework walks them linearly
for resolution, completion, and help; no string lookup tables are
maintained at runtime.

### Collections and entries

Every collection has one shape: a **container node** (`machine.floppy.drive`,
`debug.breakpoints`, `files.mounts`, `log.category`) whose class has one
member naming a **collection descriptor**, `collection_desc_t`
(`OBJ_ENTRIES(&desc, doc)`, the member called `entries`). The descriptor
says how entries are handed out: its `entry` class, and `by_index`
(`get(i)` for each index in `[0, slots)`, NULL for a hole, or `next(prev)`
for ids with no fixed bound) and/or `by_key` (`lookup(key)` and a
`next_key(prev)` iterator over the live keys). Indices are sparse and
stable: a new entry takes max-index-ever + 1 and removed ids are never
reused. A collection may be both (`appletalk.afp.volumes`). A class has at
most one collection member, and that member is what makes a node a
container (`class_collection`): meta, lint, completion and `count` read the
descriptor, never member names. A container answers `count` with its live
entries unless it declares a `count` of its own. Collection verbs (`add`,
`clear`, `find`) live on the container; a container with nothing but its
entries (and perhaps a verb or two) is built from the descriptor alone by
`object_collection_new`, which reads its `name`, `doc`, `entries_doc` and
`verbs`. Paths address entries as `drive[0]`, `slot[9]` or
`category["scsi"]`; keys are short identifiers, `[A-Za-z0-9_.-]{1,63}`
(`object_valid_key`).

Entries, and lookup-backed named children such as `drive[n].disk` and
`device[n].image`, are handed out by callbacks and never attached, so they
have no parent of their own. They get a **logical parent**
(`object_set_logical_parent(obj, parent, name, index, key)`): a
non-owning back-link (never cascade-deleted, cleared if the parent is freed
first) that gives the entry its path — `<parent>.<name>`, `<parent>[<index>]`
or `<parent>["<key>"]` — as `meta.path` and the bridge report it. The
resolver adopts any callback-backed child that was not registered the first
time it hands one out (`object_entry_at`, `object_entry_by_key`,
`object_named_child`), so a missed registration costs nothing but the path
before first use. Reference children (`child.reference`, e.g.
`screen.source`) get no logical parent.

A module whose entries are objects of their own keeps them in an **entry
cache**, `object_cache_t`: entries made on first use by index
(`object_cache_at`) or key (`object_cache_key`), each linked to the
container whenever it is set (`object_cache_set_parent`, before or after),
each knowing its index (`object_entry_index`); `object_cache_sweep` frees
the entries whose record is gone and `object_cache_clear` frees the rest
with any attached subtree.

### The root

The root's children, in their fixed order (`object_set_order`), fall in three
domains:

| Order | Node | Domain | Holds |
|---|---|---|---|
| 0 | `machine` | machine | the emulated computer that exists now |
| 10 | `scheduler` | emulator | running the machine; its `mode` / `speed` / `max_speed` are the host's pacing, reached through the machine (`mode` is the enum `paced` / `accelerated` / `turbo`) |
| 20 | `checkpoint` | emulator | save / load / snapshot |
| 21 | `pacing` | emulator | the host's pacing setting (`mode`, `speed`, `max_speed`), there with or without a machine; advanced |
| 30 | `files` | emulator | host files and disk images (`ls`, `cp`, `hd_create`, …, `download`), `files.images[n]` (the machine's configured images), `files.mounts[n]` (the image-VFS auto-mount cache, indexed by a never-reused mount serial; `find(path)`, `[n].unmount()`), `files.archive` |
| 40 | `debug` | emulator | breakpoints, logpoints, watchpoints, `debug.find` (memory search), `debug.mac` |
| 50 | `log` | emulator | `log.set(cat, level=, …)`, `log.levels`, `log.category["<cat>"]` with `level` / `stdout` / `file` / `ts` / `pc` |
| 60 | `shell` | emulator | bindings, functions, `shell.alias` |
| 70 | `catalog` | emulator | what the emulator can build or fit: `models`, `profile(model)`, `nubus_cards`, `pci_cards`, `vroms`, `proms` |
| 100 | `appletalk` | network | the simulated network |

Each root child carries its **domain** (`object_set_domain`: machine,
emulator or network), which draws the dividers at the tree's top.

### Metadata: docs and types

A class declares a one-sentence `doc`; an object can override it
(`object_set_doc`), and `object_doc` answers the object's doc, else its
class's.  A method may declare a `result_doc` and `examples`.

`meta.members(values?)` describes each member with, besides name, kind,
category, label and doc: for an attribute
its `type` descriptor `{kind, width, presentation, enum}` (presentation is
the first of sensitive / path / hex / bin / dec, `VAL_PATH` marking a string
that names a VFS path); for a method its `args` (name, doc, type, optional,
rest, default), `result` type and, when declared, `result_doc` and
`examples`; for a child `collection` (and a container's live `indices` /
`keys`) and, at the root, `domain`.

`shell.lint_members()` (internal) walks the live tree (`object_walk`, the
one walk that hands out every object once with its canonical path) and
reports every documentation gap: per class, once, the members' own gaps
(`object_member_doc_gaps`, beside `object_validate_class`: an undocumented
basic-tier argument, an untyped argument without `OBJ_ARG_POLY`, a `V_ANY`
result without `result_doc`) and an example with a path that does not
resolve (checked with `shell.highlight`); per node, a node shown in the
basic tier with no doc. What a declaration gets outright wrong (an enum
without its values) the class validator refuses. `tests/integration/member-docs`
fails on any gap not in its allow-list, which may only shrink (it is empty).

A doc says what a member is; the usage text says how to call it.  So an
argument's default is declared (`default_value`, which usage prints as
`(default …)`) rather than written into its doc; a default that is computed
is declared as `default_doc` (shown after the doc as `; omitted: …`); and
invocation forms go in `examples` (`EXAMPLES("…", "…")`). A string argument
that names a VFS path is `ARG_PATH(name, doc)`. `arg_has_default` (a value
other than none or the empty string), `member_is_readonly`,
`member_is_basic` and `member_is_listed` are the one reading of those
questions for meta, usage and lint.

`machine` is the instance; `catalog` is the catalogue. `machine.nubus` and
`machine.pci` are attached only on a machine with that bus. The root's own
methods are `objects`, `attributes`, `methods`, `help`, `time`, `quit` and
`echo`.

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

    machine.rom.identify("boot.rom").compatible[0]   # call → map → list → value
    catalog.profile("se30").capabilities.mmu.kind
    let info = machine.rom.identify("boot.rom")
    echo "${$info.checksum} ${$info["name"]}"
    for k in catalog.profile("se30").capabilities { echo "$k" }   # keys

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
`for`, `in`, `break`, `continue`, `return`, `def`, `assert`, `include`.
Literals: `true`, `false`, `none`. Held: `do`. These may not be used as
member, alias, or binding names (`object_validate_name`). `command` is a
keyword too, but contextual: only `command NAME = PATH` is the statement,
so a member may be named `command`. One table in `object.c` holds them all,
with each word's syntax; `shell.keywords` and completion read it. `on`/`off`/`yes`/`no`
are **not** reserved — they remain accepted as input coercions for
bool-typed argument slots only.

### Library conventions

- **Methods return data; surfaces do the printing.** Search results
  come back as lists (`debug.find.str(...)` → list of addresses; empty =
  not found; `[0]` is the first hit). `*.list` printers are retired:
  an indexed collection read without an index (`debug.breakpoints.entries`)
  returns the entry objects as a list, and the REPL renders a list of
  same-class objects as a table.
- **Success/failure flows as value-or-`V_ERROR`**, never a printed
  message plus a bool. `V_BOOL` returns are reserved for methods whose
  *answer* is a boolean (`files.path_exists`).
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
  and session surfaces (`scheduler`, `checkpoint`, `files`, `debug`,
  `log`, `shell`, `catalog`) live at the root.

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
- **`none` as "unset".** A slot flagged `OBJ_ARG_NONE_OK` also takes
  `none`, which the setter reads as "clear it", and its getter may answer
  `none` while unset; every other kind still has to match. It is for an
  attribute that is either a value or nothing, such as
  `machine.scc.a.output` (a file path, or `none` when no output is
  attached).
- **Enum membership.** `V_ENUM` slots validate the index is in
  `enum_values`; `V_STRING` input is looked up against the same
  table and rewritten to `V_ENUM` so the body always sees the
  enum form (see coercion below).
- **`V_OBJECT` non-NULL.** `V_OBJECT` slots reject `argv[i].obj == NULL`.
- **Default fill.** Optional parameters that declare a `default_value`
  are synthesised into the rewritten argv when the caller omits them,
  so the body reads `argv[N]` without an `argc` check. An optional
  parameter with no default that the caller skips — at the tail, or as a
  hole before a later named argument — reaches the body as `none`
  (`V_NONE`); every declared slot is readable, and `argc` counts through
  the last slot that was given or defaulted. A slot flagged
  `OBJ_ARG_GROUPED` belongs to an all-or-nothing group the body checks by
  count, and may not be skipped alone.

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
  doesn't match the slot; `default_value` and `default_doc` together.
- `V_ENUM` slot with no `enum_values` table.
- Arg-only flags (`OBJ_ARG_OPTIONAL`, `OBJ_ARG_REST`,
  `default_value`) set on an attribute slot.
- Member `flags` outside the visibility category bits.
- More than one collection member in a class, or a collection that hands
  out no entries (`by_index.get` or `by_key.lookup`), or a `next` /
  `next_key` without its `get` / `lookup`.

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
  Scripted machine setup (`machine.boot(model=..., rom=...)`,
  `machine.floppy.drive[0].insert(...)`, `machine.scsi.attach_hd(...)`) is the same
  call sequence whether it runs from a script, from the user's
  terminal, or from the URL-media auto-boot path on the web.
- **JS / WASM bridge.** `gs_eval(path, args_json, out_buf, size)`
  resolves the path, parses arguments from JSON, invokes the right
  read / write / call, and serialises the result back to JSON. JS
  reaches it through the mailbox (`src/core/mailbox/mailbox.h`, exposed
  via the lone `_get_gs_mailbox` export): a request ring the page writes
  and the worker's `shell_poll()` drains every tick, and an event ring
  the answers come back on, each carrying its request's id.
  `Atomics.waitAsync` + `emscripten_atomic_notify` carry the wake-ups —
  no polling. The worker-thread guard
  in `worker_thread.h` enforces that `gs_eval` only runs on the worker
  pthread, never via direct `Module.ccall` from the main thread. JS
  callers see numbers, strings, lists, and `{error: "…"}` shapes —
  never raw exit codes. See [`web.md`](../../../guide/web.md) for the wire layout
  and protocol.
- **Which thread runs a leaf.** Always the emulator thread. A script runs
  on the job thread (`src/core/job/job.h`), but its `node_get` /
  `node_set` / `node_call` marshal through the **seam**
  (`job_on_emulator`) and execute in the emulator thread's drain, so a
  leaf's author never sees another thread: guest state is reachable
  without locks, and the seam is what makes the interpreter's scope,
  alias and function tables the only shared ones (behind
  `job_tables_lock`). A leaf whose cost is the size of a file — flagged
  `io` (above) — hands its work to the I/O worker through `io_leaf.h` and
  answers later (`gs_result_defer` / `gs_result_complete`); the worker
  touches host files only. What a leaf prints goes through `gs_out.h`
  (`gs_outf`, never `printf`) to the client whose request it is.

  **The result contract** (what `gsEval` in `app/web2/src/bus/emulator.ts`
  resolves to): a value is the result; `null` is **only** a successful
  method that returns nothing (V_NONE); every failure is an
  `{error: "…"}` object — the core's V_ERROR message, or, for a failure of
  the bridge itself (module not ready, a thrown request), the same shape
  with `transport: true`. A result larger than the mailbox's limit
  (`GS_MBX_RESULT_MAX`, 256 KB) is such an error too, naming its size and
  the limit; it is never truncated. So `r !== null` is never a success
  test (an `{error}` satisfies it): use `gsOk(r)` for "did it work", `r === true`
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
  entry debug objects (breakpoints / logpoints / watchpoints) are attached when a
  machine is created (`system_create` → `profile->init`) and torn
  down when the machine is destroyed. Their `_init` is the place
  where the object node is allocated and attached to the root, and
  where any built-in aliases the subsystem owns get registered.

The cfg-scoped install path (`root_install`) attaches the shell namespace,
then runs every subsystem's **install hook**: a subsystem registers
`root_register_install(install, uninstall)` once (storage from
`files_init`, NuBus and PCI from their object build) and its install
attaches its own nodes with `root_attach_stub` — `files.images`,
`machine.nubus`, `machine.pci` — so the object layer includes no
subsystem header. It is idempotent for the
same `cfg` and atomic across cfg changes.  `machine.boot` and
`checkpoint.load` both build, then swap, then destroy: `system_create(new)`
builds a machine that is not yet the active one, `system_swap_in(new)` runs
`root_install(new)` and destroys the old machine, and the destroy path no-ops
when the installed cfg is no longer "its" cfg. This invariant lives in
`root.c`.  A build that fails was never installed, so the running machine's
tree, label and state are untouched.

The result is that paths like `machine.cpu.pc` resolve as soon as a machine is
booted and disappear cleanly when the machine is torn down, without
the caller having to track machine-lifetime explicitly.

## Machine lifecycle

Four operations put a machine in place or restart the one that is running.
Two are reset **levels** of the running machine; the other two build a new
machine, from a boot document or from a checkpoint. They differ in exactly
one thing: whether the machine is torn down. Only building a new machine
tears down; a reset or a power cycle keeps every device, so the
non-volatile stores, the mounted media, the Caps Lock latch and the
LaserWriter survive for the hardware's own reason -- nothing destroyed
them. A new machine inherits nothing from the old one.

| Level | Operation | What it does | Entry point |
|---|---|---|---|
| 2 | `machine.reset` | **Warm reset** (the reset button): the board's /RESET net, then the CPU back to its reset vector. RAM is kept. | `machine_method_reset` → `system_machine_reset` (`src/core/system.c`) |
| 3 | `machine.restart` | **Power cycle**: the same reset with the RAM cleared to the state a new machine's has, and the board's power-on-only state (`substrate->power_on`, and each NuBus card's `power_on`) back to its constructed values -- video VRAM included, so the old picture does not outlive the power. Nothing is torn down or rebuilt. | `machine_method_restart` → `system_machine_power_cycle` |
| -- | `machine.boot(...)` | **New machine from a document** ([Boot arguments](#boot-arguments)): validates the whole document, builds the new machine with its ROM, swaps it in and destroys the old one. | `machine_method_boot` → `machine_boot_apply` (`src/machines/machine.c`) |
| -- | `checkpoint.load(path)` | Builds a new machine from a checkpoint: it creates the new machine first and destroys the old one afterwards. | `system_checkpoint_load` → `system_restore` |

Level 1 is the 68k `RESET` instruction: the /RESET net alone, with the CPU
untouched (`system_reset_devices`). It has no verb -- it is an instruction
and a wire, not something a user does.

`machine.boot` and headless startup both go through
`machine_boot_apply`, so they share one sequence: validate, build, swap,
destroy. Every check runs before the running machine is touched, so a
rejected boot leaves it untouched
(`tests/integration/boot-config`). `tests/integration/reset-levels` runs
the level contract over every model in the registry.

### What each operation keeps

| State | `machine.boot` | `machine.restart` | `machine.reset` | `checkpoint.load` |
|---|---|---|---|---|
| Devices | New | **Kept** | **Kept** | New |
| Model, RAM size, cards | From the document; each omitted field takes the model's default | Unchanged | Unchanged | From the checkpoint's parts: the board's (model, RAM size) and one part per seated card (`nubus.slot.<n>`, `pci.slot.<n>`: its slot entry, ROM bytes and state) |
| ROM bytes | Read from the `rom=` file before the running machine is touched, and built into the new machine's ROM region at construction | Unchanged | Unchanged | From the checkpoint, by content or file reference |
| RAM contents | Zeroed (fresh `calloc`) | **Cleared** to zero | **Kept** | Restored |
| CPU registers | Reset | Reset vector, reset-state SR/VBR/CACR, MMU and TTx enables off | The same | Restored |
| PRAM (RTC parameter RAM) | Family construction defaults (`rtc_init` → `pram_defaults_apply`; tables in `src/machines/runtime/pram_defaults.h`) | **Kept** | **Kept** | Restored |
| NVRAM (`machine.nvram`: Grand Central on TNT/ANS, Heathrow on the beige G3) | Blank | **Kept** | **Kept** | From the checkpoint |
| RTC time | Host wall clock at construction; the Lisa's COPS clock starts at 1 January 1984 (`src/machines/lisa/cops.c`; hardware reference: [cops.md](../../../reference/machines/lisa/cops.md)) | **Keeps counting** | **Keeps counting** | The saved value |
| Mounted media | None. The old machine's images are closed (`system_destroy`); a CD bay is registered empty | **Kept** | **Kept** (`floppy_reset` keeps media) | From the checkpoint |
| Caps Lock latch | Released | **Kept** | Kept (`adb_reset` preserves held keys) | From the checkpoint's ADB state |
| ADB devices | New | Back at their default addresses (the bus loses power: `adb_power_on`) | Kept; the ROM's ADB SendReset resets them | Restored |
| Host pacing (`pacing.mode`, `.speed`, `.max_speed`; also reached as `scheduler.*` while a machine runs) | Host state (the platform's run loop): untouched, and the new machine runs under it | Unchanged | Unchanged | Untouched: never in a checkpoint |
| vROM/PROM offer registries | Process-global; survive | Survive | Survive | Survive |
| A slot's ROM file (`vrom=`/`prom=`, a slot's `rom=`) | The document's, an argument of that slot's card; never written to the offer registries | Unchanged | Unchanged | From the card's part of the checkpoint, which carries the ROM's bytes (a checkpoint restores without the file) |
| Each display device's monitor (the document's displays; `monitor=`, `video_sense=`) | The document's, an argument of the device: the built-in port's sense code, each card's monitor row and sense code in its slot entry | Unchanged | Unchanged | From the device's part of the checkpoint (a card's from the bus's slot table) |
| Each NuBus card's declaration ROM: Apple's or its substitute | Decided as the bus seats the card | Unchanged | Unchanged | From the card's part of the checkpoint (Apple's bytes, or nothing for the substitute, which is regenerated) |
| Object tree | Machine-scoped nodes rebuilt (`root_install`); process singletons (`machine`, `rom`, `vrom`, `prom`, `pacing`, `appletalk`) stay | Untouched | Untouched | Rebuilt |
| AppleTalk network (`appletalk.*`: the AFP server and its shares, the LaserWriter, the program-linking peer, their NBP names) | Host state: untouched, but for the LaserWriter, which restarts (a job in flight and what jobs made permanent go); the new machine plugs into it | Untouched | Untouched | As `machine.boot`: never in a checkpoint. Nor kept across a page reload: the web app saves none of it, and a reloaded page starts with only the default share |
| The machine's AppleTalk connection (`cfg->atalk`: link state and counters, ATP transactions, its ASP / AFP / ADSP / PPC sessions, forks, Apple events) | New, with no sessions; the old machine's sessions closed | Kept | Kept | Its block restored (enabled flag, link counters, session and ATP transaction numbering), with no sessions: the guest sees a restarted server |

**`machine.boot` inherits nothing from the running machine.** The
document is the whole specification. `model` and `rom` are required, and
every other field falls back to the **model's** defaults, never to the
previous machine. That holds across a model change too
(`tests/integration/boot-config`). For this reason the integration runner
passes the ROM explicitly: it starts headless with `rom=` and also exports
the same path as `$ROM`, so a script re-boots with
`machine.boot model=... rom="${$ROM}"`
(`scripts/run-integration-test.sh`). To get the machine you have again
from scratch, boot the same document again with `machine.boot`, or restore
a checkpoint with `checkpoint.load`; a bare `machine.boot()` is an error. A new boot does still see three kinds
of process-level state that are not part of any machine:

- scheduler pacing (the table above);
- the offer registries ([mac-rom.md §10](../../../reference/formats/mac-rom.md#10-rom-provisioning)),
  which a boot reads and never writes;
- the AppleTalk network (the table above), with the default share the
  platform published on it once, at startup (`system_set_default_share`).

There is no way to configure the next machine except its document: the
running machine's slot nodes describe what is installed, and a different
card is a different `machine.boot`.

**Device state is written to the device, never staged.** A caller who
wants a different clock, PRAM byte or NVRAM setting writes it on the
device, on the line after the boot: `machine.boot` leaves the guest at 0
instructions executed, so the write lands before the ROM has looked at
anything. There is no pre-boot form and no holder that survives into the
next machine.

**The /RESET net, and what a power cycle adds.** Each family binds one
`substrate->bus_reset`: every device that board wires to /RESET. Levels 1
and 2 both call it, and level 2 adds only the CPU half
(`cpu_reset_to_vector_*`, `ppc_reset`). A power cycle runs one more,
optional hook first, `substrate->power_on`, for state a power-up clears
and /RESET does not reach: the Lisa MMU's START latch and descriptor RAM,
the ASC's Power On Clear, the IIfx's OSS, IOPs and SCSI DMA, Cuda's host
handshake. Two shared pieces belong to every board: the ADB bus loses
power with the machine (`adb_power_on`), and with VIA1 reset the RTC's
`/CE` floats high, so the clock chip drops any half-done transfer
(`rtc_deselect`).

**Differences between families, and known gaps.**

- *The Lisa's warm reset.* A Lisa reset does not set the MMU's START
  latch -- only power-on does ([mmu.md](../../../reference/machines/lisa/mmu.md) §2.6) -- so
  `machine.reset` fetches the vectors through whatever map the OS left.
  What the hardware then does is the ROM's warm-start path and is not
  verified; `machine.restart` (power-on) is.
- *A failed build.* `machine.boot` and `checkpoint.load` build the new
  machine while the running one stays active; a build that fails is
  discarded and the running machine is exactly as it was
  (`tests/integration/checkpoint-failed-restore`).
- Host-side state outside the construction configuration (volume,
  camera/microphone capture sources) is not part of a boot document. The frontend asserts it again (`app/web2/src/bus/boot.ts`,
  `reconcileUiWithMachine`). A restart keeps the machine, so the
  frontend re-asserts nothing after one.

## Boot arguments

`machine.boot` takes only named arguments (`machine_boot_args`,
`src/machines/machine.c`). The shell writes them as `name=value`; the
JS bridge passes them as one JSON object (`initEmulator`,
`app/web2/src/bus/boot.ts`), whose `config` member is the configuration
document as a JSON string. An empty string, `0`, or `0xFF` for
`video_sense` means "not given". An explicitly empty value such as `rom=`
is rejected by the grammar before binding (`machine.c:1256-1264`). On
success the call returns `true`; otherwise it returns a `V_ERROR` and the
old machine keeps running.

| Argument | Kind | Default | Meaning and validation |
|---|---|---|---|
| `model` | string | **required** | Machine model id (`catalog.profile(id)` describes one). Rejected if missing or not registered (`machine.c:857-862`). |
| `rom` | string | **required** | Path to the ROM file. It must be readable and identify, by content id, as a known ROM whose compatible list contains `model` ([mac-rom.md §10](../../../reference/formats/mac-rom.md#10-rom-provisioning); `machine.c:873-899`). |
| `config` | string (JSON) | the model's default configuration | The configuration document ([below](#the-configuration-document)): options, floppy drives, storage devices, the startup device, expansion cards and the connected display. |
| `ram` | uint (KB) | the model's default memory | Sugar for `options.memory`. Must be one of the model's memory sizes (`catalog.profile(id).options`, the `memory` entry); a restored checkpoint's size passes the same check. |
| `rom2` | string | none | The second chip of a two-chip Lisa/XL ROM. It only has to be readable: the chips identify after interleaving, so per-file identification and the compatibility check are skipped (`machine.c:878-884`). |
| `slots` | string | the slots' own cards | Per-slot configuration, `SLOT=CARD[,key=value]*;...` (`machine_slots.c`). `SLOT` is the slot number as `machine.nubus.slot[N]` / `machine.pci.slot[N]` index it (decimal, or `$A` / `0xA`); `CARD` is a card id, `none` for an empty socket, or empty for the slot's own card. `mode=`, `custom=` and `rom=` set the slot's video mode, custom geometry and ROM file (`rom=substitute`: the card's substitute declaration ROM); any other key is a card option. Every entry is checked against its slot before teardown: the slot exists and takes a card, the card fits it, the mode belongs to the card (with the ROM it will run), the geometry fits it, the card accepts each option, and the ROM identifies as the card's. |
| `vrom` | string | resolved from the offers | A NuBus declaration-ROM file: the ROM of every slot whose card its content provides (sugar for those slots' `rom=`). The file must identify as a known declaration ROM (`vrom_identify_card`). |
| `video_card` | string | the slot default | Card id for the machine's **first** NuBus socket (on a machine with none, its built-in slot): sugar for that slot's `slots=` entry, which it may not contradict. Rejected on a model with no NuBus slots, and for an unknown id (with a "did you mean" hint). |
| `video_sense` | uint | the connected monitor's | A debug override of the **connected** display device's sense code; its monitor (and so its geometry) stays. 0–7 is the passive code; 8–14 is Apple's indexed numbering for monitors that answer the extended probe, which only a Quadra's DAFB built-in port takes. Rejected when the connected device is a PCI card (it takes its `monitor=` option). |
| `video_mode` | string | the card's default | Video-mode id for the first socket (sugar for its `mode=`). It must belong to that slot's card. |
| `custom_mode` | string | none | Custom resolution `WxHxD` for the first socket (sugar for its `custom=`), on the card's substitute ROM; only the 8•24 takes one (its kind's `custom_mode_fits` hook). Parsed and rejected with the reason. |
| `monitor` | string | the model's default | Sugar for `displays.builtin.monitor`: the monitor on the **built-in** video port, a monitor-catalogue id the port takes (`catalog.profile(id).displays.builtin.monitors`; the family's own legacy ids are accepted too). `none` leaves the port unconnected, which connects the monitor to the first display card instead. It resolves to the port's sense code at construction. |
| `pci_card` | string | the slot default | Card id for the machine's **first** PCI socket (sugar for its `slots=` entry). Rejected on a model with no PCI slots, and for an unknown id. |
| `prom` | string | resolved from the offers | A PCI expansion-ROM file: the ROM of every slot whose card its content provides. The file must identify as a known Open Firmware expansion ROM (`prom_identify_card`). |
| `pci_option` | string | none | `key=value[,key=value]` options for the `pci_card` socket (for example `vram=4m`; sugar for that slot's options). Each pair must be one the card's `accepts_option` hook accepts, or the boot is rejected. |

The sugar and `slots=` resolve, once, into one entry per configured slot
(`machine_slots_resolve`); below `machine_boot_apply` nothing knows the
sugar exists. A NuBus card runs Apple's declaration ROM when one is offered
(or its slot's `rom=` names one), else its substitute ROM, the emulator's
generated one ([nubus_generic_vrom.md](../peripherals/nubus_generic_vrom.md)).
A card the **document names** with neither -- a PCI card whose FCode
expansion ROM is not offered -- rejects the boot before teardown; a socket
that falls back to its *default* card degrades to an empty slot with a log
instead.

### The configuration document

`catalog.profile(id)` describes everything a model can be built with, as
one tree with every label written by the core
(`src/machines/machine_config.c`): `options` (memory, AppleTalk, the
Network Server's keyswitch and power supplies), `floppies` (each drive
position and the drive types it takes, `none` where it may be empty),
`storage` (each bus: its units with their position labels, reserved IDs,
the buses it shares an ID space with, its bays, the device types it takes
and whether the startup record can name a device on it), `slots` and the
`cards` that fit them (with each card's ROM status, monitors, startup
modes and options), `displays` (built-in video and its monitors) and the
`monitors` catalogue (`src/core/peripherals/monitor_catalog.c`).
`catalog.default_config(id)` is the model's default configuration as a
document, which is also `catalog.profile(id).defaults`.

The document mirrors the tree:

```json
{ "options":  { "memory": "8192", "appletalk": "active" },
  "floppies": { "fd0": "hd", "fd1": "none" },
  "storage":  [ { "bus": "scsi", "unit": 0, "type": "hd" },
                { "bus": "scsi", "unit": 3, "type": "cd" } ],
  "startup":  { "bus": "scsi", "unit": 0 },
  "cards":    [ { "slot": "nubus_c", "card": "mdc_8_24", "options": {} } ],
  "displays": { "builtin": { "monitor": "none" },
                "nubus_c": { "monitor": "13in_rgb", "mode": "640x480x8" } } }
```

Each top-level key, when present, is the complete set for its node; an
absent key is the model's default, and within `options`, `floppies` and
`displays` an absent member takes its default (an absent display device
is unplugged when another one is connected).  `"storage": []` is a
machine with no drives; `"startup": null` is "no default startup device"
(the ROM searches).  A `model` member must agree with `model=`.  Images
are not configuration: they are attached after the boot to the device at a
position (`machine.attach_media(bus, unit, type, path)`; `attach_hd(path,
n)` and `attach_cdrom(path)` name the default configuration's Nth hard
disk and its CD-ROM drive).  `machine.storage` lists the devices the
running machine was built with -- `{bus, bus_label, unit, position, type,
present}` each, kept with the machine and in its checkpoint -- which is what
the web app's Images panel offers an image to.

The whole document is validated before anything is built, and a
rejection names the node at fault: an option value the model does not
offer (V1); a floppy drive type the position does not take (V2); a bus the
model does not have, a unit it does not have or reserves, a unit used
twice across buses sharing an ID space, a device type the bus does not
take (V3); a slot the model does not have, a card that does not fit it, a
slot used twice or excluded by another occupied slot (V4); a card option
value the card does not declare (V5); a card whose ROM is missing and that
has no substitute (V6); a display key that is not a display device of
this configuration, a monitor the device does not take, a mode the
monitor does not have, more than one connected monitor (V7); the legacy
card or display arguments beside the document's `cards` / `displays`
(V8); a startup device not in `storage`, or on a bus the machine's startup
record cannot name (V9).  `tests/integration/machine-config-validation`
pins one case per rule.

The document is a construction input only.  The **seeding step**
(`machine_substrate_t.seed`, `src/machines/runtime/config_seed.h`) writes
the parameter-memory records that follow from it -- AppleTalk's on/off
state in SysParam (with the rest of SysParam as the machine's own ROM
initialises it, `pram_defaults_t.sysparam`) and the default startup
device -- once, when a new machine is built; never on a restore, a reset
or a power cycle, after which the store is the guest's.

A running machine keeps no record of the document it was built from: each
fact is read from the object that holds it -- `machine.id`, `machine.ram`,
`machine.rom.path` / `.id`, `machine.nubus.slot[N].card.id` and its
`declrom.path` / `.crc` (the declaration ROM the card resolved),
`machine.pci.slot[N].card.id`.  A checkpoint carries each fact in the part
of the object it belongs to, and `checkpoint.load` builds from those parts
(`src/core/machine_parts.h`; [checkpointing.md](../checkpointing.md)).

**Headless command line.** The CLI arguments fill the same document and
call `machine_boot_apply` directly (`src/platform/headless/headless_main.c:1343-1350`):

| CLI | Boot document | Notes |
|---|---|---|
| `rom=<file>` | `rom` | Required. The CLI identifies the file itself first and exits if it does not identify (`headless_main.c:1298`). The directory's `*.vrom` and `*.prom` files are offered before the boot (`offer_sibling_card_roms`, `headless_main.c:931`). |
| `model=<id>` | `model` | Defaults to the first entry of the ROM's compatible list (for the Universal ROM, `se30`). An id outside that list is refused with the list printed (`headless_main.c:1303-1326`). |
| `ram=<kb>` | `ram` | Parsed with `strtoul`. A non-number becomes `0`, which means the model default (`headless_main.c:1156`). |
| `config=<file or JSON>` | `config` | A file holding the document, or the document itself. |
| `drive=<bus>:<unit>:<type>[:<image>]` | `config.storage` | Repeatable: adds a drive to the default configuration's (or `config`'s), with its image attached after the boot. |
| `fd1=<image>` (or a second `fd=`) | `config.floppies` | Puts a drive at the second position when the default configuration leaves it empty. |
| `video_card=<id>` | `video_card` | |
| `slots=<spec>` | `slots` | |
| `monitor=<id>` | `monitor` | |
| (none) | `video_sense` | Always `-1` (unset). |

`hd=` and `cdrom=` keep their meaning: the image for the default
configuration's first hard disk and its CD-ROM drive.  `vrom`, `prom`,
`pci_card`, `pci_option`, `video_mode`, `video_sense`, `custom_mode` and
`rom2` have no CLI form. A script that needs them calls `machine.boot`
itself.

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
   `cfg->foo = foo_init(...)` call), or register a root install hook
   (`root_register_install`) when the node belongs under a root stub;
   for process-singletons, add a small `foo_class_register` and call it
   from `shell_init`. A collection is a `collection_desc_t` plus, when
   its entries are objects of their own, an `object_cache_t`.
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

- [shell.md](../shell) — terminal-side dispatch, tab completion,
  symbol resolution.
- [ARCHITECTURE.md](../../../guide/ARCHITECTURE.md) — overall code organisation.
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
