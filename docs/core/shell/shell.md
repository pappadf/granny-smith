# Shell

This document describes the shell layer in `src/core/shell/` — the
**v2 script language** (see
`local/gs-docs/proposals/proposal-shell-control-flow-and-functions.md`)
layered on the [object model](object-model.md). A line is **parsed
first and evaluated second**: statements carry typed argument
expressions, string interpolation lives inside string literals, and one
binding namespace serves variables and aliases behind a single `$`
sigil. The operations themselves live on the object tree; the shell is
the line-input and scripting surface that walks it.

## Overview

Two callers reach the emulator through the shell layer:

- The **xterm.js terminal** in the browser, where users type commands
  interactively.
- The **headless CLI** (`gs-headless`), which reads a script file
  (`script=...`), stdin, or its TCP daemon socket.

Both go through the same statement parser and interpreter
(`script.c`). JavaScript callers reach the shell through the `Shell`
class on the object root: typed object-model calls
(`gs_eval('machine.cpu.pc')`) stay on their typed paths; free-form
lines route through `gs_eval('shell.run', [line])`; multi-line sources
go through `shell.eval(text)`.

## Source Files

| File | Purpose |
|------|---------|
| [script.c](../src/core/shell/script.c) | Statement parser + interpreter: blocks, control flow, assignments, command dispatch |
| [shell.c](../src/core/shell/shell.c) | REPL entry (`shell_dispatch`), value/table formatter, prompt, init |
| [shell_var.c](../src/core/shell/shell_var.c) | Scoped binding store (`let` bindings, `--var`, alias fallback) |
| [shell_funcs.c](../src/core/shell/shell_funcs.c) | User-defined functions (`def`), the `shell.functions` surface |
| [cmd_complete.c](../src/core/shell/cmd_complete.c) | Metadata-driven tab completion (keywords, `$bindings`, tree paths) |
| [cmd_cp.c](../src/core/shell/cmd_cp.c) | Recursive-copy implementation behind `storage.cp` / `storage.import` |
| `src/core/object/expr.c` | Expression grammar and evaluator; string interpolation; `try`/`error`/`range`/`len` |

## Statements

A script is a sequence of lines; `#` starts a comment. One statement
per line, except that brace blocks span lines (see below).

| Form | Meaning |
|------|---------|
| `let NAME = EXPR` | Declare a binding in the current scope |
| `alias NAME = PATH` | Declare a reference binding (path text, re-resolved per access) |
| `$NAME = EXPR` | Mutate an existing binding (error if undeclared) |
| `PATH = EXPR` | Attribute write with a typed right-hand side |
| `CMDPATH ARG…` | Command call (argument mode) |
| `EXPR` | Expression statement (REPL prints; scripts stay silent) |
| `if EXPR { … } elif EXPR { … } else { … }` | Conditional |
| `while EXPR { … }` | Pre-test loop; the condition re-evaluates per iteration |
| `for NAME in EXPR { … }` | Iterate a list, `a..b` range, or bytes |
| `break` / `continue` | Loop control (innermost loop) |
| `return [EXPR]` | Return from the enclosing function |
| `def NAME(P1, …) { … }` | Define a function |
| `assert EXPR ["message"]` | Typed assertion; falsy or error aborts the script |

Blocks come in two layouts: **multi-line** (`{` last on its line, `}`
first on its line, `} elif COND {` / `} else {` joining the closer) and
**inline** (`if COND { stmt }` — exactly one statement, no nesting).
Empty blocks are a parse error.

## Two parsing modes

Every slot in the grammar parses in one of two modes, always known from
context:

- **Argument mode** (command-call arguments): bare words are strings —
  `machine.floppy.drive[0].insert ../../fd0.image` needs no quotes.
  Numbers, `true`/`false`/`none`, `"interpolating"` and `'raw'`
  strings, `$binding` paths, `(expr)`, and `name=VALUE` named arguments
  round out the vocabulary.
- **Expression mode** (everywhere the grammar wants a value: after
  `if`/`while`, `for … in`, `let`/`=` right-hand sides, `return`,
  `assert`, inside `(…)`, inside `${…}`, and call-form argument lists):
  the `expr.c` grammar — C-like operators, object paths as bare
  identifiers, `$name` binding reads, `a..b` half-open integer ranges,
  and the builtins `try(EXPR, FALLBACK)`, `error(msg)`,
  `range(start, stop[, step])`, `len(x)`.

## Strings and interpolation

- `"…"` — interpolating string. `$name` splices a binding; `${EXPR}`
  splices any expression; `${EXPR:FMT}` applies a printf-style format
  spec (`:08x`, `:d`, `:s`, `%…`). Escapes: `\$ \" \' \\ \n \t \r \0
  \xHH`. Interpolation runs when the statement executes; a failed
  splice fails the statement (§3.9).
- `'…'` — raw string, no interpolation, no escapes beyond `\'`.
- Curly quotes (`“…”`) are accepted as double quotes in argument mode
  (paste tolerance).

`${…}` exists **only inside double-quoted strings**. Nothing outside a
string is ever rewritten.

## Bindings — one namespace, one sigil

`$name` means "the value of binding `name`" everywhere. The store is a
stack of scopes — process globals (`--var`, built-in aliases), the
script/session top level, and one frame per function call (16-frame
cap). Reads walk top-down and fall back to the alias table.

- **`let` creates, `=` mutates.** `let x = 5` declares in the current
  scope; `$x = 6` mutates the innermost scope holding `x`; mutating an
  undeclared name is an error (no typo-shadowing).
- **Aliases are reference bindings** (`V_REF`): they store *path text*
  and re-resolve on every access, so `$pc` keeps working across
  `machine.boot`. They read and write through: `$pc = 0x400128` sets
  `machine.cpu.pc`. Built-in register aliases (`$pc`, `$d0`, `$sr`, …)
  are registered by the subsystems; `alias d = machine.floppy.drive[0]`
  adds a user reference that tracks whatever lives at that path.
- **`let` snapshots.** `let d = machine.floppy.drive[0]` captures the
  live object; the handle survives unrelated adds/removes but reads as
  an error once its object is destroyed (e.g. by a reboot).
- Path continuation works through bindings: `$d.insert ../../fd0.image`,
  `$bp.addr + 4`, `$hits[0]` (list indexing).

## Errors

`V_ERROR` propagates through expressions; any statement producing one
aborts the script after printing `line N: message`. Conditions do not
treat errors as false — an error reaching `if`/`while`/`for` aborts.
Code that expects failure says so:

```
let v = try(machine.memory.peek.l($addr), none)
if $v != none { echo "read ${$v:08x}" } else { echo "unmapped" }
```

`none` is the script-side no-value: equal only to itself, falsy, and
outside every method's result domain.

## Functions

```
def step_to(addr) {
  while machine.cpu.pc != $addr { debug.step 1 }
}

step_to 0x400128                  # command form
step_to addr=0x400128             # named argument
let r = step_to(0x400128)         # call form, in any expression
```

`def` registers the function in the shell's registry and attaches an
entry object under `shell.functions` (attributes `name`, `params`;
method `remove`). Calls bind positional-then-named arguments, push a
scope, run the body, and pop; `return EXPR` (or falling off the end →
`none`) yields the value. Recursion is allowed up to the 16-frame cap.
Functions work in call form inside any expression — including logpoint
message templates — via the expression layer's function hook.

## Output

**Formatting lives at the REPL surface.** Interactive statements print
non-`none` results: scalars as before, objects as attribute tables, and
a list of same-class objects as a **table** (columns from the class's
attributes) — `debug.breakpoints.entries` at the prompt renders the
table the retired `list` methods used to print.

**Scripts print nothing implicitly.** Script output comes from `echo`,
failing `assert` messages, and errors. A bare read in a script is
silent; wrap it in `echo "path = ${path}"` when the log line matters.

The headless REPL shows a `... ` continuation prompt while a multi-line
`{` block is open (a quote-aware depth counter; `script_needs_continuation`).

## Scripts

`gs-headless script=<file>` parses the whole file (multi-line blocks
need the full source) and interprets it; the platform pump hook drives
the scheduler between statements, so `scheduler.run N` completes before
the next statement. The first error aborts the script and exits
non-zero. `shell.script_run(path)` and `shell.eval(text)` are the
platform-neutral equivalents.

Deferred evaluation is a **parameter type**, not a quoting trick:
argument slots declared with `OBJ_ARG_TEMPLATE` (e.g.
`debug.logpoints.add message=…`) store the raw string body and evaluate
it at fire time with per-fire bindings (`$value`, `$addr`, `$size`)
layered over the shell store.

## Tab completion

- **Line start** — statement keywords (`let`, `if`, `while`, `def`, …)
  plus root-level child names and methods.
- **`$` prefix** — binding names: scope bindings first, then aliases.
- **Mid-path partials** (`machine.cpu.`, `machine.floppy.drive[0].`) —
  members of the resolved-so-far node.
- **Method-argument position** — dispatched by the resolved method's
  `arg_decl_t[i]`: enums offer their values, path arguments complete
  against the filesystem, and so on.

## See also

- [object-model.md](object-model.md) — the substrate the shell
  dispatches against, the library conventions (§6), and the reserved
  words.
- [web.md](web.md) — how the browser frontend reaches the same tree
  through the JS / WASM bridge instead of the shell layer.
- `src/core/object/expr.h` — expression grammar, interpolation, and the
  binding-callback contract.
