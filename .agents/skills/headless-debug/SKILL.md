---
name: headless-debug
description: >
  Interactive debugging via the headless emulator's TCP shell. Covers
  daemon startup, command protocol (step/run/breakpoint/memory examine),
  register inspection, logging, and common debugging workflows.
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
---

# Granny Smith Headless Daemon — Agent Skill

## Overview

Granny Smith is a browser-based Macintosh emulator. The **headless daemon mode** exposes
its interactive shell over a TCP socket, enabling AI agents to start the emulator,
send commands, and capture output reliably.

## 1. Building

```bash
cd /workspaces/granny-smith
make -f Makefile.headless
```

The binary is produced at `build/headless/gs-headless`.

## 2. Starting the Daemon

```bash
./build/headless/gs-headless --daemon rom=<ROM_FILE> [hd=<HD_FILE>] [fd=<FD_FILE>] [--port=PORT] &
```

- `--daemon` — required, enables TCP socket mode
- `rom=<file>` — ROM image (required). Example: `tests/data/roms/SE30.rom`
- `hd=<file>` — hard disk image (optional, can repeat up to 8)
- `fd=<file>` — floppy disk image (optional, can repeat up to 2)
- `--port=PORT` — TCP port (default: **6800**)
- `--speed=MODE` — scheduler mode: `max`, `realtime`, `hardware` (default: `realtime`)
- `--kill` — kill any existing daemon on the same port before starting
- `--script-stdin` — read script commands from stdin instead of a file
- `--quiet`, `-q` — suppress startup messages

**Important:** The emulator does NOT auto-run in daemon mode. It waits for commands.

**Tip:** Use `--speed=max` for debugging to avoid real-time delays. The emulator
runs as fast as the host CPU allows, making breakpoint-heavy workflows much faster.

### Startup and READY Signal

The daemon prints `READY` to stdout once the TCP listener is bound and the machine
is fully initialized. Callers can block-read for this line instead of sleeping:

```bash
./build/headless/gs-headless --daemon rom=SE30.rom &
read -r line < /proc/$!/fd/1  # or use a pipe
# $line == "READY"
```

The daemon also prints its PID: `Daemon PID: 12345`.

### Port Conflicts

If port 6800 is already in use, the daemon prints a clear error:
```
Error: port 6800 already in use. Use --port=N to specify a different port.
```

Use `--kill` to automatically kill an existing daemon on the same port before starting:
```bash
./build/headless/gs-headless --daemon --kill rom=SE30.rom &
```

### Example — SE/30 with ROM only

```bash
./build/headless/gs-headless --daemon --speed=max rom=tests/data/roms/SE30.rom &
sleep 2   # wait for daemon to initialise (or block-read for READY)
```

### Example — Macintosh Plus with hard disk

```bash
./build/headless/gs-headless --daemon rom=tests/data/roms/plus.rom hd=tests/data/hd/disk.img &
sleep 2
```

### Example — Script from stdin (no file needed)

```bash
printf 'break $2B868\nrun\ninfo regs\nquit\n' | ./build/headless/gs-headless rom=SE30.rom --script-stdin
```

## 3. Sending Commands

Every command is a one-shot TCP transaction:

```bash
echo "<command>" | nc -w 2 localhost 6800
```

The daemon reads the command, executes it, writes all output to the socket,
and closes the connection when the command finishes (or the emulator stops).

### Batch Mode (Multiple Commands per Connection)

Multiple newline-delimited commands can be sent in a single TCP connection.
They execute in order, with output flushed between commands:

```bash
printf "step\ninfo regs\nprint \$pc\n" | nc -w 5 localhost 6800
```

This is faster than separate connections and ensures commands execute
as a sequence without gaps.

**Every response ends with a status line** showing the disassembled instruction
at the current PC (like the web shell prompt). Use `prompt off` to suppress
this status line for cleaner script output:

```bash
printf "prompt off\nstep\ninfo regs\n" | nc -w 5 localhost 6800
```

```
$ echo "step" | nc -w 2 localhost 6800
4083f61c  6000  BRA       *+$023A
```

This means the PC is now at `$4083F61C` and the next instruction to execute
is `BRA *+$023A`. This applies to all commands — `step`, `info regs`, `print $pc`, `examine`, etc.

All addresses use the Motorola `$` hex prefix (e.g., `$40800000`) with uppercase
hex digits. Input accepts `$`, `0x`, or bare hex. Use `L:` or `P:` prefix for
explicit logical/physical addressing (e.g., `break P:$00801656`).

### Capturing output

```bash
RESULT=$(echo "info regs" | nc -w 2 localhost 6800)
echo "$RESULT"
```

## 4. Command Reference

Commands follow GDB/LLDB naming conventions. The `$` prefix resolves registers
(`$pc`, `$d0`) and Mac globals (`$Ticks`, `$MBState`) uniformly.

### Execution Control

| Command | Alias(es) | Description |
|---------|-----------|-------------|
| `continue [N]` | `c` | Resume execution (optionally for N instructions) |
| `run [N]` | — | Start execution (optionally stop after N instructions) |
| `step [N]` | `s`, `si` | Step one (or N) instructions |
| `next` | `n`, `ni` | Step over subroutine call |
| `finish` | `fin` | Run until current subroutine returns |
| `until <addr>` | `u` | Run until address is reached |
| `stop` | `halt` | Stop execution immediately |
| `advance <addr>.size <op> <val>` | — | Run until memory condition is met |

### Breakpoints and Watchpoints

| Command | Alias(es) | Description |
|---------|-----------|-------------|
| `break <addr>` | `b` | Set breakpoint |
| `break set <addr>` | — | Set breakpoint (explicit) |
| `break del <addr\|all>` | — | Delete breakpoint(s) |
| `break list` | — | List all breakpoints |
| `break clear` | — | Delete all breakpoints |
| `tbreak <addr>` | `tb` | Set temporary breakpoint (auto-deletes on hit) |
| `watch <addr>.size <op> <val>` | `w` | Set memory watchpoint |
| `logpoint set <addr> [msg]` | `lp` | Set logpoint |
| `logpoint del <addr\|all>` | — | Delete logpoint(s) |
| `logpoint list` | — | List all logpoints |

### Inspection

| Command | Alias(es) | Description |
|---------|-----------|-------------|
| `print <target>` | `p` | Print register, Mac global, or memory value |
| `examine <addr> [n]` | `x` | Examine raw memory (hex + ASCII, default 64 bytes) |
| `disasm [addr] [n]` | `dis`, `d` | Disassemble instructions (default: from PC) |
| `set <target> <value>` | — | Set register, flag, or memory value |

**`print` examples:**
```
p $d0              # CPU register
p $pc              # program counter
p $Ticks           # Mac global (reads value at address 0x016A)
p $MBState         # Mac global
p $0400.w          # memory word at address 0x400
```

### Info Subcommands

| Command | Alias | Description |
|---------|-------|-------------|
| `info regs` | `info r`, `i r` | CPU register dump (D0-D7, A0-A7, PC, SR, USP, SSP) |
| `info fpregs` | `info fp` | FPU register dump (FP0-FP7, FPCR, FPSR, FPIAR) |
| `info mmu` | — | MMU register dump (TC, CRP, SRP, TT0, TT1) |
| `info break` | `info b` | List all breakpoints |
| `info logpoint` | `info lp` | List all logpoints |
| `info mac` | — | Mac OS state summary (mouse, cursor, ticks) |
| `info process` | `info proc` | Current application info |
| `info events` | — | Scheduler event queue |
| `info schedule` | — | Scheduler mode and CPI |

### Tracing and Logging

| Command | Description |
|---------|-------------|
| `trace start [file]` | Start instruction tracing (optionally to a file) |
| `trace stop` | Stop tracing |
| `trace show [file]` | Show trace buffer (optionally save to file) |
| `log <category> <level>` | Enable logging for a category |
| `log <category> <level> file=<path>` | Redirect log output to a file |

### Display

| Command | Description |
|---------|-------------|
| `translate <addr>` | Show MMU address translation |
| `addrmode [auto\|expanded\|collapsed]` | Set address display format |
| `prompt [on\|off]` | Toggle trailing PC disassembly status line |
| `screenshot save <file.png>` | Save emulated screen as PNG |
| `screenshot checksum [t l b r]` | Compute screen checksum |
| `screenshot match <ref.png>` | Compare screen with reference PNG |

### Input

| Command | Description |
|---------|-------------|
| `key <name\|0xNN>` | Inject keyboard input |
| `mouse move <x> <y>` | Move mouse to position |
| `mouse down` / `mouse up` | Press/release mouse button |
| `mouse click [x y]` | Click (optional move + press + release) |
| `mouse trace start\|stop` | Log mouse position periodically |
| `post-event <what> <message>` | Post raw Mac OS event |

### Media and Configuration

| Command | Description |
|---------|-------------|
| `fd insert <path>` | Auto-detect and insert a floppy disk image |
| `fd create [--hd] <path>` | Create blank floppy |
| `hd attach <path> [id]` | Attach SCSI hard disk image |
| `hd loopback [on\|off]` | SCSI loopback test card |
| `scc loopback [on\|off]` | SCC serial port loopback |
| `rom load <file>` | Load a ROM image |
| `rom checksum <file>` | Validate and print ROM checksum |
| `rom probe [<file>]` | Check if ROM is valid |
| `setup [--model M] [--ram N]` | Configure machine model |
| `schedule [max\|real\|hw]` | Show/set scheduler mode |
| `status` | Print `running` or `idle` |
| `events` | Show pending CPU event queue |

### System

| Command | Description |
|---------|-------------|
| `help` | List all available commands |
| `help <cmd>` | Show help for a specific command |
| `script <path>` | Execute a script file |
| `quit` | Shut down the emulator daemon |

## 5. Common Workflows

### Single-step the boot sequence

Every command response includes the current PC and disassembled instruction,
so just sending `step` repeatedly produces a trace:

```bash
# Start daemon
./build/headless/gs-headless --daemon --speed=max rom=tests/data/roms/SE30.rom &
sleep 2

# Step one instruction — output shows where PC is now
echo "step" | nc -w 2 localhost 6800
# output: $40800090  4ef9  JMP       $4083F61C

echo "step" | nc -w 2 localhost 6800
# output: $4083F61C  6000  BRA       *+$023A

# Step 10 instructions at once — shows final position
echo "step 10" | nc -w 2 localhost 6800
```

No need for separate `print $pc` or `disasm` calls after stepping.

### Run to a breakpoint

```bash
# Set breakpoint
echo "break 0x40802a14" | nc -w 2 localhost 6800

# Run (connection stays open until breakpoint hit)
echo "run" | nc -w 10 localhost 6800

# Check where we stopped
echo "info regs" | nc -w 2 localhost 6800
```

Or use `until` which sets a temporary breakpoint that auto-deletes:

```bash
echo "until 0x40802a14" | nc -w 10 localhost 6800
echo "info regs" | nc -w 2 localhost 6800
```

### Examine memory

```bash
# Read 32 bytes at address 0
echo "x 0 32" | nc -w 2 localhost 6800

# Disassemble 20 instructions from current PC
echo "disasm 20" | nc -w 2 localhost 6800

# Use register names and Mac globals as addresses
echo "x \$sp 64" | nc -w 2 localhost 6800
echo "disasm \$pc 20" | nc -w 2 localhost 6800
echo "x \$Mouse 16" | nc -w 2 localhost 6800
```

### Inspect Mac globals

```bash
echo "print \$MBState" | nc -w 2 localhost 6800
echo "info mac" | nc -w 2 localhost 6800
```

### FPU register inspection

```bash
echo "info fpregs" | nc -w 2 localhost 6800
echo "print \$fpcr" | nc -w 2 localhost 6800
```

### Instruction tracing to a file

```bash
echo "trace start /tmp/trace.log" | nc -w 2 localhost 6800
echo "run 10000" | nc -w 10 localhost 6800
echo "trace stop" | nc -w 2 localhost 6800
cat /tmp/trace.log
```

### Enable logging

```bash
echo "log cpu 5" | nc -w 2 localhost 6800
echo "log scsi 10" | nc -w 2 localhost 6800
```

## 6. Tips

- Always use `nc -w 2` (timeout) instead of `nc -q 2` for reliable connection handling.
  The `-w` flag sets a timeout for both connect and idle; `-q` only affects idle after EOF
  on stdin, which can cause `nc` to exit before the daemon finishes writing its response.
- The daemon handles one connection at a time, but supports multiple commands per
  connection (batch mode). Send multi-command batches for efficiency.
- During long-running `run` commands, the daemon emits a **heartbeat** line once
  per second: `# running... 54000000 instructions (+54000000 since start)`.
  This keeps `nc -w` connections alive and lets you monitor progress. Heartbeat
  lines start with `#` so they can be filtered with `grep -v '^#'` if needed.
  For very long runs, use a generous `nc -w` timeout (e.g., `-w 300`) or send
  `stop` from another terminal if needed.
- The daemon prints `READY` to stdout after initialization. Block-read for this
  instead of using `sleep` for more reliable startup.
- ROM files: `tests/data/roms/` contains available ROM images. Use `SE30.rom`
  for SE/30, `plus.rom` for Macintosh Plus.
- Port 6800 is the default. Use `--port=PORT` to change it if there's a conflict.
  Use `--kill` to automatically kill any existing daemon on the same port.
- Use `--speed=max` for debugging sessions — it avoids real-time delays and makes
  breakpoint-heavy workflows much faster.
- **Toggle commands** like `scc loopback` and `hd loopback` without arguments
  only query the current state — they do NOT enable the feature. Always pass `on`
  or `off` explicitly: `scc loopback on`.
- **Paths are relative to the daemon's CWD** (typically the repo root).
  Use absolute paths for reliability.
- **Output buffering**: When piping daemon output through filters, stdout is
  line-buffered in script mode. If output appears delayed, the daemon uses
  `setvbuf(stdout, NULL, _IOLBF, 0)` when running scripts.
- The `step` command works correctly at breakpoints — there is a `last_breakpoint_pc`
  skip mechanism that prevents re-triggering the breakpoint on the first step.
- `set` values are hex by default (Motorola convention): `set d0 42` sets D0 to
  $42 (66 decimal). Use `0d` prefix for decimal: `set d0 0d66`.
- `$`-prefix resolves both registers and Mac globals uniformly: `print $pc`,
  `x $Ticks 4`, `break $a5`.

## 7. Pitfalls

- **VIA timer interrupts don't fire during single-step.** The VIA timers are driven
  by the scheduler, which only advances during `run`. If you need timer-dependent
  code to execute, use `run <N>` instead of `step <N>`.
- **Don't forget to kill the daemon.** Send `echo "quit" | nc -w 2 localhost 6800`
  when done, or use `kill %1` if it was backgrounded. Orphaned daemons hold the port.
  PID files are written to `/tmp/gs-headless-<port>.pid` for cleanup.
- **Large step counts can be slow.** `step 100000` steps one instruction at a time
  with output for each. Use `run 100000` for faster bulk execution (only the final
  state is printed).
- **Response may be empty if daemon isn't ready.** If `nc` returns nothing, the
  daemon likely hasn't finished initialization. Wait for the `READY` signal or
  increase the startup `sleep` delay.
- **Assertions kill the daemon silently.** If the emulator hits an internal
  assertion, the daemon process terminates without sending an error to the TCP
  client. Check if the daemon is still running with `kill -0 <pid>`.

## 8. Boot Preamble Recipes

Common boot sequences for integration test scripts:

### SE/30 boot to desktop (with HD)
```
run 120000000
screenshot --match desktop.png
```

### SE/30 boot with floppy and HD
```
fd insert /path/to/system.dsk
run 800000000
screenshot --match desktop.png
```

### Script include for shared preambles
Scripts support `include` directives to avoid copy-pasting boot sequences:
```
include tests/integration/se30-mactest/boot-preamble.script
break $2B868
run
info regs
```

## 9. Cross-References

- **Offline disassembly**: See the `disasm-tool` skill for disassembling ROM images
  and binary files without running the emulator. Useful for static analysis of large
  code regions before setting breakpoints.
- **Logging & logpoints**: See docs/log.md for the full logging system reference.
  Use `log <category> <level>` in the shell to enable runtime logging. Use
  `log <category> <level> file=<path>` to redirect log output to a file.
- **Source code**: The headless platform lives in `src/platform/headless/`. The shell
  command framework is in `src/core/shell/`. Debug command handlers are in
  `src/core/debug/`.
