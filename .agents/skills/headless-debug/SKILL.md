---
name: headless-debug
description: >
  Interactive debugging of the Granny Smith emulator through the headless
  daemon's TCP shell: launch, the object-model shell grammar, breakpoints,
  logpoints, stepping, memory/disasm/find, Mac globals, screen, scheduler,
  checkpoints, devices and input, and the pitfalls that bite.
triggers:
  - single step
  - set a breakpoint
  - examine memory
  - examine registers
  - debug the boot
  - disassemble
  - gs-headless
  - daemon
  - logpoint
  - check global variable
  - object model
---

# Headless daemon debugging

The daemon exposes the emulator's shell on a TCP port. The shell is a typed
object-model REPL: every subsystem is a dotted path (`machine.cpu.pc`,
`debug.breakpoints`, `scheduler.run`). **The model is self-describing — when
this file and the daemon disagree, trust `objects` / `attributes` / `methods`
/ `help`.**

## Build and launch

```bash
# needs gcc, make, and binutils-m68k-linux-gnu (m68k-linux-gnu-as)
make -f Makefile.headless                       # -> build/headless/gs-headless
./build/headless/gs-headless --daemon --kill --speed=max --no-prompt \
    rom=tests/data/roms/plus-v3-4d1f8172.rom "fd=path/to/floppy.image" &
# wait for the line READY on stdout; PID file: /tmp/gs-headless-<port>.pid
```

- Test media lives in `tests/data/` (a copy of the private `gs-test-data`
  repo at the revision pinned in `tests/data-version`; `scripts/fetch-test-data.sh`).
  Large images ship as `.7z` — extract with `7z e -o<dir> <file>.7z`.
- CLI args: `rom=` (required), `model=` (e.g. `iicx`; the universal
  IIx/IIcx/SE/30 ROM otherwise boots as `se30`), `ram=<KB>`, `hd=` (repeatable),
  `cdrom=`, `fd=` / `fd0=` / `fd1=`, `video_card=`, `monitor=`, `script=<file>`,
  `--var NAME=VAL`.
- Flags: `--port=N` (default 6800), `--kill` (replace a daemon on that port),
  `--no-prompt` (no status line after each reply), `--speed=turbo|paced|accelerated`
  (`max` = `turbo`), `--script-stdin`.
- The machine sits at the reset vector until you run it.
- Non-daemon `script=` runs keep free-running after the script ends: finish
  the script with `quit`.
- If the daemon never prints `READY`, the bind was blocked (sandbox) or the
  port is held by a stale daemon — try another `--port`, and launch from a
  standalone `.sh` rather than an inline multi-line background command.

## Talking to it

```bash
echo 'machine.cpu.pc' | nc -w 3 localhost 6800
printf 'debug.step\nmachine.cpu.pc\n' | nc -w 3 localhost 6800
```

- Use `nc -w <secs>`; give long runs a long `-w`. **Disconnecting cancels an
  in-flight run.**
- During `scheduler.run` the daemon prints `# running... N instructions`
  heartbeats; filter with `grep -v '^#'`.
- While a run is in flight, another connection accepts only `stop`,
  `scheduler.stop`, `shell.interrupt` or `quit`; anything else gets
  `# busy: ...`. A `stop` also cancels a script loop around the run.
- Bindings, aliases and `def`s are daemon-global: they persist across connections.
- Interactive replies echo the return value, so `echo`, `scheduler.run`,
  `debug.step` and `disasm` print a trailing `true`.

## Shell grammar (full spec: `docs/core/shell/shell.md`)

```
machine.cpu.pc                          # bare path: read and print
machine.cpu.d0 = machine.cpu.pc + 4     # typed write; RHS is an expression
debug.disasm 0x408986 3                 # argument mode: bare words are strings
debug.disasm(0x408986, 3)               # call form, usable inside expressions
let t0 = machine.cpu.instr_count        # binding; read it as $t0
$t0 = $t0 + 1                           # mutate (error if never declared)
alias d = machine.floppy.drive[0]       # reference binding, re-resolved per use
echo "pc=${machine.cpu.pc:08x}"         # ${expr[:fmt]} only inside "..."
assert machine.cpu.pc != 0 "msg"
if machine.cpu.d0 == 0 { echo "zero" }
for a in find.str("Apple", 0x400000, 0x410000) { echo "${$a:08x}" }
def where() { return "pc=${machine.cpu.pc:08x}" }
include "tests/integration/lib/mac.script"
```

- Blocks: `{` at end of line, `}` on its own line; an inline block holds one statement.
- Formats: `:d`, `:x`/`:X`, `:08x`, `:5d`, `:s`, `:%<printf>`.
- Extras: `try(EXPR, FALLBACK)`, `error(msg)`, `range()`, `a..b`, `len()`, `none`.
  An error reaching `if`/`while`/`for`, or any failed statement in a script, aborts.
- A bare statement starting with `$` must be a binding name: `$hits[0]` fails,
  `($hits[0])` works.
- Map results: `machine.profile("se30").capabilities.mmu.kind`. Methods need
  the call form before a key: `debug.frame().regs.pc`, not `debug.frame.regs`.
- Built-in aliases: `$pc $sr $ccr $sp $ssp $usp $msp $vbr $d0..$d7 $a0..$a7`, plus
  `$fp0..$fp7 $fpcr $fpsr $fpiar` when the CPU has an FPU. `shell.alias.list` shows all.
- `let x = <object>` goes stale on `machine.boot`; an `alias` survives.

## Introspection

```
objects                        # root: storage shell debug appletalk scheduler find vfs archive checkpoint machine
objects machine                # devices: cpu memory screen adb floppy scsi via1 via2 scc rtc sound nubus pci rom vrom prom config ...
attributes machine.cpu
methods debug
help debug.disasm
```

## Execution control

```
scheduler.run 1000000          # run N instructions (or until a breakpoint / stop)
scheduler.run                  # unbounded
scheduler.stop                 # from a second connection
debug.step                     # one instruction
debug.step 100
machine.cpu.instr_count
scheduler.mode                 # turbo | paced | accelerated (writable)
machine.reset                  # warm reset: keeps RAM, PRAM and media
machine.restart                # power-cycle from machine.config, keeps media
machine.boot model="iicx" rom="tests/data/roms/iix-iicx-se30-97221136.rom" ram=8192
```

- `debug.step` services no interrupts (VBL, VIA timers): `Ticks` does not move.
  Use `scheduler.run N` when interrupt-driven code must progress.
- `scheduler.run N` can retire fewer than N instructions when the guest
  sits in `STOP`; loop on `machine.cpu.instr_count` for an exact count.
- `machine.boot` is a complete document: `model=` and `rom=` are required,
  and every other field defaults per model.
- Runs are deterministic at `--speed=max`: the same command sequence gives
  the same instruction counts, registers and screen checksum.

## Breakpoints

```
debug.breakpoints.add 0x408988                          # returns the entry (note its id)
debug.breakpoints.add 0x408988 "machine.cpu.d0 == 0"    # conditional
debug.breakpoints.add 0x40802A14 "" "physical"          # physical address space
debug.breakpoints.entries                               # table of all
debug.breakpoints[0].hit_count
debug.breakpoints[0].remove
debug.breakpoints.clear
```

- A hit prints `breakpoint hit at $XXXXXXXX` after the run's `true`.
  Running again resumes past the current breakpoint.
- Ids are never reused; take them from the `add` result.
- Conditions fire on transient values; confirm with a second register if it matters.

## Logpoints (log without stopping)

```
debug.logpoints.add addr=0x408988 message="pc=${machine.cpu.pc}"
debug.logpoints.add addr=0x16A width=l mode=write message="Ticks=${$value:08x}"
debug.logpoints.add addr=0x160 end=0x170 mode=rw
debug.logpoints.add addr=0x16A mode=write value=0x2800 message="hit"
debug.logpoints.entries
debug.logpoints.clear
```

- Args: `addr` (required), `mode` = `pc` (default) / `read` / `write` / `rw`,
  `width` = `b`/`w`/`l`, `end`, `value` (fire only on this value), `space`
  = `logical`/`physical`, `message`, `level`, `category`.
- `message` is evaluated each time the logpoint fires; `$value`, `$addr` and
  `$size` are bound for memory modes.
- **Level gating:** the default `level=0` always prints. A `level=N` logpoint
  prints only when its category (`logpoint` for pc, `memory` otherwise) is at N or
  more: `debug.log memory 5`.
- Memory logpoints see CPU accesses only (including device registers), not DMA.

## Memory, disassembly, search

```
machine.memory.peek.l 0x400            # .b .w .l
machine.memory.peek.bytes 0x40a714 8   # up to 4096 bytes
machine.memory.poke.l 0x10000 0xdeadbeef
machine.memory.dump 0x100 32           # hex + ASCII
machine.memory.read_cstring 0x40a714
machine.memory.translate 0x400         # MMU translation of a logical address
debug.disasm                           # 16 instructions from PC
debug.disasm 5
debug.disasm 0x408986 3
find.str "Apple" 0x400000 0x410000     # [start, end) - end is exclusive
find.long 0x4170706c 0x400000 0x410000
find.word 0x4e75 0x408980 0x408990
find.bytes "4e 75" 0x408980 0x408990   # space-separated 2-digit hex
```

- The inspection commands never fault the guest or crash the daemon.
  Unmapped reads return `0xff...`, and writes to ROM or unmapped pages are
  dropped. Peeking a device register still has its side effects (FIFOs,
  clear-on-read).
- `find.*` returns every match as a list. Without a range it scans the
  whole address space, which is slow.

## Crash hunting

```
debug.exceptions                       # last 256 exceptions (vector, pc, sr, fault addr)
debug.exceptions filter=1              # hide routine traps/IRQs
let f = debug.frame()                  # regs + 32 disasm rows + MMU translation
($f.regs.pc)
machine.cpu.mmu                        # tc crp srp tt0 tt1 mmusr enabled (68030)
machine.irq
machine.ipl
```

## Mac OS globals and traps

```
debug.mac.globals.read "Ticks"         # 1/2/4-byte globals come back as ints
debug.mac.globals.read "KeyMap"        # bigger ones as bytes: len() = 16
debug.mac.globals.address "MBState"
debug.mac.globals.write "MBState" 0x80  # 1/2/4-byte globals only
debug.mac.globals.list
debug.mac.atrap 0xa05d                 # -> _SwapMMUMode
```

Region markers (`ScrapEnd`, ...) have no size: use `address` and peek.

## Screen

```
machine.screen.save "/tmp/now.png"          # .png only
machine.screen.checksum                     # signed int
machine.screen.checksum 0 0 100 100         # top left bottom right
machine.screen.matches "ref.png"            # true/false, silent: use this in loops
machine.screen.match "ref.png"              # prints MATCH OK/FAILED
machine.screen.match_or_save "ref.png" "/tmp/actual.png"   # writes actual on mismatch
machine.screen                              # width height depth format ...
```

## Logging

```
debug.log cpu 5                                   # category, level
debug.log cpu level=5 file=/tmp/cpu.log stdout=off ts=on
debug.log cpu 0
debug.log_levels                                  # every category and its level
```

## Checkpoints

```
checkpoint.save "/tmp/a.gscp"
checkpoint.load "/tmp/a.gscp"
```

`checkpoint.load` drops all breakpoints and logpoints. Memory (read/write)
logpoints added after a load do not fire, though pc logpoints and
breakpoints do. For memory watches, boot fresh instead.

## Devices and input

```
machine.id                                  # plus, se30, iicx, ...
machine.config                              # model ram rom vroms slot_cards video_card ...
machine.profile "se30"                      # static model description
machine.rom.identify "tests/data/roms/plus-v3-4d1f8172.rom"
machine.floppy.create "/tmp/blank.dsk"      # 800K blank, into a free drive; won't overwrite
machine.floppy.drive[1].eject
machine.floppy.drive[1].insert "/tmp/blank.dsk"
machine.scsi.attach_hd "disk.img" 0
machine.scsi.device[0]                      # vendor product image ...
machine.scsi.bus.phase
machine.via1                                # ifr ier acr pcr sr, .port_a/.port_b
machine.rtc.pram.peek 0x10                  # peek poke dump snapshot restore
machine.adb.mouse.move 100 100
machine.adb.mouse.click true                # press; the default is press too
machine.adb.mouse.click false               # release
machine.adb.keyboard.press "Return"         # names: return space esc tab delete up down left right command shift option control capslock
machine.adb.keyboard.press "a"              # single characters work
machine.adb.keyboard.press 0x2E             # ADB keycode
machine.adb.keyboard.down "Command"         # chords: down, press, up
machine.adb.keyboard.up "Command"
machine.adb.keyboard.type "hello\n"         # short line, US layout
```

For guest-paced input, `include "tests/integration/lib/mac.script"` and use
`click(x, y)`, `double_click`, `run_ticks(n)`, `wait_match(png, ceiling, step)`,
`wait_stable(ceiling)` and `wait_global(name, want, ceiling)`.

## Disk images and files (VFS)

```
storage.probe "tests/data/systems/System_6_0_8.dsk"
vfs.ls "tests/data/systems/System_6_0_8.dsk/partition1/System Folder/Finder/rsrc/"
vfs.cat ".../Finder/rsrc/vers/1.info"      # {"name":"","attrs":["purgeable"],"size":50}
storage.cp ".../Finder/rsrc/CODE/1" "/tmp/code1.bin"
storage.cp -r ".../Finder/rsrc/" "/tmp/finder-rsrc/"
```

A file's resource fork appears as `<file>/rsrc/<TYPE>/<id>`, each with a
`<id>.info` sidecar; the whole fork is at `<file>/rsrc/_raw`. Use `storage.cp`
rather than `vfs.cat` for binary data.

## Pitfalls

- A C-level `assert()` kills the daemon without a word to the client. If
  replies stop, check `kill -0 $(cat /tmp/gs-headless-6800.pid)`.
- A bare path is silent inside a `script=` file; use `echo "${...}"` to print.
- A breakpoint or pc logpoint on the **first instruction of an exception or
  interrupt handler** mostly does not fire. On a Plus, `0x8d96`, the level-1
  vector target, fired 2 times where the next instruction fired 65. Put it on
  the handler's second instruction.
- After a client disconnects mid-run, the next connection's first `while`
  loop can abort at once with `interrupted`. Re-send it.

## See also

- `disasm-tool` skill: static disassembly of ROMs and binaries.
- `docs/core/shell/shell.md`, `docs/core/shell/object-model.md`, `docs/core/shell/log.md`.
- Working examples: `tests/integration/object-debug`, `object-logpoint`,
  `object-expr`, `shell-v2`, `boot-config`, and `lib/mac.script`.
