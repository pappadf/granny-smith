# Shell

This document describes the shell layer in `src/core/shell/`. The shell
is the line-oriented interface to the [object model](object-model.md):
it tokenises input, expands `$alias` and `${expr}` substitutions,
dispatches to the right node operation, and drives tab completion. It
is not, by itself, a command framework — the operations live on the
object tree, and the shell is just one of the surfaces that walk it.

## Overview

Two callers reach the emulator through the shell layer:

- The **xterm.js terminal** in the browser, where users type commands
  interactively.
- The **headless CLI** (`gs-headless`), which reads commands from a
  script file (`script=...`) or stdin.

Both go through the same tokeniser and the same path-form dispatcher.
JavaScript callers in the web frontend reach the shell through the
`Shell` class on the object root: typed object-model calls
(`gs_eval('cpu.pc')`) stay on their typed paths, and free-form lines
from the xterm.js terminal route through `gs_eval('shell.run', [line])`
(see [`proposal-shell-as-object-model-citizen.md`](../local/gs-docs/proposals/proposal-shell-as-object-model-citizen.md)).
No dedicated bridge kind for free-form lines, no
`Module.onPromptChange` callback — the new prompt is the return value
of `shell.run`. The shell is the *line-input* surface, not a separate
command framework with its own registry.

The earlier shell had its own command registry, dispatcher, parsed-
argument framework, and JSON result protocol. All of that has moved
onto the object tree (see [object-model.md](object-model.md)). What the
shell layer keeps is the work that genuinely belongs at the line-input
boundary: tokenising user input, expanding aliases and expression
interpolations, translating the four path forms to `node_get` /
`node_set` / `node_call`, and offering metadata-driven completion.

## Source Files

| File | Purpose |
|------|---------|
| [shell.c](../src/core/shell/shell.c) | Line tokeniser, path-form dispatcher, REPL, script execution, `cp` core |
| [shell.h](../src/core/shell/shell.h) | Public dispatch and tab-completion API |
| [shell_var.c](../src/core/shell/shell_var.c) | Per-process shell-variable table for `let X=42` and bare-name reads |
| [cmd_complete.c](../src/core/shell/cmd_complete.c) | Metadata-driven tab completion that walks the object tree |
| [cmd_symbol.c](../src/core/shell/cmd_symbol.c) | `$name` resolver: alias-table lookup with a legacy fallback for MMU regs and CCR bits |
| [cmd_parse.c](../src/core/shell/cmd_parse.c) | Argument parser used by the rich-parser methods (`debug.disasm`, `debug.logpoints.add`, `find.*`) |
| [cmd_types.h](../src/core/shell/cmd_types.h) | `arg_spec`, `cmd_context`, `cmd_result` shared between rich-parser methods |
| [cmd_io.c](../src/core/shell/cmd_io.c) | I/O stream helper used by the rich-parser methods to write to stdout/stderr |
| [cmd_cp.c](../src/core/shell/cmd_cp.c) | Recursive-copy implementation behind `storage.cp` and `storage.import` |
| [cmd_image.c](../src/core/shell/cmd_image.c) | Image probe / mount helpers shared by storage and floppy/scsi classes |

## Path forms

The tokeniser produces one token stream; the dispatcher inspects the
shape of that stream to decide which kind of operation to run on the
object tree. The four forms are the same forms `${...}` interpolation
recognises in expressions (proposal §4.1):

| Shape | Meaning | Translates to |
|-------|---------|---------------|
| `path` | Read an attribute or look up an object node | `node_get(resolve(path))` |
| `path = value` | Write an attribute (setter) | `node_set(resolve(path), value)` |
| `path arg arg …` | Call a method, shell form (whitespace-separated args) | `node_call(resolve(path), argv…)` |
| `path(arg, arg, …)` | Call a method, expression form (used inside `${…}`) | `node_call(resolve(path), argv…)` |

Shell form is what users type interactively (`floppy.drives[0].insert
/tmp/fd0 false`). Expression form is what appears inside `${…}` in
scripts and logpoint messages (`${memory.peek.l(0x1000)}`). Both
compile to the same `node_call` underneath, with arguments parsed
against the method's declared `arg_decl_t` table.

A bare `path` that resolves to an *object* (rather than an attribute or
method) prints the object's attribute table — the "info"-style view
from the previous shell becomes an automatic fallback for object reads.

## Substitution

### `$alias` expansion

A token starting with `$` is looked up in the alias table (see
[object-model.md](object-model.md#path-forms)). The table is two-tier:

- **Built-in aliases** are registered by subsystems at `*_init` time.
  `cpu_init` sets up `$pc` → `cpu.pc`, `$d0` → `cpu.d0`, …, and the
  FPU subsystem adds `$fp0` … `$fpiar` when the model has an FPU.
- **User aliases** are added at runtime via `shell.alias.add NAME PATH`
  and removed via `shell.alias.remove NAME`. They cannot collide with
  built-ins or reserved words; they don't persist across the process.

When an alias resolves to a path, the shell substitutes the path
verbatim. `$pc` therefore behaves identically to `cpu.pc` everywhere a
path is expected — as a read, as the LHS of an assignment, or inside
an expression.

A small legacy fallback in `cmd_symbol.c` covers MMU registers (`$tc`,
`$crp`, `$srp`, `$tt0`, `$tt1`) and the lone-letter CCR bits
(`$c`/`$v`/`$z`/`$n`/`$x`) that aren't aliased automatically; these
resolve directly to the corresponding register/flag without going
through the alias table.

### `${…}` expression interpolation

`${expr}` evaluates an expression (see `expr.h`) and substitutes its
formatted result into the surrounding string. Inside the braces:

- `$name` is an alias lookup.
- A bare path is `node_get`, or — when followed by an argument list in
  expression form — `node_call`.
- C-like literals and operators work the way they read: `1 + 2`,
  `0xFF & 0x0F`, `==`, `!=`, `<`, `>`, `&&`, `||`, `!`.
- Truthiness follows the proposal's falsy rules: empty string,
  `"false"`, `"0"`, and any error are falsy; everything else is truthy.

`${…}` is the substrate that drives `assert ${…}`, logpoint message
templates, and any place a command line wants to splice in a computed
value.

## Shell variables

`let X = 42` stores `X` in a process-wide variable table; subsequent
references to bare `X` inside `${…}` (no leading `$`, no dots) resolve
through that table. Variables shadow bare-identifier resolution against
the object tree — useful for capturing a value once and reusing it in
later expressions. They are not persisted and clear at process exit.

## Tab completion

`shell_tab_complete(line, cursor_pos, out)` produces a list of
candidate completions for the cursor's current word. The completer
walks the same object tree everything else uses, so any new attribute
or method becomes completable the moment its class is registered:

- **Line-start position** — root-level child names (`cpu`, `memory`,
  `scsi`, …) and root-level methods (`help`, `quit`, `time`, `echo`,
  …), filtered by the typed prefix.
- **Mid-path partials** (`cpu.`, `floppy.drives[0].`) — walk the tree
  to the resolved-so-far node and offer its members.
- **Method-argument position** — dispatched by the resolved method's
  `arg_decl_t[i]` table: enum kinds offer their declared values, bools
  offer `on`/`off`/`true`/`false`, path arguments fall through to
  filesystem completion, and so on.
- **Inside `$(...)`, `${...}`, or `"..."`** — completion suppresses
  itself; expression-position completion is a separate concern that
  hasn't been wired in yet.

Indexed-child collections (`debug.breakpoints[`, `floppy.drives[`)
complete the live indices via the class's `child.next` callback.

## Scripts

The headless binary accepts a `script=<path>` argument and reads lines
from that file as if they were typed interactively. Each line goes
through the same tokeniser, alias expansion, and dispatcher.

Three root methods are particularly script-shaped:

- `assert ${pred}` — passes when the predicate is truthy (empty /
  `"false"` / `"0"` / errors are falsy); fails the script otherwise.
- `echo args…` — writes its arguments to stdout, space-separated,
  newline-terminated. Mirrors the classic Unix `echo`.
- `quit` — requests shutdown; the headless runner exits, the WASM
  build no-ops (the browser owns the page lifecycle).

Scripts use the same path forms as interactive sessions, so anything
the user types at the prompt can also be a script line and vice versa.
Integration tests in `tests/integration/` are exactly such scripts.

## Rich-parser methods

A small set of methods accept richer argument grammars than the
`arg_decl_t` table can express — for example `debug.disasm`, the
`debug.logpoints.add` family with named-key parameters (`category=foo`
`level=3`), and the `find.*` value-pattern matchers. These reach into
the legacy parser machinery (`cmd_parse.c`, `cmd_types.h`,
`arg_spec[]`) via `shell_<cmd>_argv` argv-driven entry points exported
from `debug.c`, so the rich command body lives in one place even
though it's invoked through the typed object surface.

`cmd_io.c` and the `cmd_printf` macro exist for those rich-parser
methods; they write to `ctx->out` / `ctx->err` rather than directly to
stdout/stderr. Object-model attribute reads and the simple methods
just return `value_t` and let the formatter handle output.

## REPL

Both the interactive terminal and the headless REPL ultimately reach
the same static `dispatch_command(line, &result)` in `shell.c`. The
web terminal invokes it through `gs_eval('shell.run', [line])` (the
Shell class's `run` method); the headless REPL calls
`shell_dispatch(line)` directly. Both paths run:

1. Makes a mutable copy of the input.
2. Tokenises it with quote / backslash / `$(...)` awareness (the
   tokeniser keeps `$(...)` and `${...}` payload contiguous so the
   expression layer can handle them separately).
3. Inspects the token shape to decide read / write / call.
4. Resolves the path against `object_root()`.
5. Invokes the right `node_get` / `node_set` / `node_call`.
6. Formats the resulting `value_t` (or the error) for display, or
   stuffs it into `cmd_result` for callers that need it structured.

Unknown paths fail cleanly with a `path '…' did not resolve` error.
There is no Levenshtein "did you mean" suggestion any more — paths are
typed and nestable, so the resolver returns the failure point in the
path explicitly.

## See also

- [object-model.md](object-model.md) — the substrate the shell
  dispatches against.
- [web.md](web.md) — how the browser frontend reaches the same tree
  through the JS / WASM bridge instead of the shell layer.
- `src/core/object/expr.h` — `${…}` grammar and evaluator.
- `src/core/object/alias.h` — `$name` alias-table contract.
