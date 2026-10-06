# New Age — the AV floppy controller

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, division of labor, Apple mode and
   the pin strapping, clocking
2. [Register file](#2-register-file) — the $50F2A000 window and its three decoded registers: STR/DRR at
   $101, the data register at $141, the four status bytes, reset state
3. [Behaviour](#3-behaviour) — command/execution/result phases, the Apple-mode command set, interrupts and
   the /CSTIN polling loop, DMA and terminal count, GCR recording, MFM recording, Raw Dump, mechanical timing
4. [Programming model](#4-programming-model) — the sequences the shipped driver actually performs: bring-up,
   drive probe, media identification, transfers, interrupt service, the OS-visible surface
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

**New Age** is the floppy disk controller of the AV Quadra platform: "a floppy disk controller in a 64-pin
CMOS chip" which "controls an Apple SuperDrive for all its recording densities", "uses a command set
compatible with MFM and Apple GCR formats", "generates an interrupt on disk insertion", "performs 16-byte
first-in, first-out data buffering", and "supports full asynchronous operation for DMA" [2] §"New Age"
p. 15. Apple's summary of the platform change is one sentence: "A new controller for the built-in Apple
SuperDrive disk drive is based upon Industry Standard 765, supporting both Apple's Group Code Recording
(GCR) format and DOS-compatible Modified Frequency Modulation (MFM) format" [2] §"Summary of Features"
p. 5.

The part is the **NEC µPD72070**, and the chip's own specification names it: "The 72070 Advanced Floppy
Disk Controller or **'New Age FDC'**" [1] §1.2 p. 2 — an Apple-confidential document that marks the
Apple-specific command additions "These should be disclosed only to Apple Computer" [1] §1.2 p. 3. By its
own feature list the chip is a "functional superset of µPD765A and Intel 82077" that "supports Apple GCR
format" and "supports 1MB, 2MB, 4MB and 13MB FDDs", with an 8-bit host bus, a 16-byte FIFO in the data
register, and the IBM PS/2 and PC/AT register sets for its standard host interfaces [1] §1.1–1.2 pp. 1–3.
For the standard drives it "is compatible with the NEC µPD765A and therefore maintaining compatibility with
all µPD765A existing software and copy protection schemes" [1] §1.1 p. 1.

One consequence shapes everything below: **nothing in Apple's driver is custom silicon.** Every command
the shipped driver issues is a documented µPD72070 command, and the chip's specification fully specifies
Apple GCR — track and sector layout, nibblize tables, sync groups, tag bytes and the Apple drive-interface
handshakes [1] §1.3.1 pp. 4–13. The meaningful axis is not "stock part vs Apple part" but **Apple mode vs
Standard modes**, the host interface selected by two pins (§1.4).

New Age is also a break in the Macintosh floppy lineage. Every controller from the
[IWM](../../hardware/iwm-floppy.md) onward — [SWIM](../../hardware/swim.md), and the PDM family's
[SWIM3](../pdm/swim3.md) — descends from the Apple-designed IWM state machine: register-addressed
softswitches, phase lines, and data streaming under host control. New Age is a vendor part built around
the 765 architecture: a programmed command/result protocol with a command processor of its own (§2, §3.1).
To the driver it presents no phases to wiggle and no softswitches — only a status register, a data
register, and a data-rate latch.

### 1.2 Machines that carry it

| Machine | Apple codename | FDC | Register window | DMA | Device interrupt |
|---|---|---|---|---|---|
| Macintosh Quadra 840AV | Cyclone | New Age (µPD72070) | $50F2A000 | PSC channel 3 | VIA2 window bit 5, level 2 |
| Macintosh Centris 660AV | Tempest | New Age (µPD72070) | $50F2A000 | PSC channel 3 | VIA2 window bit 5, level 2 |

Both models carry "one internal Apple SuperDrive floppy disk drive" — "capable of accepting 1.4 MB
floppy disks" — on a 20-pin internal connector [2] §"Storage and Input Devices" p. 9,
§"Floppy Disk Drive Connection" p. 28. One drive ships in either machine, but the Apple drive interface
supports exactly two FDDs (two drive-enable pins, §3.8), and the driver probes both drive numbers at
open (§4.2).

The register window's base is a constant of the platform but is carried in the machine's
decoder-information record, slot 30, with a per-device existence bit; the start-up code tests that bit
and opens the driver only when it is set [3] (*observed* in the boot ROM's start-up path and decoder
tables).

### 1.3 Division of labor

New Age does not stand alone: it is a device on the [PSC](psc.md)'s I/O bus, its data moves through a
PSC DMA channel, and its interrupt arrives through the PSC's pseudo-VIA2 window. The split:

| Function | Owner |
|---|---|
| Command/status/data window at $50F2A000; GCR/MFM encode-decode; the drive command protocol; mark search, format, eject, disk-insert detection | New Age (§2, §3) |
| Sector-data transfers to and from RAM | PSC DMA channel 3 — 8 bits wide, 4-byte buffer [2] Table 2-10 p. 29; see [PSC](psc.md) §2.5–2.6 and the arm sequence at §4.4 |
| Terminal count to the FDC on the last byte of a transfer | the PSC channel's terminal-count completion (§3.4; [PSC](psc.md) §4.4) |
| The FDC device interrupt | PSC VIA2 window bit 5, level 2, enabled with `$A0` and disabled with `$20` ([PSC](psc.md) §2.8) |
| I/O-bus and CPU-bus arbitration | the PSC — the FDC channel is highest priority on the I/O bus and third for the CPU bus [2] Table 2-11 p. 29 |
| Address decode of the window | the PSC, as decoder of the whole I/O region [2] p. 29 ([PSC](psc.md) §1.1) |
| The drive itself | the Apple SuperDrive, a variable-speed drive of the lineage the original Apple 3.5-inch drive specification defines [4] §2.3.4 |

Note what the dev note's clock table implies for wiring: the PSC's C16M output (15.6672 MHz) is listed
against "New Age, Curio" [2] Table 2-2 p. 17 — the controller's data-side clock is *distributed by the
PSC*, not generated on the spot (§1.5).

### 1.4 Apple mode: a pin-strapped host interface

The µPD72070 has four host personalities, selected not by software but by two pins:

| PCTYP1,PCTYP0 | Mode |
|---|---|
| 1,1 | **Apple mode** — which also selects active-low reset [1] §1.4 p. 16 |
| 1,0 | General |
| 0,0 | PS/2 |
| 0,1 | PC/AT |

[1] §1.4 p. 16, pin table p. 24. The AV boards must strap %11 (*inferred — unverified*: the shipped
software's contract — three decoded registers, §2.1; standard-mode commands rejected, §3.2; the Apple
drive commands issued constantly — requires Apple mode; no board-level document states the strap).

Apple mode changes three things that matter to a re-implementation:

1. **Only three register addresses decode** (§2.1) — the standard DOR/TDR/SRA/SRB/DIR/CCR registers of the
   PS/2 and PC/AT personalities do not exist here.
2. **The command set is partitioned.** Apple-mode-only: Format/Write, Disable/Enable DPLL, Eject Disk,
   Set Drive Mode, Set Motor Control, Set Enable Control, Raw Dump [1] §1.4.1 pp. 19–22. Illegal in Apple
   mode (they answer invalid command, ST0 = $80): Read Deleted Data, Write Deleted Data, Scan Equal, Scan
   Low or Equal, Scan High or Equal, Verify, Relative Seek, Dumpreg, Version [1] §1.4.1 p. 16. Available
   in all modes: Configure, Specify, Seek, Recalibrate, Sense Interrupt Status, Sense Drive Status,
   Read/Write Data, Read ID, Read A Track, Format A Track, Perpendicular Mode, Select Drive Type,
   Revision [1] §1.4.1 p. 16, p. 97.
3. **The drive interface is the Apple one** — two drive-enable pins and the CA0-2/LSTRB serial command
   protocol of the Apple drives (§3.8), instead of the four-drive DS0-DS3 pin set.

A distinction that earlier Apple material got wrong: the **Select Drive Type** command ($32) does *not*
enable Apple mode — mode is pin-strapped. It selects the Apple *drive interface* (and the drive-class
model the chip assumes), an independent axis; the datasheet's own configuration table shows Apple mode
combined with the conventional drive type for 4 MB MFM perpendicular recording [1] Table 1.4.1.2 p. 18
(§4.1).

### 1.5 Clocking

The chip takes two clocks [1] §2.1 p. 23, §7 p. 162:

| Clock | Frequency | Purpose |
|---|---|---|
| XA | 24 MHz ±0.5 % | the main clock; 10 ms stabilisation after power-up |
| XB | 15.6672 MHz | the Apple (GCR) drive's data clock; 20 MHz for the 13 MB FDD; grounded when unsupplied |

On the AV boards the 15.6672 MHz is the PSC's C16M output [2] Table 2-2 p. 17 — the clock the chip's data
side runs on is synthesized in the PSC and bused to the controller. The chip's internal clock period is
0.125 µs (an 8 MHz tick), the term that appears in the FIFO service-delay formula (§3.4) [1] §5.1.2
p. 190.

The XB requirement is also what kills the 13 MB drive on this platform: the silicon fully supports the
2TD 13 MB FDD (its own data rate, precompensation column and command rows exist), but that drive needs
XB = 20 MHz and is MFM-only [1] §2.1 p. 23, §1.3.3 — an AV board supplies neither, so the 13 MB
capability is unreachable regardless of software (§6).

## 2. Register file

### 2.1 The address window and its decode

All software-visible registers live behind the fixed base **$50F2A000**. Under Apple mode only three
register addresses are decoded [1] Table 3.1 p. 31:

| A2 A1 A0 | Access | Register |
|---|---|---|
| 1 0 0 | write | Data Rate Register (DRR) |
| 1 0 0 | read | Status Register (STR, the "MSR") |
| 1 0 1 | read/write | Data Register (the FIFO) |

The full chip has more — DOR, TDR, SRA, SRB, DIR, CCR, "to be compatible with the INTEL 82077" — but
none of them is decoded in Apple mode, so the three-register model is complete and correct here; the TDR
"is not used in this FDC" even in PC/AT mode [1] §3.1 p. 31, p. 41. Apple's board decode maps A0 to bit 6
of the byte offset with A2 hardwired to 1, giving the addresses the shipped driver uses as literals
(*observed* throughout the ROM's driver):

| Address | Direction | Register |
|---|---|---|
| $50F2A101 | read | STR |
| $50F2A101 | write | DRR |
| $50F2A141 | read/write | data register |

Byte accesses, at odd addresses. A1 is never 1 in Apple mode, so no other offset answers; what a
backing bus does at the unlisted offsets inside the window (fault, float, alias) is not established
(§6).

### 2.2 Status register (STR, read $50F2A101)

The master status register of the command protocol [1] §3.1.1 p. 32:

| Bit | Symbol | Apple-mode meaning |
|---|---|---|
| 7 | RQM | request for master — a byte may be transferred between host and FDC |
| 6 | DIO | direction — 1 = FDC to host (result phase), 0 = host to FDC (command phase) |
| 5 | EXM | execution phase, non-DMA — data requested from the host |
| 4 | CB | command busy — a command byte has been received and the result phase is not finished |
| 3 | D1I | **drive 1 installed** (standard modes: FDD3 busy) |
| 2 | D0I | **drive 0 installed** (standard modes: FDD2 busy) |
| 1 | D1B | drive 1 seek in progress, **or a seek-termination interrupt still pending** |
| 0 | D0B | drive 0 likewise |

"Read/Write commands must not be issued when [D0B/D1B] is active" [1] §3.1.2 p. 33. The D*I bits are set
by an explicit drive-install check the chip's firmware performs at initialization — not sampled
continuously (§3.3); the drive-side signal feeding them is not named by the specification (/DRVIN is the
only candidate, §6).

### 2.3 Data Rate Register (DRR, write $50F2A101)

The DRR is the configuration latch written before each media class is exercised [1] §3.1.3 pp. 34–35:

| Bits | Field | Meaning |
|---|---|---|
| 7 | S/W RST | software reset — **self-clearing**: "this bit is automatically reset by itself"; never model it as a latch |
| 6 | STDBY | power-down; "all circuits are turned off"; leaves standby on reset or on any register read/write; 100 µA standby current |
| 4:2 | PCS2:0 | write precompensation select, below |
| 1:0 | DRATE | 00 = 500/489.6 kbps · 01 = 300 · 10 = 250 (**reset default**) · 11 = 1000 kbps (2ED) or 1250 kbps (2TD) |

(Bit 5 is not defined in the register description [1] §3.1.3 p. 34.) The precompensation table, with
the 13 MB column for completeness [1] Table 3.1.8.1 p. 34:

| PCS2:0 | 2ED/2HD/2DD (ns) | 2TD, 13 MB (ns) |
|---|---|---|
| 000 | reset default | reset default |
| 001 | 41.7 | 50.0 |
| 010 | 83.3 | 100.0 |
| **011** | **125.0 — annotated "Apple's MFM value"** | **150.0 — "Apple's MFM value"** |
| 100 | 166.7 | 200.0 |
| 101 | 208.3 | not used |
| 110 | 250.0 | not used |
| 111 | 0.0 | 0.0 |

Two Apple-mode rules override the tables [1] §3.1.3 p. 34, §3.8 p. 47:

- **"In Apple Mode while recording in GCR, the precompensation value is always set to zero, independent
  of PCS2-PCS0"** — the precompensator "is always disabled" under GCR. The PCS bits only matter for MFM.
- **"For data rates under Apple mode, these bits should be set as (0,0) or (1,1)"** — the 300 and
  250 kbps settings are not legal Apple-mode choices (but see §6 on the one table row that contradicts
  this).

The reset default of the register is 250 kbps with 125 ns precompensation [1] §3.1.3 p. 35 — which is
why the driver must rewrite the DRR after every reset (§4.1). The values the shipped driver writes
(*observed*): **$9C** as the software-reset write (S/W RST, PCS %111, 500 kbps — the reset itself is what
matters, the rest of the byte is reprogrammed immediately after), **$00** before GCR operation,
**$0C** before MFM DD/HD (PCS %011, 500 kbps), **$0F** before MFM ED (PCS %011, 1000 kbps).

### 2.4 The data register ($50F2A141) and the FIFO

The data register is the byte-wide port of a **16-byte FIFO** [1] §1.2 p. 2. Its discipline during
command and result phases is strict (§3.1): the FIFO is disabled in both, so command and result bytes
move one at a time, and "after receiving all parameters, the FDC clears any FIFO data" — a flush at the
command-to-execution boundary [1] §5.1.1 pp. 121–123. Only the execution phase streams through the FIFO,
under the threshold rules of §3.4. After reset the FIFO is **disabled, depth 1** [1] §5.1.1 p. 121 —
the Configure command re-enables it (§4.1).

### 2.5 The status bytes ST0–ST3

Four status bytes come back through the data register in result phases. **ST0** [1] §3.1.9 pp. 42–43:

| Bits | Symbol | Apple-mode meaning |
|---|---|---|
| 7:6 | IC | %00 normal · %01 abnormal termination · %10 invalid command · **%11 "/CSTIN state change, floppy media inserted or removed"** |
| 5 | SE | Seek or Recalibrate terminated (normally or abnormally) |
| 4 | EC | "set when errors at the FDD occur" — a generic drive/handshake failure, broader than the 765's TRK0 meaning |
| 3 | NR | reflects /Ready — set = drive not ready |
| 2 | HD | head address at the time of the interrupt; **set to zero when Sense Interrupt Status executes** |
| 1 | FIN | reflects /CSTIN — 0 = disk present |
| 0 | DR | the drive number from the command issued |

The IC = %11 code is the 82077-style "ready changed" code repurposed for /CSTIN, not a Mac invention
[1] §3.1.9 p. 42. **Invalid command sets ST0 = $80 exactly** — the interrupt code %10 and every other
bit zero — with two triggers: an undefined opcode, and Sense Interrupt Status issued with no interrupt
cause pending [1] §3.3.2 p. 57, §5.1.7 p. 188.

**ST1** [1] §3.1.10 p. 44: bit 7 EN (end of cylinder), bit 6 always 0, bit 5 DE (**Apple mode: checksum
error in the ID or data field**; ST2's DD bit says which), bit 4 OR (overrun), bit 3 always 0, bit 2 ND
(no data — including the Read A Track interleave case, §3.2), bit 1 NW (no write), bit 0 MA (missing
address mark). **ST2** [1] §3.1.11 p. 45: bit 7 always 0, bit 6 CM (control mark), bit 5 DD (data-field
checksum, i.e. the ID/data discriminator for ST1 DE), bit 4 NC (cylinder mismatched and not $FF), bit 3
SH, bit 2 SN (scan bits — never set in Apple mode, the scan commands being illegal), bit 1 BC (cylinder
byte $FF), bit 0 MD (missing data-field mark).

**ST3**, returned by Sense Drive Status, is not a controller status at all: **every bit is an active-low
pass-through of a drive line** [1] §3.1.12 p. 45, §4.4.6 p. 105:

| Bit | Meaning (active low) |
|---|---|
| 7 | /2MB or /4MB media — **set (1) = neither, i.e. low-density media** |
| 6 | /Write protect — **0 = write protected** |
| 5 | /Ready |
| 4 | /TK0 |
| 3 | 2MB-4MB **drive** class (capability; this one reads in the positive sense) |
| 2 | /Mode ID |
| 1 | /Select Media |
| 0 | /MFM mode — lets the host verify a Set Drive Mode took effect |

`ST3 = $FF` therefore means **no drive** — every open-drain line floating high — and the driver uses
exactly that as its drive-absent test (§4.2) (*observed*). The active-low wiring is why the driver's
polarities read inverted: a *clear* bit 6 is write protection, a *set* bit 7 is low-density media.

### 2.6 Reset state

Hardware reset "sets FDC to idle state"; the pulse must be held ≤170 tCYA (~7.1 µs at 24 MHz), and Apple
mode uses **active-low reset** [1] §1.4 p. 16, §2.2 p. 23, §7 p. 162. Pin state after reset: D0–D7 become
inputs; **DMARQ and INT drive low** in Apple mode (high-impedance only in the PC/AT personality); all
FDD outputs go high-impedance [1] §2.5 p. 28. Register defaults [1] §3.1.3 p. 35, §3.3.1 p. 50,
§5.1.1 p. 121, §5.1.7 p. 188:

| Item | Reset value |
|---|---|
| DRR | 250 kbps data rate, 125 ns precompensation |
| FIFO | disabled, depth 1 |
| PRETRK | 0 |
| Select Drive Type | 00 — conventional FDD model |

The defaults are why the driver's reset sequence (§4.1) must re-issue Select Drive Type, Configure and
Specify after every software reset: pulsing DRR bit 7 undoes all three.

## 3. Behaviour

### 3.1 Command, execution and result phases

Every command runs the same three-phase protocol, coordinated entirely by STR [1] §5.1.1 pp. 121–123:

- **Command phase** — DIO = 0, and RQM must be 1 before each byte is written; "writing a byte resets
  RQM", which the chip sets again once it has absorbed the byte. **"Upon receiving the last parameter, the
  FDC enters the execution phase without setting the RQM bit"** — the final command byte (the "launch
  byte", §4.4) leaves RQM low, so a host that waits for RQM after it would deadlock. This is why the
  driver masks the FDC interrupt around the launch byte and switches to result polling (*observed*).
- **Execution phase** — the data transfer. In DMA mode the FIFO streams under the threshold rules
  (§3.4); in non-DMA mode the chip interrupts and sets RQM/EXM per byte. The FIFO is flushed at the
  boundary when the last parameter is received (§2.4).
- **Result phase** — DIO = 1, RQM per result byte. Each read "resets RQM and DIO" and **deasserts INT**;
  after the last byte "the FDC sets dummy data, then sets the RQM bit and waits for the next command",
  ending at RQM = 1, DIO = 0, CB = 0 [1] §5.1.1 p. 123 — the state in which a *short* result can be
  detected by CB alone (*observed* in the driver's one-byte-vs-two-byte Sense Interrupt Status handling,
  §3.3).

"The FIFO is disabled during the command phase to retain compatibility with the µPD765, and to provide
for proper handling of the 'Invalid Command' condition", and likewise during the result phase [1]
§5.1.1 pp. 121–122 — command and result bytes are strictly one at a time, whatever the Configure
threshold. An invalid opcode skips execution entirely and goes straight to a one-byte result phase with
ST0 = $80 (§2.5).

### 3.2 The command set

The opcode byte carries three fields [1] §3.3 p. 48: **bit 7 = MT** (multi-track; Apple's driver builds
with MT = 0, so the MT variants of the data commands never ship), **bit 6 selects the recording format —
set = MFM, clear = GCR**, and **bit 5 = TB** (tag bytes, in Apple mode; the same bit is SK in standard
modes). Bit 7 additionally serves as the on/off selector for the Apple drive commands ($9A on / $1A off,
and likewise $9B/$1B, $8B/$0B). The drive field of the data/seek commands is two bits (DR1 DR0, drives
0–3), but Apple mode has only two drive-enable pins, so only DR = 0/1 are usable [1] §5.1.7 p. 188,
pin table p. 25. The head bit is normal polarity (1 = side 1) at the command level; the pin to the drive
is internally inverted ("SIDE 0 → Head 1, 1 → Head 0") [1] §2.3 p. 25, §3.3 p. 48.

| Command | Opcode GCR / MFM | Command bytes | Result bytes | Notes |
|---|---|---|---|---|
| Configure | $13 | 4 | — | $13, $00, $0F, $00 as issued (§4.1) |
| Specify | $03 | 3 | — | only the ND bit matters in Apple mode (§3.2.1) |
| Select Drive Type | $32 | 2 | — | echo-back second byte; no result, no interrupt (§4.1) |
| Perpendicular Mode | $12 | 2 | — | echo-back; no result, no interrupt (§3.6) |
| Recalibrate | $07 | 2 | — | 3-phase Apple algorithm (§3.8) |
| Seek | $0F | 3 | — | fully handshaked; no programmed step rate (§3.8) |
| Sense Interrupt Status | $08 | 1 | **1 or 2** | variable length (§3.3) |
| Sense Drive Status | $04 | 2 | 1 (ST3) | **does not interrupt** (§3.3) |
| Read ID | $0A / $4A | 2 | 7 | byte 7 = the format byte (§4.3) |
| Read Data | $06 / $46 | 9 | 7 | DTL byte launches execution |
| Write Data | $05 / $45 | 9 | 7 | ditto |
| Read A Track | $02 / $42 | 9 | 7 | unusable on interleaved media (§3.2.2) |
| Format A Track | $0D / $4D | **6** | 7 | the filler byte launches execution (§3.2.3) |
| Format/Write | $01 / $41 | **5** | 7 | the sync-group/GAP3 byte launches execution (§3.2.3) |
| Raw Dump | $1E / $5E | 8 | 7 | returns two bus bytes per disk byte (§3.7) |
| Set Enable Control | $1B off / $9B on | 2 | — | always normal-termination interrupt (§3.3) |
| Set Motor Control | $1A off / $9A on | 2 | — | /Ready handshake, ≤1 s (§3.8) |
| Set Drive Mode | $1C GCR / $5C MFM | 2 | — | /Ready handshake, ≤800 ms (§3.8) |
| Eject Disk | $52 | 2 | — | two interrupts (§3.3) |
| Disable/Enable DPLL | $0B / $8B | 2 | — | drive-side DPLL; development only (§3.2.4) |
| Revision | $20 | 1 | 2 | firmware revision, hardware revision [1] §4.4.5 p. 97 |

Illegal in Apple mode and answered with ST0 = $80: Read Deleted Data ($4C), Write Deleted Data ($49),
Verify ($76), Scan Equal/Low or Equal/High or Equal ($51/$59/$5D), Relative Seek ($8F), Dumpreg ($0E),
Version ($10 — which otherwise always returns $90) [1] §1.4.1 p. 16, §4.4.12 p. 120, §4.4.13 p. 52.

#### 3.2.1 Specify

In Apple mode "it is not necessary to specify the Step Rate, because the STEP command is fully
handshaked with the FDD"; head load and unload times are likewise "unnecessary" [1] §4.4.10 pp. 113,
115. Only the ND bit (bit 0 of the final byte) is meaningful: ND = 0 selects DMA execution, ND = 1
selects non-DMA, where the chip drives INT and RQM with the FIFO-threshold arithmetic instead of DMARQ
[1] §5.1.2.3 p. 191. The driver runs DMA and switches to ND = 1 only for the raw track-dump paths
(*observed*).

#### 3.2.2 Read/Write Data

Nine command bytes: opcode, `(head << 2) | drive`, C, H, R, N-or-format-byte, EOT, **GSL**, DTL = 0. The
GSL byte (gap skip length) is **$1B for every 512-byte MFM format** per the datasheet's parameter table,
and "don't care" in GCR [1] Table 4.8.1 p. 90. The other parameters the same table pins: MFM N = $02,
EOT = $09 (720 KB) / $12 (1.44 MB) / $24 (2.88 MB); GCR N "should be set as 02H, 22H, or 24H" with EOT
"from 8H to CH" [1] p. 90. ($12 is a legal GCR *format* byte but not in the Read/Write set — see
§5.) The requested transfer length is the DTL byte, written 0 by Apple, and it doubles as the launch
byte (§3.1).

On normal termination the chip updates the ID bytes it reports in the result, and a host's resume logic
steps with them — the update table, which a re-implementation must reproduce [1] Table 4.8.1 p. 91:

| MT | Head | Last sector | C | H | R | N |
|---|---|---|---|---|---|---|
| 0 | 0 | < EOT | NC | NC | R+1 | NC |
| 0 | 0 | = EOT | C+1 | NC | 1 | NC |
| 0 | 1 | < EOT | NC | NC | R+1 | NC |
| 0 | 1 | = EOT | C+1 | NC | 1 | NC |
| 1 | 0 | < EOT | NC | NC | R+1 | NC |
| 1 | 0 | = EOT | NC | 1 | 1 | NC |
| 1 | 1 | < EOT | NC | NC | R+1 | NC |
| 1 | 1 | = EOT | C+1 | 0 | 1 | NC |

(NC = no change. The table is written for 1-based MFM sector numbers; in GCR, whose sectors are 0-based,
the wrap value is 0.)

The shipped driver is at odds with the C+1 rows. Its result-collection routine stores the result's C
byte as the head's current cylinder, and its seek routine skips the seek when the target equals that
value; every whole-track read it issues ends at EOT with MT = 0 (*observed*). Under the table, each such
read would leave the driver one cylinder out and send the next transfer to the wrong cylinder, to be
recovered only by retries and a recalibrate. Since the driver works on the machine, the silicon most
likely reports the cylinder it is on at EOT in Apple mode (*inferred — unverified*; §6).

With MT set, Apple-mode Read Data "will continue with data on the other side of the disk, for the
specified cylinder only" [1] §4.8.1 p. 89. On Write Data with terminal count asserted mid-field, "the
FDC will fill the remainder of the data field with zeros" [1] §4.8.6 p. 116. Address/mark search
timeouts under Apple mode are **400 ms** (GCR and the general Apple-mode statement), against two index
pulses in the standard modes [1] §4.8 pp. 92, 117.

**Read A Track** — $02/$42 — is defined but useless to Apple: "this command can only read the media with
the sector number in physically sequential order... in the case of interleaved media (like 1, 9, 2, 10,
...) the FDC can not read the sectors sequentially, but will read the sectors as they encounter, which
will set ND" [1] §5.1.7 p. 186. Every format the driver supports is interleaved (GCR 2:1, GCR 4:1
ProDOS, MFM 1–9), so the command is defined by the driver but never issued; multi-sector Read Data with
a split at the requested-sector boundary is used instead (*observed*). Its ninth command byte is the
number of sectors to be read, echoed as the residual count in result byte 6 on failure [1] §4.8.4
pp. 86–87.

#### 3.2.3 Format A Track and Format/Write

**Format A Track** takes **six** command bytes: opcode, head/drive, bytes-per-sector-or-format-byte,
sectors-per-track, GAP3 (MFM) or number-of-sync-groups (GCR), and a **filler byte** — $F6 for MFM, $00
for GCR — which is the launch byte: the driver arms its DMA channel first and writes the filler with the
FDC interrupt masked, and that write starts the execution phase [1] §4.7 p. 54 (*observed*). The
execution phase consumes **four DMA requests per sector** — the cylinder, head, sector and byte-count
entries of the interleave table [1] §4.7 p. 54. GAP3 "should be set less than 128 bytes" [1] p. 54.
"For MFM or FM recording, data is written on the disk after the index hole is detected. But for GCR
recording, the write can begin anywhere on the track" [1] p. 54. Both format commands return **7
result bytes** (ST0, ST1, ST2 and four reported ID values).

**Format/Write** ($01/$41, Apple-mode only) takes **five** bytes — no filler is specified; "the host
transfers the data as in the Write Data command" [1] §4.7.2 p. 56 — so there the sync-group/GAP3 byte
launches execution (*observed*).

The GCR format byte is $12, $22 or $24, and "all these hexadecimal values represent 512 bytes of data
and 12 Tag bytes" [1] §4.7 p. 54 — the $12-vs-$02 aliasing is a quirk (§5).

#### 3.2.4 Disable/Enable DPLL

"Under Apple mode only, the effect of this command is to set the /DPLL command bit in the FDD. This
disables the integrated Digital Phase Lock Loop (DPLL) **in the FDD**... The FDC will not handshake this
command", and "this command is used only in a development environment" [1] §4.4 p. 51. The command
targets the *drive's* data separator, not the controller's; it is a factory/diagnostic facility, and the
shipped driver defines it but never sends it (*observed*). Note the inverted mnemonic polarity (DL = 0
is *disable*). It still terminates with an interrupt [1] §5.1 p. 107.

### 3.3 Interrupts and the /CSTIN polling loop

The authoritative enumeration [1] §5.1 pp. 107–108 — under Apple mode the chip interrupts on:

1. entering the result phase of any command that returns status;
2. the end of Seek and Recalibrate;
3. the end of Disable/Enable DPLL, Set Drive Mode, Set Motor Control, Eject;
4. the execution phase in non-DMA mode, to request data;
5. **a /CSTIN status change** (media inserted or removed);
6. an invalid command.

Two amendments a re-implementation must honour. **Set Enable Control also interrupts** — "immediately
after asserting or deasserting the ENBL_B pin... the FDC will issue a normal termination interrupt"
[1] §4.4.2 p. 110 — even though the p. 107 list omits it; the shipped driver waits on that interrupt
(*observed*). And **Sense Drive Status does not interrupt**: "in the Resultant phase, the INT signal is
not output; therefore the host confirms that both DIO and RQM are 1, then reads the contents of ST3"
[1] §5.1.1 p. 121 — the driver's straight write-write-read with no interrupt wait matches exactly
(*observed*).

**Sense Interrupt Status has a variable result length.** One byte (ST0 only) after a /CSTIN change,
after DPLL/Set Drive Mode/Set Motor Control/Eject, and when nothing is pending (ST0 = $80); two bytes
(ST0 + PCN) after Seek or Recalibrate. "The host should know how many bytes should be received by
checking the first byte" [1] §5.1 pp. 108–109 — i.e. SE set means two bytes, which is exactly the
driver's test (*observed*).

**The 100 ms rule.** "The host should issue this command to the FDC within 100 ms after the interrupt
occurs. Unless [so], there may be some capability to miss the already occurred interrupt" [1] §5.1
p. 107. The reason is visible in the chip's own firmware: the two Chapter 6 flowcharts that bear on
this were read directly from the page images (they carry no extracted text) [1] Chapter 6 pp. 136,
140. The Apple-mode command-wait state:

```
Apple mode -> Initialize FDC -> Check Drive Install ("If drive is not installed, set DxI")
  -> Reset CB -> Command?
       N -> POLLING -------------------  (loop back to Reset CB)
       Y -> Reset INT, Set CB -> INVALID?
             Y -> Reset INVALID -> Send ST0
             N -> Command Processing -> Command wait
```

and POLLING:

```
Drive scan:    Set #0 -> Status check -> Set #1 -> Status check -> RET
Status check:  Status change? --N--> RET      ("Check only /CSTIN")
                     |Y
                Set INT -> Wait 100 ms -> RET
```

Three contracts fall out. **The DxI bits are set by an explicit initialization-time drive-install check**,
not sampled continuously (§2.2). **CB is 0 whenever the chip is idle or polling by design** — `Reset CB`
sits at the top of the idle loop — which makes the silicon deviation the driver records (§5: CB sometimes still
set after an interrupt) a genuine deviation from the documented design, not a misreading. And
**/CSTIN detection is a level comparison against remembered state, drive 0 and drive 1 alternately**,
with the chip's own 100 ms pause after raising INT: if the host has not collected the status by the time
polling resumes, the next scan can overwrite it — which is precisely the "may miss the already occurred
interrupt" failure the 100 ms rule warns about.

**Polling is suspended while any drive is enabled.** "The FDC can not poll the status for the two FDDs
while one ENBL_B pin is active... the FDC can not detect the event that the media is inserted into
another FDD while the ENBL_B pin is active on another FDD" [1] §4.4.2 p. 110. A media-change interrupt
can therefore only arise with no drive enabled — which is why the driver's probe sequences always finish
with Set Enable Control off (§4.2) (*observed*, and the sequence shape follows from the rule).

**Eject produces two interrupts**: "immediately after asserting the /Eject command to the FDD, the FDC
will issue a normal termination interrupt", and — "due to the extremely long time it takes to execute an
Eject command, there is no handshaking done by the FDC" — the completion arrives later as a /CSTIN
change interrupt, up to 1.5 s after the command [1] §4.4.4 p. 53, §5.2 p. 175.

**On the Mac side**, the chip's INT line lands in the PSC's pseudo-VIA2 window as **bit 5**, level 2 —
enabled with `$A0` (sense + bit 5), disabled with `$20`, polled at the flag register — and the handler
chain enters through the VIA2 dispatch slot at low memory (`VIA2DT + $14`) [PSC](psc.md) §2.8 [3]
(*observed*). The DMA channel interrupt is separate machinery and is installed only so a DMA bus error
has somewhere to land [PSC](psc.md) §4.4.

### 3.4 DMA, terminal count and the FIFO threshold

The DMA protocol the surrounding system must satisfy [1] §2.3 p. 24, §5.1.2 pp. 190–191, §7 p. 162:

- **DMARQ** is active **high** in Apple mode (only the PC/AT personality tri-states it); **DMAAK_b** is
  the active-low acknowledge. TC is **active high** in Apple and General modes, and **"TC is accepted
  only while DMAAK_b is active"**.
- **Failing to supply terminal count ends the command wrongly**: "If TC is not used as an input signal
  at the completion of the command, the Abnormal Termination will be set (ST0 = 40) and End of Cylinder
  will be set (ST1 = 80)" [1] §2.3 p. 24. On this platform the terminal count is the PSC channel's
  terminal-count completion on the last byte ([PSC](psc.md) §4.4); a channel model that completes the
  count without signalling terminal count to the FDC turns every otherwise-good transfer into an error.
- **Timing**: the DMARQ cycle must be at least 8 data-bit times; DMARQ to RD/WR-inactive and DMARQ to
  TC-active must each be within 6.5 bit times per byte (≈6.5 µs at the 1 Mbps ED rate); tTC ≥ 50 ns.
  Both **Single Transfer** and **Demand Transfer** modes are supported [1] §5.1.2.2 p. 190.

The **FIFO threshold** is programmed by Configure as a 4-bit value that "is one less" than the threshold,
which ranges 1 to 16 [1] §5.1.2 p. 189: the driver's `$0F` is therefore a **16-byte threshold — the
whole FIFO** (§4.1). Threshold semantics [1] §5.1.2.1 pp. 123, 190:

| Direction | DMARQ asserts when | DMARQ deasserts when |
|---|---|---|
| FDC to host (read) | bytes in FIFO ≥ threshold | FIFO empty |
| host to FDC (write) | free space ≥ threshold | FIFO full |

In non-DMA mode the same conditions drive **INT and RQM** instead of DMARQ [1] §5.1.2.3 p. 191. The
worst-case service delay the host must meet is `threshold × (8 / data rate) − 12 × internal clock
period`; the worked example is threshold 8 at 500 kbps = 126.6 µs [1] §5.1.2 p. 190.

### 3.5 GCR recording

The chip records and plays back Apple GCR in hardware; the host never sees nibbles except through Raw
Dump (§3.7). The format specification [1] §1.3.1 pp. 4–13:

**Speed zones** — the variable-speed drive's zones, which fix sectors-per-track (and double for
two-sided media):

| Zone | Cylinders | Sectors/track |
|---|---|---|
| 1 | 0–15 | 12 |
| 2 | 16–31 | 11 |
| 3 | 32–47 | 10 |
| 4 | 48–63 | 9 |
| 5 | 64–79 | 8 |

**Sync groups.** "A sync group is composed of a 6 byte sequence (**FF 3F CF F3 FC FF**) which guarantees
that the hardware is synchronized prior to the beginning of an address or data mark" [1] §1.3.1 p. 4.
The Format "number of sync groups" byte is the count of these groups written between sectors — the
mechanism that spaces sectors around a zone. The datasheet prescribes an *adaptive* search at format
time: start with 6 or 7 groups, increment on success, and use the last value that formatted cleanly
[1] §1.3.1 p. 4. Apple diverges (§5).

**Sector layout** [1] §1.3.1 pp. 5–6:

- **Address field**: six self-sync bytes, the address mark **$D5 $AA $96**, then track, sector, side,
  format and checksum — each GCR-nibble encoded — then two bit-slip bytes **$DE $AA**. Track and side
  pack into 16 bits with **side in bit 11 and track in bits 0–10** (only 8 bits of track ever used).
- **Data field**: six self-sync bytes, the data mark **$D5 $AA $AD**, the sector number, then **12 tag
  bytes + 512 data bytes + 3 checksum bytes**, then bit-slip **$DE $AA**. Encoded, the field takes 703
  GCR bytes.

**Encoding** is the Apple 3-into-4 nibblizing scheme (U.S. Patent 4,564,941), with full
nibblize/denibblize tables given in the specification [1] §1.3.1 Tables 1.3.2–1.3.3 pp. 8–9; the
512-byte field is not divisible by 3, so both encode and decode assume a phantom zero 513th byte
[1] §1.3.1 pp. 6–11. The data-field protection is **three interleaved rotating checksums with carry
feedback**; the address-field checksum is a simple longitudinal XOR — `track XOR sector XOR
(side,track) XOR format` — through the nibble table [1] §1.3.1 p. 12.

**Tag bytes.** The TB opcode bit governs them: TB = 1 transfers **524 bytes** per sector (tags included);
TB = 0 "the tag byte field is automatically filled with '0'" on write and "is not transferred" on read —
512 bytes only [1] §3.3 p. 48, §4.8.1 p. 90, §4.8.8 p. 117. **Apple's driver never sets TB** (*observed*:
every data opcode it issues has bit 5 clear, and its tag-buffer control call is a stub), so on this
platform tags are hardware-zero-filled on write and discarded on read. The GCR error taxonomy is six
conditions — partial address mark, bad address checksum, bad address bit-slip, bad data mark, bad data
checksum, bad data bit-slip — reported through ST1 DE/MA and ST2 DD/MD [1] §1.3.1.6 pp. 11–13.

The **write splice occurs after the bit-slip bytes**, and GCR multi-sector writes "can accommodate any
interleaving including 1:1" (MFM requires 1:n) [1] §4.8.8 p. 117.

### 3.6 MFM recording

The MFM track format [1] §1.3.2 pp. 14–15: the index field is **12 bytes of $00**, then the index mark —
three **$C2** special characters plus **$FC** — where "these special characters violate the MFM encoding
standard by missing a transition". Both ID and data fields are protected by **CCITT CRC-16**. The
canonical gaps:

| Unformatted / formatted | Gap1 | Gap2 | Gap3 | Gap4A | Gap4B |
|---|---|---|---|---|---|
| 1 MB / 720 KB | 50 | 22 | **84** | 80 | 182 |
| 2 MB / 1.44 MB | 50 | 22 | **101** | 80 | 204 |
| 4 MB / 2.88 MB | 50 | 41 | **83** | 80 | 518 |

Apple's format table uses Gap3 = 80 / 101 / 84 — a byte or three off the canonical 84 / 101 / 83 at the
ends, harmless (§5). The Apple variable-speed drive runs **600 rpm for 720 KB and 300 rpm for 1.44 MB**
across densities [1] §1.3.2 p. 14. Precompensation: 720 KB/1.44 MB want 125 ns on the inner cylinders;
"2.88 MB needs none" [1] §1.3.2 p. 15 — yet Apple writes the 125 ns value for ED with PRETRK = 0,
precompensating every ED track (§5).

**Perpendicular Mode** ($12, echo-back byte `xxxxxx D1 D0`) changes the 1 Mbps write waveform geometry,
and the selection is *independent of the DRR data rate* — the host must keep the two consistent [1]
§4.6 p. 58:

| D1 D0 | Mode | VCO low after INDEX | Gap2 at format | Gap2 on write | Gap2 VCO low / read |
|---|---|---|---|---|---|
| 00 | conventional | 33 B | 22 B | 0 B | 24 B |
| 01 | perpendicular 500 kbps | 33 | 22 | 19 | 24 |
| 10 | reserved | 33 | 22 | 0 | 24 |
| **11** | **perpendicular 1 Mbps** | **18** | **41** | **38** | **43** |

4 MB MFM *horizontal* is an "INVALID CONFIGURATION" [1] Table 1.4.1.2 p. 18 — 2.88 MB recording requires
D1 D0 = %11, which is exactly what the driver programs before its ED probe (§4.3).

### 3.7 Raw Dump

Raw Dump ($1E/$5E, eight command bytes: opcode, mode/head/drive, C, H, R, N, count MSB, count LSB — the
count a 16-bit unsigned) is the escape hatch that returns disk bytes **without denibblizing** [1] §4.9
pp. 59–61, §4.9.4 pp. 84–86. Byte 2 is `X X X RDM1 RDM0 HD DR1 DR0` — RDM in bits 4:3:

| RDM | Start of the dump |
|---|---|
| 00 | the index address mark — **illegal in GCR**, "because the FDD can not output the Index pulse from itself" [1] §4.9 p. 59 |
| 01 | after the specified **ID** address mark |
| 10 | after the **data** address mark |
| 11 | not defined |

The dump starts *at and including* the mark byte. Sync targets: MFM data $A1 / clock $0A or data
$C2 / clock $14 (ID mode tries $A1 first, then retries the same ID field for $C2); GCR syncs on data byte
**$D5** — the first byte of either mark [1] §4.9 p. 60. Once synced, CRC/checksum/bit-slip/data-mark
errors are ignored, and "it may be possible for the FDC to lose synchronization... because of write
splices. If this occurs, meaningless data will be transferred to the host" [1] §4.9 p. 59. Timeouts: a
GCR mark not found within **400 ms**; MFM within two index pulses [1] §4.9.4 p. 86.

**The returned stream is interleaved 8 × 8: one data byte followed by one clock byte**, so a requested
count of N yields **2N bytes on the bus** [1] §4.9.4 pp. 84–85. In MFM the clock byte is the real clock
pattern — the specification prints the two full 256-entry tables, the clock byte for a previous data
bit of 0 and of 1. In GCR "the clock byte that is transferred is a duplicate of the data byte. This
additional dummy clock byte is meaningless" (worked example: FA FA, 9D 9D, A7 A7, ...) [1] §4.9.4 p. 85 —
and the driver discards the GCR clock bytes entirely (*observed*).

The consequence for the OS surface is documented by Apple: a TrackDump call with search mode 0 "no
longer starts its data stream at the beginning of the track. Instead, it starts after the address field
of the first sector (GCR sector 0 or MFM sector 1)" [2] Chapter 12 p. 415 — exactly the RDM = 00
restriction surfacing at the API level.

### 3.8 Mechanical and drive-interface timing

**Seek is fully handshaked in Apple mode** — there is no programmed step rate: "the FDC executes this
command by issuing /Step to the FDD [and] reads the /Ready status line... This provides a full
handshake" [1] §5.1.7 p. 187. Explicitly unsupported: seek with no media, simultaneous seeks on more
than one drive, and checking /DIRTIN during a seek [1] p. 187. The allowable /Ready waits [1] Table 4.4.1
p. 104:

| Condition | Maximum wait for /Ready |
|---|---|
| Seek one track, no speed-zone change | **18 ms** |
| Seek one track with a speed-zone change (GCR only) | **250 ms** |
| Seek across more than two speed-zone changes | **800 ms** — the maximum for any seek |

On timeout the command interrupts with **EC** set in ST0 [1] p. 104. The drive-side step timing:
/STEP T1 ≥ 1.0 µs, T2 ≥ 0.5 µs, T3 ≥ 72 µs, T5 ≥ 37 µs, and "it is not allowed to change /DIRTN during
head movement" [1] §5.2 p. 170; /Ready after /STEP: T1 150 µs max, T2 18/250/800 ms [1] §5.2 p. 171;
speed-zone boundary crossing 250 ms max [1] §5.2 p. 177.

**Recalibrate** is a three-phase Apple algorithm, not the 765's "step out until TRK0" [1] §4.4.4
pp. 95–96:

1. reset the internal PCN, step **outward 80 times**, then check /Track0;
2. once /Track0 is active, step **inward** until /Track0 goes inactive;
3. step **outward once** and confirm /Track0 active again — then interrupt.

Six enumerated failure paths (each ending in EC) cover /Ready not seen within 800 ms after the last step,
/Track0 never asserting, /Ready not seen within 18 ms after a step, and so on [1] §4.4.4 p. 96. TK0
handshake timing: T1/T2 ≤ 3.0 ms, T3 ≤ 150 µs, T4 ≤ 18 ms [1] §5.2 p. 174.

**Motor, mode change and eject bounds:**

| Operation | Bound |
|---|---|
| Motor on to /Ready | 600 ms typical, **1 s max**; on timeout abort with EC [1] §4.4.3 p. 112, §5.2 p. 176 |
| Write gate after motor on | ≥ 600 ms [1] §5.2 p. 173 |
| Drive mode change (MFM ↔ GCR) | **800 ms max**, then abort with EC [1] §4.4.1 p. 111, §5.2 p. 178 |
| Eject to /CSTIN-change report | up to **1.5 s** [1] §5.2 p. 175 |
| GCR address/data mark search | **400 ms** [1] §4.9.4 p. 86 |
| MFM mark search | two index pulses [1] §4.9.4 p. 86 |
| RDATA sync window (T3) | GCR 190 µs; MFM 1 MB 190 µs; MFM 2/4 MB 340 µs [1] §5.2 p. 168 |
| Bit cell (T5) | GCR 2/4/6 µs; MFM 1–2 MB 2/3/4 µs; MFM 4 MB 1/1.5/2 µs [1] §5.2 p. 168 |

Note that Set Drive Mode reconfigures **the drive**, not the controller — the drive physically changes
spindle-speed regime, and ST3 bit 0 (/MFM mode) reads the result back [1] §4.4.1 p. 111.

**The drive interface.** In Apple mode the IWM-era pin names carry over with new jobs: `SEL/HDLD_b`,
`CA0/DIR_b`, `CA1/STEP_b`, `CA2/SIDE_b` become **select and command-address lines**, used "to multiplex
status to the RDATA line during a read operation, and to select addressable latches on the disk drive
during a command operation"; `LSTRB` "is used to send a command to the drive" [1] §2.3 p. 25. The
multiplexed status lines readable this way are: /DIRTIN, /STEP, /MOTOR ON, EJECT, 2MB DRIVE, 4MB DRIVE,
Mode ID, RDDATA, /DRVIN, /TACH, INDEX, /READY, /CSTIN, /WRTPRT, /TK0, /2MB MEDIA, /4MB MEDIA, MFM MODE
[1] §5.2 p. 167. Command-send timing: T1 ≥ 0.5 µs, T2 ≥ 1.0 µs, T3 ≤ 1.0 µs, T4 ≤ 0.5 µs, T5 ≥ 0.5 µs
[1] §5.2 p. 169. Write-data pulses are ≥ 1.8 µs in GCR and 4 µs in MFM [1] §5.2 p. 173. The drive-enable
pins (`ENBL0_b`, `ENBL1_b`) gate *everything*: "ENBL_B enables all communication with the Apple FDD"
[1] §2.3 p. 25.

For the drive's own numbers, the ancestor specification of this drive line — the Apple 3.5-inch
single-sided drive, the first of the variable-speed Apple drives — specifies a **continuously variable
390–605 rpm** spindle ("the motor speed is variable to allow recording to be done at fixed density as
the head moves from the outer edge of the diskette toward the center"), 12 ms maximum track-to-track
slew, 30 ms step settling, 150 ms speed-group settling, 400 ms motor start, 80 tracks, and read flux
transitions 1.89–6.36 µs apart [4] §2.3.3–2.3.4.

## 4. Programming model

The sequences below are the ones the shipped software actually performs. The driver is the ROM's
`.NewAge` floppy driver — DRVR resource id 4, opened by name at boot when the decoder record's
existence bit is set [3] — and it keeps the classic Sony driver interface, so application software sees
the usual `.Sony` calling conventions [2] Chapter 12 p. 413.

### 4.1 Bring-up and reset (ResetFDC)

After every reset the chip's defaults (§2.6) are wrong for this platform, so the driver's reset routine
issues a fixed four-command sequence (*observed*; each step's necessity follows from §2.6):

1. **DRR ← $9C** — the software reset (bit 7, self-clearing).
2. **Select Drive Type $32, %11** — the Apple FDD model. Encoding: %00 conventional (the reset
   default), %01 the 13 MB FDD, %10 reserved, **%11 the Apple FDD** [1] §5.1.7 p. 188. The second byte
   is an echo-back; no result, no interrupt. The specification phrases the effect as applying to "the
   next command", but the driver issues it once per reset and it evidently persists (§6).
3. **Configure $13, $00, $0F, $00** — byte 3 is `0 EIS EFO POL FIFOTHR[3:0]`: EFO = 0 enables the data
   FIFO [1] §3.3.1 p. 50, and FIFOTHR = $F is a **16-byte threshold** — the programmed parameter being
   one less than the threshold [1] §5.1.2 p. 189 — the whole FIFO, i.e. maximum DMA-latency tolerance.
   Byte 4 (PRETRK = $00) starts precompensation at cylinder 0.
4. **Specify $03, $00, $00** — ND = 0, DMA mode (§3.2.1); the step-rate and head-timing bytes are
   meaningless in Apple mode [1] §4.4.10 p. 113. ($03, $00, $01 — ND = 1 — is the temporary PIO
   switch the driver makes for raw track dumps, restored afterwards.)

### 4.2 Open and the drive probe

At open, per drive number 0 and 1 (*observed*):

1. ResetFDC (§4.1).
2. **Set Enable Control on** → wait for the termination interrupt → **Sense Drive Status** → read ST3.
   **$FF means no drive** — the probe stops and the drive number is not registered (§2.5).
3. **Set Enable Control off** → **Sense Interrupt Status** — clearing any pending state *before* the
   interrupt handler is installed, so a stale interrupt cannot fire into the new handler.
4. Register the drive; set the retry seed (25).
5. Drive class from ST3: the default is the 800 KB GCR class; **bit 3 set** names the SuperDrive (2 MB)
   class; with **bit 2 additionally clear**, the drive is the 4 MB ("Typhoon") class.
6. Allocate and lock the track cache (36 blocks of 516 bytes — 512 data bytes plus host-side padding;
   the four extra bytes are for the disk-copy path, not tags).
7. Install the interrupt handler at the VIA2 dispatch slot, enable VIA2 bit 5 with `$A0`, and install
   the channel-3 DMA handler — which exists solely so a DMA bus error has somewhere to land [PSC](psc.md)
   §4.4.

Step 2's shape is not style: Sense Drive Status obeys a once-only rule, quoted in full because it is a
contract a re-implementation must keep:

> "Under Apple mode only, this command should be issued to get the correct status from the FDD only
> after the 'Set Enable Control' command is issued. In other words, if this command is issued to the
> FDC more than one time after the 'Set Enable Control' command is issued, the FDC can not correctly
> inform the host the content of the Status register 3 (ST3)... Because the specific FDD of Apple has
> the same codes between the Motor off and the Select Media command to the FDD." [1] §4.4.6 p. 105

Reading ST3 drives the CA0-2/LSTRB command lines, and the Apple drive's "Select Media" command code
collides with its "Motor off" code — a second ST3 read perturbs the drive's state. Hence exactly one
read per Set Enable Control.

**Set Enable Control's own rules** [1] §4.4.2 p. 110: only one ENBL_B may be active at a time; **the
chip does not remember which** — host bookkeeping is mandatory; /CSTIN polling is suspended while any
is active (§3.3); and the command "checks no status... has no error conditions and the FDC always
informs the host of a normal termination".

### 4.3 Media identification (SetUpFDC)

On first access after insertion the driver runs a probe ladder (*observed*; every interrupt the ladder
waits for is one of the causes of §3.3):

1. **Set Enable Control on** → **Recalibrate** → Sense Interrupt Status (two result bytes, SE set).
2. **Sense Drive Status** → ST3 → media class by the driver's ladder: bit 7 names low-density media;
   else bit 1 names high-density; a combination only a 4 MB-class drive can report maps to extended.
3. **Set Motor Control on** (up to 1 s for /Ready, §3.8) → **Perpendicular Mode $12, $00**.
4. **GCR probe**: Set Drive Mode GCR (up to 800 ms) → DRR ← $00 → **Read ID (GCR)** — one command, whose
   result byte 7 is the format byte: $02 or $12 ⇒ 400 KB, $22 ⇒ 800 KB, $24 ⇒ ProDOS, anything else an
   unknown GCR format.
5. **MFM probe** (SuperDrive class or better): Set Drive Mode MFM → DRR ← $0C → a **loop of Read ID
   (MFM)** calls walking the track and tracking the maximum sector number until it wraps; a maximum of
   9 ⇒ 720 KB, else 1.44 MB.
6. **ED probe** (4 MB class only): Perpendicular Mode $12, $03 → DRR ← $0F → Read ID ⇒ 2.88 MB.
7. All probes failed ⇒ unformatted. Then the driver derives the block count and sides, and reads write
   protection from ST3 bit 6 (clear = protected, §2.5).

Read ID "returns the first available address header... after a sync byte field", with a timeout of two
index pulses in MFM and "two revolutions... within 400 msec" in GCR [1] §4.4.4 p. 95 — which is what
makes the single-shot GCR format sniff and the repeated-MFM-ID track walk both work. Every probe arm
carries a reset-and-retry recovery (reset, wait, re-enable, retry) because a probe can leave the chip
mid-command when the media is unformatted; repeated failure is reported as a blank disk (*observed*;
the failure mode itself is not documented — §6).

The MFM probe is the path the Dev Note documents at the API level: with a SuperDrive the driver "reads
from and writes to" the GCR formats plus 720 KB and 1440 KB MFM; with the older 800 KB GCR drive, the
GCR formats only; "it does not support the Apple 400K GCR floppy disk drive or the Macintosh HD20 hard
disk drive" [2] Chapter 12 p. 414. (400 *KB disks* in an 800 KB-or-better drive are fine — the 400K
*drive*, with its fixed-zoom stepping, is what is unsupported.) The driver additionally carries the
complete 2.88 MB ED path — format-table entry, probe step, perpendicular-mode setup — gated on a drive
reporting the 4 MB class, which no shipped configuration supplies (*observed*; the Dev Note's supported
list stops at 1440 KB).

### 4.4 Transfers

The PSC-side arming is [PSC](psc.md) §4.4's contract: pause channel 3, spin on FROZEN, program the
inactive register set with a physical address and byte count, release, and mask the FDC's VIA2 interrupt
for the launch. On the New Age side (*observed*):

- The driver writes the command bytes up to but excluding the last, then arms the PSC set, then writes
  the **launch byte** with the interrupt masked — the DTL byte ($00) for Read/Write Data, the filler
  byte for Format A Track, the sync-group byte for Format/Write (§3.1, §3.2.3). The mask is lifted as
  the transfer runs.
- Multi-sector transfers that must split at a requested-sector boundary use **both** PSC register sets:
  the driver reads the active-set bit and programs the other set mid-transfer [PSC](psc.md) §4.4.
- Completion is taken by **polling**, not interrupt: the driver polls for DMA completion on a
  ~4 ms timeout; the channel-3 interrupt handler exists for bus errors only. The GCR interleave tables
  (2:1 Mac, 4:1 ProDOS) and the MFM interleave (1–9) are issued through the format commands' four
  execution bytes per sector (§3.2.3).
- **Formatting a GCR track is two passes**: first a deliberately bogus format — sync-group count $3F and
  sector count $3F — whose over-long write erases the track, then, after a 1 ms wait, the real format
  with the actual values. The driver hardcodes **8 sync groups** for all three GCR formats; it does not
  implement the specification's adaptive search (§3.5). Both divergences are *observed* in the shipped
  driver; the specification prescribes neither.
- Track-dump paths (TrackDump) switch Specify to ND = 1, run Raw Dump in PIO mode through the
  FIFO-threshold INT/RQM arithmetic (§3.4), take 2 bus bytes per disk byte (§3.7), discard the GCR clock
  duplicates, and restore ND = 0 afterwards.

### 4.5 Interrupt service and media change

The FDC interrupt handler is a level-2 VIA2 client (§3.3). Its service routine and the driver's two
polling loops all begin by testing bit 5 of the VIA2 flag register and bail out if it is clear
(*observed*). The handler reads ST0 — via Sense Interrupt Status — and dispatches on the interrupt
code [3] (*observed*):

| ST0[7:6] | Action |
|---|---|
| %11 — /CSTIN change | check FIN (disk present); queue a deferred task that posts a disk-insert event to the system |
| %10 — invalid command | run the reset sequence (§4.1) |
| other | command completion bookkeeping |

The driver's own timeout constants, scaled through the machine's timing primitive (*observed*): 512 ms
per polled PIO byte, 256 ms per command, ~4 ms for the DMA-done poll, 64 ms for the probe-recovery arm,
and a 3 s motor-off policy timer. The 3 s timer is pure policy — the chip has no motor-off timeout of
its own (§3.8).

### 4.6 The OS-visible surface

The Dev Note's Chapter 12 documents the deltas against the previous floppy drivers [2] pp. 413–415:

- **Unsupported control calls**: TrackCache (csCode 9), KillI/O (csCode 1) and TagBuffer (csCode 8) are
  not implemented — TagBuffer returns -17, KillI/O returns -1. TrackCache is dropped because "the read
  process would try to cache everything on the track being read".
- **TrackDump with search mode 0** starts after the address field of the first sector instead of at the
  track start (§3.7).
- **DriveStatus** against an uninstalled drive returns -56 (nsDrvErr) rather than -64, with invalid
  data in the parameter block; reference numbers 0 and 1 return valid data.
- **Merged error codes**: the controller "returns only one error code for a bad address mark. There is
  no differentiation in the address mark between a bad slip bit and a wrong track number" — so
  badBtSlpErr, seekErr and noAdrMkErr collapse into noAdrMkErr, and badDBtSlp and noDtaMkErr into
  noDtaMkErr. initIWMErr, twoSideErr and spdAdjErr are not applicable. And "the noNybErr error used to
  mean a byte timeout. With the New Age driver it indicates a timeout error resulting from waiting for
  New Age to respond to a command."

The merged-address-mark behavior follows from the chip's taxonomy: the six GCR error conditions
(§3.5) reach the host only through the ST1/ST2 bits the driver maps, and the driver's mapping keeps no
state that would separate a bit-slip failure from a wrong-track address field.

## 5. Quirks & errata

- **The Command-Busy deviation — an erratum the driver tolerates.** The specification states that when
  Sense Interrupt Status is issued after an interrupt, bit 4 (command busy) of the status register is
  0 [1] §5.1.7 p. 189, and that the result phase ends at CB = 0 [1] §5.1.1 p. 123. The driver's own
  comment on its interrupt-status routine records that the real chip "does not always have the Command
  Busy bit clear, as it should" after an interrupt, and the routine therefore **waits for CB to clear**
  before it issues Sense Interrupt Status; its result-collection routine, by contrast, waits for CB to
  be **set** — the result phase — before reading the seven status bytes (*observed* in the ROM's
  driver code). The two waits are exactly the documented idle (CB = 0) and result-phase (CB = 1)
  states, so a re-implementation faithful to the document satisfies both; the erratum is a CB that
  lingers set for a while after an interrupt, which the driver absorbs. (The firmware flowcharts confirm
  CB was *designed* to be 0 whenever idle, §3.3.)
- **STR and DRR share one address.** $50F2A101 reads as status and writes as the data-rate latch; there
  is no way to read the DRR back (§2.1, §2.3).
- **GCR precompensation is always zero** in Apple mode, whatever PCS2-0 say — the precompensator is
  disabled under GCR, so the field is MFM-only (§2.3).
- **Apple-mode data rates are 00 or 11 only.** The 300 and 250 kbps encodings are documented as not
  legal in Apple mode (§2.3) — with one internal contradiction (§6).
- **Invalid command is exactly $80** — and Sense Interrupt Status with nothing pending returns the same
  $80 (§2.5). A host that uses $FF as a "no status yet" sentinel can tell the two apart.
- **ST3 is a bank of active-low drive lines.** $FF = no drive, clear bit 6 = write protected, set bit 7
  = low-density media; only bit 3 (drive class) reads positive (§2.5).
- **Sense Drive Status: once per Set Enable Control.** A second read returns wrong ST3 because the
  drive's Select Media and Motor off command codes collide (§4.2).
- **Set Enable Control has no memory and no errors.** The chip does not remember which drive is
  enabled; /CSTIN polling stops while any is active; it always terminates normally (§3.3, §4.2).
- **Sense Interrupt Status has a variable result length** — 1 byte or 2 — and the host must decide from
  the first byte (SE set ⇒ 2 bytes). The chip also requires collection within 100 ms or the cause can
  be lost (§3.3).
- **The last command byte launches execution and leaves RQM low.** Hosts that wait for RQM after
  writing it deadlock; the driver masks the interrupt and switches to polling (§3.1).
- **Terminal count is mandatory.** Without TC at the end of a transfer the command ends with
  ST0 = $40 and ST1 = $80 (§3.4) — on this platform the PSC channel must deliver it.
- **Raw Dump doubles the byte count.** N requested bytes yield 2N on the bus, half of them clock bytes
  — meaningless duplicates in GCR (§3.7).
- **Read A Track cannot read interleaved media** — a documented restriction that makes it useless for
  every Mac format, not a defect (§3.2.2).
- **$12 is a GCR format byte but not a Read/Write N byte.** Format commands take $12/$22/$24; Read/Write
  take $02/$22/$24 — and the driver uses $12 for 400 KB in both roles, so a re-implementation should
  accept $12 as an alias of $02 in Read/Write (§3.2.3, §4.3).
- **Eject takes two interrupts** — an immediate termination and a /CSTIN change up to 1.5 s later
  (§3.3, §3.8).
- **Apple's GCR format diverges from the specification's algorithm**: fixed 8 sync groups plus a
  two-pass format with a $3F bogus first pass, instead of the adaptive search (§3.5, §4.4).
- **2.88 MB is precompensated against the format chapter's advice** — "2.88 MB needs none", yet the
  driver writes the 125 ns value with PRETRK = 0, every track (§3.6).
- **Apple's Gap3 values differ from the canonical table by a byte or three** at the 720 KB and 2.88 MB
  ends (80 vs 84, 84 vs 83) — harmless (§3.6).
- **The 13 MB drive was real silicon that no board can reach** — it needs XB = 20 MHz and is MFM-only
  (§1.5).
- **ST0 bit 2 (HD) exists** and is zeroed by Sense Interrupt Status — a bit the older 765 documentation
  family does not carry in this meaning (§2.5).
- **D0B/D1B mean "seek running or seek interrupt pending"** — issuing Read/Write group commands while
  they are set is forbidden (§2.2).

## 6. Open questions

1. **Most of the chip's Chapter 5 waveforms and the remaining Chapter 6 flowcharts are page images
   without extracted text.** Only the Apple-mode command-wait and POLLING charts have been read directly
   (transcribed in §3.3); the rest of the firmware algorithms — CONFIGURE, the format and data
   commands, the five RAW DUMP charts — and all the electrical timing diagrams remain unread. The two
   charts that were read also confirm that the remaining ones exist and are labeled per command [1]
   Chapter 6 pp. 136–200.
2. **The /CSTIN poll cadence beyond the flowchart.** Detection is a level comparison against remembered
   state with a 100 ms post-interrupt pause, but the interval between drive scans, and any filtering
   between the drive's /CSTIN line and the comparison, are not specified (§3.3).
3. **The chip's revision history.** The Revision command ($20, two result bytes: firmware revision,
   hardware revision [1] §4.4.5 p. 97) exists precisely to distinguish silicon steps, and no shipped
   software ever issues it (*observed*). Which step introduced or fixed the Command-Busy deviation of
   §5, and what the two bytes return on the AV boards, are both unknown.
4. **EFO polarity.** The specification contradicts itself within two sentences — "when this EFO is set
   a low (0), the Data FIFO is enabled", then "once the EFO bit is set a high(1), the FIFOTHR bits is
   used" [1] §3.3.1 p. 50. Apple's EFO = 0 with a maximal threshold is only consistent with
   0-enables; that is inference, not a documented resolution.
5. **The source of D0I/D1I.** The drive-install check that sets them is in the firmware (§3.3), but the
   drive-side signal feeding it is never named; /DRVIN — present in the multiplexed status list [1]
   §5.2 p. 167 — is the only candidate (*inferred — unverified*).
6. **Whether Select Drive Type latches.** The specification says it selects the type "the next command"
   runs against [1] §5.1.7 p. 188; Apple issues it once per reset and relies on it persisting. Whether
   it does, in silicon, is untested.
7. **The Apple-mode data-rate contradiction.** §3.1.3 forbids DRATE 01/10 in Apple mode, while the
   configuration table's "1 MByte (MFM) Horizontal" Apple-mode row prescribes "10 or 01" [1] Table 1.4.1.2
   p. 18. The driver uses 00 for 720 KB — correct for a 600 rpm variable-speed drive (§3.6) — but which
   setting the silicon actually honors is untested.
8. **The unformatted-disk hang.** The driver's probe-recovery arm exists because a probe against a
   generic unformatted disk can leave the chip mid-command (*observed* as the driver's workaround); the
   specification describes no such failure. The two candidate mechanisms are the Sense-Drive-Status-once
   rule (§4.2) and the 400 ms mark search outrunning the driver's own 256/512 ms timeouts
   (*inferred — unverified*).
9. **Why Apple replaced the adaptive sync-group search** with a fixed count of 8 and the $3F erase pass
   (§3.5, §4.4). The specification prescribes the search; nothing documents the substitution.
10. **Board identity of the part.** Whether the AV machines carry a discrete µPD72070 or an
    Apple-integrated derivative (the specification is written as a chip data sheet, and Apple's document
    calls the result a "64-pin CMOS chip" [2] p. 15), and the PCTYP1:0 strapping on the board
    (§1.4, *inferred*), are both unconfirmed against hardware documentation.
11. **Window behavior at unlisted offsets.** What the backing bus does at $50F2A000-area addresses other
    than $101 and $141 — fault, float, or alias — and the power-on values of the STR's D*I bits, are
    not established (§2.1, §2.6).
12. **The chip-to-connector pin mapping.** The chip's Apple drive interface (SEL/HDLD_b, CA0-2, LSTRB,
    two ENBL_b pins — §3.8) and the machine's 20-pin internal floppy connector (PH0–PH3, SEL, /ENBL,
    RD, WR, /WRREQ [2] Table 2-9 p. 28) are different pin sets; the board-level conversion between them
    is not documented anywhere in the evidence.
13. **The ED precompensation question.** 2.88 MB "needs none" [1] §1.3.2 p. 15, yet Apple writes the
    125 ns value with PRETRK = 0 (§3.6) — deliberate tuning or copied boilerplate is not determinable.
14. **Raw Dump mode %11** is "not defined" [1] §4.9 p. 59 — what the chip actually does with it is
    unknown.
15. **The result ID at EOT.** Whether Read/Write Data terminated at EOT with MT = 0 reports C+1 (the
    specification's table, §3.2.2) or the current cylinder (what the shipped driver's cylinder tracking
    implies) is untested against silicon.

## References

1. NEC Corporation, *µPD72070 Advanced Floppy Disk Controller ("New Age FDC") specification*,
   Apple-confidential edition (200-page OCR text with per-page images) — §1.1–1.2 pp. 1–3 (features,
   the "New Age FDC" name, the disclose-to-Apple-only command set); §1.3.1 pp. 4–13 (GCR format: speed
   zones, sync groups, sector layout, nibblize/denibblize tables, checksums, error taxonomy);
   §1.3.2 pp. 14–15 (MFM format, gaps, CRC, spindle speeds, precompensation advice); §1.3.3 (the
   13 MB FDD); Table 1.4.1.2 p. 18 (mode/drive-type configurations); §1.4 pp. 16–22 (PCTYP strapping,
   command availability by mode); §2 pp. 23–28 (pins, clocks, DMA pins, reset state); §3 pp. 31–48
   (register file: STR/DRR/data register, ST0–ST3, command bit fields); §3.3.1 p. 50 (Configure);
   §4 pp. 50–120 (per-command specifications: DPLL, Dumpreg, Eject, Select Drive Type p. 105 footnote,
   Recalibrate, Read ID, Set Enable Control, Set Drive Mode, Set Motor Control, Specify, Read/Write
   Data and Table 4.8.1, Perpendicular Mode, Raw Dump, Revision, Version); §5 pp. 107–123 (interrupt
   enumeration, Sense Interrupt Status, the PIO handshake, FIFO threshold semantics); §5.1.2.2
   pp. 190–191 (DMA modes and timing); §5.1.7 pp. 186–189 (Apple-mode command notes: Read A Track,
   Seek, Select Drive Type); §5.2 pp. 162–178 (electrical and drive-interface timing); Chapter 6
   pp. 136–200 (firmware flowcharts — the command-wait state at p. 136 and POLLING at p. 140 read
   directly from the page images and transcribed in §3.3).
2. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and Macintosh Centris 660AV
   Computers*, Developer Press, 1993 — §"Summary of Features" p. 5 (the new controller, "based upon
   Industry Standard 765"); §"Storage and Input Devices" p. 9 (one internal SuperDrive); §"New Age"
   p. 15 (the chip description and its five listed functions); Table 2-2 p. 17 (C16M 15.6672 MHz to
   New Age); Table 2-9 p. 28 (the 20-pin floppy disk drive connector); Tables 2-10 and 2-11 p. 29
   (the FDC DMA channel's width, buffer and arbitration priorities); Chapter 12 "New Age Floppy Disk
   Driver" pp. 413–415 (supported drives and formats, unsupported control calls, TrackDump behavior,
   the merged error codes).
3. Macintosh Quadra 840AV / Centris 660AV boot ROM (2 MB mask ROM, image checksum $5BF10FD1) — full
   disassembly, resource-container decode and driver analysis: the `.NewAge` driver (DRVR id 4) and
   the start-up path that opens it on the decoder existence bit; the decoder-information record's
   slot-30 base $50F2A000; the driver's register literals ($50F2A101/$50F2A141, PSC channel-3
   aliases, VIA2 IER/IFR and bit 5); the reset, open, probe, transfer, format and interrupt-service
   sequences of §4; the launch-byte, two-pass-format, timeout and interleave behavior cited
   *observed* throughout.
4. Apple Computer, Inc., *Specification for 3.5-inch Single-Sided Disk Drive*, Apple part number
   699-0285 — the ancestor specification of the Apple variable-speed drive line: §2.3.3 access and
   motor timings, §2.3.4 the 390–605 rpm continuously variable spindle and format geometry, §2.3.2
   transfer rate (flux transitions 1.89–6.36 µs).
5. U.S. Patent 4,564,941 (Wooley et al.) — the Apple 3-into-4 GCR nibblizing scheme the
   µPD72070's GCR encode/decode implements [1] §1.3.1.
