---
name: headless-debug
description: >
  Interactive debugging via the headless emulator's TCP shell. Covers daemon
  startup, the typed object-model shell grammar (bare-path reads, setters,
  shell-form and call-form method invocation, ${...} expression
  interpolation), introspection (objects/attributes/methods/help), and the
  command surfaces for cpu, memory, debug.breakpoints/logpoints,
  debug.mac.globals, scheduler, screen, machine.profile, rom.identify,
  scsi/floppy/scc/via/rtc/nubus, mouse/keyboard/find, and common workflows.
triggers:
  - single step
  - set a breakpoint
  - hit a breakpoint
  - examine memory
  - examine registers
  - trace the boot
  - debug the boot
  - step through
  - disassemble
  - gs-headless
  - daemon
  - TCP
  - netcat
  - check registers
  - run to address
  - examine low memory
  - check global variable
  - object model
  - shell expression
---

# Granny Smith Headless Daemon — Agent Skill

Granny Smith is a browser-based Macintosh emulator. The headless daemon
exposes its interactive shell over a TCP socket so an agent can start the
emulator, send commands, and capture output reliably.

The shell is a typed object-model REPL. Every emulator subsystem is an
object reachable by a dotted path: `cpu.pc`, `memory.peek.l 0x400`,
`debug.breakpoints.add 0x40802A14`, `screen.save "/tmp/x.png"`,
`machine.profile "se30"`.

## 1. Building

```bash
cd /workspaces/granny-smith
make -f Makefile.headless
```

Binary: `build/headless/gs-headless`.

## 2. Starting the daemon

```bash
./build/headless/gs-headless --daemon --kill --speed=max --no-prompt \
    rom=tests/data/roms/SE30.rom [hd=disk.img] [fd=floppy.dsk] &
sleep 2  # or block-read for "READY" on stdout
```

Flags:
- `--daemon` — required, enables TCP socket mode.
- `rom=<file>` — required. ROMs live under `tests/data/roms/`
  (`SE30.rom`, `Plus_v3.rom`, `IIcx.rom`; vrom: `SE30.vrom`,
  `Apple-341-0868.vrom`).
- `hd=<file>` (×8) / `fd=<file>` (×2) — pre-attach disks.
- `--port=N` — TCP port (default **6800**).
- `--speed=max|realtime|hardware` — default `realtime`. Use `max` for
  debugging.
- `--kill` — kill any existing daemon on the same port before starting.
- `--no-prompt` — recommended. Suppresses the trailing PC-disassembly
  status line otherwise appended to every response.
- `--quiet` / `-q` — suppress the startup banner.
- `--script-stdin` — read script commands from stdin instead of a file.

The daemon prints `READY` on stdout once the TCP listener is bound and
the machine is fully initialised. It also prints `Daemon PID: <n>`. PID
files go to `/tmp/gs-headless-<port>.pid`.

The emulator does not auto-run; it waits for commands.

### Example — SE/30, fast, no trailing prompt

```bash
./build/headless/gs-headless --daemon --kill --speed=max --no-prompt \
    rom=tests/data/roms/SE30.rom &
sleep 2
echo "objects" | nc -w 2 localhost 6800
```

## 3. Sending commands over TCP

Each command is one or more newline-delimited lines on a TCP connection:

```bash
echo "<command>" | nc -w 2 localhost 6800
printf "cpu.pc\ndebug.step\ncpu.pc\n" | nc -w 5 localhost 6800
```

Always use `nc -w <secs>` (timeout for both connect and idle), not
`nc -q`, which can race with daemon output.

The daemon handles one connection at a time but accepts multiple
commands per connection. Output is line-buffered and flushed between
commands.

**Heartbeats during long runs.** While `scheduler.run` is in flight the
daemon emits one heartbeat per second:
`# running... 54000000 instructions (+54000000 since start)`. Filter
with `grep -v '^#'` if you don't want them.

**Stop preempts a run.** Sending any command on a fresh connection
(typically `scheduler.stop`) breaks the scheduler out of its loop.

**Disconnect cancels a run.** If the driving client disconnects mid-run,
the daemon detects EOF and stops the scheduler instead of burning the
budget.

## 4. The shell grammar

### 4.1 Four input shapes

The shell accepts exactly four shapes:

1. **Bare path read** — `cpu.pc`, `memory.ram_size`, `machine.id`,
   `scsi.bus.phase`. Prints the value using the attribute's declared
   formatter (e.g. `cpu.pc` → `0x40826cc6` because the attribute has a
   hex flag).
2. **Setter** — `cpu.d0 = 0x1234`, `sound.enabled = false`. The RHS is
   parsed using the attribute's declared kind. Hex / decimal /
   `true`/`false` are accepted as literals; computed values go inside
   `${...}` (see 4.2).
3. **Shell-form method call** — `path arg1 arg2`, whitespace-separated:
   `debug.breakpoints.add 0x40802A14`, `screen.save "/tmp/x.png"`,
   `find.str "Apple" $0x40800000..$0x40810000`. Strings with spaces are
   double-quoted. Top-level call form `path(arg, arg)` is not accepted
   — that syntax is reserved for inside `${...}`.
4. **Expression in argument or interpolation** — `${expr}` evaluates and
   is spliced into the surrounding context. Use it as a method argument
   (`cpu.d0 = ${cpu.pc + 4}`), inside a string
   (`echo "pc=${cpu.pc}"`), or as the predicate to `assert`/the source
   for `echo`. A bare `${expr}` typed as a whole line does not print —
   the daemon re-dispatches the substituted text as a command and
   errors with "Unknown command" on the value. To print an expression,
   wrap it: `echo ${cpu.pc + 4}`.

### 4.2 `${expr}` interpolation

`${...}` is the form of expression substitution in the shell. It works
in two places:

- **As an argument**: the result is spliced into the command line.
  `cpu.d0 = ${cpu.pc + 4}`, `assert ${cpu.pc == 0x40800090}`,
  `echo ${cpu.pc + 4}`.
- **Inside a double-quoted string literal**: the result is
  interpolated textually. `echo "pc=${cpu.pc} d0=${cpu.d0}"`.

Inside `${...}` the body is a typed expression:

- arithmetic / bitwise / comparison / logical / ternary operators with
  C-like precedence: `+ - * / % << >> & | ^ == != < > <= >= && || ! ~ ?:`.
  Operators only work inside `${...}` — `cpu.d0 = cpu.pc + 4` at the
  top level is a syntax error.
- literals: `42`, `0x1234`, `0b1010`, `0o17`, `$1234`, `0d100`, `1.0`,
  `"text"`, `true`/`false`/`on`/`off`/`yes`/`no`.
- bare identifiers are paths (`cpu.pc`, `memory.ram_size`); they are
  not alias-resolved here. Aliases require an explicit `$` prefix even
  inside `${...}`: `${$pc + 4}` works, `${pc + 4}` errors with "path
  'pc' did not resolve."
- method calls use call form with parens and commas:
  `${memory.peek.l(0x400)}`, `${debug.breakpoints.add(0x400100)}`,
  `${debug.mac.globals.read("MBState")}`. Shell-form is not accepted
  here.
- indexed children use brackets:
  `${debug.breakpoints[3].addr}`, `${floppy.drives[0].present}`.
  Indices are stable global IDs (sparse, never reused) — the first
  breakpoint added in a fresh process is `[0]`, the next `[1]`, and
  removing one leaves a hole.
- short-circuit: `&&` and `||` skip the unused arm. Errors propagate
  and count as falsy.
- truthiness: `assert ${...}` succeeds when the predicate is truthy;
  numbers are truthy iff non-zero, strings iff non-empty, lists iff
  non-empty, errors are always falsy.
- format spec: an optional trailing `:fmt` controls the to-string
  conversion. The default (no spec) uses the value's native formatter,
  which respects the attribute's hex/decimal flag. Supported specs:

  | Spec        | Effect                                           |
  |-------------|--------------------------------------------------|
  | (none)      | native formatter (e.g. `cpu.pc` → `0x4080002a`)  |
  | `:d`        | force decimal                                    |
  | `:x` / `:X` | force lowercase / uppercase hex (no `$` prefix)  |
  | `:s`        | string passthrough (rejects non-strings)         |
  | `:<W>d`     | space-padded decimal: `${42:5d}` → `"   42"`     |
  | `:0<W>x`    | zero-padded hex: `${cpu.pc:08x}` → `"4080002a"`  |
  | `:%<printf>`| printf-style escape hatch: `${name:%-10s}`       |

  Padding-sensitive specs need to be quoted at the call site if the
  result is consumed as a single argument: an unquoted `${0xab:8X}`
  produces `      AB`, which the line tokenizer then splits on
  whitespace. Wrap in `"..."` (`echo "${0xab:8X}"`) to keep it intact.

### 4.3 Aliases

`shell.alias.list` shows the live set. Built-in aliases cover the CPU
register file: `pc`, `sr`, `ccr`, `ssp`, `usp`, `msp`, `vbr`, `sp`,
`d0..d7`, `a0..a7`, `fpcr`, `fpsr`, `fpiar`, `fp0..fp7`. Mac low-memory
globals are not aliased — read them via
`debug.mac.globals.read("Name")`.

User aliases:

```
shell.alias.add foo cpu.d0
shell.alias.remove foo
```

Resolution: `$name` is alias substitution. There is no auto-fallthrough
to "try as a root child"; root paths are always written without `$`.
Inside `${...}`, alias substitution still requires the leading `$`.

### 4.4 Top-level root methods

Shown by `methods emu` (or `methods` with no argument):
`objects`, `attributes`, `methods`, `help`, `time`, `quit`, `assert`,
`echo`, `download`.

```
echo "hello"                              # → hello
echo "pc=${cpu.pc} d0=${cpu.d0}"          # → pc=0x40826cc6 d0=0x0
assert ${cpu.pc == 0x40826cc6}            # → ASSERT OK: true
assert ${cpu.pc == 0} "boot stalled"      # → ASSERT FAILED: boot stalled
time                                      # → 1778263528 (Unix epoch)
```

## 5. Introspection — discover what's there

Use these four root methods at any prompt to learn the model live; they
are how you should answer "what does this object expose?" before
guessing.

```
objects                       # list root children (cpu, memory, debug, ...)
objects cpu                   # list child objects of cpu (e.g. ["fpu"])
attributes cpu                # list cpu attributes
attributes debug.breakpoints  # attributes on a sub-object
methods debug                 # methods on the debug object
help debug.disasm             # docstring for one member
```

Sample (will drift; treat as illustration):

| Object       | Children         | Selected attributes                                       | Selected methods                                            |
|--------------|------------------|-----------------------------------------------------------|-------------------------------------------------------------|
| `cpu`        | `fpu`            | `pc sr ccr ssp usp msp vbr sp d0..d7 a0..a7 c v z n x instr_count` | (none)                                              |
| `cpu.fpu`    | —                | `fp0..fp7 fpcr fpsr fpiar`                                | (none)                                                      |
| `memory`     | `peek poke`      | `ram_size rom_size`                                       | `read_cstring(addr) dump(addr, n)`                          |
| `memory.peek`| —                | —                                                         | `b(addr) w(addr) l(addr)`                                   |
| `memory.poke`| —                | —                                                         | `b(addr,v) w(addr,v) l(addr,v)`                             |
| `scheduler`  | —                | `running mode cpi cycles instr_count frequency`           | `run([n]) stop()`                                           |
| `debug`      | `mac breakpoints logpoints` | —                                              | `log(cat, level) disasm([addr], [count]) step([n])`         |
| `debug.breakpoints` | indexed entries (by id) | —                                          | `add(addr, [cond], [space]) clear() list()`                 |
| `debug.logpoints`   | indexed entries (by id) | —                                          | `add(spec) clear() list()`                                  |
| `debug.mac`  | `globals`        | —                                                         | `atrap(opcode)`                                             |
| `debug.mac.globals` | —         | —                                                         | `read(name) write(name, val) address(name) list()`          |
| `machine`    | —                | `id name freq ram created`                                | `profile(id) boot(model, ram) register(id, created)`        |
| `rom`        | —                | `path loaded checksum size name`                          | `load(path) identify(path)`                                 |
| `vrom`       | —                | `path loaded size`                                        | `load(path) identify(path)`                                 |
| `screen`     | —                | `width height`                                            | `save(path) match(ref) match_or_save(ref, [actual]) checksum([t l b r])` |
| `find`       | —                | —                                                         | `str(text, [range]) bytes(hex, [range]) long(v, [range]) word(v, [range])` |
| `scsi`       | `devices bus`    | `loopback hd_models`                                      | `identify_hd(p) identify_cdrom(p) attach_hd(p, [id]) attach_cdrom(p, [id])` |
| `scsi.bus`   | —                | `phase target initiator`                                  | (none)                                                      |
| `floppy`     | `drives`         | `type sel`                                                | `identify(path) create(path)`                               |
| `floppy.drives` | indexed (`[0]`, `[1]`) | per-entry: `index present track side motor_on disk` | per-entry: `eject() insert(path)`                |
| `scc`        | `a b`            | `loopback pclk_hz rtxc_hz`                                | `reset()`                                                   |
| `scc.a` / `scc.b` | —           | `index dcd tx_empty rx_pending`                           | (none)                                                      |
| `rtc`        | —                | `time read_only pram`                                     | `pram_read pram_write`                                      |
| `via1` / `via2` | `port_a port_b` | `ifr ier acr pcr sr freq_factor`                       | (none)                                                      |
| `nubus`      | —                | —                                                         | `cards()`                                                   |
| `mouse`      | —                | —                                                         | `move(x, y) click([button], [mode]) trace(start\|stop)`     |
| `keyboard`   | —                | —                                                         | `press(key)` (named keys like `"Return"`, single letters not accepted) |
| `checkpoint` | —                | `auto`                                                    | `probe() clear() load([path]) save([path]) snapshot()`      |
| `storage`    | `images`         | —                                                         | `import cp list_dir find_media hd_create hd_download partmap probe list_partitions mounts unmount path_exists path_size` |
| `vfs`        | —                | —                                                         | `ls([path]) mkdir(path) cat(path)`                          |
| `archive`    | —                | —                                                         | `identify(path) extract(path, dst)`                         |
| `shell.alias`| —                | —                                                         | `add(name, path) remove(name) list()`                       |

When in doubt, run `objects` / `attributes <path>` / `methods <path>` /
`help <path>` against the live daemon — the model is self-describing.

## 6. Common workflows

### 6.1 Inspect CPU state

```
cpu.pc                         # 0x40826cc6
cpu.d0                         # 0x0
cpu.sp                         # via the alias; same as cpu.a7
cpu.fpu.fpcr                   # FPU control reg
echo "pc=${cpu.pc} sp=${cpu.sp}"
```

### 6.2 Single-step

```
debug.step                     # one instruction
debug.step 100                 # 100 instructions, no per-step output
cpu.pc                         # see where we ended up
debug.disasm                   # 16 instructions forward from PC (default)
debug.disasm 5                 # 5 instructions forward from PC
debug.disasm 0x40802A14 5      # 5 instructions forward from an explicit addr
```

`debug.step N` runs N instructions then stops; it does not emit
per-step disassembly. To see each instruction, loop one-step batches
and print PC + `debug.disasm 1`. The two-arg `debug.disasm` form lets
you peek at code anywhere without moving PC first.

### 6.3 Run to a breakpoint

```
debug.breakpoints.add 0x40802A14                   # plain
debug.breakpoints.add 0x40802A14 "cpu.d0 == 0"     # conditional
debug.breakpoints.add 0x40802A14 "" "physical"     # physical-space
debug.breakpoints.list
scheduler.run                                      # runs to break or stop
scheduler.run 1000000                              # bounded run (instruction budget)
cpu.pc
debug.breakpoints.clear
```

Conditional predicates are expression strings — `cpu.d0 == 0`,
`mmu.enabled` (when an MMU object exists), etc.; they share the
expression grammar. Inspect a live entry inside an expression:
`${debug.breakpoints[<id>].hit_count}`.

### 6.4 Memory poke / peek / dump / search

```
memory.ram_size                                       # 8388608 (SE/30 default)
memory.peek.l 0x400                                   # 32-bit BE long
memory.peek.b 0x400                                   # one byte
memory.poke.l 0x10000 0xdeadbeef                      # write
memory.dump 0x100 32                                  # hex+ASCII
memory.read_cstring 0x40800030                        # null-terminated string

find.str "Apple" $0x40800000..$0x40810000             # half-open range
find.long 0x4170706c $0x40800000..$0x40810000         # 32-bit BE
find.bytes "4e 75" "$0x40800000..$0x40810000 all"     # range + "all" as one quoted arg
```

Range syntax: `$<start>..$<end>` half-open, or `$<start> <count>`.
Append `all` to lift the default 16-hit cap. Ranges are passed as a
single `rest` string argument; quote them when in doubt:
`find.str "Apple" "$0x40800000..$0x40810000 all"`.

### 6.5 Memory logpoints (no-halt watchers)

`debug.logpoints.add` takes a single string spec re-tokenised into the
logpoint grammar. Wrap the spec in **single quotes** so the shell
leaves any `${...}` placeholders intact for the logpoint parser to
expand at fire time:

```
debug.logpoints.add '0x40800090 "hello pc=${cpu.pc}"'
debug.logpoints.add '--write 0x16A.l "Ticks bumped pc=${cpu.pc} val=${lp.value}"'
debug.logpoints.add '--read  L:0x1200D0C3.b'
debug.logpoints.list
debug.logpoints.clear
```

Spec components: `[--write|--read|--rw]` (PC logpoint when omitted),
`[L:|P:]<addr>[.b|.w|.l]`, `"message"`, `level=<n>`,
`category=<name>` (default `logpoint` for PC, `memory` for read/write).

Single quotes opt out of `${...}` substitution entirely — the body is
passed through to the logpoint parser verbatim, so deferred
placeholders survive. Inside double quotes `${...}` is expanded
immediately (useful only when you want the *current* value baked into
the message text). Available placeholders at fire time: `${cpu.pc}`,
`${cpu.d0}`, …, `${lp.value}`, `${lp.addr}`, `${lp.size}`,
`${lp.instruction_pc}`, `${memory.peek.b(addr)}`,
`${memory.read_cstring(addr)}`. Streaming output requires the matching
log category to be enabled — `debug.log "memory" 1`.

### 6.6 Mac low-memory globals

```
debug.mac.globals.read "Ticks"          # → 0xf999
debug.mac.globals.read "MBState"        # → 0x80
debug.mac.globals.address "MBState"     # → 0x172
debug.mac.globals.list                  # ["BusErrVct", "MonkeyLives", ..., 471 names]
debug.mac.atrap 0xa05d                  # → "_SwapMMUMode"
```

Sizes: 1/2/4-byte globals come back as unsigned ints; larger blobs
(KeyMap, EventQueue, …) come back as byte buffers.

### 6.7 Screen capture and matching

```
screen.save "/tmp/now.png"
screen.checksum                                       # whole framebuffer
screen.checksum 0 0 100 100                           # rectangle
screen.match "tests/integration/<test>/expected.png"
screen.match_or_save "ref.png" "/tmp/actual.png"
```

Screenshot path must end in `.png`. `match` returns true on
byte-identical framebuffers.

### 6.8 Machine config and ROM probing

```
machine.id                                            # "se30"
machine.ram                                           # 8192 (KB)
machine.freq                                          # 15667200 (Hz)
machine.profile "se30"                                # full profile (JSON string)
machine.profile "plus"                                # ditto

rom.path                                              # currently loaded ROM
rom.checksum                                          # "97221136" (8-hex string)
rom.name                                              # "Universal IIx/IIcx/SE/30 ROM"
rom.identify "tests/data/roms/Plus_v3.rom"            # full info map (JSON)
```

`machine.profile(id)` returns a JSON-string carrying `id`, `name`,
`freq`, `ram_options`, `ram_default`, `ram_max`, `floppy_slots`,
`scsi_slots`, `has_cdrom`, `cdrom_id`, `needs_vrom`. `rom.identify(path)`
returns `{recognised, compatible, checksum, name, size}`. For
unreadable paths both return an error.

### 6.9 Scheduler control

```
scheduler.running                                     # false (idle) / true
scheduler.cycles                                      # cumulative cycle count
scheduler.run                                         # unbounded (until breakpoint/stop)
scheduler.run 1000000                                 # instruction-budgeted
scheduler.stop                                        # halt; safe to send on a
                                                      # second connection mid-run
scheduler.mode                                        # max / realtime / hardware
```

`debug.step N` is sugar for "run N instructions, then stop".

### 6.10 Logging

```
debug.log "cpu" 5                                     # enable cpu category at level 5
debug.log "scsi" 10                                   # high-volume scsi
debug.log "memory" 1                                  # required for memory logpoints
debug.log "cpu" 0                                     # disable
```

The second arg accepts either an integer level or a full named-arg
spec string (`level=5 file=/tmp/cpu.log ts=on`).

### 6.11 Floppy / SCSI / input

```
floppy.create "/tmp/blank.dsk"                        # 800K blank, auto-mounts
floppy.drives[0].insert "/tmp/blank.dsk"              # mount into drive 0
echo ${floppy.drives[0].present}                      # true
floppy.drives[0].eject                                # zero-arg call

scsi.attach_hd "tests/data/hd/system.img" 0
scsi.bus.phase                                        # bus_free / cmd / data / ...

mouse.move 100 100
mouse.click
keyboard.press "Return"
```

## 7. Pitfalls

- **Operators only work inside `${...}`.** `cpu.d0 = cpu.pc + 4` at the
  top level is a syntax error; write `cpu.d0 = ${cpu.pc + 4}`.
- **Top-level `path(arg)` is rejected** — the call form is reserved for
  inside `${...}`. At the prompt use shell form: `cpu.step 1000`,
  `debug.breakpoints.add 0x400`. Inside `${...}` use call form:
  `${cpu.step(1000)}`, `${debug.breakpoints.add(0x400)}`.
- **Aliases require `$`.** `${pc}` errors; `${$pc}` works. Bare `pc`
  is treated as a path against the root, which fails.
- **Padding-sensitive format specs need quoting.** An unquoted
  `${0xab:8X}` expands to `      AB`, which the line tokenizer splits
  on whitespace. Quote the substitution (`echo "${0xab:8X}"`) when the
  consumer wants it as a single token.
- **Indexed children are keyed by stable id, not position.** A fresh
  process numbers from 0; subsequent `clear` + `add` keeps incrementing
  ids monotonically (sparse, never recycled). A stale id surfaces as
  `'breakpoints[<id>]' is empty` (the diagnostic names the parent
  object that the user typed) — read the id from the `add` result and
  use that.
- **Indexed-child reads of attributes work both at the top level and
  inside `${...}`** (`debug.breakpoints[<id>].addr`,
  `floppy.drives[0].present`), but the index must resolve to a live
  entry: a stale id gives "entries[N] is empty" rather than
  dispatching. Method calls on indexed entries also work in both forms
  — shell-form at the prompt (`floppy.drives[0].insert "/tmp/x.dsk"`,
  `debug.breakpoints[<id>].remove`) and call-form inside expressions
  (`${debug.breakpoints[<id>].remove()}`).
- **Logpoint specs need single quotes for deferred `${...}`.** The
  shell substitutor expands `${...}` immediately inside double-quoted
  strings; wrap the whole logpoint spec in `'...'` so placeholders
  reach the logpoint parser intact (`debug.logpoints.add '0x... "${cpu.pc}"'`).
- **VIA timer interrupts don't fire during single-step.** Use
  `scheduler.run N` instead of `debug.step N` if timer-driven code
  needs to make progress.
- **Keyboard `press` accepts named keys like `"Return"`, `"Space"`,
  or hex bytes (`0x24`), but not single letters.** The usable forms
  are the symbolic key names recognised by the ADB key table and the
  raw scancode hex.
- **Heartbeats during long runs.** Filter with `grep -v '^#'`. Don't
  let short `nc -w` timeouts kill long runs — disconnect cancels the
  run.
- **Assertions kill the daemon silently.** A C-level `assert()` fail
  terminates the process without notifying the TCP client. Check
  `kill -0 <pid>` (or just `objects | nc -w 2 localhost 6800`) if
  responses dry up.

## 8. Cross-references

- **Disassembly without a running emulator**: `disasm-tool` skill —
  for static analysis of ROMs and binaries before placing breakpoints.
- **Source layout**: object model in `src/core/object/`; debug
  commands in `src/core/debug/`; shell entry point in
  `src/core/shell/shell.c`; headless TCP daemon in
  `src/platform/headless/headless_main.c`.
- **Integration test scripts** under `tests/integration/` are reliable
  usage examples — `tests/integration/object-eval/test.script`,
  `tests/integration/object-debug/test.script`.
- **Script files** (`--script <path>` or `--script-stdin`) accept the
  same line grammar plus `# comments` and `include <other.script>`.
  The `include` directive is recognised in script files only, not
  over the TCP shell.
