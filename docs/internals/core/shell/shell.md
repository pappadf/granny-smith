# Shell

This document describes the shell layer in `src/core/shell/` — the
**v2 script language** layered on the [object model](../object). A line is **parsed
first and evaluated second**: statements carry typed argument
expressions, string interpolation lives inside string literals, and one
binding namespace serves variables and aliases behind a single `$`
sigil. The operations themselves live on the object tree; the shell is
the line-input and scripting surface that walks it.

## Overview

Two callers reach the emulator through the shell layer:

- The **Terminal console** in the browser, where users type commands
  interactively.
- The **headless CLI** (`gs-headless`), which reads a script file
  (`script=...`), stdin, or its TCP daemon socket.

Both go through the same statement parser and interpreter
(`script.c`). Every client reaches it the same way: a free-form line or a
whole source is posted to the mailbox as a **script job** (`REQ_SCRIPT`,
see "Scripts" below) and runs on the job thread; typed
object-model calls (`gs_eval('machine.cpu.pc')`) stay on their typed
paths. The `Shell` class on the object root keeps `shell.run` and
`shell.eval(text)` as leaves for a caller that wants a line run inline on
the emulator thread (the unit suites, a script running another script),
plus `shell.complete`, `shell.expand`, the alias leaves and
`shell.interrupt`.

## Source Files

| File | Purpose |
|------|---------|
| [script.c](../../../../src/core/shell/script.c) | Statement parser + interpreter: blocks, control flow, assignments, command dispatch |
| [shell.c](../../../../src/core/shell/shell.c) | REPL entry (`shell_dispatch`), value/table formatter, prompt, init |
| [shell_var.c](../../../../src/core/shell/shell_var.c) | Scoped binding store (`let` bindings, `--var`, alias fallback) |
| [shell_funcs.c](../../../../src/core/shell/shell_funcs.c) | User-defined functions (`def`), the `shell.functions` surface |
| [cmd_complete.c](../../../../src/core/shell/cmd_complete.c) | Metadata-driven tab completion (keywords, `$bindings`, tree paths) |
| [highlight.c](../../../../src/core/shell/highlight.c) | `shell.highlight`: syntax classes of a line or block, paths resolved against the live tree |
| [cmd_cp.c](../../../../src/core/shell/cmd_cp.c) | Recursive-copy implementation behind `files.cp` / `files.import` |
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
| `for NAME in EXPR { … }` | Iterate a list, map (keys), `a..b` range, or bytes |
| `break` / `continue` | Loop control (innermost loop) |
| `return [EXPR]` | Return from the enclosing function |
| `def NAME(P1, …) { … }` | Define a function |
| `assert EXPR ["message"]` | Typed assertion; falsy or error aborts the script |
| `include EXPR` | Parse + execute another script file in place (see Scripts) |

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
  `range(start, stop[, step])`, `len(x)`,
  `contains(haystack, needle)` (substring test over two strings —
  what a row uses to assert on a guest serial console's text, since
  the machines that boot Unix narrate rather than draw).

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

## List literals

`[a, b, c]` builds a list. Elements are full expressions, may be of mixed
kinds, may nest, and a trailing comma is allowed:

```
let nums   = [1, 2, 3]
let mixed  = [1, "two", 3 + 4]     # -> [1, two, 7]
let table  = [[1, 2], [3, 4]]
let empty  = []
assert $table[1][0] == 3
for n in [10, 20, 30] { … }
```

This does not collide with indexing. `[` in **postfix** position — after a
path or a value, as in `slot[9]` or `$table[1]` — is an index and is consumed
by the path parser; `[` where a **primary** is expected can only begin a list.
The two readings never overlap, so no disambiguation rule is needed.

`for` already iterated lists, so a literal is directly iterable with no
further machinery.

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
`{` block is open (a quote-aware depth counter; `script_needs_continuation`,
exposed as the hidden `shell.needs_continuation(text)` so a console can
tell whether Enter submits or breaks the line).

**Structured results ride beside the text.** Inside a job, a value the REPL
prints is bracketed by two annotation records in the job's record stream —
`value_begin` before its text and `value` after it, carrying the value as
tagged JSON (`"json"`, or `"truncated":true` when that would not fit a
record) — and every statement error (`script_report_error`, the single
reporter: the text still goes to stderr unchanged) adds an `error` record
with `file`, `line`, `message` and the stderr `lines`.  The printed text is
byte-identical either way; a consumer that wants only text ignores the
annotations.  Every record, text included, is bounded by a quarter of the
event ring (`gs_mailbox_record_max`), measured on the escaped text.

## Scripts

`gs-headless script=<file>` runs the file as one job (`include "<file>"`)
on the job thread, the way every statement runs on every platform
(`src/core/job/job.h`): the interpreter never touches guest state itself,
each object-tree access is served by the emulator thread at a frame
boundary, and a `scheduler.run N` (or `debug.step N`) waits inside its
call until those N instructions have run, so the next statement sees the
machine stopped. A bare `scheduler.run` in a script (a file, stdin, a
daemon statement) holds the script until the machine stops — a
breakpoint, an assertion, a stop from a control connection — which is how
the suites are written; only the browser's terminal lets a bare
`scheduler.run` return at once and run on (Ctrl-C stops it). The first
error aborts the script and exits non-zero. `shell.script_run(path)` and `shell.eval(text)` are the
platform-neutral equivalents (run inline, on the emulator thread, as a
leaf).

Headless's own loop is the browser's tick minus the frame pacing: one
frame-unit while the machine runs, then the mailbox drain that serves the
job's calls and the daemon's or stdin's statements. What a statement
prints reaches the client through the same drain: every stdout site in the
core goes through the output sink (`gs_out.h`), a job's text is delivered
as output records in order before the statement's result, and the driver
writes it to stdout (or the daemon's socket) as it arrives. `--framed`
adds `@event <kind> <json>` lines for every core event (`mode_started`,
`mode_ended` with its reason), `@out <json>` for each output record
(`{"event":"output","id":..,"client":..,"text":..}`), `@progress <json>`
for an I/O job's progress (`{"id":..,"done":..,"total":..}`), and
`@value_begin <json>` / `@value <json>` / `@error <json>` for the
annotation records, in stream order among the `@out` lines (an `@out` line
may split where an annotation falls; the concatenated text is unchanged),
and `@end ok|error` after each statement, for a client that wants to parse
where a statement ended rather than time out on silence. Ctrl-C cancels
the statement in flight, else stops a run stdin started, else stops the
machine; the daemon's control connection does the same for the daemon's
client.

Two bisecting aids fold a thread back in: `--io=sync` runs every I/O job
(a copy, an export, a checkpoint's write) on the emulator thread, and
`--jobs=inline` runs scripts there too — the interpreter executes inside
the drain, and a `scheduler.run N` waits by driving frames itself instead
of holding a call. Output is then printed directly rather than captured.
Both modes are meant to produce the same stdout as the threaded run
(`scripts/compare-jobs-inline.sh` runs the integration corpus both ways and
diffs it); a difference is a bug in the threading, not in the script.

`include "path"` pulls another script file into the run at the point of
the statement: its `def`s land in the shared function registry, its
top-level statements execute immediately, and its own `include`s nest
(depth-capped, with a canonical-path cycle guard). A **relative** path
resolves against the directory of the *including file* — not the
process CWD — so a suite script can `include "../lib/mac.script"`
wherever the daemon was started. Errors keep accurate attribution: the
diagnostic carries the included file's name and line. The integration
suite's shared library (`tests/integration/lib/`) is the primary
consumer.

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
  `arg_decl_t[i]`: enums offer their values, bools `true`/`false`, and a
  string argument declared `VAL_PATH` completes against the filesystem
  (through the VFS).  The flag decides, not the argument's name: a
  `path` argument that names an object path gets no file candidates.

With `shell.complete(line, cursor, true)` each candidate comes back as
`{text, kind, doc, task}` (`kind` ∈ `object`, `collection`, `attr`,
`method`, `alias`, `keyword`, `value`), and a `context` says where the
cursor is: `{method, arg_index, arg_name}`, where `arg_index` is the
*declared* slot (a `name=` word names its own slot; earlier `name=` words do
not count as positionals), all `none` outside an argument position.
`cursor` and the returned span are UTF-8 byte offsets.

## Highlighting

`shell.highlight(text)` returns the syntax classes of a line or block as a
list of `{start, end, class}` spans. Offsets are UTF-8 bytes; spans are
half-open, ordered and non-overlapping. The text is read the way the
parser reads it: keyword forms, assignments, call forms, and commands whose
arguments are in argument mode. Path segments resolve against the live tree
as they are read.

| Class | For |
|---|---|
| `keyword` | reserved words (`if`, `for`, `in`, `return`, `true`, …) |
| `decl` | `let`, `alias`, `def` |
| `variable` / `alias` | `$name`, by whether an alias of that name exists |
| `number`, `string` | literals (`0x…`, `0b…`, decimal, floats; `"…"`, `'…'`) |
| `interp` | `${`, a `:FMT` suffix and `}` inside a double-quoted string |
| `operator`, `comment` | operators and brackets; `#` to the end of the line |
| `object`, `attribute`, `method` | a resolved path segment, by what it names |
| `enum` | an argument word matching its declared values, or the value of an enum attribute's assignment |
| `unknown` | the first path segment that does not resolve, and every segment after it |

A bare word in argument mode is a string and gets no class. A word before
`(` that does not resolve is a function call (`method`); so is the name of
a `def` function at the head of a statement. Partial input still gets spans
for what lexes (an unterminated string runs to the end); the call never
fails.

## Help and usage

`help <path>` prints the usage text of any path, and `shell.usage(path)`
returns it as `{signature, arg_spans, text}` — one renderer
(`src/core/object/usage.c`), so help, the command browser and a signature
hint cannot disagree:

- **Method** — the signature `<full.path> <arg> [optional] [rest…]`, an enum
  argument writing its values (`[space: logical | physical]`); one aligned
  line per argument (name, type text, doc, `(default …)`); `Returns: …` and
  `e.g.  …` lines when the member declares `result_doc` / `examples`; a
  blank line and the doc wrapped at 72 columns.  `arg_spans[i]` is the byte
  span of argument i's bracketed form in the signature.
- **Attribute** — `<full.path> : <type text>` (plus ` (read-only)`), then
  ` = <value>` unless sensitive or unreadable, a blank line and the doc.
- **Node** — `<full.path> — <label>`, its doc, then `attributes:`,
  `methods:` and `children:` lines (basic and advanced tiers).

`shell.keywords` lists every reserved word with its one-line syntax;
`shell.tasks` (internal) lists the command browser's task chips in order.

## See also

- [object-model.md](../object) — the substrate the shell
  dispatches against, the library conventions (§6), and the reserved
  words.
- [web.md](../../../guide/web.md) — how the browser frontend reaches the same tree
  through the JS / WASM bridge instead of the shell layer.
- `src/core/object/expr.h` — expression grammar, interpolation, and the
  binding-callback contract.
