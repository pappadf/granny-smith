# SWIM and SuperDrive Floppy Subsystem

**Contents:**

1. [Introduction](#introduction) — Purpose, SWIM overview, relationship to IWM,
   SuperDrive
2. [Hardware Overview](#hardware-overview) — IC packages, pinouts, signal
   descriptions, clock
3. [Memory-Mapped I/O](#memory-mapped-io) — Address decoding, register stride,
   machine-specific mapping
4. [Operating Modes](#operating-modes) — IWM mode vs ISM mode, mode switching
5. [IWM Mode](#iwm-mode) — Register set, state machine, read/write paths
6. [ISM Mode](#ism-mode) — Register set, FIFO, handshake, data transfer
7. [Parameter RAM](#parameter-ram) — Read timing, write pre-compensation,
   calculation formulas
8. [Trans-Space Machine](#trans-space-machine) — MFM encoding logic, mark byte
   generation
9. [Error Correction Machine](#error-correction-machine) — Asymmetry/speed
   correction
10. [CRC Generation](#crc-generation) — CRC-CCITT-16, initialization, verification
11. [Drive Communication Protocol](#drive-communication-protocol) — Signal
    architecture, sense registers, commands, LSTRB pulse
12. [Head Select](#head-select) — SWIM vs IWM-era head selection
13. [Motor and Speed Control](#motor-and-speed-control) — GCR variable speed, MFM
    constant speed, motor-off timer
14. [GCR Disk Format](#gcr-disk-format) — 400K/800K geometry, zones, sector format,
    encoding, checksums
15. [MFM Disk Format](#mfm-disk-format) — 720K/1440K geometry, sector format,
    encoding, mark bytes
16. [Disk Format Detection](#disk-format-detection) — Drive capability, media
    sensing, software probing
17. [Read Operations](#read-operations) — GCR reads (IWM mode), MFM reads (ISM
    mode), mark searching
18. [Write Operations](#write-operations) — GCR writes, MFM writes, CRC insertion,
    pre-compensation
19. [Timing Specifications](#timing-specifications) — Step, settle, eject, spin-up,
    data rates, tachometer/INDEX dual-mode signal
20. [Reset State](#reset-state) — Power-on defaults for all registers
21. [Error Register](#error-register) — Error conditions, latching behavior
22. [SWIM Detection](#swim-detection) — Software detection of IWM vs SWIM
23. [Real-Time Constraints](#real-time-constraints) — Interrupt masking, FIFO
    buffering, polling loops
24. [SWIM Variants](#swim-variants) — SWIM, SWIM2, SWIM3 differences
25. [Edge Cases and Quirks](#edge-cases-and-quirks) — Mode switching hazards, timer
    blocking, copy protection

---

## Introduction

### Purpose and Scope

This document describes the SWIM (Sander-Wozniak Integrated Machine) floppy
controller chip and the SuperDrive (FDHD) mechanism, with the explicit goal of
enabling the implementation of a behaviorally exact emulator of the subsystem.
The scope includes:

- The **SWIM chip** as seen by the CPU, in both IWM-compatible and ISM modes
- The **SuperDrive (FDHD) mechanism** and its communication protocol
- The **GCR and MFM disk formats** at the sector level
- All **timing, encoding, and protocol details** necessary for exact emulation

The SWIM chip is the evolutionary successor to the IWM (Integrated Woz Machine).
While the IWM supports only GCR-encoded 400K and 800K disks, the SWIM adds
support for MFM-encoded 720K and 1440K disks while maintaining full backward
compatibility with IWM software. The SWIM achieves this by containing two
independent cores:

- An **IWM core** for GCR operations (identical to the standalone IWM chip)
- An **ISM (Integrated Sander/Moyer) core** for MFM operations

Only one core is active at a time, selected by software through a deliberate
handshake sequence. A crossbar switch routes the external pins to whichever core
is active.

### Machines Using SWIM

The original SWIM chip (part numbers 344S0061-A and 344S0062) appears in:

| Machine | Internal Drives | External Port | Notes |
|---------|----------------|---------------|-------|
| Macintosh IIx | 2 internal bays | None | /ENBL2 for second bay |
| Macintosh IIcx | 1 internal | 1 DB-19 | |
| Macintosh SE/30 | 1 internal | 1 DB-19 | |

All three machines use identical core memory mapping and asynchronous protocols.
The 68030 CPU communicates with the SWIM asynchronously via the GLUE chip (part
344S0602 or 344S0602-A).

**HD20 limitation:** Although the IIcx and SE/30 expose a DB-19 external floppy
port that can physically accept an HD20 hard drive, the SE/30 and IIx system ROMs
**lack the HD20 boot drivers** required to start up from an HD20 via the SWIM
port.

### Naming: "SWIM"

The name SWIM stands for **Sander-Wozniak Integrated Machine**, combining the
names of two Apple engineers: Wendell Sander (who designed the ISM logic for MFM
support) and Steve Wozniak (whose original Disk II design evolved into the IWM).

---

## Hardware Overview

### IC Packages

The SWIM is available in three packages:

**28-pin DIP** (pin-compatible with IWM):

| Pin | Signal | Pin | Signal |
|-----|--------|-----|--------|
| 1 | PHASE 0 | 28 | PHASE 1 |
| 2 | PHASE 2 | 27 | PHASE 3 |
| 3 | A0 | 26 | Vcc |
| 4 | A1 | 25 | Q3/HDSEL |
| 5 | A2 | 24 | FCLK |
| 6 | A3 | 23 | /RESET |
| 7 | /DEV | 22 | RDDATA |
| 8 | WRDATA | 21 | SENSE |
| 9 | /WRREQ | 20 | /ENBL1 |
| 10 | D0 | 19 | /ENBL2 |
| 11 | D1 | 18 | D7 |
| 12 | D2 | 17 | D6 |
| 13 | D3 | 16 | D5 |
| 14 | GND | 15 | D4 |

**28-pin PLCC** — Surface-mount variant with the same pinout as the 28-pin DIP.
Both 28-pin packages (DIP and PLCC) are drop-in pin-compatible with the older
IWM chip, allowing existing logic board layouts to use the SWIM without trace
changes.

**44-pin PLCC** exposes additional signals not available in the 28-pin packages:

| Pin | Signal | Description |
|-----|--------|-------------|
| 10 | MOTORENB | Active when MotorOn set or motor-off timer running |
| 16 | 3.5SEL | Programmable output (inverse of Setup bit 1) |
| 33 | DAT1BYTE | High when ISM FIFO has a transferable byte (= Handshake bit 7) |
| 38 | HDSEL | Head select output |

### Pin Descriptions

**FCLK** — Master clock input. 15.6672 MHz for Macintosh systems (14.3 MHz for
Apple II). All internal timing derives from this clock. The Setup register bit 3
(FCLK/2) divides this by 2 for compatibility with 800K drives.

**/RESET** — Active-low reset. Initializes all registers to default values, resets
any current operation, and returns the chip to IWM-compatible mode.

**/DEV** — Active-low chip select. The chip only responds to bus transactions when
/DEV is asserted.

**A0–A3** — Register address lines. In IWM mode, A0 selects read (0) vs write
(1). In ISM mode, A3 selects read (1) vs write (0).

**D0–D7** — Bidirectional 8-bit data bus.

**PHASE 0–3** — In IWM mode: directly driven as outputs to control the Sony drive
state lines (CA0, CA1, CA2, LSTRB). In ISM mode: individually programmable as
inputs or outputs via the Phase register.

**RDDATA** — Serial data input from the disk drive read head.

**WRDATA** — Serial data output to the disk drive write head.

**/WRREQ** — Active-low write request. The drive asserts this when it can accept
data on WRDATA.

**SENSE** — General-purpose input. In Macintosh systems, wired to the drive's RD
output for polling drive status registers.

**/ENBL1, /ENBL2** — Drive enable outputs. Active low. /ENBL1 selects the primary
drive, /ENBL2 selects the secondary drive.

**Q3/HDSEL** — Dual-function pin. On reset, functions as Q3 input (IWM data
latching clock). When Setup bit 0 is set, becomes HDSEL output for head
selection.

---

## Memory-Mapped I/O

### Address Decoding

The SWIM uses a **512-byte (0x200) register stride**, identical to the Mac Plus
IWM. The register index is decoded from the byte address as:

```
register = (byte_address >> 9) & 0xF
```

This means each register occupies 1 byte followed by 511 bytes of padding. With
16 registers, the total mapped region is 8,192 bytes (0x2000).

### Machine-Specific Base Addresses

| Machine | Base Address | End Address | Notes |
|---------|-------------|-------------|-------|
| Mac Plus | 0x00D80000 | 0x00DFFFFF | 512K window, IWM only |
| SE/30 | 0x50016000 | 0x50017FFF | 8K mapped region |
| IIx/IIcx | Same decode | Same stride | I/O block mirrors every 0x20000 |

The SE/30 I/O block mirrors every 0x20000, so the effective mask is
`address & 0x1FFFF`.

### Register Addresses (Physical)

For the SE/30 at base 0x50016000:

| Register | Offset | Physical Address |
|----------|--------|-----------------|
| 0 | 0x0000 | 0x50016000 |
| 1 | 0x0200 | 0x50016200 |
| 2 | 0x0400 | 0x50016400 |
| ... | ... | ... |
| 15 | 0x1E00 | 0x50017E00 |

Note: References to "IOBase+$40 through IOBase+$4F" in System 7.1 documentation
use logical/software notation, not physical stride. The physical stride is always
512 bytes.

---

## Operating Modes

The SWIM has two mutually exclusive operating modes:

### IWM Mode (Default)

After reset, the SWIM operates in IWM mode. In this mode it is functionally
identical to the standalone IWM chip. IWM mode is used for all GCR operations
(400K and 800K disks). The IWM core provides:

- A single 1-byte shift register (no FIFO)
- MSB=1 latch-based byte detection
- Software-driven GCR encoding/decoding

### ISM Mode

ISM (Integrated Sander/Moyer) mode provides the hardware support needed for MFM
operations. The ISM core provides:

- A 2-byte FIFO buffer
- Hardware mark byte detection
- CRC-CCITT-16 generation and checking
- Configurable read timing parameters (peak shift compensation)
- Write pre-compensation
- Trans-Space Machine for MFM encoding
- Error correction machine for phase lock

### Mode Switching: The 4-Write Handshake

Switching from IWM mode to ISM mode requires a specific 4-write handshake
sequence. Software must write the following values to Mode register bit 6 in
order:

```
1, 0, 1, 1
```

This is accomplished by writing to the Mode register (via Q7H with L6=1, L7=1 in
IWM addressing) with bit 6 set or cleared in the specific sequence. The sequence
was deliberately chosen to be unlikely to occur during normal IWM operation, so
that legacy GCR software cannot accidentally enter ISM mode.

In practice, the writes go to the Q7H address with the following byte values (the
other bits are "don't care" for the handshake):

```
$57, $17, $57, $57   (bit 6: 1, 0, 1, 1)
```

**Switching back to IWM mode:** Write zeros to Mode register bit 6 from ISM mode.
For example, write 0xF8 to the "write zeros" register (address 6 in ISM
addressing).

**Hazards:**
- MotorOn (Mode bit 7) **must be disabled** before switching to IWM mode
- After switching to IWM mode, the first action must be to **clear L7** (read the
  register at address 14) to put the IWM state machine in a known state
- Phase line levels carry over between mode switches without glitches; the
  physical drive sees no discontinuity
- **/DEV timing constraint:** When entering ISM mode, a minimum of **4 FCLK
  cycles** (or **8 FCLK cycles** if Setup FCLK/2 is set) must elapse between
  /DEV going high and the next /DEV low assertion

### Why IWM for GCR, ISM for MFM

On the original SWIM (SWIM1), the ROM **never uses ISM mode for GCR**. The
division is strict:

- IWM core = GCR encoding/decoding
- ISM core = MFM encoding/decoding

This is because the SWIM1 contains two separate hardware cores connected through
a crossbar. Later SWIM variants changed this (see [SWIM Variants](#swim-variants)).

---

## IWM Mode

When operating in IWM mode, the SWIM presents the same programming model as the
standalone IWM chip. This section summarizes the IWM interface; see
[floppy.md](floppy.md) for complete IWM details specific to the Mac Plus.

### IWM State Machine

The IWM uses a latch-based state machine driven by 8 address lines that control
8 state bits. Each access to a softswitch address both updates the corresponding
state bit and potentially accesses a register.

The 8 state bits are:

| State Bit | Description |
|-----------|-------------|
| CA0 (PHASE0) | Control line 0 to drive |
| CA1 (PHASE1) | Control line 1 to drive |
| CA2 (PHASE2) | Control line 2 to drive |
| LSTRB (PHASE3) | Command strobe to drive |
| ENABLE | Drive enable (activates /ENBL) |
| SELECT | Drive select (internal vs external) |
| Q6 (L6) | Register select bit 0 |
| Q7 (L7) | Register select bit 1 |

### IWM Register Selection

The combination of Q6, Q7, and ENABLE selects which IWM register is accessed:

| L7 | L6 | MotorOn | Read/Write | Register |
|----|-----|---------|------------|----------|
| 0 | 0 | 0 | R | Read All Ones ($FF) |
| 0 | 0 | 1 | R | Read Data |
| 0 | 1 | X | R | Read Status |
| 1 | 0 | X | R | Read Write-Handshake |
| 1 | 1 | 0 | W | Set Mode |
| 1 | 1 | 1 | W | Write Data |

**Critical constraint:** The IWM state machine cannot be traversed by arbitrary
register-to-register paths. Certain transitions cause unintended side effects
(writing garbage data). Two state diagrams govern safe transitions: one for chip
configuration (MotorOn=0) and one for reading/writing (MotorOn=1).

### IWM Mode Register

Write-only. Reset value: 0x00. Current value is mirrored in Status register bits
0–4.

| Bit | Name | 0 = | 1 = |
|-----|------|-----|-----|
| 0 | LATCH | Latch off | Latch mode (MSB=1 detection) |
| 1 | ASYNC | Synchronous handshake | Asynchronous handshake |
| 2 | NOTIMER | 1-sec motor-off delay enabled | Immediate motor-off |
| 3 | FAST | Slow mode (4 us/bit) | Fast mode (2 us/bit) |
| 4 | 8MHZ | FCLK ÷ 7 | FCLK ÷ 8 |
| 5 | TEST | Normal | Test mode (timer counts 10x faster) |
| 6 | ISM/IWM | Part of 4-write handshake | Part of 4-write handshake |
| 7 | — | Reserved | Reserved |

Standard Macintosh setting: **$1F** (latch on, async, no timer, fast, 8 MHz).

**Timer blocking:** When the motor-off timer is counting down (NOTIMER=0 and
motor was recently disabled), writes to the Mode register are blocked for
approximately 1 second. This timer exists for Apple II compatibility and is never
exercised on Macintosh systems (which always set NOTIMER=1).

### IWM Status Register

Read-only.

| Bit | Description |
|-----|-------------|
| 7 | SENSE input (drive register value selected by CA0/CA1/CA2/SEL) |
| 6 | Reserved (0) |
| 5 | /ENBL active (1 when either drive enabled) |
| 4–0 | Mirror of Mode register bits 4–0 |

### IWM Handshake Register

Read-only. Used during write operations to pace the CPU.

| Bit | Description |
|-----|-------------|
| 7 | Write buffer empty (ready for next byte) |
| 6 | Underrun status (1=writing, 0=underrun occurred) |
| 5–0 | Reserved (all 1s) |

### IWM Data Register

**Read path (latched):** With the motor running, the IWM shifts incoming bits
from RDDATA into an internal shift register. When a byte with MSB=1 forms, it
latches into the Data register and clears the shifter. This relies on the
property that all valid GCR bytes have MSB=1. Bytes with MSB=0 never latch.
Reading the Data register clears it to 0.

**Write path:** Writing a byte loads it into the write buffer. When the shift
register becomes empty, the buffer contents transfer to the shift register for
serialization to WRDATA. The CPU must poll the Handshake register to pace writes.

### IWM Limitations

- Single 1-byte shift register with no FIFO buffering
- If the CPU fails to read before the next 8 bits arrive, data is irreversibly
  overwritten
- All interrupts must be disabled during read/write operations
- No hardware CRC support; checksums computed entirely in software
- No mark byte detection; software must scan for prologue patterns byte by byte

---

## ISM Mode

The ISM provides significantly more hardware support than the IWM core,
specifically designed for MFM operations.

### ISM Register Set

In ISM mode, A3 selects read (A3=1) vs write (A3=0). The 16 registers are:

| Addr | Write Register | Read Register |
|------|---------------|---------------|
| 0 | wData | rData |
| 1 | wMark | rMark |
| 2 | wCRC / wIWMConfig | rError |
| 3 | wParameter | rParameter |
| 4 | wPhase | rPhase |
| 5 | wSetup | rSetup |
| 6 | wModeZeros | rStatus (same as addr 14) |
| 7 | wModeOnes | rHandshake (same as addr 15) |
| 8 | — | rData |
| 9 | — | rMark |
| 10 | — | rError |
| 11 | — | rParameter |
| 12 | — | rPhase |
| 13 | — | rSetup |
| 14 | — | rStatus |
| 15 | — | rHandshake |

**Access timing constraint:** Minimum spacing between ISM register accesses is
**4 FCLK cycles** (or 8 FCLK cycles if Setup bit 3 FCLK/2 is set). This
corresponds to approximately 256 ns at 15.6672 MHz.

Unlike the IWM, ISM registers can be accessed in any order without forbidden
paths or side effects.

### ISM ↔ IWM Address Correspondence

The ISM register addresses correspond to IWM state bits as follows:

| ISM Addr | IWM State Equivalent |
|----------|---------------------|
| 0 | PHASE0=0 |
| 1 | PHASE0=1 |
| 2 | PHASE1=0 |
| 3 | PHASE1=1 |
| 4 | PHASE2=0 |
| 5 | PHASE2=1 |
| 6 | PHASE3=0 |
| 7 | PHASE3=1 |
| 8 | MOTORON=0 |
| 9 | MOTORON=1 |
| 10 | DRIVESEL=0 |
| 11 | DRIVESEL=1 |
| 12 | L6=0 |
| 13 | L6=1 |
| 14 | L7=0 |
| 15 | L7=1 |

### IWM Status Register Access in ISM Mode

While in ISM mode, software can still read the **IWM status register** by
accessing register address 14 with the IWM Q6/Q7 line state set to Q6=1, Q7=0.
This backward-compatible path is used by MacTest and ROM diagnostic code to read
drive sense registers (TACH/INDEX, /DRVEXIST, /CSTIN, etc.) while the SWIM is in
ISM mode.

The access path is:

1. Software touches ISM address 13 (L6 set) and address 14 (L7 clear)
2. This puts the internal Q6/Q7 lines in the Q6=1, Q7=0 state
3. Any subsequent read from ISM addresses dispatches through the **IWM status
   register** path, returning bit 7 = SENSE (the drive status register selected
   by CA0/CA1/CA2/SEL)

The SWIM chip's internal crossbar enables this: the Q6/Q7 state machine is shared
between the IWM and ISM cores, and the Q6=1/Q7=0 combination always routes to the
IWM status readback path regardless of which core is active.

This is particularly important for the TACH/INDEX signal: MacTest's MEASURE_SPEED
function enters ISM mode (via SWIM_ENABLE), then reads the INDEX signal by polling
IWM status bit 7 through this backward-compatible path.

### Data Register (rData / wData) [Address 0/8]

**Read (ACTION=1):** Returns the next byte from the FIFO. If the byte is a mark
byte, reading it from the Data register (rather than the Mark register) sets
error bit 1. If the FIFO is empty, returns **$FF** and sets error bit 2
(underrun).

**Read (ACTION=0):** Returns correction register values. Two consecutive reads
return the even-transition error byte and odd-transition error byte respectively.

**Write (ACTION=1):** Pushes a data byte into the FIFO for writing to disk. If
the FIFO is full, sets overrun error.

### Mark Register (rMark / wMark) [Address 1/9]

**Read:** Returns the next mark byte from the FIFO without triggering a mark
error. This is the correct way to read mark bytes.

**Write:** Pushes a byte with a dropped clock transition (mark byte) into the
FIFO. Used to write address marks ($A1 for MFM, etc.).

### CRC Register (wCRC) [Address 2, ACTION=1]

Writing any value to this register pushes an **M_CRC token** into the FIFO. This
token is queued behind any existing data bytes. When the FIFO serializer
encounters this token during output, it emits exactly 2 CRC-CCITT bytes computed
over all data since the last CRC initialization (which occurs on FIFO clear).

If the FIFO is full when the CRC write occurs, an overrun error is set.

### IWM Configuration Register (wIWMConfig) [Address 2, ACTION=0]

Modifies IWM-mode behavior via bits 5–7:

| Bit | Name | Description |
|-----|------|-------------|
| 5 | Latch IWM Async as Sync | D7 latched in async mode as if synchronous |
| 6 | 16M/8M | IWM timer takes 2x longer when set |
| 7 | Override IWM timer | Kill timer via MotorOn=0 + toggle drive select |

### Parameter Register (rParameter / wParameter) [Address 3/11]

Access to the 16-byte parameter RAM (see [Parameter RAM](#parameter-ram)). Uses
an auto-incrementing index counter. The counter resets to 0 on any access to the
Mode-zeros register (address 6) or on /RESET.

### Phase Register (rPhase / wPhase) [Address 4/12]

Reset value: **$F0** (all phases configured as outputs, all low).

| Bits | Description |
|------|-------------|
| 7–4 | Direction: 0=input, 1=output for PHASE3–PHASE0 |
| 3–0 | State: current level of PHASE3–PHASE0 |

Phase line levels carry over between IWM and ISM mode switches without glitches.
The drive sees no discontinuity when modes are switched.

In ISM mode, the LSTRB pulse for drive commands is generated by software
manipulating the Phase register directly:

1. OR Phase3 + Phase3 enable bits into wPhase (assert high + enable output)
2. Wait (VIA delay)
3. AND ~Phase3 into wPhase (deassert, keep enable)

### Setup Register (rSetup / wSetup) [Address 5/13]

Reset value: **$00**.

| Bit | Name | Description |
|-----|------|-------------|
| 0 | Q3/HDSEL | 0=Q3 input for IWM data latching; 1=HDSEL output |
| 1 | 3.5SEL | Output is inverse of bit value |
| 2 | GCR/Normal | 1=GCR mode (bypass MFM-specific logic) |
| 3 | FCLK/2 | Divide clock by 2 (for 800K compatibility) |
| 4 | Enable ECM | Enable Error Correction Machine |
| 5 | IBM/Apple | 1=pulse-based sensing (IBM drives); 0=transition-based (Apple) |
| 6 | Bypass TSM | Bypass Trans-Space Machine (must set for GCR) |
| 7 | Enable Timer | Enable MotorOn timer (~0.5 sec at 16 MHz); requires ACTION toggle to activate |

For MFM operation, the typical setup is: ECM enabled (bit 4=1), TSM not bypassed
(bit 6=0), HDSEL enabled (bit 0=1).

For GCR operation via ISM (SWIM2 only): GCR bit set (bit 2=1), TSM bypassed
(bit 6=1).

### Mode / Status Register [Address 6/7 write, 14 read]

The Mode register uses a split-write mechanism: writing to address 6 **clears**
specified bits (write zeros), writing to address 7 **sets** specified bits (write
ones). Reading address 14 returns the current status.

Reset value: **$00** (but overall SWIM resets to mode byte $40 indicating IWM
mode; the ISM Mode register itself resets to $00).

| Bit | Name | Description |
|-----|------|-------------|
| 0 | Clear FIFO | Toggle 1→0 to clear FIFO, init CRC, clear errors |
| 1 | Enable Drive 1 | Asserts /ENBL1 (with MotorOn) |
| 2 | Enable Drive 2 | Asserts /ENBL2 (with MotorOn) |
| 3 | ACTION | Start read/write operation (set last after preloading FIFO) |
| 4 | Write/Read | 0=Read mode, 1=Write mode |
| 5 | HDSEL | Head select output (if Setup bit 0 set) |
| 6 | ISM/IWM | Clearing this bit switches back to IWM mode |
| 7 | MotorOn | Enables /ENBL outputs; must set before ACTION |

**FIFO Clear sequence:**
1. Set the Read/Write mode bit (bit 4) first
2. Set bit 0 to 1
3. Clear bit 0 to 0

This sequence: zeros both FIFO bytes, resets FIFO position to empty, initializes
CRC accumulator to $FFFF, clears the error register, and resets the shift
register.

**ACTION behavior:**
- For reads: ISM enters mark-searching state. Incoming data is examined
  byte-by-byte but **not placed into the FIFO** until a mark byte pattern is
  recognized. The first byte available after ACTION is always a mark byte.
- For writes: At least 1 byte must be preloaded into the FIFO before setting
  ACTION.
- ACTION is automatically cleared on write errors but **not** on read errors.

**MotorOn must be disabled before switching to IWM mode.**

### Error Register (rError) [Address 10]

See [Error Register](#error-register) section.

### Handshake Register (rHandshake) [Address 15]

Read-only status register for real-time transfer coordination.

| Bit | Name | Description |
|-----|------|-------------|
| 0 | Mark | Next byte in FIFO is a mark byte |
| 1 | CRC Error | 0 = CRC over received data + CRC bytes is not zero |
| 2 | RDDATA | Current state of RDDATA input pin |
| 3 | SENSE | Current state of SENSE input pin |
| 4 | MotorOn | 1 if MotorOn set or motor-off timer active |
| 5 | Error | 1 if any error register bit is set |
| 6 | Two Ready | Read: 2 bytes in FIFO; Write: 2 bytes empty in FIFO |
| 7 | One Ready | Read: ≥1 byte in FIFO; Write: ≥1 byte empty in FIFO |

**Bit 7 is the primary polling target** for data transfer loops. Software polls
this bit (BPL loop on the byte) waiting for it to go high before reading or
writing data.

**Bit 3 (SENSE)** reflects the instantaneous state of the drive's SENSE/RD line
for whichever drive register is currently addressed by the CA0/CA1/CA2/SEL
lines. This is the ISM-mode equivalent of IWM Status register bit 7. In MacTest
and other diagnostic software, this bit is used to poll the TACH/INDEX signal
(register 0111) during speed measurements. The CA lines must be configured for
the desired sense key before reading this bit.

**Bit 1 (CRC Error)** is valid on the second CRC byte. After reading a complete
field including both CRC bytes, this bit indicates whether the CRC matched (1=OK,
0=error).

The DAT1BYTE pin on the 44-pin PLCC package directly reflects bit 7.

### FIFO Architecture

The ISM contains a **2-byte deep FIFO** (`fifo[0]` and `fifo[1]`). This doubles
the allowable CPU response latency compared to the IWM's single-byte buffer.

For MFM at 500 kbit/s, a new byte arrives every **16 microseconds**. The 2-byte
FIFO extends the allowable latency to approximately **32 microseconds** before
overrun occurs.

The FIFO position tracks how many bytes are currently buffered (0, 1, or 2).

---

## Parameter RAM

The ISM contains 16 bytes of parameter RAM that control the timing of read data
recovery and write pre-compensation. These parameters are loaded by software and
must be configured before any MFM read or write operation.

### Parameter Index and Auto-Increment

Parameters are accessed through the Parameter register (address 3/11). An
internal index counter auto-increments after each access. The counter resets to 0
when:

- The Mode-zeros register (address 6) is accessed, or
- /RESET is asserted

### Read Timing Parameters (Bytes 0x0–0xB)

These parameters define the boundary thresholds used by the ISM's data separator
to classify incoming flux transitions as "short" (2 us), "medium" (3 us), or
"long" (4 us) cells.

| Index | Name | Purpose |
|-------|------|---------|
| 0x0 | MIN | Minimum cell time lower bound |
| 0x1 | MULT | Correction multiplier for error correction calibration |
| 0x2 | SSL | Short-Short-Long boundary |
| 0x3 | SSS | Short-Short-Short boundary |
| 0x4 | SLL | Short-Long-Long boundary |
| 0x5 | SLS | Short-Long-Short boundary |
| 0x6 | RPT | Repeat: upper limit for longest legal cell |
| 0x7 | CSLS | Compensating SLS (used when previous "long" resolves as "short") |
| 0x8 | LSL | Long-Short-Long boundary |
| 0x9 | LSS | Long-Short-Short boundary |
| 0xA | LLL | Long-Long-Long boundary |
| 0xB | LLS | Long-Long-Short boundary |

In the parameter names, the three letters encode **previous cell / current cell /
boundary type**:

- **S** = Short (2 us nominal for MFM)
- **L** = Long (3 us or 4 us nominal for MFM)

The "boundary type" letter indicates the decision threshold:

- **S** = classify current cell as short
- **L** = classify current cell as long

### Write Timing Parameters (Bytes 0xC–0xF)

| Index | Name | Purpose |
|-------|------|---------|
| 0xC | LATE(7–4) / NORMAL(3–0) | Write pre-compensation: late shift / normal timing |
| 0xD | TIME0 | Write "0" bit timing (delay for no-pulse cell) |
| 0xE | EARLY(7–4) / NORMAL(3–0) | Write pre-compensation: early shift / normal timing |
| 0xF | TIME1 | Write "1" bit timing (delay for pulse cell) |

### Half-Clock Encoding

All parameter values are stored in **half-clock units**: internal counters
decrement on both rising and falling clock edges. This effectively doubles the
timer resolution compared to whole-clock counting.

**Internal delay compensation:** The hardware subtracts fixed pipeline delays:

- MIN: reduced by 3 clocks (6 half-clocks)
- xSx/xLx boundary parameters: reduced by 2 clocks (4 half-clocks)
- TIME0/TIME1: reduced by 2 clocks (4 half-clocks)

**Rounding rules for encoding real-valued half-clock counts:**

| Fractional Part | Encoding |
|----------------|----------|
| .00 ≤ f < .25 | Round down: 2i + 0 |
| .25 ≤ f < .75 | Round to .50: 2i + 1 |
| .75 ≤ f ≤ .99 | Round up: 2(i+1) |

### Calculation Formulas

Given flux transition spacings t2 (short), t3 (medium), t4 (long) in FCLK
cycles, and peak shift t_ps:

```
t_MIN     = t2 / 2
t_MULT    = (implementation-specific calibration value)
t_SSL     = (t2 + t3) / 2
t_SSS     = (t2 + t3) / 2 - t_ps
t_SLL     = (t3 + t4) / 2 - t_ps
t_SLS     = (t3 + t4) / 2 - 2 * t_ps
t_RPT     = 3/4 * t2       (upper limit timeout)
t_CSLS    = (compensating variant of SLS)
t_LSL     = (t2 + t3) / 2 + t_ps
t_LSS     = (t2 + t3) / 2
t_LLL     = (t3 + t4) / 2
t_LLS     = (t3 + t4) / 2 - t_ps
t_TIME0   = t3 - t2
t_TIME1   = t2
t_NORMAL  = max(t2 / 4, 7)
```

For Macintosh SuperDrives, better results are obtained by ignoring peak shift in
the xSx/xLx parameters and factoring it back into Sxx only: approximately 1
clock for SSx parameters and 0.5 clock for SLx parameters.

### Example: 15.6672 MHz, MFM 1440K

At 15.6672 MHz with 2/3/4 us nominal cell timings:

- t2 = 31.33 clocks, t3 = 47.00 clocks, t4 = 62.67 clocks
- Peak shift t_ps = 1.57 clocks
- Pre-compensation t_pc = 1.96 → 2.0 clocks

**Actual Parameter RAM values loaded by System 7.1 (in auto-increment order,
index 0xF first):**

```
Raw bytes written: $18 $41 $2E $2E $18 $18 $1B $1B $2F $2F $19 $19 $97 $1B $57 $3B
```

Reversed to index order (0x0 first):

| Index | Parameter | Value | Decoded |
|-------|-----------|-------|---------|
| 0x0 | MIN | $18 | 24 half-clocks |
| 0x1 | MULT | $41 | 65 |
| 0x2 | SSL | $2E | 46 half-clocks |
| 0x3 | SSS | $2E | 46 half-clocks |
| 0x4 | SLL | $18 | 24 half-clocks |
| 0x5 | SLS | $18 | 24 half-clocks |
| 0x6 | RPT | $1B | 27 half-clocks |
| 0x7 | CSLS | $1B | 27 half-clocks |
| 0x8 | LSL | $2F | 47 half-clocks |
| 0x9 | LSS | $2F | 47 half-clocks |
| 0xA | LLL | $19 | 25 half-clocks |
| 0xB | LLS | $19 | 25 half-clocks |
| 0xC | LATE/NORMAL | $97 | Late=9, Normal=7 |
| 0xD | TIME0 | $1B | 27 half-clocks |
| 0xE | EARLY/NORMAL | $57 | Early=5, Normal=7 |
| 0xF | TIME1 | $3B | 59 half-clocks |

### GCR Parameter Values (15.6672 MHz)

For GCR operation via ISM (SWIM2 only, not used on SWIM1):

| Index | Parameter | Value |
|-------|-----------|-------|
| 0x0 | MIN | $1A |
| 0x1 | MULT | $40 |
| 0x2–0xB | All boundaries | $3C |
| 0xC | LATE/NORMAL | $88 |
| 0xD | TIME0 | $3C |
| 0xE | EARLY/NORMAL | $88 |
| 0xF | TIME1 | $3C |

---

## Trans-Space Machine

The Trans-Space Machine (TSM) converts parallel data bytes into the serial MFM
bit pattern for writing to disk. It is controlled by Setup register bit 6: when
bit 6 = 0 the TSM is active (for MFM); when bit 6 = 1 the TSM is bypassed (for
GCR).

### MFM Encoding Rules

MFM (Modified Frequency Modulation) uses two types of flux transitions:

- **Data pulse:** A "1" data bit produces a pulse in the center of its bit cell
- **Clock pulse:** Two adjacent "0" data bits produce a clock pulse on their
  shared boundary

The TSM implements this as a lookup from current and next data bits to a
"trans-space" output:

| Current Bit | Next Bit | Trans-Space Output |
|-------------|----------|-------------------|
| 0 | 0 | 1 (clock pulse) |
| 0 | 1 | 0, 1 (space then data pulse) |
| 1 | 0 | 0 (space only) |
| 1 | 1 | 1 (data pulse) |

### Output to Write Timing

The trans-space output maps to the write timing parameters:

- **"0" (space):** Delay TIME0 FCLK cycles (no pulse emitted)
- **"1" (pulse):** Delay TIME1 FCLK cycles, then emit a flux transition

For standard MFM with 2/3/4 us cell timings:

- 2 us cell = TIME1 alone
- 3 us cell = TIME1 + TIME0
- 4 us cell = TIME1 + TIME0 + TIME0

### Mark Byte Generation

Mark bytes are special MFM patterns with deliberately dropped clock pulses that
cannot occur in normal data. The TSM detects the bit pattern "1000" within a mark
byte and drops the clock bit before the last zero, creating an illegal spacing.

Two types of mark bytes exist in MFM:

**Address/Data mark ($A1):** Contains a dropped clock pulse that creates a 4 us
gap starting with "0" instead of the normal "101" pattern. Binary $A1 =
10100001. The missing clock occurs between bits 4 and 3.

**Index mark ($C2):** Contains a dropped clock in a different position within a
four-zero run. Binary $C2 = 11000010.

These illegal patterns allow the controller to unambiguously identify the start
of address and data fields, even in the presence of data that might otherwise
look like a mark sequence.

---

## Error Correction Machine

The Error Correction Machine (ECM) compensates for analog signal degradation by
measuring timing errors in the incoming flux transitions and providing correction
values. It is enabled by Setup register bit 4.

### Operation

When ACTION is set for a read operation, the ECM:

1. Searches for 32 consecutive pairs of minimum-length cells (these appear in the
   $00 sync field before mark bytes)
2. Checks whether the first non-minimum cell belongs to a mark byte; if not,
   restarts the search
3. Tracks the timing error for both cells in each pair (alternating positive and
   negative magnetic transitions)
4. Computes separate error values for even-transition and odd-transition timing

### Correction Register

Two consecutive reads of the Data register (with ACTION=0) return:

1. **Even transitions error byte**
2. **Odd transitions error byte**

Value interpretation:

- 0–192: cells are too long by this amount (in half-clock units)
- 193–255: cells are too short by (256 − value) half-clock units

### Using Correction Values

The average of the two error bytes represents the overall speed error. The
corrected clock frequency is:

```
FCLK' = FCLK × (1 + corrAvg / 256)
```

Software should recalculate all parameter RAM values using FCLK' and reload them
before reading sector data.

### Error Conditions

The ECM sets error register bits for:

- **Bit 3:** Correction error (correction value exceeds acceptable range)
- **Bit 4:** Transition too narrow (before MIN cell time expired)
- **Bit 5:** Transition too wide (no transition before MIN + xSx + xLx + RPT
  timeout)
- **Bit 6:** Unresolved transition (three marginal transitions in a row)

---

## CRC Generation

The SWIM uses CRC-CCITT-16 for MFM data integrity checking.

### Polynomial

```
G(x) = x^16 + x^12 + x^5 + 1
```

This is the standard CCITT polynomial, encoded as **0x1021**.

### Initialization

The CRC accumulator is initialized to **$FFFF** (all ones) whenever the FIFO is
cleared (via the Mode register bit 0 toggle). The initialization value is the
same for both read and write operations.

### Per-Bit Algorithm

For each data bit processed:

```
xorBit = CRC[15] XOR dataBit
CRC = CRC << 1
CRC[0] = 0
if xorBit:
    CRC[5]  = CRC[5] XOR 1     (x^5 tap)
    CRC[12] = CRC[12] XOR 1    (x^12 tap)
    CRC[0]  = 1                 (x^0 tap)
```

Equivalently, this is a standard CRC-CCITT-16 MSB-first computation.

### Verification

When receiving data, running the received data bytes plus the two CRC bytes
through the CRC generator produces a zero remainder if the data is error-free.
The Handshake register bit 1 reflects this: it reads 1 when the CRC is zero
(valid) after processing the second CRC byte.

### CRC Insertion During Writes

Writing to the wCRC register (address 2, ACTION=1) enqueues a CRC token in the
FIFO. When the serializer encounters this token, it emits the current 2-byte CRC
value. The CRC covers all data bytes since the last FIFO clear.

Typical write sequence for one MFM field:

```
Clear FIFO (inits CRC to $FFFF)
Write mark bytes ($A1 × 3) via wMark
Write field ID byte ($FE/$FB) via wData
Write field data bytes via wData
Write to wCRC (queues 2 CRC bytes)
Write gap bytes via wData
Clear ACTION
```

---

## Drive Communication Protocol

### Signal Architecture

The Sony/SuperDrive uses a multiplexed communication protocol with:

**6 inputs to drive:**

| Signal | Source | Description |
|--------|--------|-------------|
| CA0 | PHASE 0 | Control address bit 0 |
| CA1 | PHASE 1 | Control address bit 1 |
| CA2 | PHASE 2 | Control address bit 2 |
| LSTRB | PHASE 3 | Command latch strobe |
| /ENBL | /ENBL1 or /ENBL2 | Drive enable (active low) |
| SEL/HDSEL | HDSEL pin | Head select / address bit 3 |

**1 output from drive:**

| Signal | Destination | Description |
|--------|-------------|-------------|
| RD | SENSE + RDDATA | Multiplexed status/data output |

The single RD output is multiplexed: it carries either a status bit or read data,
depending on the address set by CA0/CA1/CA2/SEL.

### Address Encoding

The 4-bit drive register address is encoded as: **CA1–CA0–SEL–CA2**

### Sense Registers (Read)

Drive status is read by setting CA0/CA1/CA2/SEL to select the desired register,
then reading SENSE (IWM Status bit 7) or Handshake bit 3 (ISM mode).

| Addr | CA2-CA1-CA0-SEL | Register | Description |
|------|-----------------|----------|-------------|
| 0 | 0-0-0-0 | DIRTN | Current step direction |
| 1 | 0-0-0-1 | /CSTIN | 0 = disk in place |
| 2 | 0-0-1-0 | /STEP | 0 = head currently stepping |
| 3 | 0-0-1-1 | /WRTPRT | 0 = disk write-protected |
| 4 | 0-1-0-0 | /MOTORON | 0 = motor running |
| 5 | 0-1-0-1 | mfmDrv | 1 = SuperDrive (FDHD capable) |
| 6 | 0-1-1-0 | /WRTPRT | 0 = write protected (alternate) |
| 8 | 1-0-0-0 | RDDATA0 | Read data, lower head |
| 9 | 1-0-0-1 | RDDATA1 | Read data, upper head |
| 10 | 1-1-0-0 | SIDES | Drive capability: 0 = single-sided, 1 = double-sided |
| 11 | 1-1-0-1 | /READY | 0 = drive ready |
| 13 | 1-1-1-0 | /DRVIN | 0 = drive exists |
| 14 | 1-1-1-1 | TACH/INDEX | See [Tachometer / INDEX Signal](#tachometer--index-signal) |
| 15 | — | NEWINTF | New interface / twoMeg sense |

**Register 0111 (TACH/INDEX) dual-mode behavior:** This register changes its
function depending on the drive's operating mode. In GCR mode, it reports the
high-frequency FG tachometer signal (60 pulses/revolution). In ISM mode with
motor on, it reports a low-frequency INDEX signal (2 pulses/revolution). See
[Tachometer / INDEX Signal](#tachometer--index-signal) for full details.

### Control Commands (Write)

Commands are sent by setting the address lines and pulsing LSTRB. The CA2 line
carries the command value (0 or 1) for the selected latch.

| Addr | Bits | Command | Description |
|------|------|---------|-------------|
| 0 | 0-0-0-0 | DIRTN=0 | Direction inward (toward higher tracks) |
| 1 | 0-0-0-1 | DIRTN=1 | Direction outward (toward track 0) |
| 4 | 0-1-0-0 | STEP low | Step (assert) |
| 5 | 0-1-0-1 | STEP high | Step (acknowledge/release) |
| 6 | 0-1-1-0 | MFM mode | Switch drive to MFM mode |
| 7 | 0-1-1-1 | GCR mode | Switch drive to GCR mode |
| 8 | 1-0-0-0 | Motor ON | Start disk motor |
| 9 | 1-0-0-1 | Motor OFF | Stop disk motor |
| 12 | 1-1-0-0 | Eject low | Eject (assert) |
| 13 | 1-1-0-1 | Eject high | Eject (release) |

### LSTRB Pulse Generation

**In IWM mode:** The LSTRB pulse is generated by toggling the Phase 3 softswitch:

1. Write to Ph3H address (LSTRB goes high)
2. Wait (VIA delay, approximately 1 µs minimum)
3. Write to Ph3L address (LSTRB goes low)

**In ISM mode:** The LSTRB pulse is generated by manipulating the Phase register:

1. OR Phase3 state bit + Phase3 output-enable bit into wPhase
2. Wait (VIA delay)
3. AND ~Phase3 state bit into wPhase (clear data, keep output enabled)

### Command Protocol

The full sequence for issuing a drive command:

1. Set CA2 to the **value** to write into the selected latch (0 or 1)
2. Set CA0, CA1, and SEL to select which latch
3. Pulse LSTRB high then low (minimum 1 µs pulse width)

Constraints:

- CA0, CA1, CA2, SEL must not change while LSTRB is high
- CA0 and CA1 must be returned to 1 before changing SEL
- EJECT is unlatched (write-only) and always reads as 1

### Step Sequence

To step the head one track:

1. Set DIRTN (command 0 or 1) for desired direction
2. Assert STEP low (command 4)
3. Assert STEP high / acknowledge (command 5)
4. Wait for step completion (~12 ms per step)

The drive internally counts the step pulse and moves the head one track in the
set direction.

### Eject Sequence

The standard Macintosh eject procedure:

1. Seek to track 40 (reduce mechanical stress)
2. Motor off
3. Select drive
4. Wait 200 ms
5. Assert eject (LSTRB high for 750 ms)
6. Release eject

---

## Head Select

### SWIM-Era Head Selection

On SWIM-equipped Macintosh systems (SE/30, IIx, IIcx), the HDSEL line is
controlled **exclusively by the SWIM chip**. There is no OR gate, no multiplexer,
and no VIA port B interaction for head selection.

**In ISM mode:** Bit 5 of the Mode register directly controls the HDSEL output
pin (when Setup bit 0 enables HDSEL mode on the Q3/HDSEL pin).

**In IWM mode:** The SWIM translates the CA1/CA2 state combinations to drive the
HDSEL line, maintaining compatibility with the IWM softswitch programming model.

This differs from the Mac Plus, where VIA Port A bit 5 controls the SEL line to
the drive for head selection.

---

## Motor and Speed Control

### SuperDrive Spindle Motor Architecture

The SuperDrive (Sony MP-F75W) uses a **4-pole brushless DC pancake motor** with
the following sensing elements:

| Component | Function |
|-----------|----------|
| 3 Hall-effect sensors | 6-step commutation (120° spacing) |
| FG (frequency generator) winding | 60-pulse/revolution tachometer for speed control |
| Inductive index sensor | 1 index pulse/revolution for sector positioning |

The FG winding is a zig-zag trace on the motor PCB, underneath the rotor magnet.
The rotor carries a ring magnet with 4 alternating N-S poles (2 pole pairs). The
FG winding produces 60 electrical pulses per mechanical revolution.

The motor controller IC (internal to the drive) uses the FG signal for closed-loop
speed regulation. In GCR mode (variable speed), the controller adjusts RPM based
on the PWM speed signal from the host. In MFM mode (fixed 300 RPM), the
controller locks to 300 RPM regardless of the PWM input.

### GCR Mode: Variable Speed (CLV)

For GCR disks (400K/800K), the drive operates in **Constant Linear Velocity**
mode. The motor speed varies by track zone to maintain a constant data rate at
the read/write head:

| Tracks | Zone | RPM | Data Rate |
|--------|------|-----|-----------|
| 0–15 | 0 | 394 | 489.6 kbit/s |
| 16–31 | 1 | 429 | 489.6 kbit/s |
| 32–47 | 2 | 472 | 489.6 kbit/s |
| 48–63 | 3 | 525 | 489.6 kbit/s |
| 64–79 | 4 | 590 | 489.6 kbit/s |

Speed is controlled by a PWM signal:

- TTL-level signal at 20–40 kHz (typically 22 kHz with dithering)
- Duty cycle range: 9.4% to 91%
- Generated by the ASG (Analog Signal Generator) reading the disk speed buffer in
  RAM

The TACH signal from the drive provides feedback: **60 pulses per revolution**,
50% duty ±10%. Software uses TACH to verify the motor has reached the correct
speed for the current zone.

### MFM Mode: Constant Speed

For MFM disks (720K/1440K), the drive operates at a constant **300 RPM**. No
speed changes are needed when seeking across tracks. The data rate is a constant
**500 kbit/s** for 1440K and **250 kbit/s** for 720K.

In MFM mode (or more precisely: when the SWIM is in ISM mode with motor on),
drive register 0111 switches from the 60-pulse FG tachometer to the INDEX signal.
See [Tachometer / INDEX Signal](#tachometer--index-signal) for timing details.

### Motor-Off Timer

The SWIM includes a motor-off timer that counts **2^23 = 8,388,608 FCLK cycles**
(approximately 1.07 seconds at 7.8336 MHz, or approximately 0.5 seconds at
15.6672 MHz).

The timer is controlled by:

- **IWM Mode bit 2 (NOTIMER):** 0 = timer active, 1 = immediate motor-off
- **ISM Setup bit 7:** Enable MotorOn timer

During the countdown, writes to the Mode register are blocked.

**Macintosh usage:** All Macintosh systems set NOTIMER=1 during initialization,
so the timer is **never exercised** on Macintosh hardware. It exists solely for
Apple II compatibility (IIc, IIgs with 5.25" drives). An emulator targeting only
Macintosh software can treat the timer as a no-op.

---

## GCR Disk Format

### Geometry

GCR disks use a variable number of sectors per track, organized into 5 speed
zones. This is possible because the drive varies its rotational speed by zone
(CLV), allowing more sectors on the longer outer tracks.

| Tracks | Zone | Sectors/Track | RPM | Track Bytes | Total Sectors |
|--------|------|---------------|-----|-------------|---------------|
| 0–15 | 0 | 12 | 394 | 9,320 | 192 |
| 16–31 | 1 | 11 | 429 | 8,559 | 176 |
| 32–47 | 2 | 10 | 472 | 7,780 | 160 |
| 48–63 | 3 | 9 | 525 | 6,994 | 144 |
| 64–79 | 4 | 8 | 590 | 6,224 | 128 |

**Single-sided (400K):** 80 tracks × 1 side = 800 sectors × 512 bytes = 400 KB

**Double-sided (800K):** 80 tracks × 2 sides = 1,600 sectors × 512 bytes = 800 KB

### GCR Encoding

GCR (Group Code Recording) encodes data such that:

- A "1" bit produces a flux transition (NRZI: transition = 1, no transition = 0)
- Bit cell timing: 2 µs in fast mode
- Maximum two consecutive "0" bits allowed (three possible pulse spacings: 2 µs,
  4 µs, 6 µs)
- MSB of every encoded byte must be "1" (used for byte boundary detection)

There are **81 valid GCR byte values** (bytes with MSB=1 and no more than one
pair of consecutive zeros). Of these:

- 2 are reserved as mark bytes: **$D5** and **$AA**
- 64 are used for data encoding (6-bit to 8-bit mapping)
- 15 are unused

### GCR Codeword Table

Maps 6-bit nibbles (0x00–0x3F) to 8-bit GCR bytes:

| Offset | Codewords |
|--------|-----------|
| 0x00 | 96, 97, 9A, 9B, 9D, 9E, 9F, A6 |
| 0x08 | A7, AB, AC, AD, AE, AF, B2, B3 |
| 0x10 | B4, B5, B6, B7, B9, BA, BB, BC |
| 0x18 | BD, BE, BF, CB, CD, CE, CF, D3 |
| 0x20 | D6, D7, D9, DA, DB, DC, DD, DE |
| 0x28 | DF, E5, E6, E7, E9, EA, EB, EC |
| 0x30 | ED, EE, EF, F2, F3, F4, F5, F6 |
| 0x38 | F7, F9, FA, FB, FC, FD, FE, FF |

### Nibblization (3-to-4 Encoding)

Data bytes are grouped into sets of three (BYTEA, BYTEB, BYTEC) and encoded into
four 6-bit nibbles:

```
NIBL1 = A7 A6 B7 B6 C7 C6    (high 2 bits of each byte)
NIBL2 = A5 A4 A3 A2 A1 A0    (low 6 bits of BYTEA)
NIBL3 = B5 B4 B3 B2 B1 B0    (low 6 bits of BYTEB)
NIBL4 = C5 C4 C3 C2 C1 C0    (low 6 bits of BYTEC)
```

Each nibble is then encoded through the GCR codeword table to produce a disk
byte. Thus 3 data bytes become 4 disk bytes (33% overhead).

### GCR Checksum Algorithm

Three accumulator registers (CSUMA, CSUMB, CSUMC) are maintained during encoding
and decoding. For each 3-byte group:

```
1. Rotate CSUMC left by 1 bit (bit 7 wraps to bit 0 and carry)
2. CSUMA += BYTEA + carry from step 1
3. BYTEA ^= CSUMC
4. CSUMB += BYTEB + carry from step 2
5. BYTEB ^= CSUMA
6. CSUMC += BYTEC + carry from step 4
7. BYTEC ^= CSUMB
8. Nibblize and encode the XORed BYTEA/BYTEB/BYTEC
```

After all data groups, the three checksum bytes (CSUMA, CSUMB, CSUMC) are
themselves nibblized and written as a 4-byte group.

### GCR Sector Format

Each sector consists of four fields:

**Header Sync Field (6.25 bytes):**

```
FF 3F CF F3 FC FF
```

Self-synchronizing bit-slip pattern. Should be made as large as possible during
formatting to buffer speed variations.

**Header Field (11 bytes):**

| Bytes | Content |
|-------|---------|
| D5 AA 96 | Address marks (header prologue) |
| 1 byte | Track number (low 6 bits, GCR encoded) |
| 1 byte | Sector number (GCR encoded) |
| 1 byte | Side: bit 5=side, bits 0–4=high track bits (GCR encoded) |
| 1 byte | Format: bit 5=sides flag, bits 0–4=interleave factor (GCR encoded) |
| 1 byte | Checksum: XOR of Track, Sector, Side, Format (GCR encoded) |
| DE AA | Bit slip marks (epilogue) |
| 1 byte | Pad (write electronics off) |

Format byte values:

- $02 = single-sided, 2:1 interleave
- $22 = double-sided, 2:1 interleave

**Data Sync Field (6.25 bytes):**

```
FF 3F CF F3 FC FF
```

Written every time the data field is written (not just during formatting).

**Data Field (710 bytes):**

| Bytes | Content |
|-------|---------|
| D5 AA AD | Data marks (data prologue) |
| 1 byte | Sector number (GCR encoded) |
| 699 bytes | 524 data bytes (12 tag + 512 user) nibblized to 699 GCR bytes |
| 4 bytes | 24-bit checksum nibblized to 4 GCR bytes |
| DE AA | Bit slip marks (epilogue) |
| 1 byte | Pad (write electronics off) |

The 524 data bytes = 12-byte tag field + 512-byte user data. These are encoded as
4 × ⌈524/3⌉ = 699 data nibbles + 3 checksum bytes → 4 GCR bytes = 703 total
GCR bytes in the data portion.

### Block Numbering

**Single-sided (400K):** Sequential by track:

```
block = cumulative_sectors_before_track + sector
```

| Tracks | Sectors/Track | Cumulative at Start |
|--------|---------------|-------------------|
| 0–15 | 12 | track × 12 |
| 16–31 | 11 | 192 + (track − 16) × 11 |
| 32–47 | 10 | 368 + (track − 32) × 10 |
| 48–63 | 9 | 528 + (track − 48) × 9 |
| 64–79 | 8 | 672 + (track − 64) × 8 |

**Double-sided (800K):** Cylinder-interleaved (both sides of one track together):

```
Blocks 0–11:   Side 0, Track 0
Blocks 12–23:  Side 1, Track 0
Blocks 24–35:  Side 0, Track 1
Blocks 36–47:  Side 1, Track 1
...
```

Formula:

```
cylinder_sectors = 2 × sectors_per_track
offset = block mod cylinder_sectors
track  = block ÷ cylinder_sectors
side   = offset ÷ sectors_per_track
sector = offset mod sectors_per_track
```

### Sector Interleave (2:1)

Sectors are physically ordered on the track with 2:1 interleave to give the CPU
time to process one sector before the next arrives:

| Zone (Tracks) | Sectors | Physical Order |
|---------------|---------|----------------|
| 0–15 | 12 | 0, 6, 1, 7, 2, 8, 3, 9, 4, 10, 5, 11 |
| 16–31 | 11 | 0, 6, 1, 7, 2, 8, 3, 9, 4, 10, 5 |
| 32–47 | 10 | 0, 5, 1, 6, 2, 7, 3, 8, 4, 9 |
| 48–63 | 9 | 0, 5, 1, 6, 2, 7, 3, 8, 4 |
| 64–79 | 8 | 0, 4, 1, 5, 2, 6, 3, 7 |

---

## MFM Disk Format

### Geometry

MFM disks use a **constant number of sectors per track** (the drive spins at a
fixed 300 RPM):

| Format | Sectors/Track | Sides | Tracks | Total Sectors | Capacity |
|--------|---------------|-------|--------|---------------|----------|
| 720K | 9 | 2 | 80 | 1,440 | 720 KB |
| 1440K | 18 | 2 | 80 | 2,880 | 1,440 KB |

720K and 1440K use the same MFM encoding; the difference is media density:

- **720K:** DD (double-density) media, same physical media as 800K GCR disks
- **1440K:** HD (high-density) media, requires SuperDrive (FDHD) with HD-capable
  head

### MFM Encoding

MFM uses clock and data pulses within fixed-width bit cells:

- A "1" data bit always produces a pulse in the **center** of its bit cell
- A "0" data bit produces no center pulse
- Between two adjacent "0" bits, a **clock pulse** is inserted at their shared
  boundary

This creates three possible pulse spacings (in nominal 2 µs bit cells):

| Spacing | Pattern | Description |
|---------|---------|-------------|
| 2 µs | 11 or data 00→clock | Two consecutive data pulses or clock+data |
| 3 µs | 01 or 10 | Data-to-clock or clock-to-data |
| 4 µs | 101 | Isolated clock between two "1" data bits |

Nominal bit cell: 2 µs. Data rate: 500 kbit/s (1440K) or 250 kbit/s (720K).

### Peak Shift

Peak shift is an electromagnetic phenomenon where closely-spaced flux transitions
push apart, while widely-spaced transitions are pulled closer together. This
affects the apparent timing of pulse transitions:

- Short cells (2 µs) appear slightly wider
- Long cells (4 µs) appear slightly narrower

The SWIM compensates for peak shift on reads via the parameter RAM boundary
thresholds, and on writes via pre-compensation (shifting outgoing pulses
early or late by approximately 125 ns at 15.6672 MHz).

### MFM Mark Bytes

Mark bytes are special patterns with deliberately missing clock pulses. These
patterns cannot occur in normal MFM-encoded data and serve as unambiguous field
delimiters.

**Address/Data mark $A1 (with missing clock):**

$A1 in binary: 10100001. In normal MFM encoding, there would be a clock pulse
between bits 4 and 3 (both zeros). The mark version **drops this clock**, creating
a 4 µs gap where only a 3 µs gap should exist. This illegal pattern is
detectable by hardware.

**Index mark $C2 (with missing clock):**

$C2 in binary: 11000010. The missing clock occurs at a different position in the
four-zero run.

Three consecutive $A1 marks (or $C2 marks) precede each field identifier:

- Three $C2 marks + $FC = index field
- Three $A1 marks + $FE = address (ID) field
- Three $A1 marks + $FB = data field

### MFM Track Format

```
[80 bytes] Gap 4a: $4E
[12 bytes] Sync: $00
[4 bytes]  Index mark: mark-$C2, mark-$C2, mark-$C2, $FC
[50 bytes] Gap 1: $4E

FOR EACH SECTOR (9 or 18 sectors):
  [12 bytes] Sync: $00
  [4 bytes]  ID Address Mark: mark-$A1, mark-$A1, mark-$A1, $FE
  [1 byte]   Track number
  [1 byte]   Side number (0 or 1)
  [1 byte]   Sector number (1-based)
  [1 byte]   Block size code ($02 = 512 bytes)
  [2 bytes]  CRC (over mark bytes + ID field)
  [22 bytes] Gap 2 (intra-sector): $4E
  [12 bytes] Sync: $00
  [4 bytes]  Data Address Mark: mark-$A1, mark-$A1, mark-$A1, $FB
  [512 bytes] Sector data
  [2 bytes]  CRC (over mark bytes + data)
  [84/101 bytes] Gap 3 (inter-sector): $4E
```

Gap 3 sizes:

- 720K (9 sectors): 84 bytes
- 1440K (18 sectors): 101 bytes

**Key differences from GCR:**

- No 12-byte tag field (MFM sectors contain only 512 bytes of user data)
- Block size byte is always $02 (meaning 512 bytes) for both 720K and 1440K
- CRC-CCITT-16 replaces GCR's XOR-based checksums
- Fixed sectors per track (no zones)
- Sector numbers are 1-based (1 through 9 or 1 through 18)

### MFM Block-to-Track Conversion

```
quotient = block ÷ sectors_per_track
side     = quotient & 1
track    = quotient >> 1
sector   = (block mod sectors_per_track) + 1     (1-based)
```

---

## Disk Format Detection

Detecting what kind of disk is inserted requires three layers of probing:

### Layer 1: Drive Capability

Read the `mfmDrv` sense register (address 5, CA0=1 CA1=0 CA2=0 SEL=1). If this
returns 1, the drive is a SuperDrive capable of reading both GCR and MFM disks.
If 0, the drive is an 800K-only drive that supports only GCR.

### Layer 2: Media Type (HD vs DD)

Read the `NEWINTF` / `twoMeg` sense register (address 15). If this returns 1, the
disk has the HD hole (high-density media). HD media is **exclusively** used for
1440K MFM — it is never formatted as GCR.

### Layer 3: Software Probing for DD Media

DD media (no HD hole) can be formatted as either 800K GCR or 720K MFM. Software
must probe:

1. **Try GCR first:** Stay in IWM mode, search for GCR address mark prologue
   ($D5 $AA $96). If found, the disk is 800K GCR.
2. **Try MFM:** If GCR fails, switch to ISM mode, issue the `SETMFM` command
   (write to drive register 6: CA0=0 CA1=1 CA2=1 SEL=0), configure parameter
   RAM for MFM, and search for MFM address marks ($A1 with missing clock). If
   found, the disk is 720K MFM.

---

## Read Operations

### GCR Read (IWM Mode)

GCR reading uses the IWM core's single-byte shift register with software-driven
mark detection:

1. **Seek and spin:** Step to target track, wait for head settle (~12 ms per step
   + 30 ms settle). Start motor, wait ~400 ms for speed.
2. **Enable data path:** Assert ENABLE, set Q6=0 Q7=0 for Data register read.
3. **Poll for data:** Read the Data register repeatedly. A valid byte is present
   when bit 7 = 1 (the MSB=1 latch property). The Data register clears to 0
   after reading.
4. **Header scan:** Search for the prologue sequence $D5 $AA $96 in the byte
   stream. Parse the header fields (track, sector, side, format, checksum).
   Verify checksum via XOR. Validate epilogue bytes $DE $AA.
5. **Data scan:** Continue reading until data prologue $D5 $AA $AD is found.
6. **Decode data:** Read 699 GCR bytes, de-nibblize back to 524 bytes (12 tag +
   512 data). Verify the 3-byte checksum. Validate epilogue bytes $DE $AA.

**Critical constraint:** All interrupts must be disabled (`ORI #$0700, SR`) during
the read loop. The IWM has no FIFO — if the CPU misses a single byte, the data
is lost.

### MFM Read (ISM Mode)

MFM reading uses the ISM's hardware mark detection and FIFO:

1. **Seek and spin:** Step to target track, wait for head settle. Motor at
   constant 300 RPM.
2. **Switch to ISM mode:** Execute the 4-write handshake.
3. **Configure:** Load parameter RAM with MFM timing values. Set Setup register
   (enable ECM, TSM active, HDSEL enabled). Select drive and side via Mode
   register.
4. **Clear FIFO:** Toggle Mode bit 0 (1→0) to clear FIFO and init CRC.
5. **Set ACTION:** Set Mode bit 3. The ISM begins searching for mark bytes in the
   incoming data stream.

   During mark search:
   - Incoming data is examined byte-by-byte but **not placed into the FIFO**
   - Handshake bit 7 remains 0
   - When the ISM detects the mark byte pattern ($A1 with missing clock), the
     mark byte enters the FIFO

6. **Read mark bytes:** Poll Handshake bit 7 (BPL loop). When it goes high, read
   the mark byte via rMark register (not rData — reading a mark from rData sets
   error bit 1). Expect three $A1 bytes followed by $FE (address field) or $FB
   (data field).
7. **Read field data:** Read track, side, sector, block-size bytes via rData.
   Read 2 CRC bytes via rData.
8. **Check CRC:** After reading the second CRC byte, check Handshake bit 1. If
   1, CRC is valid.
9. **Read data field:** If the address field matches the desired sector, continue
   to the next mark sequence ($A1 $A1 $A1 $FB), then read 512 data bytes via
   rData, followed by 2 CRC bytes. Verify CRC.

**Real-time constraint:** With the 2-byte FIFO, the CPU has approximately 32 µs
to respond to each byte at 500 kbit/s. Interrupts must still be masked during
the transfer loop.

### FIFO Empty Behavior

Reading the Data register when the FIFO is empty returns **$FF** (sentinel value)
and sets error bit 2 (underrun). Software should never rely on reading an empty
FIFO — always poll Handshake bit 7 first.

---

## Write Operations

### GCR Write (IWM Mode)

1. Seek to target track, start motor, wait for speed
2. Enable data path in write mode (Q6=1, Q7=1)
3. Write the complete sector stream byte-by-byte:
   - Header sync field ($FF self-sync bytes)
   - Header prologue ($D5 $AA $96)
   - Encoded header fields
   - Header epilogue ($DE $AA) + pad byte
   - Data sync field
   - Data prologue ($D5 $AA $AD)
   - Encoded data (699 nibblized bytes)
   - Encoded checksum (4 bytes)
   - Data epilogue ($DE $AA) + pad byte
4. Between each byte, poll the Handshake register bit 7 (write buffer empty)
5. After the final byte, wait for Underrun status (bit 6 goes from 1 to 0),
   confirming the shift register has emptied

### MFM Write (ISM Mode)

1. Configure ISM for write: Set Mode bit 4 (Write), select drive and side
2. Clear FIFO (toggle Mode bit 0, initializes CRC to $FFFF)
3. **Preload FIFO:** Write at least 1 byte to the FIFO before setting ACTION
4. Set ACTION (Mode bit 3)
5. Write the field:
   - Sync bytes ($00) via wData
   - Mark bytes ($A1) via wMark — the TSM handles the missing clock
     automatically
   - Field ID byte ($FE or $FB) via wData
   - Field content via wData
   - CRC insertion via wCRC — enqueues 2 CRC bytes computed over all data since
     FIFO clear
   - Gap bytes ($4E) via wData
6. Clear ACTION when done

### Write Pre-Compensation

The SWIM applies write pre-compensation automatically based on the LATE, EARLY,
and NORMAL values in parameter RAM bytes 0xC and 0xE. The TSM shifts outgoing
flux transition pulses:

- **Early:** When the next transition is close (would cause peak shift to push
  them apart), shift the current pulse earlier
- **Late:** When the previous transition was close, shift the current pulse later
- **Normal:** No shift applied

The pre-compensation amount is approximately 125 nanoseconds for standard MFM at
15.6672 MHz (about 2 FCLK cycles).

---

## Timing Specifications

### Mechanical Timing

| Operation | Timing | Notes |
|-----------|--------|-------|
| Step (track to track) | 12 ms | Maximum slew rate |
| Head settle | 30 ms max | After last step, before read/write |
| Zone speed change | 150 ms | Motor RPM transition between GCR zones |
| Motor spin-up | 400 ms | From off to stable RPM |
| Eject pulse | 750 ms | LSTRB must stay high for this duration |
| Post-eject wait | 200 ms | Before eject LSTRB assertion |
| Sector-to-sector | 10 ms | Rotational latency between adjacent sectors |
| VBL task interval | 500 ms | Periodic drive status check |
| Auto power-off | 2.5 sec | Motor auto-off if no activity |

### Data Rates

| Format | Bit Rate | Byte Rate | Byte Period |
|--------|----------|-----------|-------------|
| GCR 400K/800K | 489.6 kbit/s | 61,200 bytes/s | 16.3 µs |
| MFM 720K | 250 kbit/s | 31,250 bytes/s | 32.0 µs |
| MFM 1440K | 500 kbit/s | 62,500 bytes/s | 16.0 µs |

### Tachometer / INDEX Signal

Drive register 0111 (`CA0=1, CA1=1, SEL=1`, sense key `$0B`) is a **dual-mode
signal** that changes its function depending on whether the SWIM is in IWM
(GCR) mode or ISM mode with motor on.

#### GCR Mode: 60-Pulse FG Tachometer

In IWM/GCR mode, the register outputs the **frequency generator (FG) winding**
signal from the SuperDrive's brushless DC spindle motor: **60 pulses per
revolution**, ~50% duty cycle ±10%, pulse period accuracy ±0.2%.

This high-frequency signal is used by the ROM's `P_Sony_MakeSpdTbl` routines to
measure motor speed and generate speed correction values for each GCR speed zone.

| Zone | Tracks | RPM | TACH half-period | ROM speed bounds (decimal) |
|------|--------|-----|------------------|---------------------------|
| 0 | 0–15 | 394 | ~1.27 ms | 4,405 – 4,585 |
| 1 | 16–31 | 429 | ~1.17 ms | 4,806 – 5,002 |
| 2 | 32–47 | 472 | ~1.06 ms | 5,287 – 5,503 |
| 3 | 48–63 | 525 | ~0.95 ms | 5,874 – 6,114 |
| 4 | 64–79 | 590 | ~0.85 ms | 6,608 – 6,878 |

(Speed bounds from the Floppy Emu firmware, credited to Apple/Sony docs.)

**Flutter note:** The 128K/512K ROM's `P_Sony_MakeSpdTbl` has a divide-by-zero
bug that crashes (Sad Mac 0F0004) if two successive speed measurements return
identical values. A ~0.25% amplitude flutter at ~640 ms period is standard
defensive practice in emulators. Later ROMs (SE/30 etc.) may not have this bug
but flutter remains realistic behavior.

#### ISM Mode: INDEX Signal (2 Pulses per Revolution)

When the SWIM is in ISM mode with `ISM_MODE_MOTOR_ON` set, the same register
switches to a **low-frequency INDEX signal**. This was discovered by Steve
Chamberlin (Floppy Emu / Big Mess o' Wires) through logic analyzer traces of a
Macintosh LC formatting a 1440K floppy disk, and independently corroborated by
the System 7.1 source code (`SonyMFM.a` `mDoFormat` routine).

The SuperDrive produces **2 INDEX pulses per revolution** at a fixed 300 RPM,
giving a **100 ms period** per complete pulse cycle.

**Asymmetric duty cycle:** The INDEX pulse is a short HIGH spike (~2 ms) produced
by the inductive sensor detecting the hub index mark on the disk, followed by a
long LOW phase (~98 ms). The asymmetry is critical for the MacTest diagnostic's
MEASURE_SPEED function (see below).

**Signal characteristics:**

| Parameter | Value |
|-----------|-------|
| Pulses per revolution | 2 |
| Motor speed | 300 RPM (fixed) |
| Revolution period | 200 ms |
| Pulse cycle period | 100 ms |
| HIGH (index mark) | ~2 ms |
| LOW (between marks) | ~98 ms |

#### SWIM Passes the Signal Unmodified

The SWIM chip does **not** gate, divide, or filter the TACH/INDEX signal. The
drive's RD output for register 0111 connects to the IWM/SWIM's SENSE input, which
appears as bit 7 of the IWM status register (in IWM mode) or bit 3 of the ISM
handshake register (in ISM mode). The CPU reads the **instantaneous raw state**
of the signal. All timing, counting, and speed calculation is performed entirely
in software by the CPU, typically using VIA Timer 2.

#### INDEX Signal Speed Measurement

The MacTest SE/30 diagnostic at `$392C0` (MEASURE_SPEED) measures the INDEX
signal period using VIA T2 overflow counting. The result is a 32-bit composite
value: `D5 * 65536 + timer_delta`, where D5 counts VIA T2 wraps (one wrap =
65,536 VIA ticks = ~83.7 ms at 783 kHz) and timer_delta is the remaining elapsed
VIA ticks.

The measurement algorithm:

1. Align to falling edge (wait for INDEX=0)
2. Reset VIA T2
3. Poll INDEX with timer edge detection until INDEX=1 (rising edge):
   - During the ~98 ms LOW phase, VIA T2 wraps once (at ~83.7 ms), so D5=1
4. Wait for INDEX=0 (falling edge): ~2 ms HIGH pulse
5. Read timer: total elapsed ~100 ms = ~78,336 VIA ticks
6. Result = 1 × 65536 + 12,800 = 78,336

The MacTest bounds for this measurement are **[77,125 – 79,475]**, corresponding
to 100 ms ±1.5% (standard floppy speed tolerance). The calculated midpoint value
of **78,336** (at exactly 300 RPM) sits almost exactly at the center.

**Why the asymmetric duty cycle matters:** The D5 wrap counter only increments
during the wait-for-INDEX=1 loop (the LOW phase). For D5 to reach 1, the LOW
phase must exceed 65,536 VIA ticks (~83.7 ms). A symmetric 50/50 duty cycle
(50 ms each) would produce D5=0 and a result of only ~39,168 — far below the
expected bounds. The ~98 ms LOW phase of the real INDEX signal ensures exactly
one timer wrap.

### Drive Specifications

| Parameter | Value |
|-----------|-------|
| Motor speed range | 394–590 RPM (GCR variable) / 300 RPM (MFM constant) |
| Total tracks | 80 (numbered 0–79) |
| Track pitch | — |
| Flux transition timing | 1.89–6.36 µs (GCR) |
| FG tach signal (GCR) | 60 pulses/revolution, ~50% duty cycle |
| INDEX signal (MFM) | 2 pulses/revolution, ~2% duty cycle (~2 ms HIGH) |
| Motor type | 4-pole brushless DC with FG winding + 3 Hall sensors |

---

## Reset State

On /RESET or power-on, all SWIM registers initialize to known values:

| Register / State | Reset Value | Notes |
|-----------------|-------------|-------|
| Mode (overall) | $40 | Bit 6 set = IWM-compatible mode |
| ISM Mode | $00 | All bits clear |
| Setup | $00 | All features disabled |
| Phase | $F0 | All phases as outputs, all low |
| Parameter RAM | All zeros | Must be loaded before MFM operation |
| Parameter index | 0 | Auto-increment counter reset |
| FIFO[0], FIFO[1] | $00 | Both FIFO slots empty |
| FIFO position | 0 | Empty |
| CRC | $FFFF | Standard initialization |
| Error | $00 | No errors |
| Shift register | $00 | Clear |
| DEVSEL | 0 | No drive selected |
| SEL35 | true | — |
| HDSEL | false | Side 0 selected |

After reset, the chip is in IWM mode. Software must execute the 4-write
handshake to enter ISM mode for MFM operations.

---

## Error Register

### Reading and Clearing

The Error register (ISM address 10, read) uses **read-clear** semantics: reading
the register atomically returns the current value and clears it to zero.

### First-Error-Wins Latching

Once any error bit is set, **all subsequent errors are silently discarded** until
the register is read (and cleared). There is no fixed priority order — whichever
error condition occurs first wins.

### Error Bit Definitions

| Bit | Value | Name | Condition |
|-----|-------|------|-----------|
| 0 | $01 | Underrun | CPU too slow providing write data; FIFO emptied |
| 1 | $02 | Mark Error | Mark byte read from Data register instead of Mark register |
| 2 | $04 | Overrun | CPU too slow consuming read data; FIFO overflowed |
| 3 | $08 | Correction Error | ECM correction value out of acceptable range |
| 4 | $10 | Too Narrow | Flux transition before MIN cell time |
| 5 | $20 | Too Wide | No transition before MIN + boundary + RPT timeout |
| 6 | $40 | Unresolved | Three consecutive marginal transitions |
| 7 | — | Reserved | Always 0 |

### Practical Notes

- Underrun and overrun are mutually exclusive in practice (one applies during
  writes, the other during reads)
- Mark error (bit 1) is the most commonly encountered error in normal operation;
  it occurs when software reads a mark byte from the wrong register
- Error bits 3–6 are only relevant when the Error Correction Machine is enabled

---

## SWIM Detection

System software must detect whether the installed chip is an IWM or SWIM.
System 7.1 uses the following procedure (called `Check4SWIM`):

1. Save the current IWM mode state
2. Write the 4-write magic sequence ($57, $17, $57, $57 to the Q7H address) to
   attempt ISM mode entry
3. Write a test value ($F5) to wPhase, then read it back from rPhase
4. If the read-back matches: SWIM detected (set `isSWIM = $FF`)
5. Switch back to IWM mode by writing $F8 to wZeroes (clears Mode bit 6)
6. Restore IWM mode state

**Detection result values:**

| Value | Meaning |
|-------|---------|
| $00 | IWM (original chip, no ISM support) |
| $FF | SWIM (original) |
| $FE | SWIM2 |
| $01 | No chip detected |

---

## Real-Time Constraints

### No DMA

The original SWIM has **zero DMA capability**. The 68030 CPU must execute a rigid
polling loop to transfer every byte to/from the FIFO. This has profound
implications:

- The OS must mask **all interrupts** during floppy communication:
  ```
  ORI #$0700, SR    ; Set IPL to 7, mask all interrupts
  ```
- If any interrupt distracts the CPU for even a few microseconds during an active
  read or write, the FIFO overflows or underflows, causing a failed sector
- System responsiveness is completely suspended during disk I/O

### Timing Budget

| Format | Byte Period | FIFO Depth | Effective Deadline |
|--------|-------------|------------|-------------------|
| GCR 800K (IWM) | 16.3 µs | 1 byte | 16.3 µs |
| MFM 1440K (ISM) | 16.0 µs | 2 bytes | 32.0 µs |
| MFM 720K (ISM) | 32.0 µs | 2 bytes | 64.0 µs |

### ISM Access Pacing

A minimum of **4 FCLK cycles** (approximately 256 ns at 15.6672 MHz) must elapse
between consecutive ISM register accesses. If Setup bit 3 (FCLK/2) is set, this
doubles to **8 FCLK cycles**. Violating this constraint produces undefined
behavior.

---

## SWIM Variants

### SWIM (Original, 1988)

- Part numbers: 344S0061-A, 344S0062
- Dual-core architecture: IWM + ISM with crossbar
- IWM for GCR, ISM for MFM only
- No DMA, 2-byte FIFO
- Full error correction machine and write pre-compensation
- Machines: IIx, IIcx, SE/30

### ASIC Integration

Some later Apple ASICs integrate SWIM-compatible floppy control without a
standalone SWIM package. For example, the **Sonora** chip integrates the
functions of V8 and SWIM for the Macintosh LC II.

### SWIM2 (Cost-Reduced)

- Unified single core (ISM only, with GCR support via S_GCR bit in Setup)
- ISM bus interface retained
- Supports 16 MHz and 32 MHz clock (high-speed MFM up to 1 Mbit/s)
- Removes ISM error correction and post-compensation
- Removes "half clocking" complexity
- The three "special bits" via CRC register with ACTION=0 (Override, M16/8,
  Modify) are no-ops or repurposed
- No DMA
- Machines: Centris 650, Quadra 800, and other 68040-era Macs

### SWIM3 (DMA-Capable)

- Adds **DMA and interrupts** (NEC 765-style DMA model)
- New pins: DMAreq, DMAack, IntReq
- **Hardware head positioning:** Step register with hardware loop, 80 µs
  inter-step delay, generates step_done interrupt
- **Read control:** Hardware searches for mark bytes, detects ID fields, updates
  current track/sector registers, generates interrupts
- **Multi-sector DMA transfers:** When desired sector ID matches, SWIM3
  automatically starts DMA for 512 bytes (MFM)
- **Write protocol:** Uses $99 escape code system where the next byte is a
  command:
  - Write $A1/$C2 mark bytes
  - Write CRC
  - Disable escaping for 512 data bytes
  - End transfer
- Machines: Power Macintosh systems

### Compatibility Implications

When targeting SWIM1 (SE/30, IIx, IIcx):

- GCR operations must use IWM mode exclusively
- ISM mode is MFM-only
- No DMA — all transfers are PIO with interrupts disabled
- Full parameter RAM configuration required for MFM
- Error correction machine is available and should be enabled for MFM reads

---

## Edge Cases and Quirks

### Mode Switching Hazards

- MotorOn **must** be low before switching from ISM to IWM mode
- After switching to IWM mode, the **first action** must be to clear L7 (read
  address 14) to reset the IWM state machine
- The 4-write handshake sequence is deliberately obscure to prevent legacy GCR
  software from accidentally entering ISM mode
- /DEV timing constraints apply during mode switches

### Timer Blocking

When the motor-off timer is counting down (IWM Mode bit 2 NOTIMER=0):

- **Writes to the Mode register are blocked** for up to ~1 second
- In test mode (bit 5=1), the timer runs 10x faster (~100 ms)
- Macintosh systems avoid this by always setting NOTIMER=1

### Phase Line Stability

Phase line levels are preserved across IWM↔ISM mode switches. The physical drive
sees no glitch or discontinuity. This is important because drive latches retain
their state — an accidental phase change during a mode switch could issue an
unintended drive command.

### FIFO Preload Requirement

For ISM write operations, at least **1 byte must be preloaded** into the FIFO
before setting the ACTION bit. Failure to preload will cause an immediate
underrun.

### Mark Byte Handling

- The first byte available after setting ACTION for a read is always a mark byte
- Mark bytes **must** be read via the rMark register; reading them via rData sets
  error bit 1
- The Handshake register bit 0 indicates when the next byte is a mark byte
- During mark search, incoming data is examined but not buffered — the FIFO stays
  empty and Handshake bit 7 stays 0

### CRC Token Queuing

Writing to wCRC does not immediately emit CRC bytes. It pushes an **M_CRC token**
into the FIFO queue. The CRC bytes are emitted only when the serializer reaches
that token. If the FIFO is full when the CRC write occurs, an overrun error is
set.

### Error Register Race

Due to first-error-wins latching, if multiple error conditions occur
simultaneously or in rapid succession, only the first one is captured. All
subsequent errors are silently lost until the register is read and cleared.

### TACH/INDEX Register Dual-Mode Behavior

Drive register 0111 (sense key `$0B`) is the most poorly documented register in
the SuperDrive interface. Its behavior changes based on the SWIM's operating mode:

- **IWM mode:** Reports the FG tachometer — 60 high-frequency pulses per
  revolution, used for GCR speed zone measurement
- **ISM mode (motor on):** Reports the INDEX signal — 2 low-frequency pulses per
  revolution with asymmetric duty cycle (~2 ms HIGH, ~98 ms LOW)

The mode switch happens at the drive level: when the SWIM enters ISM mode and
asserts motor on, the drive's internal multiplexer routes the INDEX sensor output
to register 0111 instead of the FG tach output. The SWIM chip itself passes the
signal through unchanged.

This dual-mode behavior is the source of significant confusion in emulation. The
GCR speed bounds (~4,400–6,900) and MFM speed bounds (77,125–79,475) use
completely different measurement regimes on the same register address.

**Key constraints for emulation:**

1. The INDEX LOW phase must exceed ~83.7 ms (65,536 VIA ticks at 783 kHz) to
   trigger exactly one VIA T2 overflow in the MacTest MEASURE_SPEED algorithm
2. A symmetric 50/50 duty cycle produces incorrect results — the real signal is
   highly asymmetric
3. The signal must be based on `scheduler_time_ns()` (CPU-cycle-derived time) to
   stay synchronized with VIA T2 timer reads, which also use CPU cycles

### /DRVEXIST ISM-Mode Behavior

Drive register `$0D` (/DRVEXIST) also has mode-dependent behavior. In ISM mode,
it returns 1 (drive present). In IWM compatibility mode, it returns 0. The MacTest
`CHECK_DRIVE_STATUS` function validates this by reading the register twice: once
in ISM mode (expects 1) and once after switching to IWM mode (expects 0).

### Copy Protection

Some copy-protection schemes directly access IWM hardware registers, bypassing
the ROM's Sony driver. These schemes may:

- Use non-standard mode register settings
- Manipulate GCR encoding/decoding timing
- Depend on exact byte positions within tracks
- Rely on specific gap sizes or sync field lengths

An accurate emulator must faithfully reproduce the IWM register behavior at the
cycle level to support such software.

### IOP-Mediated Systems

On the Macintosh IIfx, the SWIM is controlled by a dedicated I/O Processor (IOP)
with its own DMA RAM. The main CPU communicates with the IOP, not the SWIM
directly. Copy-protection schemes that directly access IWM registers may fail on
IOP-mediated architectures. This does not apply to SE/30, IIx, or IIcx, where
the CPU accesses the SWIM directly.

### POST Error Code

POST error code **$0C** indicates the system was unable to access the IWM/SWIM
chip during startup diagnostics.
