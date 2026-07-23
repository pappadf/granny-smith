---
name: headless-debug
description: >
  Interactive debugging via the headless emulator's TCP shell. Covers daemon
  startup, the typed object-model shell grammar (bare-path reads, setters,
  shell-form and call-form method invocation, ${...} expression
  interpolation), introspection (objects/attributes/methods/help), and the
  command surfaces for cpu, memory, debug.breakpoints/logpoints,
  debug.mac.globals, scheduler, screen, machine.profile, machine.rom.identify,
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
object reachable by a dotted path: `machine.cpu.pc`, `machine.memory.peek.l 0x400`,
`debug.breakpoints.add 0x40802A14`, `machine.screen.save "/tmp/x.png"`,
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
    rom=tests/data/roms/iix-iicx-se30-97221136.rom [hd=disk.img] [fd=floppy.dsk] &
sleep 2  # or block-read for "READY" on stdout
```

Flags:
- `--daemon` — required, enables TCP socket mode.
- `rom=<file>` — required. ROMs live under `tests/data/roms/`
  (`iix-iicx-se30-97221136.rom`, `plus-v3-4d1f8172.rom`, `iix-iicx-se30-97221136.rom`; vrom: `builtin-se30-video-4f71ff1a.vrom`,
  `mdc-8-24-revb-d1629664.vrom`).
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

**Troubleshooting: daemon writes nothing / never prints `READY`.** The
binary works (`--help` prints) but `--daemon` produces an empty log and
never binds — the TCP `bind` is being blocked or the port is stuck.
Causes seen in practice: (1) launched under a **sandboxed runner** that
denies the socket bind — relaunch with the sandbox disabled; (2) a
**stale daemon** still holding the port even though it's not obviously
running — a *fresh port number* usually unsticks it; (3) inline,
multi-line backgrounded launches that silently fail to start — put the
launch in a standalone `.sh` script. The daemon binary itself is fine;
this is a launch-environment problem.

### Example — SE/30, fast, no trailing prompt

```bash
./build/headless/gs-headless --daemon --kill --speed=max --no-prompt \
    rom=tests/data/roms/iix-iicx-se30-97221136.rom &
sleep 2
echo "objects" | nc -w 2 localhost 6800
```

## 3. Sending commands over TCP

Each command is one or more newline-delimited lines on a TCP connection:

```bash
echo "<command>" | nc -w 2 localhost 6800
printf "machine.cpu.pc\ndebug.step\ncpu.pc\n" | nc -w 5 localhost 6800
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

The shell speaks the **v2 script language** (see
`docs/core/shell/shell.md`): parse first, evaluate second; one binding
namespace behind the `$` sigil; interpolation only inside strings.

### 4.1 Statements

```
machine.cpu.pc                      # bare path -> read & print
machine.cpu.d0 = machine.cpu.pc + 4 # typed attribute write (RHS is an expression)
machine.cpu.step 1000               # method call, argument mode
machine.memory.peek.l(0x400)        # call form; works in any expression
let t0 = scheduler.host_user_ns     # create binding; read back as $t0
$t0 = $t0 + 1                       # mutate (error if undeclared)
alias d = machine.floppy.drive[0]   # reference binding (re-resolves per use)
echo "pc=${machine.cpu.pc:08x}"     # ${...} interpolation inside strings
assert machine.cpu.pc == 0x40800090 "boot stalled"
if machine.cpu.d0 == 0 { echo "zero" }
while machine.cpu.pc != $t0 { debug.step 1 }
for addr in find.str("Apple", 0x40800000, 0x40810000) { echo "${$addr:08x}" }
def where() { return "pc=${machine.cpu.pc:08x}" }
```

Multi-line blocks open with `{` at end-of-line and close with `}` on
its own line (`} elif COND {` / `} else {` join the closer); the REPL
shows a `... ` continuation prompt while a block is open. Inline blocks
hold exactly one statement.

In **argument mode** (after a command path) bare words are strings —
`machine.floppy.drive[0].insert /tmp/fd0.image` needs no quotes; use
`"..."` (interpolating), `'...'` (raw), `$binding`, `(expr)`, or
`name=value` named arguments for everything else. Everywhere else is
**expression mode**: bare identifiers are object paths, `$name` reads a
binding, and the C-like operator set applies.

### 4.2 Expressions and `${...}`

`${expr}` lives **only inside double-quoted strings** and splices the
expression's formatted value; `${expr:fmt}` applies a format spec:

  | Spec        | Effect                                           |
  |-------------|--------------------------------------------------|
  | (none)      | native formatter (e.g. `machine.cpu.pc` -> `0x4080002a`) |
  | `:d`        | force decimal                                    |
  | `:x` / `:X` | force lowercase / uppercase hex (no prefix)      |
  | `:s`        | string passthrough (rejects non-strings)         |
  | `:<W>d`     | space-padded decimal: `${42:5d}` -> `"   42"`    |
  | `:0<W>x`    | zero-padded hex: `${machine.cpu.pc:08x}`         |
  | `:%<printf>`| printf-style escape hatch: `${name:%-10s}`       |

Expression-mode extras: `a..b` half-open integer ranges,
`try(EXPR, FALLBACK)` (the only catch mechanism), `error(msg)`,
`range(start, stop[, step])`, `len(x)`, and the `none` literal (equal
only to itself, falsy). Errors are not truth values — an error reaching
`if`/`while`/`for` aborts the script; probe expected failures with
`try(..., none) == none`. Indexed children use brackets
(`debug.breakpoints[3].addr`); list results index too (`$hits[0]`).

Map-shaped results (`machine.profile`, `machine.rom.identify`,
`debug.frame`, …) are typed `V_MAP` values: read keys with dotted
`map.key` or bracket `map["key"]` segments, on paths, call results, and
bindings alike — `machine.profile("se30").capabilities.mmu.kind`,
`machine.config.vroms[0].card_id`, `$info.checksum`. `for k in MAP`
iterates keys; `len(map)` counts entries; `${MAP}` interpolates as
compact JSON text.

Map-shaped results (`machine.profile`, `machine.rom.identify`,
`debug.frame`, …) are typed `V_MAP` values: read keys with dotted
`map.key` or bracket `map["key"]` segments, on paths, call results, and
bindings alike — `machine.profile("se30").capabilities.mmu.kind`,
`machine.config.vroms[0].card_id`, `$info.checksum`. `for k in MAP`
iterates keys; `len(map)` counts entries; `${MAP}` interpolates as
compact JSON text.

### 4.3 Bindings and aliases

`$name` resolves scoped `let` bindings first, then the alias table.
Aliases are **reference bindings**: they store path text, re-resolve on
every access (so they survive `machine.boot`), and write through
(`$pc = 0x400128`). Built-ins cover the CPU register file: `pc`, `sr`,
`ccr`, `ssp`, `usp`, `msp`, `vbr`, `sp`, `d0..d7`, `a0..a7`, `fpcr`,
`fpsr`, `fpiar`, `fp0..fp7`. Mac low-memory globals are not aliased —
read them via `debug.mac.globals.read("Name")`.

```
alias foo = machine.cpu.d0        # statement form
shell.alias.add foo machine.cpu.d0  # method form (same table)
shell.alias.remove foo
let d = machine.floppy.drive[0]   # snapshot: object handle; goes stale on reboot
```

### 4.4 Top-level root methods

Shown by `methods emu` (or `methods` with no argument):
`objects`, `attributes`, `methods`, `help`, `time`, `quit`, `echo`,
`download`. (`assert` is a statement keyword, not a method.)

```
echo "hello"                                      # -> hello
echo "pc=${machine.cpu.pc} d0=${machine.cpu.d0}"  # -> pc=0x40826cc6 d0=0x0
assert machine.cpu.pc == 0x40826cc6               # silent on success
assert machine.cpu.pc == 0 "boot stalled"         # -> ASSERT FAILED: boot stalled
time                                              # -> 1778263528 (Unix epoch)
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
| `machine.cpu.fpu`    | —                | `fp0..fp7 fpcr fpsr fpiar`                                | (none)                                                      |
| `memory`     | `peek poke`      | `ram_size rom_size`                                       | `read_cstring(addr) dump(addr, n)`                          |
| `machine.memory.peek`| —                | —                                                         | `b(addr) w(addr) l(addr)`                                   |
| `machine.memory.poke`| —                | —                                                         | `b(addr,v) w(addr,v) l(addr,v)`                             |
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
| `machine.scsi.bus`   | —                | `phase target initiator`                                  | (none)                                                      |
| `floppy`     | `drives`         | `type sel`                                                | `identify(path) create(path)`                               |
| `machine.floppy.drive` | indexed (`[0]`, `[1]`) | per-entry: `index present track side motor_on disk` | per-entry: `eject() insert(path)`                |
| `scc`        | `a b`            | `loopback pclk_hz rtxc_hz`                                | `reset()`                                                   |
| `machine.scc.a` / `machine.scc.b` | —           | `index dcd tx_empty rx_pending`                           | (none)                                                      |
| `rtc`        | —                | `time read_only pram`                                     | `pram_read pram_write`                                      |
| `via1` / `via2` | `port_a port_b` | `ifr ier acr pcr sr freq_factor`                       | (none)                                                      |
| `nubus`      | —                | —                                                         | `cards()`                                                   |
| `mouse`      | —                | —                                                         | `move(x, y) click([button], [mode]) trace(start\|stop)`     |
| `keyboard`   | —                | —                                                         | `press(key)` (named keys like `"Return"`, single letters not accepted) |
| `checkpoint` | —                | `auto`                                                    | `probe() clear() load([path]) save([path]) snapshot()`      |
| `storage`    | `images`         | —                                                         | `import cp list_dir find_media hd_create partmap probe list_partitions mounts unmount path_exists path_size` (save a mounted disk via `machine.scsi.device[N].image.export`) |
| `vfs`        | —                | —                                                         | `ls([path]) mkdir(path) cat(path)`                          |
| `archive`    | —                | —                                                         | `identify(path) extract(path, dst)`                         |
| `shell.alias`| —                | —                                                         | `add(name, path) remove(name) list()`                       |

When in doubt, run `objects` / `attributes <path>` / `methods <path>` /
`help <path>` against the live daemon — the model is self-describing.

## 6. Common workflows

### 6.1 Inspect CPU state

```
machine.cpu.pc                         # 0x40826cc6
machine.cpu.d0                         # 0x0
machine.cpu.sp                         # via the alias; same as machine.cpu.a7
machine.cpu.fpu.fpcr                   # FPU control reg
echo "pc=${machine.cpu.pc} sp=${machine.cpu.sp}"
```

### 6.2 Single-step

```
debug.step                     # one instruction
debug.step 100                 # 100 instructions, no per-step output
machine.cpu.pc                         # see where we ended up
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
debug.breakpoints.add 0x40802A14 "machine.cpu.d0 == 0"     # conditional
debug.breakpoints.add 0x40802A14 "" "physical"     # physical-space
debug.breakpoints.list
scheduler.run                                      # runs to break or stop
scheduler.run 1000000                              # bounded run (instruction budget)
machine.cpu.pc
debug.breakpoints.clear
```

Conditional predicates are expression strings — `machine.cpu.d0 == 0`,
`mmu.enabled` (when an MMU object exists), etc.; they share the
expression grammar. Inspect a live entry inside an expression:
`${debug.breakpoints[<id>].hit_count}`.

### 6.4 Memory poke / peek / dump / search

```
machine.memory.ram_size                                       # 8388608 (SE/30 default)
machine.memory.peek.l 0x400                                   # 32-bit BE long
machine.memory.peek.b 0x400                                   # one byte
machine.memory.poke.l 0x10000 0xdeadbeef                      # write
machine.memory.dump 0x100 32                                  # hex+ASCII
machine.memory.read_cstring 0x40800030                        # null-terminated string

find.str "Apple" 0x40800000 0x40810000        # start, end (inclusive)
find.long 0x4170706c 0x40800000 0x40810000    # 32-bit BE
find.bytes "4e 75" 0x40800000 0x40810000      # hex byte string
```

Each `find.*` method returns the **complete list of match addresses**
(empty list = not found; `$hits[0]` is the first hit). Omitting
`start`/`end` scans the whole address space — prefer explicit ranges;
a full-space scan crosses IO regions and is slow. Iterate results with
`for addr in find.str(...) { ... }` or capture with `let`.

**Inspection is side-effect-free and crash-proof.** `peek` / `dump` /
`poke` / `read_cstring` / `peek.bytes` and `find.*` use a debug
translation path that never faults the guest, never crashes the daemon
on a bad/unmapped address, and never injects a bus error into the
running guest. So it is safe to inspect or poke garbage / unmapped
addresses (e.g. while chasing a stale pointer at a breakpoint, or
`find`-scanning across unmapped pages). Unmapped reads return all-ones
(`0xFF…`); writes to ROM or unmapped pages are dropped silently; device
registers are still dispatched — so peeking a *stateful* device
register (a FIFO, or an IRQ-clear-on-read bit) can still trigger the
normal device side effect. Prefer the device objects (`via1`, `scsi`,
…) when you need a guaranteed-clean register read. (Before mid-2026
these commands ran the full CPU memory path and *could* both crash the
daemon and silently bus-error the guest you were inspecting.)

### 6.5 Memory logpoints (no-halt watchers)

`debug.logpoints.add` takes named arguments (shell v2 §6.2); the
`message=` slot is a **fire-time template** (§6.3) — its string body is
stored raw and evaluated on every fire:

```
debug.logpoints.add addr=0x40800090 message="hello pc=${machine.cpu.pc}"
debug.logpoints.add addr=0x16A width=l mode=write level=5 message="Ticks pc=${machine.cpu.pc} val=${$value:08x}"
debug.logpoints.add addr=0x1200D0C3 mode=read width=b space=physical
debug.logpoints.entries
debug.logpoints.clear
```

Arguments: `addr` (required), `mode` (`pc` default / `read` / `write`
/ `rw`), `width` (`b`/`w`/`l`, memory modes), `end` (range), `message`
(template), `level`, `category` (default `logpoint` for PC, `memory`
for read/write), `value` (fire only on matching value), `space`
(`logical`/`physical`).

**Memory logpoints only see CPU accesses.** They hook the CPU
read/write path, so they do **not** fire on bus-master DMA or other
device-engine writes (e.g. the IIfx SCSI DMA writes straight to host
RAM, bypassing the hook). To catch a DMA writer of a physical byte, use
a gdb hardware watchpoint on the host address (the RAM image base plus
the physical offset) or an emulator-side trace in the device's transfer
loop, not a logpoint.

Available in message templates at fire time: any expression
(`${machine.cpu.pc}`, `${machine.memory.peek.b(0x400)}`,
`${machine.memory.read_cstring(0x40800030)}`) plus the per-fire
bindings `$value`, `$addr`, `$size` (memory modes). Streaming output
requires the matching log category to be enabled — `debug.log memory 1`.

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
machine.screen.save "/tmp/now.png"
machine.screen.checksum                                       # whole framebuffer
machine.screen.checksum 0 0 100 100                           # rectangle
machine.screen.match "tests/integration/<test>/expected.png"
machine.screen.match_or_save "ref.png" "/tmp/actual.png"
```

Screenshot path must end in `.png`. `match` returns true on
byte-identical framebuffers.

### 6.8 Machine config and ROM probing

```
machine.id                                            # "se30"
machine.ram                                           # 8192 (KB)
machine.freq                                          # 15667200 (Hz)
machine.profile "se30"                                # full profile (typed map)
machine.profile "plus"                                # ditto

machine.rom.path                                              # currently loaded ROM
machine.rom.checksum                                          # "97221136" (8-hex string)
machine.rom.name                                              # "Universal IIx/IIcx/SE/30 ROM"
machine.rom.identify "tests/data/roms/plus-v3-4d1f8172.rom"            # full info map (typed)
```

`machine.profile(id)` returns a typed map carrying `id`, `name`,
`freq`, `ram_options`, `ram_default`, `ram_max`, `floppy_slots`,
`scsi_slots`, `hd_bus`, `has_cdrom`, `cdrom_id`, `capabilities`,
`video_slots`. `machine.rom.identify(path)` returns
`{recognised, compatible, checksum, name, size}`. Key access works
directly (`machine.profile("se30").capabilities.mmu.kind`); `${…}`
interpolation renders a map as compact JSON. For unreadable paths both
return an error.

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

**Execution is deterministic.** The emulator is single-threaded and
cycle-driven; with a fixed `machine.rtc.time` and `--speed=max`, a fresh boot
reproduces bit-identical state for the same command sequence
(instruction count, PC, registers, framebuffer checksum) — verified
identical across runs out to hundreds of millions of instructions. So
events land at reproducible instruction counts, and you can pinpoint
where two scenarios diverge by running the same budget and diffing
`machine.cpu.pc` / registers / `machine.screen.checksum`. (If you ever see apparent
non-determinism, suspect the harness, not the core: a torn disk image
from copying it while a daemon still holds it, a daemon crash that
truncated a run, or a breakpoint condition matching a transient value.)

**`scheduler.run N` can retire fewer than N instructions.** A guest
idle `STOP` consumes the cycle budget without retiring instructions, so
a bounded run may "undershoot" its target. Loop and check
`machine.cpu.instr_count` to reach a specific instruction count.

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
machine.floppy.create "/tmp/blank.dsk"                        # 800K blank, auto-mounts
machine.floppy.drive[0].insert "/tmp/blank.dsk"              # mount into drive 0
echo "${machine.floppy.drive[0].present}"                    # true
machine.floppy.drive[0].eject                                # zero-arg call

machine.scsi.attach_hd "tests/data/hd/system.img" 0
machine.scsi.bus.phase                                        # bus_free / cmd / data / ...

machine.adb.mouse.move 100 100
machine.adb.mouse.click
machine.adb.keyboard.press "Return"
```

### 6.12 Reading individual resources (synthetic /rsrc tree)

The image-VFS exposes each HFS file's resource fork as a directory tree
addressable as `<file>/rsrc/<TYPE>/<id>`. The data fork stays at `<file>`,
the Finder info stays at `<file>/finf`, and the raw fork bytes are
still available at `<file>/rsrc/_raw` (the previous `<file>/rsrc`
semantics, relocated under the new directory).

```
# Mount an image without auto-mounting via path-walk.
storage.probe "tests/data/systems/System_6_0_8.dsk"

# What resource types does the Finder carry?
vfs.ls "tests/data/systems/System_6_0_8.dsk/partition1/System Folder/Finder/rsrc/"

# How many CODE resources, and what are their IDs?
vfs.ls "tests/data/systems/System_6_0_8.dsk/partition1/System Folder/Finder/rsrc/CODE/"
# 5
# 5.info
# 9
# 9.info
# ...

# Read a `vers` resource's bytes.
vfs.cat "tests/data/systems/System_6_0_8.dsk/partition1/System Folder/Finder/rsrc/vers/1"

# Read the matching .info sidecar (small JSON, safe over TCP).
vfs.cat "tests/data/systems/System_6_0_8.dsk/partition1/System Folder/Finder/rsrc/vers/1.info"
# {"name":"","attrs":["purgeable"],"size":50}

# For binary resources (CODE, PICT, SND), use storage.cp rather than vfs.cat
# so the bytes don't go through the TCP response stream.
storage.cp "tests/data/.../Finder/rsrc/CODE/1" "/tmp/code1.bin"

# Recursive dump of the whole fork as ordinary files.
storage.cp -r "tests/data/.../Finder/rsrc/" "/tmp/finder-rsrc/"
```

Conventions:

- `<TYPE>` is the 4-byte resource type, MacRoman-transcoded to UTF-8.
  Quote the path when the type has a trailing space: `"…/rsrc/STR /128"`.
- `<id>` is the signed int16 resource ID as base-10 with a leading `-`
  for negatives: `"…/rsrc/DRVR/-16"`.
- `<file>/rsrc/_raw` is the entire raw resource fork (use this for
  format-level inspection or as a hand-off to an external parser).
- Files with an empty resource fork have *no* synthetic tree —
  `storage.path_exists "<file>/rsrc"` returns false rather than true.

## 7. Pitfalls

- **Errors abort scripts.** In v2 the first failed statement aborts the
  script (and a bare read of an empty slot is an error). Probe expected
  failures with `try(EXPR, none) == none` instead of relying on
  error-and-continue.
- **Scripts print nothing implicitly.** A bare `machine.cpu.pc` line is
  silent in a script (it prints at the interactive prompt); wrap in
  `echo "${machine.cpu.pc}"` when the log line matters.
- **Bindings need `$` on every read.** `let t0 = ...` then `$t0`;
  mutating an undeclared name (`$typo = 1`) errors instead of creating.
- **Padding-sensitive format specs**: `${0xab:8X}` inside a string
  keeps its spaces (`echo "[${0xab:8X}]"`).
- **Indexed children are keyed by stable id, not position.** A fresh
  process numbers from 0; subsequent `clear` + `add` keeps incrementing
  ids monotonically (sparse, never recycled). A stale id surfaces as
  `'breakpoints[<id>]' is empty` — read the id from the `add` result
  (it returns the entry object) and use that. Index-less reads
  (`debug.breakpoints.entries`) return the whole collection as a list.
- **Template strings evaluate at fire time.** `message="...${$value}..."`
  is stored raw because the slot is template-typed; every *other*
  double-quoted string interpolates immediately at statement time.
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
  responses dry up. (Memory inspection — `peek`/`dump`/`poke`/`find` —
  no longer crashes on bad addresses; see §6.4.)
- **Conditional breakpoints fire on a *transient* match.** A condition
  like `machine.cpu.a2 == 0x1234` at a function-entry PC triggers whenever A2
  *momentarily* equals the value — e.g. before the intended register
  load, so you can stop on the "wrong" hit. Confirm with a second field
  (`machine.cpu.d2`, a count), or break at a PC where the register is known to
  hold the value.

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
- **Script files** (`script=<path>` or `--script-stdin`) accept the
  same statement grammar plus `# comments`; multi-line blocks are
  parsed across lines. Over the TCP shell, lines accumulate while a
  `{` block is open.
