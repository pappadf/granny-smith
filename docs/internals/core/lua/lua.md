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

This replaces the shell's own script language for files
(`docs/internals/core/shell/shell.md` still describes that language, which
the terminal's line form and the tests not yet ported use).  The design and
its rationale are in the gs-docs proposal "Lua as the script language".

## Source files

| File | Purpose |
|------|---------|
| `gs_lua.h` | The runners (`gs_lua_run_file`, `gs_lua_run_source`), `vars`, the interrupt, the error reporter |
| `gs_lua.c` | The state, the node proxies, the value conversions, the `gs` module's C half, `require`, the hooks |
| `gs_lua_lib.lua` | The library's Lua half (`gs.try`, `F"…"`, templates), embedded by `gs_lua.mk` through `scripts/bin2c.py` |
| `gs_lua.mk` | The rule for the embedded header, included by both builds and the `lua_bridge` unit suite |

Entry points: a `.lua` path given to `include`, to `shell.script_run` or to
the headless `script=` runs on the Lua state (`script.c`, `include_exec_file`);
`shell.lua(text)` runs source text with the REPL's printing.  A script runs
where the shell language's scripts run (the job thread when the platform has
one; inline otherwise), so every tree access goes through the seam like any
leaf call and `scheduler.run(n)` returns when the run has ended.

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
| ref (an alias) → a proxy of its path | `gs.bytes(int, width)` or `gs.bytes(string)` → bytes |

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
| `gs.str(v)` | a value as the old `${…}` spliced it: a list as `[a, b]`, a map as its compact JSON, nil as `none`; `tostring` of a tree map is the same |
| `F"text ${expr} ${expr:08x}"` | a template filled from the caller's locals, parameters, upvalues and the globals; `:spec` is a `string.format` spec |
| `gs.template(s)` | the compiled template, a function of an environment table |
| `gs.bytes(v, w)` | a bytes value, big-endian; `gs.bytes("…")` the string's bytes |
| `gs.hex(v [, width])`, `gs.contains(s, needle)` | formatting and a plain substring test |
| `gs.node(path)` | what a path names (a proxy, or an attribute's value); `gs.path(p)`, `gs.is_node(x)` |
| `gs.list(x)`, `gs.map(t)` | a collection or list read as a sequence; a table tagged as a map value (so a call passes it positionally, and `gs.str` renders it `{}` when empty) |
| `gs.fmt(path)` | the attribute's plain text |
| `gs.none`, `gs.is_none(v)` | the in-list spelling of none |
| `gs.quit_signal`, `gs.interrupt_signal` | the values `quit()` and Ctrl-C raise; `gs.try` re-raises them, a plain `pcall` that swallows one is re-interrupted at the next hook |

**One pitfall of `F`.**  It reads the caller's locals with `luadebug.getlocal`
and the function's upvalues with `luadebug.getupvalue`; a module-level
local that the function names *only inside the template* is not an upvalue
of that function and reads as `nil`.  Name it in code too, or spell the
message with `..`.

## Interrupts and failures

`shell.interrupt` (Ctrl-C in the three clients) and a job cancel unwind the
running chunk at its next instruction-count hook with
`gs.interrupt_signal`.  A failed chunk is reported through the shell's
`script_report_error` (an error annotation in a job's record stream; stderr
outside one) as its message and traceback, and `script=` exits 1.

## Tests

`tests/unit/suites/lua_bridge` attaches a probe class to the root and runs
chunks against it: both value directions, named arguments, errors, the
globals rule, `vars`, the library, quit and interrupt.
`tests/integration/debug` is the first integration test in Lua;
`tests/integration/lib/mac.lua` is the suite library.

## The port, and what still runs on the shell language

Every integration test is `test.lua` (`scripts/script-to-lua.py` did the
mechanical part; the residue it marks `-- PORT:` was finished by hand), the
custom runners generate Lua, and the libraries are `lib/*.lua`.  What a
port has to watch for, learned from this one: a list held in a map field
is 1-based too (`walk.steps[0]` → `[1]`); a bare path the old language ran
as a zero-argument command (`machine.videoin.pattern`) is a call; the old
`!call(...)` is `gs.truthy`; a `"\x00"` in a needle was a real NUL the old
`val_str` cut the string at, so the needle meant its prefix (the device-tree
test's console prints the four characters `\x00`, which Lua spells `"\\x00"`);
and a wait that samples the screen must sample after each run, never before
the first, or a sample-exact row shifts by a quantum.

Still on the shell language's interpreter: the terminal's lines and
`shell.eval`, the breakpoint conditions and logpoint templates (`debug.c`),
the tests of that language itself (`shell-v2`, `shell-class`,
`shell-commands`, `shell-command-words`, `object-expr`, `object-metadata`,
`object-model-reorg`, `object-logpoint`, `object-debug`, `daemon-statements`),
and the two scripts the Playwright specs feed to the browser terminal
(`tnt-pci-voodoo2/draw-webgpu.script`, `glide-init.script`).  The
terminal's command form as a line-level front end to Lua, the coroutine
driver that would retire the job thread, and the deletion of the old
interpreter follow.
