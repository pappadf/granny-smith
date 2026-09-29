# The IIfx Apple SCSI DMA chip

The Macintosh IIfx (1990) replaces the earlier machines' CPU-paced "pseudo-DMA" SCSI data path with a one-off Apple custom chip, drawing 343S0064-A, that wraps an
enhanced NCR 53C80-compatible SCSI cell in a true 68030 bus-master DMA engine.
This page documents the whole part: the wrapper's register window down to the
bit, its relation to the embedded 53C80, the DMA/FIFO/byte-routing machinery,
bus arbitration, interrupts, watchdog, the programming sequences Apple
published and the sequences real operating systems actually perform.

**Contents:**

1. [Overview](#1-overview) — what the part is, the IIfx wiring, package, modes
2. [Register file](#2-register-file) — decode, summary map, per-register detail, reset state
3. [Behaviour](#3-behaviour) — PIO, hardware handshake, bus-master DMA, FIFO, alignment, interrupts, watchdog, auto-arbitration, reset, test modes, timing
4. [Programming model](#4-programming-model) — Apple's published sequences; observed A/UX 3.0.1 driver behaviour
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

## 1. Overview

### 1.1 What the part is

The SCSI DMA chip is a QFP-100 Apple ASIC whose purpose is "to provide a high
speed interface from the 68030 bus to the ANSI Small Computer Systems Interface
(SCSI) bus" [1] §6.0. It contains two cooperating halves:

* An **embedded enhanced 53C80 cell** — "an enhancement of the familiar 53C80
  circuit" that handles all SCSI protocol: selection, reselection, arbitration,
  phase, REQ/ACK handshaking, parity, RST, BSY, SEL, ATN, MSG, C/D and I/O
  [1] §6.1.1.1, §6.3. The enhancements over a stock 53C80 are automatic SCSI
  bus arbitration and improved asynchronous transfer (up to 3 MB/s) [1] §6.0,
  §6.1.1.1.
* An **Apple DMA wrapper**: slave/programmed-I/O register interface, a
  bus-master engine capable of 32-bit 68030 memory cycles, a 32-bit DMA address
  register and byte counter, a one-longword FIFO with byte-routing logic, a
  programmable watchdog timer, and auto-arbitration control [1] §6.1.1.

The division of labour is the key design fact: **the wrapper accelerates data
bytes only**. "Only data transfers are handled by the DMA channel; other SCSI
bus protocol is handled in software running on the host processor" [1] §6.0,
and "All control information is transferred to or from the SCSI Bus via the
8 byte-wide 53C80 registers no matter which mode of operation is chosen" [1]
§6.7.2. Command, status and message phases always go through the 53C80
registers under CPU control.

The chip also carries a "DMA bypass mode (to remain compatible with current
software written for the 53C80)" [1] §6.0 — with the wrapper's DMA engine
disabled, the part behaves as an ordinary byte-at-a-time 53C80 host adapter,
which is how software written for earlier Macintosh SCSI hardware runs
unchanged.

Published features [1] §6.0: 32-bit DMA data transfers; 4-gigabyte direct
addressing range (32-bit address register); block transfers of up to 4 gigabytes
(32-bit byte counter); support for misaligned (non-modulo-4) buffer addresses
*into "normal (not Nubus) memory"*; non-modulo-4 byte counts; programmable
watchdog; 3 MB/s asynchronous transfer rate; automatic SCSI bus arbitration,
with the "old" software-controlled scheme still supported.

### 1.2 Where the IIfx puts it

The part is specific to the Macintosh IIfx logic board. The chip itself decodes
registers only from its `/CS`-qualified address lines (§2.1); board-level
address decode is the IIfx's. *Observed*: the A/UX 3.0.1 kernel sets its SCSI
chip-base global to `$50F08000` at boot (`pstart`, `$0005810A`:
`MOVE.L #$50F08000,$0005B20A`) and drives all wrapper registers at offsets
from that base [4]. The exact decode width and aliasing of the IIfx I/O region
around this window are not established here (§6).

The chip's open-drain `/INT` output does not reach a VIA on the IIfx; it is
routed through the IIfx's OSS interrupt controller. *Observed*: A/UX's level-2
interrupt dispatcher reads a pending-sources word at OSS base `$50F1A000` +
`$202` and dispatches bit 1 of that word to the SCSI interrupt handler [4].

Apple's official 1990 position on the software side: "the IIfx hardware has
SCSI DMA capability, [but] the Macintosh System Software does not yet take
advantage of it. Apple recommends that you wait until the Macintosh System
Software implements support for the IIfx SCSI DMA" [3]. A/UX, Apple's own UNIX,
does drive the bus-master engine; §4.5 documents its observed behaviour.

### 1.3 Package and pins

The part is a 100-pin QFP ("IC, Custom SCSI DMA Controller, QFP-100", drawing
343S0064-A, rev. A) [1]. Supply is 5 V (4.75–5.25 V), 0–70 °C; the open-drain
outputs `/BR` and `/INT` assume 3.3 kΩ pull-ups [1] Table 7. Significant pins:

| Pin(s) | Signal | Function |
| ------ | ------ | -------- |
| 43–49, 51–52 | `/SD(7..0)` | SCSI data bus (internal 8-bit SD bus to the 53C80 cell) |
| 53 | `/SDBP` | SCSI data parity |
| 54–64 | `/SATN /SBSY /SACK /SRST /SMSG /SSEL /SCD /SREQ /SIO` | SCSI control signals |
| 6–42 | `D(31..0)` | 32-bit processor data bus (DMA and PIO) |
| 2–5, 87–100 | `MA(15..0)` | 16-bit multiplexed address bus (also carries register select, §2.1) |
| 65 | `TEST` | High = 53C80 isolation test mode (pinout redefined; manufacturing only) |
| 67 | `CPUCLK` | Chip clock, max 25 MHz |
| 68 | `/CS` | Chip select, asserted for PIO |
| 69 | `/INT` | Interrupt, open drain |
| 70 | `/RESET` | Hardware reset (>= 80 ns low) |
| 71 | `/BERR` | Bus error input (DMA cycles) |
| 72–75 | `/BGACK /BGOUT /BGIN /BR` | 68030-style bus arbitration |
| 79 | `/DSACK0` | Asynchronous termination: driven in PIO, sampled in DMA |
| 80 | `/STERM` | Synchronous termination input (DMA cycles; preferred) |
| 81 | `FC1` | Function code bit 1, driven low during DMA cycles (non-CPU space) |
| 82–84 | `R/W`, `/DSACK1`, `/AS` | Bus control: inputs in PIO, outputs in DMA |
| 85–86 | `/ABEN`, `/ALE` | External high-address latch output enable / latch clock |

Because the chip multiplexes its 32-bit DMA address onto 16 address pins, two
external 8-bit latches (74F574 or equivalent) hold the high half: the chip
drives `A[31:16]` on `MA[15:0]` and pulses `/ALE` to capture it, then drives
`A[15:0]` on `MA[15:0]` and asserts `/ABEN` to enable the latched high half onto
the bus [1] §6.5.1, Table 2.

### 1.4 Operating modes

Four top-level modes [1] §6.2: **Slave (PIO)**, **Master (DMA)**, **Test**
(§3.12) and **Reset** (§3.11). Independently of those, the *data-transfer*
method is one of [1] §6.7.2:

| Method | Selected by | Data movement |
| ------ | ----------- | ------------- |
| Polled | nothing special | CPU polls `$040`/`$050` and handshakes each byte through `$000` |
| Hardware handshake ("pseudo DMA", "blind") | `$080` bit 3 | CPU reads/writes `$060`/`$000`; the chip withholds `/DSACK0` until the SCSI byte handshake completes |
| DMA (bus master) | `$080` bit 0 + 53C80 start register | Chip moves bytes between main memory and the 53C80 without the CPU |

In DMA mode the chip "becomes the bus master and operates much like a
less-powerful 68030" [1] §6.2.2: it arbitrates for the bus through `/BR`,
`/BGIN`/`/BGOUT` and `/BGACK`, and runs 68030-protocol memory cycles. DMA
transfers are normally 4 bytes (32 bits) per memory cycle, against one byte
per PIO transfer [1] §6.0.

## 2. Register file

### 2.1 Address decode

With `/CS` asserted, the chip decodes the register select from address bits
`MA[8:4]`; `MA[3:0]` are *not* part of the register select inside the chip [1]
§6.1.2, §6.6.1:

```
decoded register = (A[8:0] >> 4) & 0x1F
```

Consequences an implementer must reproduce:

* Registers are spaced `$10` apart; the 16 addresses inside each slot are
  aliases of the same register.
* The bytewide 53C80 registers occupy `$000`–`$070`; the 32-bit Apple
  registers occupy `$080`–`$180` (32-bit stride of `$40`).
* 8-bit 53C80 accesses use data byte lane `D[31:24]` (the timing tables give
  `D[31:24]` setup/hold for register cycles) [1] Table 7; the Apple 32-bit
  registers use the full `D[31:0]`.
* The 68030 bus is big-endian and "MSB's go in LS addresses" [1] §6.5.3: for a
  longword at `A[1:0]=00`, byte lane 0 is `D[31:24]`:

| `A[1:0]` | Byte lane | Data bits |
| -------- | --------- | --------- |
| `00` | 0 | `D[31:24]` |
| `01` | 1 | `D[23:16]` |
| `10` | 2 | `D[15:8]` |
| `11` | 3 | `D[7:0]` |

### 2.2 Register summary

All offsets are relative to the `/CS` base [1] §6.6, Figure 11:

| Offset | Width | Read | Write |
| ------ | ----- | ---- | ----- |
| `$000` | 8 | Current SCSI Data | Output Data |
| `$010` | 8 | Initiator Command | Initiator Command |
| `$020` | 8 | Mode | Mode |
| `$030` | 8 | Target Command | Target Command |
| `$040` | 8 | Current SCSI Bus Status | Select Enable |
| `$050` | 8 | Bus and Status | Start DMA Send |
| `$060` | 8 | Input Data | Start DMA Target Receive |
| `$070` | 8 | Reset Parity/Interrupts | Start DMA Initiator Receive |
| `$080` | 32 | DMA Control/Status | DMA Control |
| `$0C0` | 32 | DMA Byte Count | DMA Byte Count |
| `$100` | 32 | DMA Address | DMA Address |
| `$140` | 32 | Watchdog Timer | Watchdog Timer |
| `$180` | 32 | FIFO | FIFO (FIFO-loopback test mode only) |

### 2.3 The embedded 53C80 registers (`$000`–`$070`)

"The byte-wide 53C80 registers are mapped to address bits MA[8-4] = $00 to
$07. Refer to the 53C80 design manual for a detailed description of each bit's
function" [1] §6.6.1. The eight registers are the standard 5380/53C80 set;
their bit-for-bit behaviour is the subject of the NCR design manual [2] and is
summarized on this tree's 5380 page (`ncr-5380.md` §2–§3). Per-register names
are as in §2.2 above; the read/write split at `$040`, `$050`, `$060`, `$070`
(select-enable vs. bus status, start-DMA triggers, input data, reset-interrupt
vs. start-DMA) is exactly the 5380's.

IIfx-specific deltas against a bare 5380, all evidenced by [1]:

* **`$010` bit 6 (ICR) — tri-state test.** Writing 1 "causes all output lines
  on the SCSI DMA to be held in a high impedance state until bit 6 of Register
  `$010` is written with a 0" [1] §6.2.3.2. Note the scope: on this part the
  whole chip's outputs go high-impedance — processor-bus outputs and bus-master
  control lines included — not only the SCSI drivers.
* **`$020` bit 0 (MR ARBITRATE) — auto-arbitration hook.** With the wrapper's
  `ARBEN` set (`$080` bit 12), starting arbitration through MR bit 0 runs the
  Apple automatic sequence using the SCSI ID in `$080` bits 9–11 (§3.10,
  §4.4); with `ARBEN` clear, the stock 53C80 software arbitration applies [1]
  §6.7.1.
* **`$030` bit 7 (TCR, read) — Last Byte Sent.** A 53C80-family feature, not a
  5380 one: the NCR 53C80 "uses bit 7 of this register to determine when the
  last byte of a DMA transfer is sent to the SCSI bus", because the Bus and
  Status END-OF-DMA bit only reflects when the last byte was received from the
  DMA side [2]. Bits 6:4 read as reserved (normally 0); bits 3:0 are the
  standard assert-REQ/MSG/C/D/I/O bits [2].
* **MR bit 1 (DMA MODE) — auto-cleared on loss of BSY.** "If this error occurs,
  all SCSI Outputs will be tristated and Register `$020` bit 1 (DMA Mode) will
  be automatically cleared" [1] §6.8.1.4.
* **`$040` (write, Select Enable) — all-zero disables selection interrupts.**
  "if Register `$040` contains all zeros, this interrupt won't be enabled" [1]
  §6.8.1.6.
* **Phase-mismatch interrupt qualification.** The phase-mismatch IRQ is
  generated "on the falling edge of `/SREQ` if there is a Phase Mismatch Error
  and Register `$050` (DMA Send) has been written"; on this event the SCSI
  outputs are tristated [1] §6.8.1.5.

### 2.4 DMA Control register (`$080`)

32-bit control/status. Bit definitions [1] §6.6.6:

| Bit | Name | R/W | Meaning |
| --- | ---- | --- | ------- |
| 0 | DMA Enabled | R/W | 1 = Apple bus-master DMA operations enabled |
| 1 | SCSI Interrupts Enabled | R/W | gates 53C80 IRQ, DMA completion, bus-error termination and auto-arbitration-win through to `/INT` |
| 2 | WD Timer Interrupts Enabled | R/W | gates watchdog timeout through to `/INT` |
| 3 | Hardware Handshake Mode | R/W | 1 = hardware-handshake (blind) transfers (§3.2) |
| 4 | 53C80 Reset / Bytes Left in FIFO | W strobe / R status | **write** 1 = hardware-reset the internal 53C80 cell (does not tri-state the SCSI DMA and does not reset the wrapper's registers); **read** 1 = bytes remain in the FIFO |
| 5 | FIFO Loopback/Internal Counters Test Mode | R/W | 1 = test mode (§3.12.3) |
| 6 | SCSI Interrupt Pending | R only | 1 = 53C80 IRQ pending *or* the DMA operation terminated successfully |
| 7 | WD Timer Interrupt Pending | R only | 1 = watchdog timeout pending |
| 8 | DMA halted by bus error | R only | 1 = the DMA operation was halted by a bus error |
| 9–11 | ID0–ID2 | R/W | binary SCSI ID used during auto-arbitration |
| 12 | ARBEN | R/W | 1 = enable auto-arbitration |
| 13 | WONARB | R only | 1 = auto-arbitration won the bus |
| 14–31 | reserved | — | "reserved for future expansion" |

Write behaviour:

* Writing bit 4 = 1 issues one hardware reset pulse to the embedded 53C80 cell
  — held long enough to meet the cell's 100 ns minimum reset-assertion
  requirement. "It is not necessary to clear bit 4 of the DMA Control Register
  after performing a DMA Control Register Software Reset; the 53C80 cell will
  only be hardware reset once per assertion of bit 4" [1] §6.2.4.2. This reset
  does not touch the wrapper's own registers, counters, FIFO or latches.
* Clearing bit 0 (DMAEN) stops any active bus-master operation and releases the
  processor bus mastership signals. *Inferred — unverified:* the bus-error and
  completion latches are cleared by this disable-and-reinitialise, since [1]
  names no other clear path for them besides hardware reset.
* Clearing bit 12 clears WONARB [1] §6.6.6 (ARBEN "enable" semantics; the win
  indication does not outlive the feature being enabled).

Read behaviour: the writable bits (0–3, 5, 9–12) read back as written; bit 4
reads as FIFO-not-empty; bits 6–8 and 13 are live status; reserved bits read
as 0.

Reset value: hardware reset clears the entire register and all latches [1]
§6.2.4.1.

### 2.5 DMA Byte Count register (`$0C0`)

32-bit read/write. "This 32 bit register contains the current DMA byte count"
[1] §6.6.5. During a transfer "the byte count is decremented by one … each
time a byte is transferred to or from the 53C80" [1] §6.5.3 — per *SCSI-side*
byte, not per memory longword. A count of zero means the operation is
complete; the datasheet's non-aligned examples end each with "Count = 0 → DMA
Complete" [1] §6.5.3.

### 2.6 DMA Address register (`$100`)

32-bit read/write. "This 32 bit register contains the current DMA address.
It may be read or written" [1] §6.6.4. Like the count, the address is advanced
by one per byte transferred to or from the 53C80 [1] §6.5.3. The address is a
raw bus address: DMA cycles are physical 68030-bus cycles (§3.3), so no MMU
translation is or can be applied. A/UX programs it with kernel buffer-cache
physical addresses *observed* [4].

The phrase "current DMA address", plus the datasheet's per-byte increment
rule, describes a live counter that the CPU may also rewrite between
operations. The datasheet does not state at which event a written value takes
effect on a *running* engine (e.g. whether a write mid-operation, or between
operations, unconditionally reloads an internal counter). This ambiguity is
load-bearing for chunked transfers — see §6 (open questions).

### 2.7 Watchdog Timer register (`$140`)

32-bit read/write. "This register is written with the watchdog timer count.
The watchdog counter is loaded with this count and begins to count down
immediately at a rate of 1/2 the frequency of the CPUCLK input" [1] §6.6.3.
See §3.8 for reload, expiry and interrupt-clear behaviour. A read returns the
reload value, not necessarily the live counter (*inferred — unverified*: the
datasheet distinguishes "Watchdog Timer Register" (initial value) from
"Watchdog Timer Counter" (current value) [1] §6.1.1.1 and gives no live-counter
readback path).

### 2.8 FIFO register (`$180`)

32-bit read-only in normal operation ("In normal operation it may only be
read"); in FIFO-loopback test mode it is read/write [1] §6.6.2, §6.2.3.3. It
exposes the contents of the one-longword FIFO that assembles or disassembles
4-byte longwords from/into SCSI-side bytes [1] §6.1.1.1.

### 2.9 Reset state

A hardware reset (`/RESET` low >= 80 ns) "resets all parts of the SCSI DMA's
DMA logic and registers and issues a hardware reset to the 53C80 cell" [1]
§6.2.4.1: the DMA control register, address, count, watchdog and FIFO state
clear, pending Apple interrupt latches clear, bus-master lines release, and
the SCSI outputs release through the cell's reset. Reset assertion to the
cell is deliberately delayed/stretched so the cell sees its required 100 ns
[1] §6.2.4.1. Full operation resumes 8 `CPUCLK` periods (`8ø`) after `/RESET`
rises [1] Table 7.

## 3. Behaviour

### 3.1 Slave (PIO) cycles

In slave mode the 68030 is bus master and "the SCSI DMA looks like an
asynchronous (`/DSACK`-driving) memory controller" [1] §6.2.1. The CPU
presents `MA[8:4]`, `R/W` and `/AS` with `/CS`; the chip returns data — 8 bits
on `D[31:24]` for 53C80 registers, 32 bits on `D[31:0]` for Apple registers —
and terminates the cycle by asserting `/DSACK`. `/DSACK1` "indicates the
completion of a 32 bit PIO data transfer"; `/DSACK0` terminates asynchronous
transfers generally [1] Table 2. The timing table's key PIO numbers: `/DSACK`
falls within 30 ns of the `CPUCLK` rising edge and rises within 20 ns of `/AS`
rising; read data sets up >= 25 ns before `/DSACK` falls and holds >= 5 ns
after `/AS` rises [1] Table 7.

### 3.2 Hardware-handshake transfers

With `$080` bit 3 set, "the SCSI DMA will respond by asserting `/DSACK0` only
after the byte transfer has been executed on the SCSI Bus. If the transfer
does not occur for some reason, the 68030 bus cycle will be terminated by a
bus error generated by a system bus cycle timer (external to the SCSI DMA)"
[1] §6.7.2.2 — the chip itself never times a stalled handshake cycle out.
Handshake data cycles: writes go to `$000` (Output Data), reads come from
`$060` (Input Data) [1] Figures 8–9. The handshake timing is keyed to the
SCSI side: `/SREQ` falling (write) or rising (read) edge to the start of the
write-out phase is 3–5 clock periods (`3ø`–`5ø`), and `D[31:24]` setup to
`/DSACK0` falling is >= 10 ns [1] Table 7.

### 3.3 Bus-master DMA cycles

After the driver loads the address and byte count and sets up the 53C80 for
data transfer, "the DMA operation will begin by writing to one of the DMA
start registers" [1] §6.5.1. From then on:

```
53C80 asserts internal DREQ (FIFO full on SCSI→memory)
        → wrapper asserts /BR, breaks the /BGIN→/BGOUT daisy chain
        → on /BGIN + end of current 68030 cycle: assert /BGACK
        → drive A[31:16] on MA[15:0], pulse /ALE (external latch captures)
        → drive A[15:0] on MA[15:0], assert /ABEN (latched high half enabled)
        → drive R/W, SIZ1:SIZ0, FC1 (low: non-CPU space), /AS
        → data phase: latch D[31:0] (memory read) or drive D[31:0] (memory write)
        → termination: /STERM (synchronous, intended/preferred),
           /DSACK0 (asynchronous), or /BERR (error)
        → deassert; continue or release the bus (/BGACK, /BR)
```

"It is intended that all DMA transfers with memory be synchronous on the 68030
bus (`/STERM` supplied by memory controller). However, the signals `/DSACK0`
and `/BERR` will also terminate a DMA data transfer" [1] §6.5.1. When the chip
does not need the bus it passes `/BGIN` through to `/BGOUT`; when it does, it
negates `/BGOUT` and takes the bus on grant [1] Table 2, §6.5.1. FC1 is driven
low during DMA cycles to indicate a non-CPU-space operation [1] Table 2.

The trigger condition is stated for the SCSI-to-memory direction: "The 53C80
requests each DMA transfer (when the FIFO is full) via the internal signal
DREQ" [1] §6.5.1. *Inferred — unverified:* the memory-to-SCSI direction is the
mirror — a fetch when the FIFO has drained and the 53C80 can accept another
byte — since the same FIFO/breadth machinery serves both directions.

### 3.4 The FIFO and byte routing

The byte router "interfaces between the 32 bit CPU data bus and the 8 bit SD
(SCSI Data) bus", supporting misaligned DMA transfers, direct PIO to the
53C80, and "a one long word FIFO for DMA data transfers" [1] §6.4. The FIFO
"assemble[s] or disassemble[s] 4 byte longwords from, or into bytes" [1]
§6.1.1.1. Model it as one 32-bit word plus a valid-byte count in `[0,4]`; the
CPU-side readback of "bytes left in the FIFO" is `$080` bit 4 [1] §6.6.6.

* **SCSI → memory (Apple "DMA Write"):** each byte arriving from the 53C80 is
  placed in the byte lane selected by the two low bits of the current DMA
  address. When the most-significant-lane boundary is reached — i.e. the FIFO
  cannot accept another byte without crossing into the next longword — a
  68030 bus write is initiated; "The DMA address which will be gated onto the
  address bus will be the current address minus the number of bytes in the
  FIFO", and "the size bits will be determined by the number of bytes in the
  FIFO" [1] §6.5.3.
* **Memory → SCSI (Apple "DMA Read"):** the first bus read is shortened to end
  on a longword boundary (table below), bytes drain from the FIFO to the 53C80
  in address order, and the last bus read is shortened to the remaining byte
  count [1] §6.5.3. *Inferred — unverified:* the read address is the current
  DMA address at fill time (the counter advances per SCSI-side byte, so the
  memory-side fetch runs ahead of the address the 53C80 side has consumed).

In both directions "the byte count is decremented by one and the address is
incremented by one each time a byte is transferred to or from the 53C80" [1]
§6.5.3 — the counters track the *SCSI side*, not the memory side.

Apple's direction names, from the processor-bus point of view [1] §6.7.2.3:

| Apple name | Start register written | Data direction | Processor bus cycle |
| ---------- | --------------------- | -------------- | ------------------- |
| DMA Read | `$050` (Start DMA Send) | memory → FIFO → SCSI | read |
| DMA Write | `$070` (Start DMA Initiator Receive) | SCSI → FIFO → memory | write |
| (target receive) | `$060` (Start DMA Target Receive) | SCSI → FIFO, target role | write if DMA enabled |

### 3.5 Non-modulo-4 transfers

"The method … the SCSI DMA handles non-modulo 4 addresses and non-modulo 4
byte counts is by adjusting for non-modulo 4 addresses on the first 68030 bus
transfer and adjusting for non-modulo 4 byte counts on the last 68030 bus
transfer" [1] §6.5.3. The size outputs follow the 68030 encoding [1] §6.5.3:

| SIZ1 | SIZ0 | Size |
| ---- | ---- | ---- |
| 0 | 1 | byte |
| 1 | 0 | word |
| 1 | 1 | 3 bytes |
| 0 | 0 | longword |

The first bus cycle of a memory→SCSI transfer is sized to end on the next
longword boundary:

| Starting `A[1:0]` | Max bytes in first cycle | SIZ1:SIZ0 |
| ---------------- | ------------------------- | --------- |
| `00` | 4 | `0:0` |
| `01` | 3 | `1:1` |
| `10` | 2 | `1:0` |
| `11` | 1 | `0:1` |

*Reconstructed from [1]'s worked examples:* the datasheet's own copy of this
table is garbled in the available scan; the mapping above is re-derived from
the "adjusting on the first transfer" rule and the examples below (a start
address of `…0101` yields a 3-byte first cycle with `SIZ=1:1`).

Worked example from [1] §6.5.3 — **SCSI → memory, start address `…0101`, count
9** (the byte lane is chosen by the current address; the running size count is
the number of bytes in the FIFO):

| Byte count | DMA addr | Byte lane | SIZ1:SIZ0 | Event |
| ---------- | -------- | --------- | --------- | ----- |
| `1001` | `0101` | 1 → `D[23:16]` | `0:1` | first byte in |
| `1000` | `0110` | 2 → `D[15:8]` | `1:0` | |
| `0111` | `0111` | 3 → `D[7:0]` | `1:1` | → bus write of 3 bytes to addr `0101` |
| `0110` | `1000` | 0 → `D[31:24]` | `0:0` | size bits `0:0` because count > 4 |
| `0101` | `1001` | 1 → `D[23:16]` | `0:0` | |
| `0100` | `1010` | 2 → `D[15:8]` | `0:0` | |
| `0011` | `1011` | 3 → `D[7:0]` | `1:0` | → bus write of 4 bytes to addr `1000` |
| `0010` | `1100` | 0 → `D[31:24]` | `0:1` | size bits `1:0` because count < 4 |
| `0001` | `1101` | 1 → `D[23:16]` | — | → bus write of 2 bytes to addr `1100` |
| `0000` | done | | | count = 0 → DMA complete |

The memory→SCSI direction runs the same arithmetic as a read: the same start
address and count produce bus *reads* of 3 bytes from `0101`, 4 from `1000`
and 2 from `1100` [1] §6.5.3. A second worked write (start `…0101`, count 2)
transfers 2 bytes to addr `0101` and completes at count 0 [1] §6.5.3.

### 3.6 Transfer completion

When the byte count reaches zero and no FIFO bytes remain, the DMA operation
terminates successfully: `$080` bit 6 reads 1 ("the DMA operation has
terminated successfully" [1] §6.6.6) and, if bit 1 is set, `/INT` asserts. On
the 53C80 side the end-of-process condition applies: with `$020` bits 3 (EOP
interrupt enable) and 1 (DMA mode) set, an internal `/EOP` asserted together
with `/DACK` and either `/IOR` or `/IOW` for >= 100 ns generates the 53C80 IRQ
[1] §6.8.1.3. For *send* operations the 53C80-level EOP does not mean the last
byte reached the wire — the Last Byte Sent bit (TCR bit 7, §2.3) exists for
exactly that distinction [2].

### 3.7 Bus errors

A `/BERR` termination of a bus-master cycle halts the operation: `$080` bit 8
("the requested DMA operation has been halted due to a bus error") reads 1 and
the interrupt asserts if `$080` bit 1 is set [1] §6.6.6. The datasheet does not
specify the FIFO's fate on a bus-error halt; the conservative model drops the
in-flight bytes, since "no longer continues" is the only stated behaviour.

### 3.8 Watchdog timer

The watchdog monitors DMA activity: "If no DMA activity happens before it
counts down, it will interrupt the CPU (if watchdog timer interrupts are
enabled — DMA Control Register bit 2 asserted) and terminate the DMA
operation. Any DMA activity causes the timer to be reloaded with the value
stored in the Watchdog Timer Register" [1] §6.5.2. Details:

* The counter counts down at `CPUCLK / 2`, starting immediately when `$140` is
  written [1] §6.6.3.
* Reload happens per DMA activity; "If no DMA operation occurs, no reload will
  take place" [1] §6.6.3. *Inferred — unverified:* the reload granularity is
  per successful DMA memory bus cycle / per byte transferred, not merely per
  whole operation — a whole-operation reload would make the timer useless
  against a mid-transfer stall.
* On reaching zero the counter **stops** ("After it has counted to zero it will
  not continue to count"), sets `$080` bit 7, and interrupts if bit 2 is set
  [1] §6.6.3. Expiry also terminates the DMA operation [1] §6.5.2.
* Watchdog interrupts are cleared by reading **or** writing `$140`, or by
  hardware reset [1] §6.6.3.

### 3.9 Interrupt model

`/INT` is an open-drain, active-low output (3.3 kΩ pull-up assumed) carrying
two families [1] §6.8:

```
/INT = ( $080.1 AND (53C80 IRQ OR DMA-done OR DMA-bus-error OR WONARB) )
     OR ( $080.2 AND watchdog-timeout )
```

`$080` bit 1 "allows the internal signal IRQ (interrupt request from the 53C80
cell) to get to the `/INT` output"; bit 6 reads back whether that IRQ has
reached the DMA logic side [1] §6.8.1. Clearing: a 53C80-side IRQ clears on a
read of `$070` or on `/RESET` [1] §6.8.1; a watchdog IRQ clears on a read or
write of `$140` [1] §6.8.2.

The 53C80-originated sources and their enable/verify conditions [1] §6.8.1:

| Source | Enable | Fires when | Verify via |
| ------ | ------ | ---------- | ---------- |
| `/SRST` assertion | — (cannot be disabled) | `/SRST` driven low, or `$010` bit 7 written 1: asserts `/SRST`, releases all SCSI bus signals, IRQ | `$050` bit 4 |
| Parity error | `$020` bits 5, 4 (and 1 for DMA receive) | parity checked on read of `$000` and write of `$070` | `$050` bits 5, 4 |
| End of process | `$020` bits 3, 1 | internal `/EOP` + `/DACK` + (`/IOR` or `/IOW`) >= 100 ns | `$050` bit 4 |
| Loss of BSY | `$020` bit 2 | `/SBSY` inactive for 400 ns while bit 2 set; tristates SCSI outputs and auto-clears `$020` bit 1 | `$050` bits 2, 4 |
| Phase mismatch | `$020` bit 1 | `$030[2:0]` != `$040[4:2]`, on the falling edge of `/SREQ` once `$050` (Start DMA Send) has been written; tristates SCSI outputs | `$050` bit 3 = 0, bit 4 |
| Chip selected | `$040` (select enable) nonzero | `/SSEL` asserted, `/SBSY` deasserted, `/SIO` deasserted, ID matches `$040` for 400 ns | `$050` bit 4 |
| Chip reselected | `$040` nonzero | same with `/SIO` asserted | `$050` bit 4 |
| Auto-arbitration won | `$080` bit 12 + MR bit 0 | arbitration won | `$080` bit 13, `$050` bit 4 |

### 3.10 Auto-arbitration

The enhanced cell performs the SCSI Arbitration phase in hardware. The SCSI ID
comes from `$080` bits 11:9 as a binary number 0–7, driven onto the bus as the
matching single data bit; `ARBEN` (bit 12) enables the feature and `WONARB`
(bit 13) reports the win [1] §6.6.6. With `ARBEN` set, writing MR bit 0
(`$020` bit 1… bit 0) starts the automatic sequence using that ID; on a win,
`WONARB` reads 1 and the 53C80 IRQ asserts (gated to `/INT` by `$080` bit 1)
[1] §6.7.1, §6.8.1.8. With `ARBEN` clear, the stock 53C80 software arbitration
— ID in ODR, `ICR.AIP`/`ICR.LA` polled by the CPU — still works [1] §6.7.1.

### 3.11 Reset paths

Four reset modes [1] §6.2.4:

| Mode | Trigger | Scope |
| ---- | ------- | ----- |
| SCSI DMA hardware reset | `/RESET` low >= 80 ns | entire chip: wrapper logic/registers/counters/latches + 53C80 cell; bus-master released; SCSI outputs released |
| DMA Control software reset | write `$080` bit 4 = 1 | 53C80 cell only (held >= its 100 ns requirement); wrapper registers untouched; whole chip not tri-stated |
| ICR software reset | write `$010` bit 7 = 1 | asserts `/SRST` + 53C80 IRQ, releases all SCSI bus signals, clears other internal 53C80 logic — a software SCSI bus reset |
| SCSI bus hardware reset | external `/SRST` low | 53C80 IRQ asserts, other internal 53C80 logic/registers clear |

Clear-up for the last two: write `$010` bit 7 = 0 (or deassert `/SRST`), then
read `$070` to clear the interrupt; a hardware reset or DMA Control software
reset also clears the state [1] §6.2.4.3–4.

### 3.12 Test modes

Three test modes [1] §6.2.3:

1. **53C80 isolation test** — `TEST` (pin 65) high redefines the pinout to
   bring internal 53C80 signals to the pads. Manufacturing mode; normal
   operation occurs with `TEST` low. Pads are valid <= 40 ns after `TEST`
   falls [1] Table 7.
2. **Tri-state test** — write `$010` bit 6 = 1: all outputs high-impedance
   until bit 6 is written 0 (§2.3).
3. **FIFO loopback / internal counters test** — write `$080` bit 5 = 1:
   `$180` becomes writable as well as readable, and the DMA count, DMA address
   and watchdog counters are connected so that "each nibble of each counter
   will count independently and will be incremented or decremented (… the DMA
   Count and Watchdog counters decrement while the DMA Address counter
   increments) by a read of their respective initial-value-holding registers
   (Registers `$0C0`, `$100`, or `$140`)" [1] §6.2.3.3. That is: a read of
   `$0C0` returns the count and then decrements every nibble; a read of `$100`
   returns the address and then increments every nibble; a read of `$140`
   returns the reload, clears the watchdog IRQ latch, and decrements every
   nibble of the live counter. (*Inferred — unverified:* the post-read update
   model — the scan does not state whether the incremented/decremented value or
   the old value is visible on the read that caused it.)

### 3.13 Timing

General [1] Table 7: `CPUCLK` <= 25 MHz (period >= 40 ns, high >= 16 ns, low >=
16 ns); `/RESET` assertion >= 80 ns; `/RESET` rising to full operation 8 clock
periods; `TEST` falling to valid pads <= 40 ns. PIO: `/AS` falls 3–18 ns from
the `CPUCLK` falling edge; `/DSACK` falls <= 30 ns from the rising edge and
rises <= 20 ns after `/AS` rises; `/AS` holds 20 ns–2ø after `/DSACK` falls.
DMA (bus-master): `/BR` fall and `/BGOUT` break <= 40 ns; `/BGACK` <= 65 ns;
high-order address valid <= 55 ns, invalid with low-order valid <= 50 ns;
`/ALE` fall <= 45 ns, rise <= 40 ns; `/ABEN`, `R/W`, `SIZ`, `FC1` <= 45 ns;
`/AS` fall/rise <= 18 ns; `/STERM`/`/DSACK0`/`/BERR` sampled within 20 ns of
their clock edges. Output rise/fall at 100 pF: `/AS`, `/DSACK0`, `/DSACK1`
<= 5 ns; all other outputs <= 17 ns [1] Table 8.

Two system-level notes from the datasheet: the read-cycle data hold time (t35)
"is longer than the data hold timing of the 68030" [1] Note 1; and in systems
with more than two bus masters besides the 68030, the daisy-chain delay from
`/BGOUT` to the `/BGACK` setup (t29) "must be less than the round trip delay of
the daisy chain above the SCSI DMA" [1] Note 2.

## 4. Programming model

### 4.1 Polled transfers

Standard 53C80 operation; "No special register setup is needed" [1] §6.7.2.1:
the CPU polls `$040`/`$050` for REQ and phase match, moves bytes through
`$000`/`$060`, and handshakes ACK through `$010` bit 4. This is also the
53C80-compatibility path the "DMA bypass mode" preserves [1] §6.0.

### 4.2 Hardware-handshake sequences (Apple's published recipes)

For writes, memory → SCSI [1] §6.7.2.2:

```
W $080 = $00000008     ; hardware-handshake enable
W $020 = $02           ; 53C80 DMA mode
W $010 = $09           ; assert SCSI data bus, assert BSY
W $030 = $00           ; I/O not asserted — data-out phase match
W $050 = $XX           ; Start DMA Send
repeat: W $000 = data  ; each CPU write completes only after the byte reaches SCSI
```

For reads, SCSI → memory [1] §6.7.2.2:

```
W $080 = $00000008     ; hardware-handshake enable
W $020 = $02           ; 53C80 DMA mode
W $010 = $08           ; deassert SCSI data bus, assert BSY
W $030 = $01           ; assert I/O — data-in phase match
W $070 = $XX           ; Start DMA Initiator Receive
repeat: R $060         ; each CPU read completes only after a byte arrives from SCSI
```

### 4.3 Bus-master DMA sequences (Apple's published recipes)

For DMA reads, memory → SCSI [1] §6.7.2.3:

```
W $140 = <watchdog>    ; fill watchdog timer register
W $080 = $00000007     ; DMA enable + SCSI interrupt enable + watchdog interrupt enable
W $100 = <address>     ; starting DMA address (physical)
W $0C0 = <count>       ; byte count
W $020 = $0A           ; enable EOP interrupt + DMA mode
W $010 = $09           ; assert SCSI data bus, assert BSY
W $030 = $00           ; data-out phase
W $050 = $XX           ; Start DMA Send — the operation begins
```

For DMA writes, SCSI → memory [1] §6.7.2.3, the same head with the receive
tail:

```
W $140 = <watchdog>
W $080 = $00000007
W $100 = <address>
W $0C0 = <count>
W $020 = $0A
W $010 = $08           ; deassert SCSI data bus, assert BSY
W $030 = $01           ; data-in phase
W $070 = $XX           ; Start DMA Initiator Receive
```

After the interrupt [1] §6.7.2.3:

```
R $080                 ; bits 8..6: bus error / watchdog / SCSI-or-DMA-done
R $050                 ; 53C80 bus-and-status, for the SCSI-interrupt reason
R $070                 ; clear the 53C80 interrupt
W $020 = $00           ; clear DMA/EOP mode bits
W $080 = $00000000     ; disable and clear Apple DMA state
W $010 = $00           ; clear initiator-command bits
W $030 = $00           ; clear phase bits (receive path)
```

### 4.4 Auto-arbitration sequence

Apple's printed sequence for the Arbitration phase [1] §6.7.1 (transcribed from
the document's flow table):

```
W $140 = <watchdog>
W $080 = ARBEN | ID[2:0] | WD_INT_EN | SCSI_INT_EN
W $020 = $01           ; ARBITRATE — starts the automatic sequence
wait for /INT
R $080                 ; check WONARB (bit 13); on loss, R $050 for status
R $070                 ; clear the interrupt
W $080 = $00000000     ; clear setup bits
```

### 4.5 What real software does

**A/UX 3.0.1 (observed from kernel disassembly [4]).** The kernel selects the
IIfx path with a chip-type global (`$0005ABBA` = `$0B`) and a chip-base global
(`$0005B20A` = `$50F08000`) planted by `pstart`. Bulk transfers are chunked:
the driver walks a software scatter-gather list of 10-byte entries
{32-bit base, 32-bit length, 2 pad} and re-arms the engine once per entry, in
one SCSI command per 8 KB chunk, clamping each arm to 32 KB (`scsi_vio`,
`$10049068`: `CMPI.L #$8000`). Each arm is programmed by `scsi_in`
(`$1004915C`, read direction; `scsi_out` is the write mirror):

```
$100 = <destination physical address>          ; per-arm address from the SG entry
$0C0 = <arm byte count>                        ; e.g. $2000
$080 <- (R $080) & $F7 ; then | $05             ; clear bit 3; DMA enable + watchdog int enable
$140 = $02FAF080                                ; watchdog reload = 50,000,000
$020 |= $0A                                     ; 53C80 DMA mode + EOP interrupt enable
$080 |= $02                                     ; add SCSI interrupt enable -> $07
W $070                                          ; Start DMA Initiator Receive — go
```

(Some of these are read-modify-write sequences; the observed register-write
series per arm is: `$080=$00`, `$020=$00`, `$020=$01`, `$020=$00`, `$080=$08`,
`$080=$00`, `$080=$08`, `$020=$00`, `$020=$02`, `$050` (CDB out), then the
data-phase arming above.)

The interrupt handler `scsiirq` (`$1004A882`) snapshots `$050` and `$040`,
acknowledges the wrapper side with a read-modify-write of `$080` clearing bit
3, classifies the cause from the 53C80 status bits (true end-of-DMA on a
data-phase arm, bus error, phase change, selection/reselection, parity), and —
for every non-spurious cause — reads `$070`, which clears the 53C80 IRQ. On a
wrapper bus error it additionally writes `$140 = 0` (disarm the watchdog) and
clears `$080` bit 2. The end-of-arm handler `dodmadone` (`$1004A1FE`) reads
the residual from `$0C0` to advance its transfer state, and the verbose trace
path reads both `$0C0` and `$100` back as live counters. `/INT` reaches the
kernel through the OSS level-2 dispatcher (`pstart` `$00058AD8`), which maps
bit 1 of the OSS pending word at `$50F1A202` to the SCSI handler [4].

**Macintosh System Software.** Apple's stated 1990 position is that System
Software does not use the IIfx SCSI DMA capability and that developers should
wait for OS support [3]; the compatibility consequence is that the chip must
behave as a plain 53C80 when the wrapper is never armed. The precise register
sequences the IIfx ROM and later SCSI Manager versions perform against the
wrapper are not established here (§6).

## 5. Quirks & errata

* **`$080` bit 4 is two unrelated things.** Write-only reset strobe on the
  write side; FIFO-not-empty status on the read side [1] §6.6.6. A driver that
  writes status back verbatim (read-modify-write) will spuriously reset the
  53C80 whenever the FIFO happens to be non-empty. The A/UX handler
  read-modify-writes `$080` with explicit masks [4].
* **Bit 4 does not need clearing.** The cell is reset once per assertion; the
  bit is a strobe, not a mode [1] §6.2.4.2.
* **The watchdog stops at zero** rather than wrapping, and reloads only on
  DMA activity [1] §6.6.3 — a quiet bus expires it even though nothing is
  wrong with the transfer that armed it.
* **The watchdog clock is `CPUCLK/2`, not the 68030 clock.** `CPUCLK` is the
  chip's own input (max 25 MHz) [1] Table 7; the IIfx's 68030 runs faster.
  Watchdog reload values must be scaled against the board's `CPUCLK`, whose
  frequency is not established here (§6).
* **Loss of BSY and phase mismatch have destructive side effects** beyond the
  IRQ: both tristate the SCSI outputs, and loss of BSY auto-clears MR bit 1
  (DMA mode) [1] §6.8.1.4–5. A driver that assumed DMA mode survived the
  error loses its transfer context.
* **Select Enable = 0 disables selection interrupts** [1] §6.8.1.6 — unlike a
  naive reading of the 5380, the select/reselect interrupt is gated by a nonzero
  mask.
* **Misaligned DMA is only supported into "normal (not Nubus) memory"** [1]
  §6.0. Non-modulo-4 transfers rely on shortened bus cycles that NuBus memory
  does not serve; buffers for misaligned DMA must live in main memory.
* **ICR bit 6 tri-states the whole chip** — processor-bus outputs and
  bus-master control included, not just the SCSI drivers [1] §6.2.3.2.
* **The FIFO register is read-only in normal operation**; writes land only in
  loopback test mode [1] §6.6.2.
* **`/INT` is open-drain** and its timing assumes 3.3 kΩ pull-ups; `/BR` is
  open-drain too [1] Table 7.
* **The DMA-read data hold (t35) exceeds the 68030's own hold time** [1]
  Note 1 — harmless to a 68030 master, relevant to anyone retiming the bus.
* **Datasheet defects (as scanned):** the first-cycle size table in §6.5.3 is
  garbled (reconstructed in §3.5 from the worked examples); §6.8.2's prose
  says "bit 6" for the watchdog-pending indication while the register table —
  followed here — says bit 7 [1].
* **A real-software tension in the address model (unresolved).** *Observed:*
  for a chunked 80 KB read issued as ten 8 KB arms, the A/UX kernel writes
  `$100` once per arm with the *same* value each time and never programs the
  other destination addresses into the chip [4]; under the documented
  semantics — write `$100`, run to count zero, byte-wise increment — every arm
  should land on the same 8 KB. The kernel nevertheless consumes the ten
  chunks as if they landed at successively advancing addresses. Whether real
  hardware auto-advances the address counter across operations, whether the
  kernel's staging feeds different values per arm, or whether real-hardware
  A/UX is itself fragile here is not settled by the published documentation;
  see §6.

## 6. Open questions

1. **`CPUCLK` on the IIfx board.** The chip accepts up to 25 MHz; the IIfx's
   68030 runs at 40 MHz, so the chip's clock is a divided or separate feed.
   Its actual frequency — which scales every watchdog reload — is not
   established from the available material.
2. **IIfx address decode of the `/CS` region.** The A/UX kernel base
   `$50F08000` is observed [4]; the full decode width, the low alias of the
   same window, and the IIfx's I/O mirroring rules around it are not
   established here. Earlier machines expose separate polled-register and
   handshake apertures for the 5380; whether the IIfx decodes additional
   apertures of this chip beyond the `/CS` window is unverified.
3. **The chunked-DMA address behaviour.** The paradox of §5 (same `$100` value
   written per arm, kernel expecting advancing destinations) has no answer in
   the published chip documentation: it predicts each arm overwrites the same
   buffer. Possible resolutions — undocumented address-counter behaviour in
   the chip, a kernel staging path not yet identified in the disassembly, or
   genuine fragility of A/UX 3.0.1 on real hardware when the buffer allocator
   scatters a chunked transfer — are unverified against real hardware.
4. **`$100` readback semantics between operations.** "Contains the current
   DMA address" suggests a live counter; whether a read between operations
   returns the last written value or an advanced counter, and at which event a
   written value is sampled, is not stated. A/UX reads `$100` back only for
   logging, so its driver does not disambiguate this.
5. **Reserved `$080` bits 14–31** ("reserved for future expansion"): whether
   they read as zero and ignore writes on production silicon is unverified.
6. **`/DSACK0` vs `/DSACK1` per access width.** The pin table ties `/DSACK1`
   to "completion of a 32 bit PIO data transfer" and the timing tables list
   both for register cycles, but the exact assertion pattern for 8-bit vs
   32-bit register accesses is not spelled out.
7. **The TEST-pin redefined pinout** (isolation-test Table 3) is a
   manufacturing mode and is not transcribed here.
8. **Whether any shipping Mac OS version ever arms the bus-master engine.**
   Apple's 1990 statement says System Software did not [3]; ROM-level analysis
   suggests later SCSI Manager revisions contain IIfx DMA-aware code paths,
   but no sequence from a shipping ROM is verified here.
9. **FIFO state after a bus-error halt**, and the send-direction DREQ trigger
   condition, are inferred (§3.3, §3.7) rather than documented.

## References

1. Apple Computer, Inc., *IC, Custom SCSI DMA Controller, QFP-100*, engineering
   specification and drawing 343S0064-A, rev. A (production release), c. 1990.
2. NCR Corporation, *NCR 5380/53C80 SCSI Interface Chip Design Manual* (the
   embedded cell's register-level reference, referred to by [1] §6.6.1).
3. R. Collyer, *Macintosh IIfx: The Inside Story*, Apple Technical Note
   HW #09, Apple Computer, Inc., April 1990.
4. A/UX 3.0.1 kernel disassembly, SCSI driver block: `pstart` level-2 OSS
   dispatcher (`$00058AD8`) and chip-base setup (`$0005810A`); `scsi_dmatype`
   (`$10048E80`); `scsi_vio` (`$10048FB0`); `scsi_in` (`$1004915C`);
   `scsiirq` (`$1004A882`); `scsisched` (`$1004993C`); `dodmadone`
   (`$1004A1FE`).
