---
name: headless-debug
description: >
  Use when you need to interactively debug the emulator: single-stepping
  the CPU, setting breakpoints, examining registers or memory, or tracing
  the boot sequence via the headless daemon's TCP shell.
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
echo "<command>" | nc -q 1 localhost 6800
```

The daemon reads the command, executes it, writes all output to the socket,
and closes the connection when the command finishes (or the emulator stops).

### Capturing output

```bash
RESULT=$(echo "td" | nc -q 1 localhost 6800)
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
| `td` | Display all CPU registers (D0-D7, A0-A7) |
| `get pc` | Get program counter |
| `get sr` | Get status register |
| `get <reg>` | Get register value (d0-d7, a0-a7, pc, sr, ssp, usp) |
| `set <reg> <val>` | Set register or memory |
| `br <addr>` | Set breakpoint at address |
| `br del <addr>` | Delete breakpoint |
| `br del all` | Delete all breakpoints |
| `br` | List breakpoints |
| `x <addr> [nbytes]` | Examine memory in hex/ASCII (default 64 bytes, max 512) |
| `disasm <addr> [n]` | Disassemble n instructions at address |

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

```bash
# Start daemon
./build/headless/gs-headless --daemon rom=tests/data/roms/SE30.rom &
sleep 2

# Check initial state
echo "get pc" | nc -q 1 localhost 6800
echo "td" | nc -q 1 localhost 6800

# Step one instruction at a time
echo "s" | nc -q 1 localhost 6800
echo "get pc" | nc -q 1 localhost 6800

# Step 10 instructions
echo "s 10" | nc -q 1 localhost 6800
```

### Run to a breakpoint

```bash
# Set breakpoint
echo "br 0x40802a14" | nc -q 1 localhost 6800

# Run (connection stays open until breakpoint hit)
echo "run" | nc -q 5 localhost 6800

# Check where we stopped
echo "get pc" | nc -q 1 localhost 6800
echo "td" | nc -q 1 localhost 6800
```

### Examine memory

```bash
# Read 32 bytes at address 0
echo "x 0 32" | nc -q 1 localhost 6800

# Disassemble 20 instructions from current PC
PC=$(echo "get pc" | nc -q 1 localhost 6800 | sed 's/PC = //')
echo "disasm $PC 20" | nc -q 1 localhost 6800
```

### Enable logging

```bash
echo "log cpu 5" | nc -q 1 localhost 6800
echo "log scsi 10" | nc -q 1 localhost 6800
```

## 6. Tips

- Always use `nc -q 1` (or `nc -q 2` for stepping) to ensure `nc` exits after getting the response.
- The daemon handles one connection at a time. Wait for each command to complete before sending the next.
- For long-running commands (`run` without a limit), use `nc -q 5` or higher timeout, and send `stop` from another terminal if needed.
- ROM files: `tests/data/roms/` contains available ROM images. Use `SE30.rom` for SE/30, `plus.rom` for Macintosh Plus.
- Port 6800 is the default. Use `--port=PORT` to change it if there's a conflict.
