# Macintosh Plus Floppy Subsystem

**Contents:**

1. [Introduction](#introduction) — Purpose, IWM overview, Sony drive,
   terminology
2. [CPU-visible Interface](#macintosh-plus-cpu-visible-interface) — Softswitch
   addresses, drive registers, commands
3. [IWM Internal Registers](#iwm-internal-registers) — Mode, Status, Handshake,
   Data
4. [Disk Geometry](#disk-geometry) — Zones, track layout, block numbering,
   interleave
5. [Sector Format](#sector-format) — Header/data fields, GCR encoding
6. [Read/Write Algorithms](#readwrite-algorithms) — Practical sequences
7. [Edge Cases and Quirks](#edge-cases-timing-and-quirks) — Timing, special
   behaviors
8. [Emulator Implementation Notes](#practical-emulator-implementation-notes)

---

## Introduction

### Purpose and Scope

This document describes the floppy disk subsystem of the Macintosh Plus, with
the explicit goal of enabling the implementation of a behaviorally exact
emulator of the subsystem. The scope includes both sides of the hardware
boundary:

- The **IWM (Integrated Woz Machine)** floppy controller chip as seen by the CPU
- The **Sony 3.5-inch microfloppy drive mechanism** as seen by the IWM

The Macintosh disk interface uses three main components:

- **IWM**: reads/writes/format/eject logic, and controls most “disk
  state-control lines” (CA0/CA1/CA2/LSTRB, enable, drive select).
- **VIA**: provides the `SEL` line (used as “head select” for double-sided
  reads, and also part of status multiplexing).
- **ASG (Analog Signal Generator)**: reads the “disk speed buffer” in RAM and
  generates a PWM signal used to control the drive motor speed (for zoned CLV).

Unlike higher-level disk documentation, this text focuses on **register-level
behavior, timing, signaling conventions, and physical assumptions** that are
observable by software. These details matter for ROM correctness,
copy-protection compatibility, and accurate modeling of edge cases such as motor
timing, head stepping delays, and raw GCR bitstreams.

This documentation targets the **Macintosh 128K, 512K, and Plus**, using 400 KB
and 800 KB GCR disks. Later systems using **SWIM** and **MFM (1.44 MB
“SuperDrive”)** technology are explicitly out of scope.

---

### What Is the IWM?

**IWM** stands for **Integrated Woz Machine**. The name is a direct reference to
**Steve Wozniak**, who designed the original Apple II Disk II controller. That
controller was famous for its minimal hardware and heavy reliance on clever
software timing.

The IWM is the evolutionary successor to the Disk II controller:

- It preserves the **same conceptual programming model** (phase lines, Q6/Q7
  state machine, software-visible timing effects).
- It integrates functionality that previously required discrete logic into a
  **single custom chip**.
- It adds support for **3.5-inch drives**, variable bit timing, and higher-level
  handshake modes while remaining backward-compatible with Apple II software
  conventions.

Internally, the IWM is _not_ a “disk controller” in the modern sense. It does
**not** understand tracks, sectors, filesystems, or even bytes on disk in a
semantic way. Instead, it provides:

- A **bit-serial read/write datapath**
- A small number of **mode and status registers**
- A **software-controlled state machine** driven by Q6/Q7
- Direct access to **one-bit signals** coming from (and going to) the drive

The Macintosh ROM (the so-called _Sony driver_) performs all higher-level
tasks—seeking, synchronization, GCR decoding, checksum verification, and retry
logic—on top of this primitive interface .

From an emulator perspective, the IWM should be thought of as a
**timing-sensitive I/O device**, not a block-based disk controller.

---

### Why “Sony”?

The term **“Sony drive”** appears throughout Macintosh documentation, ROM
symbols, and historical discussions. This is not a generic label—it refers to
the **actual manufacturer** of the 3.5-inch floppy drive mechanism used in early
Macintosh computers.

Apple sourced its original 3.5-inch microfloppy drives from **Sony**, at a time
when the format itself was still emerging. As a result:

- The Macintosh floppy subsystem was designed **around the electrical and
  mechanical behavior of Sony’s drive**, not around an industry-standard
  controller abstraction.
- The ROM disk driver is historically named the **Sony driver**, and this name
  persists even in later documentation and code.
- Many observable behaviors—such as step timing, tachometer pulse rates, eject
  timing, and motor characteristics—come directly from Sony’s hardware
  specifications rather than Apple-defined standards .

Importantly, the Sony drive is **not a “smart” drive**. It does not perform
sectoring, encoding, or buffering. Instead, it exposes:

- A **single raw read-data line**
- Several **one-bit status signals** (disk present, write protect, track-0,
  tachometer, etc.)
- A small set of **latched control inputs** (step, direction, motor on/off,
  eject)

All interpretation of the magnetic data on disk—including byte framing and error
handling—is performed by the IWM and system software.

---

### A Tight Hardware–Software Co-Design

The Macintosh floppy subsystem is best understood as a **co-designed system**
rather than a clean layering of controller → drive → media.

Key characteristics that matter for emulation:

- **GCR encoding and IWM latch behavior** are intentionally matched: all valid
  GCR bytes have their MSB set, which the IWM uses to detect byte boundaries.
- **Variable-speed (CLV) operation** means that rotational speed changes by
  track zone; timing cannot be modeled with a single constant RPM.
- **Motor control and stepping delays** are observable to software and used by
  the ROM for synchronization and error recovery.
- The ROM depends on **exact status bit semantics**, including active-low
  conventions and transient states.
- The disk interface is implemented by the IWM, VIA, and ASG modules as
  described above.

As a result, an accurate emulator must model:

- The IWM and the Sony drive as **separate but interacting components**
- The **latency and ordering** of signal changes
- The distinction between **raw bitstream behavior** and higher-level decoded
  data

---

### Terminology Used in This Document

To avoid ambiguity, the following conventions are used throughout:

- **IWM** refers specifically to the Integrated Woz Machine chip.
- **Drive** or **Sony drive** refers to the 3.5-inch microfloppy mechanism and
  its interface logic, not to any software driver.
- **Sony driver** refers to the Macintosh ROM disk driver that controls the IWM
  and interprets disk data.
- **Sector**, **track**, and **zone** refer to _on-disk logical constructs_,
  implemented entirely in software.
- **Raw data** refers to the GCR-encoded byte stream as seen at the IWM data
  register, prior to decoding.

---

### Signal Conventions

#### Active-low naming

Many signals in the Sony spec are written with a leading slash, e.g. `/ENBL`,
`/STEP`, `/TACH`. In the text below:

- `/SIGNAL = 0` usually means **asserted/active**
- `/SIGNAL = 1` usually means **deasserted/inactive**

Example: The drive only communicates when `/ENBL` is low.

#### “RD”, “RDDATA”, and “SENSE”

The Sony drive has **one output line**, `RD`, which is **multiplexed** to output
either:

- a **status bit** (e.g., `/TK0`, `/WRTPRT`, `/TACH`, etc), or
- **read data** (`RDDATA`) from a selected head.

This selection is done with `CA0`, `CA1`, `CA2`, and `SEL`.

The Macintosh does not read `RD` directly in software; it reads the IWM’s
**SENSE input**, exposed as bit 7 of the **IWM status register**.

Conceptually:

```
Drive(RD) ──> IWM(SENSE) ──> StatusRegister.bit7 ──> 68000 reads byte
```

---

## Macintosh Plus CPU-visible interface

### IWM softswitch address map

The IWM is on the **lower byte** of the 68000 data bus, so software uses **odd
addresses** for byte accesses.

The Macintosh Plus maps the classic “softswitch” style control to fixed
addresses:

| Function | “Off” address | “On” address |
| -------- | ------------: | -----------: |
| CA0      |    `0xDFE1FF` |   `0xDFE3FF` |
| CA1      |    `0xDFE5FF` |   `0xDFE7FF` |
| CA2      |    `0xDFE9FF` |   `0xDFEBFF` |
| LSTRB    |    `0xDFEDFF` |   `0xDFEFFF` |
| ENABLE   |    `0xDFF1FF` |   `0xDFF3FF` |
| SELECT   |    `0xDFF5FF` |   `0xDFF7FF` |
| Q6       |    `0xDFF9FF` |   `0xDFFBFF` |
| Q7       |    `0xDFFDFF` |   `0xDFFFFF` |

Every access to one of these softswitch locations should:

1. Update the corresponding **IWM state bit** (phase line, motor/enable, drive
   select, Q6/Q7), and
2. Potentially **access an IWM register** (data/status/mode/handshake) depending
   on Q6/Q7 and R/W direction (see later sections).

This “state bit + register access simultaneously” behavior is core IWM design.

- **SELECT**: chooses which physical drive is being addressed (e.g., internal vs
  external).
- **ENABLE (/ENBL)**: must be active for _any_ drive communication; when
  disabled, RD is high-Z and latches reset to inactive states.

**Enable State Behavior (/ENBL high):**

When `/ENBL` goes high (drive disabled), the drive enters a known quiescent
state:

- **RD output**: Goes to high impedance (floats to logic 1 via IWM internal
  pullup)
- **DIRTN latch**: Retains last direction setting (not reset)
- **MOTORON latch**: Preset to inactive (motor off, reads as 1)
- **STEP latch**: Preset to inactive (not stepping, reads as 1)
- **Eject mechanism**: Any in-progress eject continues; new ejects cannot start

"Inactive states" means the drive defaults to a safe, non-operating condition:
motor stopped, not stepping, ready to accept new commands when re-enabled. The
direction latch is explicitly _not_ reset, preserving the last step direction
for subsequent operations.

The interface of the individual drive is **6 inputs** (`SEL`, `CA2`, `CA1`,
`CA0`, `/ENBL`, `LSTRB`) and **one output** (`RD`). `/ENBL` must be low for any
communication.

### Reading registers

When `/ENBL` is low, the host reads `RD` by setting `SEL,CA2,CA1,CA0` to choose
which internal signal is driven onto `RD`:

| CA2 | CA1 | CA0 | SEL | Register addressed | Information in register        |
| --- | --- | --- | --- | ------------------ | ------------------------------ |
| 0   | 0   | 0   | 0   | DIRTN              | Head step direction            |
| 0   | 0   | 0   | 1   | /CSTIN             | Disk in place                  |
| 0   | 0   | 1   | 0   | /STEP              | Set to 0 when head is stepping |
| 0   | 0   | 1   | 1   | /WRTPRT            | Disk locked                    |
| 0   | 1   | 0   | 0   | /MOTORON           | Disk motor running             |
| 0   | 1   | 0   | 1   | /TKO               | Head at track 0                |
| 0   | 1   | 1   | 1   | /TACH              | Tachometer                     |
| 1   | 0   | 0   | 0   | RDDATA0            | Read data, lower head          |
| 1   | 0   | 0   | 1   | RDDATA1            | Read data, upper head          |
| 1   | 1   | 0   | 0   | SIDES              | Single- or double-sided drive  |
| 1   | 1   | 0   | 1   | /READY             | Disk ready for reading/writing |
| 1   | 1   | 1   | 0   | /DRVIN             | = 0 when drive connected       |
| 1   | 1   | 1   | 1   | NEWINTF            | implements ready handshake?    |

### Writing commands

The host can send commands to control stepping direction, motor, and eject.

**Command write sequence:** SELECT drive → set CA0/1/2 & SEL for command →
ENABLE on → pulse **LSTRB on → off** (≥1 µs).

| CA2 | CA1 | CA0 | SEL | Command  | Effect                                        |
| --- | --- | --- | --- | -------- | --------------------------------------------- |
| 0   | 0   | 0   | 0   | DIRTN    | Set step direction toward higher track #      |
| 1   | 0   | 0   | 0   | DIRTN    | Set step direction toward lower track #       |
| 0   | 0   | 1   | 0   | /STEP    | Set to 0 to step 1 track in current direction |
| 0   | 1   | 0   | 0   | /MOTORON | Start motor                                   |
| 1   | 1   | 0   | 0   | /MOTORON | Stop motor                                    |
| –   | 1   | 1   | 0   | EJECT    | Eject disk                                    |

- **Head settle:** Allow approximately **12–30 ms** for the head to settle after
  each step; if a **zone change** requires a new target RPM, permit up to **~150
  ms** for the speed transition.
- **Spin-up:** The motor spins up to the correct speed within **~400 ms**, and
  only operates when a disk is inserted.
- **Eject timing:** Eject begins immediately on the leading edge of **LSTRB**
  going high, but the signal must remain high for at least **~750 ms** to
  complete a successful eject. If **LSTRB** is released too soon, the drive may
  enter a partially ejected state or fail to eject the disk fully.

**Protocol (simplified):**

1. Set `CA2` to the **value** you want written into the selected latch (0 or 1).
2. Set `CA0`, `CA1`, and `SEL` to select which latch.
3. Pulse `LSTRB` high then low.

- `CA0, CA1, CA2, SEL` must not change while `LSTRB` is high.
- `CA0` and `CA1` must be returned to 1 before changing `SEL`.

EJECT is _unlatched output only_ and "cannot be read" (always reads as 1).

### Semantics of key status/control bits

Selected definitions from the Sony spec:

- `/CSTIN`: 0 only when a disk is in the drive
- `/WRTPRT` (in some tables `/WRTROT`): 0 only when disk is write-protected **or
  when no disk is in the drive**
- `/TK0`: 0 only when head is at track 0; **12 ms delay** after `/STEP` asserted
  before it’s valid
- `/TACH`: 60 pulses per rotation, 50% duty ±10%, pulse period accuracy ±0.2%
- `SIDES`: 0 for single-sided, 1 for double-sided drive
- `/DRVIN`: 0 only if selected drive is actually connected

Control bits:

- `/DIRTN`: 0 = towards center (higher track numbers), 1 = towards outer edge
  (towards track 0)
- `/STEP`: falling edge starts step; drive sets it back to 1 when step completes
- `/MOTORON`: low turns motor on if disk present

### VIA Head Select (SEL)

**HEADSEL** (SEL line to drive) is controlled via VIA Port A, bit 5—**not
through IWM**.

The SEL line serves dual purposes:

1. **Head selection** for double-sided drives (0 = lower head, 1 = upper head)
2. **Drive register address bit 3** for the multiplexed status/control system

This means software must manipulate VIA Port A to select heads and access
certain drive registers.

### ROM Parameter Encoding

The Macintosh ROM uses a **different bit ordering** for the `AdrDisk` routine
parameter than the natural CA0/CA1/CA2/SEL order:

```
ROM param bits:  ca1-ca0-sel-ca2
     bit 3        bit 2  bit 1  bit 0
```

| ROM Param | Key (CA0+CA1\*2+CA2\*4+SEL\*8) | Function        |
| --------- | ------------------------------ | --------------- |
| `$00`     | 0                              | Direction low   |
| `$01`     | 1                              | Direction high  |
| `$02`     | 2                              | Disk in place   |
| `$04`     | 4                              | Step            |
| `$08`     | 8                              | Motor on        |
| `$09`     | 9                              | Motor off       |
| `$0A`     | 10                             | Track 0         |
| `$0B`     | 11                             | Ready           |
| `$0E`     | 14                             | Tachometer      |
| `$0F`     | 15                             | Drive installed |

### Reading Drive Registers: AdrAndSense Routine

The ROM's `AdrAndSense` routine follows this sequence to read a drive status
register:

1. Set CA0=1, CA1=1 (safe state for SEL changes)
2. Configure CA2 based on bit 0 of parameter
3. Configure SEL based on bit 1 of parameter
4. Conditionally clear CA0 based on bit 2
5. Conditionally clear CA1 based on bit 3
6. Set Q6=1, Q7=0 (status read mode)
7. Read Status register—bit 7 is the drive register value

### Motor Speed Control (/PWM)

The `/PWM` signal controls disk motor speed:

- TTL-level signal with fixed pulse rate 20–40 kHz
- Duty cycle defined as % time signal is logic 0
- Works for duty cycles 10%–90%
- Implementation typically uses 22 kHz with dithering for finer resolution

The drive may be “dithered” in duty cycle, effectively lowering the control
frequency while increasing resolution.

---

## IWM Internal Registers

The IWM has four internal registers: Mode, Status, Handshake, and Data. These
are selected by the combination of ENABLE, Q6, and Q7 lines.

| ENABLE | Q6  | Q7  | Access | Register                              |
| ------ | --- | --- | ------ | ------------------------------------- |
| on     | off | off | R      | **Data** (read track stream, latched) |
| on     | on  | on  | W      | **Data** (write track stream)         |
| on     | on  | off | R      | **Status**                            |
| on     | off | on  | R      | **Handshake**                         |
| off    | on  | off | W      | **Mode**                              |

### Mode Register

The Mode register is write-only; its current value is mirrored in the low 5 bits
of the Status register.

**Recommended Mac setting:** `$1F` at early boot; commonly left unchanged. Some
titles tweak Mode (e.g., clock speed) for copy protections.

**Bit definitions (per IWM Specification rev19, 1982):**

| Bit | Name     | 0 =               | 1 =                         |
| --- | -------- | ----------------- | --------------------------- |
| 0   | LATCH    | latch off         | latch mode (use with async) |
| 1   | ASYNC    | synchronous       | asynchronous handshake      |
| 2   | NOTIMER  | 1-sec motor delay | no motor-off delay          |
| 3   | FAST     | slow (4 µs/bit)   | fast (2 µs/bit)             |
| 4   | 8MHZ     | 7 MHz clock       | 8 MHz clock                 |
| 5   | TEST     | normal operation  | test mode                   |
| 6   | MZ-RESET | —                 | reset MZ flag               |
| 7   | —        | reserved          | reserved                    |

**What $1F enables:**

The value `$1F` (binary `00011111`) configures the IWM for 3.5" Sony drive
operation:

| Bit | Value | Setting                | Meaning                              |
| --- | ----- | ---------------------- | ------------------------------------ |
| 4   | 1     | 8 MHz clock            | FCLK divided by 8 for timing         |
| 3   | 1     | Fast mode (2 µs/bit)   | Double data rate for 3.5" drives     |
| 2   | 1     | No spindle delay       | Motor stops immediately on disable   |
| 1   | 1     | Asynchronous handshake | IWM handles write timing; CPU polls  |
| 0   | 1     | Latch mode enabled     | Data register latches on MSB=1 bytes |

This is the correct configuration for Macintosh 3.5" floppy I/O. The 5.25" Disk
II mode uses `$00` (7 MHz, 4 µs/bit, synchronous, 1-second motor delay, no
latching).

**Note:** The IWM specification states bit 4 = 1 means 8 MHz clock. However,
some sources (notably Apple IIGS documentation) use `$0F` with bit 4 = 0 for 7
MHz. The Macintosh uses `$1F` as confirmed by ROM disassembly.

**Programming sequence (robust):** Status → confirm → ENABLE off → Q6 on → write
byte to Q7 on → re-read Status and verify bits 0–4 reflect change (retry on
failure).

### Status Register

The Status register is read-only and provides both drive state and Mode mirror.

**Bit layout:**

- **Bit 7 (SENSE):** sampled 1-bit "drive register" selected by CA2/CA1/CA0/SEL.
- **Bit 6:** reserved.
- **Bit 5:** mirrors ENABLE (1 when path enabled).
- **Bits 4–0:** mirror **Mode** bits 4–0.

**To read a drive register via Status:** SELECT drive → set CA0/1/2 and SEL to
pick the "register" → ENABLE on → Q6 on → read from **Q7 off** address
(0xDFFDFF).

### Handshake Register

Used only during **writes** to pace the CPU. Read: ENABLE on → Q7 on → read **Q6
off** (0xDFF9FF).

**Bit layout:**

- Bit 7: **Ready for data** (1 when data register empty → refill permitted)
- Bit 6: **Underrun status** (1 = no underrun; 0 = underrun occurred and writing
  stopped)
- Bits 5–0: reserved.

Idle state: Ready=1, Underrun=0.

### Data Register

- **Read path (latched):** With motor running, IWM shifts track bits into an
  internal shifter. **When a byte with MSB=1 forms,** it latches into Data and
  clears the shifter (Mac latch mode relies on "all valid GCR bytes have MSB=1";
  bytes with MSB=0 never latch). Reading clears the Data register to 0.
  Sequence: ENABLE on → **Q7 off** → read via **Q6 off**.
- **Write path:** Write one encoded byte at a time: **Q7 on**, write value to
  **Q6 on**. You must wait for Handshake "Ready for data" between bytes, and
  stop when "Underrun" indicates the last bit was shifted to media.

---

## Disk Geometry

The Sony specification defines the disk geometry as follows: there are 80
tracks, numbered from 0 at the outer edge to 79 at the inner edge of the disk.
Each disk has at least one side, referred to as side 0; double-sided drives add
a second side, side 1. To maintain a nearly constant data density across the
disk surface, the number of sectors per track is not fixed. Instead, the disk is
divided into zones, with tracks in the outermost zone containing 12 sectors per
track, and the number of sectors gradually decreasing to 8 per track in the
innermost zone. This zoned layout ensures efficient use of the available space
and consistent data rates across all tracks.

| Track | RPM | bit/s  | bit/t | Track Bytes | µs/bit | Sectors | Min Bytes/Sector |
| ----- | --- | ------ | ----- | ----------- | ------ | ------- | ---------------- |
| 00–15 | 394 | 489600 | 74558 | 9320        | 2.04   | 12      | 733.5            |
| 16–31 | 429 | 489600 | 68476 | 8559        | 2.04   | 11      | 733.5            |
| 32–47 | 472 | 489600 | 62237 | 7780        | 2.04   | 10      | 733.5            |
| 48–63 | 525 | 489600 | 55954 | 6994        | 2.04   | 9       | 733.5            |
| 64–79 | 590 | 489600 | 49790 | 6224        | 2.04   | 8       | 733.5            |

**Column definitions:**

- **Track Bytes**: Total encoded bytes that fit on one track at this zone's RPM.
  This is the raw track buffer size an emulator should allocate per track.
- **Min Bytes/Sector**: Minimum sector size (733.5 code bytes), comprising
  header sync, header, data sync, and data fields. The remaining track capacity
  forms inter-sector gaps filled with sync bytes.

### Track Buffer Structure

A track consists of sectors laid out sequentially with inter-sector gaps. The
total track capacity ("Track Bytes" above) must accommodate all sectors plus
gap/sync overhead:

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Track N (e.g., 9320 bytes for tracks 0–15)                              │
├──────────┬──────────┬──────────┬─────┬──────────┬───────────────────────┤
│ Sector 0 │ Gap/Sync │ Sector 6 │ ... │ Sector 5 │ Remaining Sync Fill   │
│ (~731 B) │  (FFs)   │ (~731 B) │     │ (~731 B) │        (FFs)          │
└──────────┴──────────┴──────────┴─────┴──────────┴───────────────────────┘
```

**Gap/sync bytes:** The space between sectors and at the end of the track is
filled with `$FF` sync bytes. The header sync field should be made "as large as
possible" during formatting to buffer expansion of the previous sector's data
field due to drive speed variation.

**Practical gap calculation:**

- Minimum sector size: 733.5 bytes (6.25 + 11 + 6.25 + 710)
- Total minimum for N sectors: N × 733.5 bytes
- Available gap space: Track Bytes − (N × 733.5)

| Zone  | Sectors | Min Sector Total | Track Bytes | Available Gap |
| ----- | ------- | ---------------- | ----------- | ------------- |
| 00–15 | 12      | 8802             | 9320        | ~518 bytes    |
| 16–31 | 11      | 8069             | 8559        | ~490 bytes    |
| 32–47 | 10      | 7335             | 7780        | ~445 bytes    |
| 48–63 | 9       | 6602             | 6994        | ~392 bytes    |
| 64–79 | 8       | 5868             | 6224        | ~356 bytes    |

The gap bytes are distributed across inter-sector sync fields. Since actual
sector encoding may produce slightly fewer than 733.5 bytes (sync fields use
10-bit "bit slip" patterns that pack into ~6.25 bytes), the practical gap is
slightly larger.

This disk format provides a total of 800 sectors (blocks), numbered sequentially
from 0 to 799. Block 0 corresponds to sector 0 on track 0, while block 799 is
sector 7 on track 79.

### Block Numbering and Cylinder Interleaving

**Single-sided (400 KB):** Blocks are numbered sequentially by track:

```
Block = sum of sectors on tracks 0..(track-1) + sector
```

Explicit per-zone calculation:

| Track Range | Sectors/Track | Cumulative Sectors at Track Start |
| ----------- | ------------- | --------------------------------- |
| 0–15        | 12            | track × 12                        |
| 16–31       | 11            | 192 + (track − 16) × 11           |
| 32–47       | 10            | 368 + (track − 32) × 10           |
| 48–63       | 9             | 528 + (track − 48) × 9            |
| 64–79       | 8             | 672 + (track − 64) × 8            |

**Double-sided (800 KB):** Blocks are interleaved by cylinder (both sides of one
track together):

```
Blocks 0–11:   Side 0, Track 0, Sectors 0–11
Blocks 12–23:  Side 1, Track 0, Sectors 0–11
Blocks 24–35:  Side 0, Track 1, Sectors 0–11
... and so on
```

General formula for double-sided:

```
offset_in_cylinder = block mod (2 × sectors_per_track)
track = block ÷ (2 × sectors_per_track)
if offset_in_cylinder < sectors_per_track:
    side = 0
    sector = offset_in_cylinder
else:
    side = 1
    sector = offset_in_cylinder − sectors_per_track
```

**Sector Interleave (2:1):**

To allow time for the CPU to process one sector before the next physical sector
arrives, sectors are written in an interleaved order on the track. The 2:1
interleave pattern for each zone is:

| Zone (tracks) | Sectors | Physical order on track              |
| ------------- | ------- | ------------------------------------ |
| 0–15          | 12      | 0, 6, 1, 7, 2, 8, 3, 9, 4, 10, 5, 11 |
| 16–31         | 11      | 0, 6, 1, 7, 2, 8, 3, 9, 4, 10, 5     |
| 32–47         | 10      | 0, 5, 1, 6, 2, 7, 3, 8, 4, 9         |
| 48–63         | 9       | 0, 5, 1, 6, 2, 7, 3, 8, 4            |
| 64–79         | 8       | 0, 4, 1, 5, 2, 6, 3, 7               |

The interleave factor is encoded in the header "Format" byte (bits 0–4 = 2 for
standard 2:1 interleave).

---

## Sector Format

A Macintosh Plus floppy sector is divided into four main sections: the header
sync field, header field, data sync field, and data field. Together, these
fields comprise a minimum of 733.5 code bytes per sector.

| Field             | Length     | Description / Contents                    |
| ----------------- | ---------- | ----------------------------------------- |
| Header Sync Field | 6.25 bytes | Bit slip pattern for synchronization      |
| Header Field      | 11 bytes   | Sector metadata and checksum              |
| Data Sync Field   | 6.25 bytes | Bit slip pattern for synchronization      |
| Data Field        | 710 bytes  | Encoded data, checksum, and field markers |

### Header Sync Field

This field provides a recognizable bit pattern to synchronize the hardware state
machine with the disk data. It is written only during formatting and should be
as long as possible to accommodate drive speed variations.

| Field Name        | Length     | Pattern / Notes                      |
| ----------------- | ---------- | ------------------------------------ |
| Header Sync Field | 6.25 bytes | `FF 3F CF F3 FC FF` (bit slip bytes) |

- Contains at least 5 "bit slip" bytes.
- Buffers expansion of the previous sector's data field due to speed variation.

### Header Field

Identifies the sector and provides essential metadata. The header field is
structured as follows:

| Byte(s)    | Description                                       |
| ---------- | ------------------------------------------------- |
| `D5 AA 96` | Address marks (identifies this as a header field) |
| Track      | Encoded low 6 bits of track number                |
| Sector     | Encoded sector number                             |
| Side       | Encoded high 2 bits of track number and side bit  |
| Format     | Encoded format specification                      |
| Checksum   | XOR of Track, Sector, Side, and Format fields     |
| `DE AA`    | Bit slip marks (identifies end of field)          |
| off        | Pad byte (write electronics turned off)           |

**Side field breakdown:**

- Bit 5: 0 = side 0, 1 = side 1
- Bit 0: high-order bit of track number
- Bits 1–4: reserved (should be 0)

**Format field breakdown:**

- Bit 5: 0 for single-sided
- Bits 0–4: format interleave (2 for standard 2:1 interleave)

### Data Sync Field

Serves to synchronize the hardware state machine with the start of the data
field. This sync pattern is written each time the data field is written to disk.

| Field Name      | Length     | Pattern / Notes                      |
| --------------- | ---------- | ------------------------------------ |
| Data Sync Field | 6.25 bytes | `FF 3F CF F3 FC FF` (bit slip bytes) |

### Data Field

Contains the actual sector data, encoded for GCR storage.

| Byte(s)      | Description                                  |
| ------------ | -------------------------------------------- |
| `D5 AA AD`   | Data marks (identifies this as a data field) |
| Sector       | Encoded sector number                        |
| Encoded Data | 524 data bytes encoded into 699 code bytes   |
| Checksum     | 24-bit checksum encoded into 4 code bytes    |
| `DE AA`      | Bit slip marks (identifies end of field)     |
| off          | Pad byte (write electronics turned off)      |

Out of 524 data bytes, the first 12 bytes contain the sector "tag", and the
remaining 512 bytes contain the user data.

### GCR Encoding

Each sector contains 524 user data bytes, followed by a 3-byte checksum. Data is
encoded using **GCR (Group Code Recording)**, a scheme that eliminates the need
for separate clock bits and allows more data to fit on each track.

The encoding process works as follows:

- User data bytes are grouped into sets of three: `BYTEA`, `BYTEB`, and `BYTEC`.
- These bytes are transformed into 6-bit nibbles, which are then mapped to GCR
  codewords using a lookup table.
- During encoding, three checksum registers (`CSUMA`, `CSUMB`, `CSUMC`) are
  updated to generate a 3-byte checksum for the sector.

This approach ensures that all written bytes conform to the GCR codeword set,
maximizing data density and maintaining reliable synchronization on disk.

1. Rotate CSUMC left:
   - CSUMC[7:0] ← CSUMC[6:0] + CSUMC[7] as carry
   - Carry ← CSUMC[7]
2. CSUMA ← CSUMA + BYTEA + carry from step 1
3. BYTEA ← BYTEA XOR CSUMC
4. CSUMB ← CSUMB + BYTEB + carry from step 2
5. BYTEB ← BYTEB XOR CSUMA
6. CSUMC ← CSUMC + BYTEC + carry from step 4
7. BYTEC ← BYTEC XOR CSUMB
8. Convert BYTEA, BYTEB, and BYTEC to 6-bit nibbles:
   - NIBL1 ← A7 A6 B7 B6 C7 C6 (high bits of the bytes)
   - NIBL2 ← A5 A4 A3 A2 A1 A0 (low bits of BYTEA)
   - NIBL3 ← B5 B4 B3 B2 B1 B0 (low bits of BYTEB)
   - NIBL4 ← C5 C4 C3 C2 C1 C0 (low bits of BYTEC)
9. Write GCR(NIBL1), GCR(NIBL2), GCR(NIBL3), and GCR(NIBL4)

The following table lists the 64-entry GCR (Group Code Recording) codeword
mapping used to encode 6-bit nibbles into 8-bit disk bytes. Each row shows the
offset (in hexadecimal) and the corresponding 8 codewords for that range.

| Offset | Codewords                      |
| ------ | ------------------------------ |
| 0x00   | 96, 97, 9A, 9B, 9D, 9E, 9F, A6 |
| 0x08   | A7, AB, AC, AD, AE, AF, B2, B3 |
| 0x10   | B4, B5, B6, B7, B9, BA, BB, BC |
| 0x18   | BD, BE, BF, CB, CD, CE, CF, D3 |
| 0x20   | D6, D7, D9, DA, DB, DC, DD, DE |
| 0x28   | DF, E5, E6, E7, E9, EA, EB, EC |
| 0x30   | ED, EE, EF, F2, F3, F4, F5, F6 |
| 0x38   | F7, F9, FA, FB, FC, FD, FE, FF |

---

## Read/Write Algorithms

### Reading a Sector (Plus / 800 KB)

1. **Seek & spin:**
   - Use TRACKUP/TRACKDN to set direction; **TRACKSTEP** to target track.
   - Wait until **STEP** reports still (Status SENSE=1) and any zone speed
     change has settled (allow **~150 ms** worst-case).
   - **MOTORON**; wait **~400 ms** to nominal RPM. Confirm with **TACH** (60
     pulses/rev).
2. **Start streaming:** ENABLE on; repeatedly read the **Data** register (Q7 off
   then read at Q6 off). Bytes will present only when a legal GCR byte crosses
   the head (MSB=1 latch).
3. **Header scan:** In software, search for address prologue `D5 AA 96`, then
   parse header (track, side, sector, format, checksum). Skip with checksum
   mismatch.
4. **Find the matching sector N header; continue to next prologue `D5 AA AD`
   (data).**
5. **Decode data field:** Convert the series of legal GCR bytes to 512 logical
   bytes + 12 tag bytes; verify checksum; return the 512 bytes. (Exact 6&2
   nibblization)
6. **Repeat** if multiple sectors requested; respect gaps/self-sync.

### Writing a Sector

1. Seek/spin as above.
2. Wait for the target rotational position (software usually watches headers).
3. **Write loop:**
   - For each on-disk byte (including prologues, data, checksum, epilogue,
     gaps), do:
     - Wait Handshake **Ready for data** (bit7=1), then write byte via **Q7 on /
       write to Q6 on**.
   - After the final byte, wait for **Underrun=0→1** transition to confirm shift
     register emptied and writing stopped.

> **Critical:** The IWM will silently drop a Data-register byte if you post it
> too soon (before the previous byte transfers to the write shifter). Always
> pace on Handshake bit 7.

---

## Edge Cases, Timing, and Quirks

- **Head settle:** Inside Macintosh calls out ~**12 ms** per step before
  **TK0/STEP** stable; empirical notes suggest **up to 30 ms** worst case.
  Emulate a distribution, not a fixed number.
- **Zone changes:** Permit **~150 ms** for RPM transition across zone boundaries
  before trusting headers/data.
- **Byte-latch behavior:** Only bytes with MSB=1 latch in read mode; any
  unlatchable patterns in the flux stream must not surface as bytes.
- **Even-address I/O:** 68000 even-address reads are undefined (no D8–D15
  connected); even-address writes "work" (UDS/LDS both active). If OS/app
  misbehaves, mirror real quirks.
- **Single vs dual drive:** SELECT chooses internal vs external; Plus supports
  two drives, 128K/512K effectively one due to PWM-speed logic.

---

## Media Geometry and Interoperability

- **ZCAV layout** increases sectors/track toward the outer zone (12 max),
  outperforming PC 3.5" double-density (9 s/t fixed). This—and GCR—makes Mac
  400/800 KB **incompatible** with PC controllers; only SWIM-era Macs and
  flux-level devices can bridge.
- **Capacities:** 400 KB (single-sided) and 800 KB (double).

---

## Practical Emulator Implementation Notes

- **Track Source:** If you emulate at the raw track level, store per-track
  **encoded** byte streams (including prologues, gaps, and self-sync). The Data
  register should drip bytes at the zone's bit-rate; only present bytes when
  MSB=1. Use an internal shifter for bit timing and byteization that mirrors the
  latch rule.
- **Handshake pacing (writes):** Model the two-stage buffer (Data →
  Write-shift-reg) exactly; assert Handshake "Ready" once the shift-reg consumes
  the posted byte; latch Underrun when shift-reg empties and Data is empty.
- **Tach pulses:** Generate **60 pulses per revolution** accessible via TACH
  drive register (polled by ROM for speed control / calibration).
- **PWM (128K/512K only):** If you plan to support **pre-Plus** ROMs, implement
  the sound-buffer-LSB PWM mapping (ROM sums table-mapped values over windows to
  compute duty cycle; the speed bounds vs duty cycle are documented).
- **ROM driver reliance:** Many protections depend on exact sector headers,
  interleave, and precise prologue/epilogue and gap lengths. Aim to match
  MAME/FluxEngine parameters when generating tracks.
