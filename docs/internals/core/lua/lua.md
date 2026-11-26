# Lua scripting

`src/core/lua/` embeds Lua 5.4 (vendored under `third-party/lua/`, MIT) as
the script language and binds the object tree to it.  A script is a plain
Lua file; what makes it an emulator script is that the tree's root children
are its globals.

```lua
local mac = dofile("../lib/mac.lua")

machine.boot{model = "iix", ram = 8192, video_card = "mdc_8_24", rom = vars.ROM}
machine.rtc.time = 3000000000
machine.floppy.drive[0].insert("../../data/systems/System_7_0_1.image")
scheduler.run(40000000)
local spent = mac.wait_match("goldens/iix-7.0.1-finder.png", 900000000, 20000000)
assert(machine.screen.width == 640, F"width ${machine.screen.width}")
echo(F"pc=${machine.cpu.pc:08x} spent=${spent}")
quit()
```

Lua is the one script language: files, the terminal's and the daemon's
lines (a line is either a **command line** in argument mode or a Lua chunk,
see "The terminal"), breakpoint conditions and logpoint templates.  The
design and its rationale are in the gs-docs proposal "Lua as the script
language".

## Source files

| File | Purpose |
|------|---------|
| `gs_lua.h` | The runners (`gs_lua_run_file`, `gs_lua_run_source`, `gs_lua_run_line`), `vars`, the interrupt, the error reporter, the terminal's classification, conditions and templates |
| `gs_lua.c` | The state, the node proxies, the value conversions, the `gs` module's C half, `require`, the hooks, job tasks (`job_glue_task_*`) |
| `gs_lua_lib.lua` | The library's Lua half (`gs.try`, `F"…"`, templates), the terminal's command form (`gs.shell_line`), the shell spelling of conditions and templates; embedded by `gs_lua.mk` through `scripts/bin2c.py` |
| `gs_lua.mk` | The rule for the embedded header, included by both builds and the `lua_bridge` unit suite |

Entry points: every `REQ_SCRIPT` -- a terminal or daemon line, a headless
stdin statement, the headless `script=` file (posted as
`dofile("path")`) -- runs as a **job** whose task is a coroutine running
`gs.shell_line(text)` (see "Jobs").  `shell.run(line)`, `shell.eval(text)`,
`shell.lua(text)` and `shell.script_run(path)` run on the calling stack
instead, without suspending; the debugger runs conditions and templates
the same way.

## The tree from Lua

**Root children are globals.** `machine`, `scheduler`, `debug`, `files`,
`shell`, `log`, `checkpoint`, `catalog`, `appletalk`, and the root verbs
(`objects`, `attributes`, `methods`, `help`, `time`) resolve through the
global table's `__index` whenever the name is not a Lua global.  Declaring
a global that names a root object is an error, so a script cannot shadow
`machine` by accident.  Lua's own `debug` library is reachable as
`luadebug`, since `debug` is the tree's debugger.

**Node proxies.** `machine`, `machine.cpu`, `machine.floppy.drive[0]` and
`debug.breakpoints.entries` are proxies: userdata holding path text, not a
node.  Indexing one appends a segment and resolves the path:

| The path names | You get |
|---|---|
| an object, a named child, a collection entry | another proxy (the entry is validated: an empty slot raises) |
| an index-less collection | a proxy; `#entries` is its count, `gs.list(entries)` its entries |
| an attribute | its value, read now (`node_get`) |
| a method | a callable proxy |

Because a proxy holds text, `local d = machine.floppy.drive[0]` survives a
`machine.boot` the way an alias does, and re-resolves on each use.  Path
indices stay what the tree calls them (0-based ids, `["key"]` keys);
`drive[0]` is a path, not a Lua table.  `tostring(proxy)` is its path; two
proxies are equal when their paths are.

**Writes.** `machine.cpu.d0 = 0x1234` is `node_set` with the converted value
and the attribute's own validation (widths, enums, read-only).

**Calls.** `machine.floppy.drive[0].insert("x.img", false)` is `node_call`
with positional arguments.  When the last argument is a table whose keys are
strings, its entries are the **named arguments**, bound by `node_bind_args`
against the declared slots: `machine.boot{model = "plus", rom = vars.ROM}`,
`debug.logpoints.add{addr = pc, message = "hit"}`, `probe.add(2, 3, {scale = 10})`.
A map that must travel positionally is a value, not named arguments, when
it came from the tree or is tagged with `gs.map(t)`.  Validation, defaults
and enum coercion happen in `node_call` exactly as for every other caller.

**Errors.** A `VK_ERROR` from any node operation is raised as a Lua error
whose value is a table `{text = "...", kind = "error"}` with a `__tostring`;
`pcall` catches it, `gs.try` wraps that, and an uncaught one ends the script
with its text and a traceback through the shell's error reporter.  A
condition does not see an error as false: `if machine.nosuch then` raises.

## Values

| Tree → Lua | Lua → tree (against the declared slot) |
|---|---|
| `VK_NONE` → `nil`; inside a list, `gs.none`, so sequences have no holes (`gs.is_none`) | `nil` or `gs.none` → none |
| bool, int, uint → boolean, integer (64-bit) | boolean, integer → bool, int (the validator's width-checked int/uint reinterpretation applies: `pc = -1` stores `0xFFFFFFFF`) |
| float → number | a non-integral number → float |
| string, bytes → string (byte-safe) | string → string, or the enum the slot declares |
| enum → its label | — |
| list → a **1-based** sequence | a sequence → list |
| map → a table whose `pairs` keeps the tree's insertion order | a table with string keys → map (the order kept when the table came from the tree) |
| object → a proxy | a proxy → the object it names |
| error → raised | a function → refused |
| — | `gs.bytes(int, width)` or `gs.bytes(string)` → bytes |

Widths and the hex display flag do not survive into a Lua integer; a script
formats explicitly (`("%08x"):format(pc)`, `gs.hex(pc, 8)`, `F"${pc:08x}"`)
and `gs.fmt("machine.cpu.pc")` gives the plain text the old `${…}` spliced.
Lua 5.4's bitwise operators bind tighter than its comparisons, the same
reading the shell language chose, so `sr & 0x2000 == 0x2000` means
`(sr & 0x2000) == 0x2000`.  `~` is xor and `^` is exponentiation.

## The library

Made by `gs_lua.c` and `gs_lua_lib.lua`; everything a script needs beyond
Lua's own `string`, `table`, `math`, `utf8`, `coroutine`, `luadebug` and the
`os` clock functions (no `io` beyond `io.write`; host files are `files`).

| Name | Meaning |
|---|---|
| `echo(...)`, `print(...)` | the arguments' text, space-separated, one line, to the client whose script this is |
| `quit()` | end the script and ask the platform to stop (exit code 0) |
| `vars.NAME` | the `--var NAME=VALUE` definitions (strings; `nil` when unset); `vars.TMP_DIR` is `"tmp"` |
| `dofile(path)`, `require "a.b"` | relative to the running file's directory; `require` maps dots to directories and appends `.lua` |
| `gs.try(fn, fallback)` | `fn()`'s result, or `fallback` (called with the error when it is a function) when it raised |
| `gs.ok(fn)` | true when `fn()` ran without raising |
| `gs.truthy(fn)` | what the old `!call(...)` tested: false when `fn()` raised or returned nil, false, 0, "" or an empty table |
| `gs.str(v)` | a value as the old `${…}` spliced it: a list as `[a, b]`, a map as its compact JSON, nil as the empty string; `tostring` of a tree map is the same |
| `F"text ${expr} ${expr:08x}"` | a template filled from the caller's locals, parameters, upvalues and the globals; `:spec` is a `string.format` spec |
| `gs.template(s)` | the compiled template, a function of an environment table |
| `gs.bytes(v, w)` | a bytes value, big-endian; `gs.bytes("…")` the string's bytes |
| `gs.hex(v [, width])`, `gs.contains(s, needle)` | formatting, and a plain substring test (over a list, whether an item is `needle`, as `files.identify(p).fits`) |
| `gs.node(path)` | what a path names (a proxy, or an attribute's value); `gs.path(p)`, `gs.is_node(x)` |
| `gs.list(x)`, `gs.map(t)` | a collection or list read as a sequence; a table tagged as a map value (so a call passes it positionally, and `gs.str` renders it `{}` when empty) |
| `gs.fmt(path)` | the attribute's plain text |
| `gs.none`, `gs.is_none(v)` | the in-list spelling of none |
| `gs.quit_signal`, `gs.interrupt_signal` | the values `quit()` and Ctrl-C raise; `gs.try` re-raises them, a plain `pcall` that swallows one is re-interrupted at the next hook |

**Two pitfalls of `F`.**  It reads the caller's locals with `luadebug.getlocal`
and the function's upvalues with `luadebug.getupvalue`; a module-level
local that the function names *only inside the template* is not an upvalue
of that function and reads as `nil`.  Name it in code too, or spell the
message with `..`.  And `return F"…"` is a tail call: the caller's frame is
gone when `F` runs, so its locals read as `nil`.  Write `return (F"…")`,
which Lua does not tail-call (the converter emits that form).

## Jobs

A job's task is a coroutine (`job_glue_task_new`) that the drain resumes
one step at a time (`docs/internals/core/job/job.md`).  The bridge suspends
it after a tree call that started a mode the client waits for
(`scheduler.run(N)`), while a deferred I/O answer is outstanding, and when
its 4 ms time slice is spent (the count hook, every 1000 instructions);
the drain resumes it when the wait is over.  So `scheduler.run(n)` returns
when the run has ended, a deferred leaf's call returns the work's own
answer, and a computing script leaves the machine running.  Only the
task's own coroutine suspends for the drain, and only where Lua can yield:
`dofile` and `pcall` / `gs.try` can, a `require` at load time, a
`string.gsub` callback and a coroutine the script made itself cannot (a
tree call there does not defer, and waits for its mode by running frames
where the platform has a frame runner).

## The terminal

Each line the terminal or the daemon sends (`gs.shell_line`) is classified
once (`gs.classify`, nothing evaluated):

1. A **command line**: the head is a tree path (`machine.cpu.pc`,
   `machine.floppy.drive[0].insert`), a command word (`ls`, `run`, `stop`,
   a `shell.command.add` declaration) or `$alias`, and what follows is
   nothing, `= value`, or -- after a method, unless it starts with `(` or
   `:` -- argument-mode words.  A bare head reads (a method is called) and
   prints the value as the tree formats it (hex for an address, a table for
   an object); `path = value` assigns, the value an expression or a bare
   word naming nothing (an enum label: `scheduler.mode = turbo`).
   Arguments are positional words and `name=value` (no spaces at `=`):
   `"..."` with `${expr}` filled from the globals (a template slot, like a
   logpoint's `message`, keeps its text raw), `'raw'`, `(lua expr)`,
   `$alias` (with a path continuation), and bare words -- `true`, `false`,
   `none`, numbers (`0x10`, `-3`, `1.5`), else strings.  A `#` or `--`
   after a blank, outside quotes, starts a comment.
2. Anything else is **Lua**: tried as `return <line>` first, so an
   expression prints its results (`machine.cpu.pc + 4`), then as a
   statement.  `$name` in a Lua line reads the alias of that name (`$pc`),
   else the global.  A bare word naming a global function is called
   (`quit`); one naming nothing is an error.  A method followed by `(` is a
   Lua call (`machine.memory.poke.l(a, v)`).

A line whose first word is a Lua global is Lua, so a script's function of
the same name shadows a command.  A Lua chunk that does not compile because
the input ended (`for i = 1, 3 do`) asks for another line
(`gs.needs_continuation`, the daemon's statement assembler and the page's
Enter key); a command line never does.  `shell.highlight` colours the two
forms as they run (`highlight.c`: argument-mode classes for a command line,
a Lua lexer's for a chunk, tree paths resolved against the live tree) and
`shell.complete` completes tree paths, arguments and aliases.

## Conditions and templates

A breakpoint condition and a logpoint message are text a person typed, in
the spelling the terminal shares with the old shell: `$name` reads an alias
(or a global), `&&` `||` `!=` `!` are the C operators.  `gs.condition`
translates a condition once into `return (...)` and compiles it (an error
at `debug.breakpoints.add` is "invalid breakpoint condition '...': why");
each hit calls it non-yieldably, and false, nil, 0 and none are false, an
error true (so a typo does not swallow hits).  `gs.shell_template` compiles
a template once: `${expr}` and `${expr:08x}` as in `F`, `$name` and a bare
tree path keeping the tree's text, the per-fire bindings `value` and `addr`
reading as hex and `size` as a number.  `shell.expand(text)` expands one
against the globals.  An alias is read when the code runs, so adding or
removing one takes effect at once.

## Interrupts and failures

`shell.interrupt` (Ctrl-C in the three clients) and a job cancel unwind the
running chunk at its next instruction-count hook with
`gs.interrupt_signal`.  A failed chunk is reported through
`script_report_error` (`src/core/shell/report.c`: an error annotation in a
job's record stream; stderr outside one) as its message, and a traceback
when a file's code was running, and `script=` exits 1.

## Tests

`tests/unit/suites/lua_bridge` attaches a probe class to the root and runs
chunks against it: both value directions, named arguments, errors, the
globals rule, `vars`, the library, quit and interrupt.
`tests/unit/suites/shell_highlight` checks the classification and the
colouring of both forms.  `tests/integration/shell-terminal` drives the
terminal's forms through `shell.run`; `object-logpoint`, `object-debug` and
`lisa-debugger` the debugger's conditions and templates; every other
integration test is a `test.lua`, with `tests/integration/lib/mac.lua` the
suite library.

## Porting a script in the old shell language

The old language (`let`, `def`, `if … { }`, `include`, `${…}` in every
string) is gone.  `scripts/script-to-lua.py` converts a `.script` file to
Lua for anyone who still has one; what it cannot map confidently it marks
`-- PORT:`.  What a port has to watch for: a list read from the tree is
1-based (`walk.steps[0]` → `[1]`) while path indices stay what the tree
calls them (`drive[0]`); a bare path the old language ran as a
zero-argument command (`machine.videoin.pattern`) is a call; the old
`!call(...)` is `gs.truthy`; Lua's `assert` reports its message without the
old `ASSERT FAILED:` prefix; a `let` inside a loop or `if` body that names a
binding of the enclosing function rebinds it, so it is an assignment in
Lua, never a new `local`; and a top-level binding that a function defined
above it reads must be declared before that function.
