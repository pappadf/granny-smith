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

**Important:** The emulator does NOT auto-run in daemon mode. It waits for commands.

### Example — SE/30 with ROM only

```bash
./build/headless/gs-headless --daemon rom=tests/data/roms/SE30.rom &
sleep 2   # wait for daemon to initialise
```

### Example — Macintosh Plus with hard disk

```bash
./build/headless/gs-headless --daemon rom=tests/data/roms/plus.rom hd=tests/data/hd/disk.img &
sleep 2
```

## 3. Sending Commands

Every command is a one-shot TCP transaction:

```bash
echo "<command>" | nc -w 2 localhost 6800
```

The daemon reads the command, executes it, writes all output to the socket,
and closes the connection when the command finishes (or the emulator stops).

**Every response ends with a status line** showing the disassembled instruction
at the current PC (like the web shell prompt). For example:

```
$ echo "s" | nc -w 2 localhost 6800
4083f61c  6000  BRA       *+$023A
```

This means the PC is now at `$4083F61C` and the next instruction to execute
is `BRA *+$023A`. This applies to all commands — `s`, `td`, `get pc`, `x`, etc.

All addresses use the Motorola `$` hex prefix (e.g., `$40800000`) with uppercase
hex digits. Input accepts `$`, `0x`, or bare hex. Use `L:` or `P:` prefix for
explicit logical/physical addressing (e.g., `br P:$00801656`).

### Capturing output

```bash
RESULT=$(echo "td" | nc -w 2 localhost 6800)
echo "$RESULT"
```

## 4. Command Reference

### Execution Control

| Command | Description |
|---------|-------------|
| `run` | Start execution (keep connection open until stopped) |
| `run <N>` | Run exactly N instructions then stop |
| `s` | Single-step one instruction |
| `s <N>` | Step N instructions |
| `stop` | Stop execution |
| `status` | Return `1` if running, `0` if idle |

### Debugger

| Command | Description |
|---------|-------------|
| `td` | Display all CPU registers (D0-D7, A0-A7) with CPU/MMU header |
| `get pc` | Get program counter |
| `get sr` | Get status register |
| `get mmu` | Dump MMU register state (TC, CRP, SRP, TT0, TT1) |
| `get <reg>` | Get register value (d0-d7, a0-a7, pc, sr, ssp, usp) |
| `set <reg> <val>` | Set register or memory |
| `br <addr>` | Set breakpoint at address (supports L:/P: prefix) |
| `br del <addr>` | Delete breakpoint |
| `br del all` | Delete all breakpoints |
| `br` | List breakpoints |
| `x <addr> [nbytes]` | Examine memory in hex/ASCII (default 64 bytes, max 512) |
| `disasm <addr> [n]` | Disassemble n instructions at address |
| `addrmode [mode]` | Set address display: auto, expanded, collapsed |
| `translate <addr>` | Show MMU translation for an address |

### Machine Info

| Command | Description |
|---------|-------------|
| `machine` | Show current machine model and specs |
| `schedule` | Show/set scheduler mode (max, real, hw) |
| `events` | Show pending CPU event queue |

### Disk & ROM

| Command | Description |
|---------|-------------|
| `load-rom <file>` | Load a ROM image |
| `attach-hd <file> [id]` | Attach SCSI hard disk |
| `insert-fd <file>` | Insert floppy disk image |

### Logging

| Command | Description |
|---------|-------------|
| `log <category> <level>` | Enable logging for a category |
| `logpoint <addr> [msg]` | Set a logpoint at an address |
| `trace start [file]` | Start instruction tracing |
| `trace stop` | Stop tracing |

### System

| Command | Description |
|---------|-------------|
| `help` | List all available commands |
| `quit` | Shut down the emulator daemon |

## 5. Common Workflows

### Single-step the boot sequence

Every command response includes the current PC and disassembled instruction,
so just sending `s` repeatedly produces a trace:

```bash
# Start daemon
./build/headless/gs-headless --daemon rom=tests/data/roms/SE30.rom &
sleep 2

# Step one instruction — output shows where PC is now
echo "s" | nc -w 2 localhost 6800
# output: $40800090  4ef9  JMP       $4083F61C

echo "s" | nc -w 2 localhost 6800
# output: $4083F61C  6000  BRA       *+$023A

echo "s" | nc -w 2 localhost 6800
# output: $4083F858  46fc  MOVE      #$2700,SR

# Step 10 instructions at once — shows final position
echo "s 10" | nc -w 2 localhost 6800
```

No need for separate `get pc` or `disasm` calls after stepping.

### Run to a breakpoint

```bash
# Set breakpoint
echo "br 0x40802a14" | nc -w 2 localhost 6800

# Run (connection stays open until breakpoint hit)
echo "run" | nc -w 10 localhost 6800

# Check where we stopped
echo "get pc" | nc -w 2 localhost 6800
echo "td" | nc -w 2 localhost 6800
```

### Examine memory

```bash
# Read 32 bytes at address 0
echo "x 0 32" | nc -w 2 localhost 6800

# Disassemble 20 instructions from current PC
PC=$(echo "get pc" | nc -w 2 localhost 6800 | sed 's/PC = //')
echo "disasm $PC 20" | nc -w 2 localhost 6800
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
- The daemon handles one connection at a time. Wait for each command to complete
  before sending the next.
- For long-running commands (`run` without a limit), use `nc -w 10` or higher
  timeout, and send `stop` from another terminal if needed.
- Always `sleep 2` (or `sleep 3-4` for larger ROMs) after starting the daemon
  before sending the first command. The daemon needs time to initialize.
- ROM files: `tests/data/roms/` contains available ROM images. Use `SE30.rom`
  for SE/30, `plus.rom` for Macintosh Plus.
- Port 6800 is the default. Use `--port=PORT` to change it if there's a conflict.
- The memory examine command is `x <addr> [nbytes]`, not `read` or `mem`.

## 7. Pitfalls

- **VIA timer interrupts don't fire during single-step.** The VIA timers are driven
  by the scheduler, which only advances during `run`. If you need timer-dependent
  code to execute, use `run <N>` instead of `s <N>`.
- **Don't forget to kill the daemon.** Send `echo "quit" | nc -w 2 localhost 6800`
  when done, or use `kill %1` if it was backgrounded. Orphaned daemons hold the port.
- **Large step counts can be slow.** `s 100000` steps one instruction at a time
  with output for each. Use `run 100000` for faster bulk execution (only the final
  state is printed).
- **Response may be empty if daemon isn't ready.** If `nc` returns nothing, the
  daemon likely hasn't finished initialization. Increase the startup `sleep` delay.

## 8. Cross-References

- **Offline disassembly**: See the `disasm-tool` skill for disassembling ROM images
  and binary files without running the emulator. Useful for static analysis of large
  code regions before setting breakpoints.
- **Logging & logpoints**: See docs/log.md for the full logging system reference.
  Use `log <category> <level>` in the shell to enable runtime logging.
- **Source code**: The headless platform lives in `src/platform/headless/`. The shell
  command handler is in `src/core/shell/`.
