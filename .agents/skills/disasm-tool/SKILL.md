---
name: disasm-tool
description: >
  Standalone 68000/68030 disassembler for ROM images and binary files.
  Covers CLI usage, address calculation, output format, and common
  workflows for offline static analysis without the emulator.
triggers:
  - disassemble a file
  - disassemble ROM
  - disasm tool
  - offline disassembly
  - static disassembly
  - decode instructions
  - rom analysis
  - binary analysis
  - read ROM bytes
  - what's at address
  - hex dump
  - trap lookup
  - A-trap
  - explore ROM
---

# Granny Smith `disasm` Tool — Agent Skill

## Overview

`disasm` is a standalone command-line 68000/68030 disassembler that works on raw
binary files (ROM images, disk dumps, code extracts). It uses the same decoder as
the emulator's built-in disassembler but runs independently — no emulator or
daemon needed.

## 1. Building

```bash
cd /workspaces/granny-smith/tools/disasm
make
```

The binary is produced at `tools/disasm/disasm`.

To rebuild from scratch:

```bash
make clean && make
```

## 2. Location

```
tools/disasm/disasm          # the built executable
tools/disasm/disasm.c        # main source (CLI, annotation logic)
tools/disasm/trap_lookup.c   # A-trap name resolver
tools/disasm/stubs.c         # linker stubs (gs_assert_fail)
tools/disasm/platform.h      # minimal platform override (force-included)
tools/disasm/trap_lookup.h   # header for trap_lookup.c
tools/disasm/Makefile         # build rules
```

Core sources (`cpu_disasm.c`, `cpu_decode.h`, `mac_traps_data.c`) are compiled
directly from `src/` via `-I` include paths — not copied. A force-included
`platform.h` override provides the `PLATFORM_H` guard and minimal platform
stubs, following the same pattern as `tests/unit/support/platform.h`.

## 3. Command-Line Interface

```
./tools/disasm/disasm [OPTIONS] <input_file>
```

### Options

| Flag | Long form | Description | Default |
|------|-----------|-------------|---------|
| `-o` | `--offset <bytes>` | Starting byte offset in the input file (decimal or `0xHEX`) | `0` |
| `-l` | `--length <bytes>` | Number of bytes to disassemble from the offset | entire file from offset |
| `-a` | `--address-offset <addr>` | Base address added to displayed addresses (decimal or `0xHEX`) | `0` |
| `-n` | `--count <n>` | Maximum number of instructions to disassemble | unlimited |
| `-h` | `--help` | Show help message | — |

### Address Calculation

The displayed address for each instruction is:

```
displayed_address = address_offset + file_offset + position_within_buffer
```

For example, if a ROM is memory-mapped at `0x40800000` and you want to
disassemble from byte offset `0x2A14`:

```bash
./tools/disasm/disasm -a 0x40800000 -o 0x2A14 -n 10 rom.bin
```

The first instruction will be displayed at address `0x40802A14`.

## 4. Output Format

Each line uses the same format as the emulator's built-in disassembler:

```
<address>  <opcode>  <mnemonic>  <operands>  [; -> $<target>]
```

- **address**: 8-digit lowercase hex virtual address
- **opcode**: 4-digit lowercase hex first instruction word only (matching emulator)
- **mnemonic**: left-aligned in a 10-character field
- **operands**: instruction operands
- **branch annotation** (optional): for PC-relative branches (`Bcc`, `BRA`,
  `BSR`, `DBcc`, `FBcc`, `PBcc`), appends `; -> $HHHHHHHH` with the absolute
  destination address

The column layout matches the emulator's `debugger_disasm()` exactly:
`"%08x  %04x  %-10s<operands>"`.

### Example Output

```
40802a14  3e7c  MOVEA.W   #$2000,A7
40802a18  7e00  MOVEQ     #$00,D7
40802a1c  41fa  LEA       *-$218,A0
40802a3c  660a  BNE.S     *+$000C       ; -> $40802A48
40802a72  6600  BNE       *+$01CA       ; -> $40802C3C
```

## 5. Common Workflows

### Disassemble the beginning of a ROM

SE/30 ROM is mapped at `0x40800000`:

```bash
./tools/disasm/disasm -a 0x40800000 -n 50 tests/data/roms/SE30.rom
```

### Disassemble from a specific offset

The SE/30 entry point is at ROM offset `0x90`:

```bash
./tools/disasm/disasm -a 0x40800000 -o 0x90 -n 20 tests/data/roms/SE30.rom
```

### Disassemble a specific byte range

Show exactly 256 bytes starting at offset `0x3F61C`:

```bash
./tools/disasm/disasm -a 0x40800000 -o 0x3F61C -l 256 tests/data/roms/SE30.rom
```

### View a code region without address remapping

If you just want raw offsets (no base address):

```bash
./tools/disasm/disasm -o 0x90 -n 10 tests/data/roms/SE30.rom
```

Addresses will start from `0x00000090`.

### Disassemble a non-ROM binary file

Works on any raw 68K binary (disk sector dumps, code resources, etc.):

```bash
./tools/disasm/disasm -a 0x1000 mycode.bin
```

## 6. ROM Base Addresses

| Machine | ROM file | Base address |
|---------|----------|-------------|
| SE/30 | `tests/data/roms/SE30.rom` | `0x40800000` |
| Mac Plus | `tests/data/roms/Plus_v3.rom` | `0x00400000` |
| IIcx | `tests/data/roms/IIcx.rom` | `0x40800000` |

## 7. Tips

- The tool reads big-endian binary data natively — no byte-swapping needed for
  Motorola 68K ROM images.
- Offset and length values accept both decimal (`144`) and hex (`0x90`) formats.
- A-trap instructions (opcodes `0xA000`–`0xAFFF`) are automatically resolved to
  human-readable names (e.g., `_SwapMMUMode`, `_NewPtr`).
- PC-relative target annotations (`; -> $addr`) appear for all PC-relative operands
  including branch instructions (Bcc, BRA, BSR, DBcc) as well as LEA, PEA, JSR,
  and JMP that use PC-relative addressing modes.
- PMMU instructions (PMOVE, PFLUSH, PFLUSHA, PLOAD, PTEST) are fully decoded with
  register names (TC, SRP, CRP, TT0, TT1, MMUSR) and function code operands.
- A summary line at the end shows the total number of instructions and bytes
  disassembled.
- The output format is identical to the emulator's built-in disassembler
  (`debugger_disasm()`), so disasm tool output can be compared directly with
  headless emulator single-step traces.

## 8. Pitfalls

- The disassembler processes bytes linearly — it cannot distinguish code from data.
  If you disassemble a region containing inline data (e.g., lookup tables or string
  literals embedded between subroutines), those bytes will be decoded as (nonsense)
  instructions. Use `-n` or `-l` to limit output to known code regions.
- The `-a` (address offset) flag only affects displayed addresses and branch target
  annotations. It does not change which bytes are read from the file. Use `-o` to
  control file position.
- DBcc displacement zero (`*+$0000`) means the branch target is the DBcc instruction
  itself — a common self-loop pattern (e.g., `DBF D0,*+$0000` for a busy-wait).

## 9. Cross-References

- **Headless emulator debugging**: See the `headless-debug` skill for interactive
  debugging (single-step, breakpoints, memory examine) using the headless daemon.
  The daemon's `disasm` command uses the same disassembler engine as this tool.
- **Source code**: `src/core/cpu/cpu_disasm.c` is the disassembler core, included
  by both this tool and the emulator. `src/core/cpu/cpu_decode.h` is the shared
  instruction decoder template.
