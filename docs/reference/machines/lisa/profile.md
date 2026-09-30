# ProFile and Widget — the Lisa parallel hard disks

**Contents:**

1. [Overview](#1-overview) — what the two drives are, which machines carry them, the host port vs the
   drive-side controller, and what the interface is *not*
2. [Register file](#2-register-file) — the DB-25 connector, the 6522 wiring (ports A/B, CA1/CA2/CB1/CB2),
   addressing and aliases, the registers as Apple's drivers program them, interrupts, the expansion-card port images
3. [Behaviour](#3-behaviour) — the handshake primitive, single-block protocol, Widget system commands,
   state bytes and replies, status bytes, block and page-label format, checksums, interleave, sparing, timing
4. [Programming model](#4-programming-model) — boot ROM probe and bootstrap, OS driver init and state machines,
   the OS boot loader, a second drive on an expansion card, observed guest behaviour
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

This page is the device reference for the Lisa family's hard-disk subsystem: the external **Apple ProFile**
(5 MB and 10 MB) and the internal **Widget** (the Lisa 2/10's Winchester). It expands, and where they differ
supersedes, the summary in [lisa.md](lisa.md) §14; the host-side VIA wiring is specified in
[lisa.md](lisa.md) §10 and the level-1 interrupt demux in [lisa.md](lisa.md) §12, which this page cites rather
than restates.

### 1.1 What the parts are

The ProFile is a self-contained hard-disk subsystem: a drive mechanism plus its own intelligent controller
in an external cabinet, cabled to the Lisa's general-purpose 8-bit **parallel port** — a 25-pin D connector
with pin 7 blocked to make an RS-232 cable impossible [1] §3.5.2 pp. 3-20 – 3-21. The port exists for exactly
this purpose: "This interface is normally used to connect a hard disk, such as the Apple ProFile, to the
Lisa" [1] §3.5.2, and the hardware manual's parallel-port chapter describes the ProFile as "a typical
application" of it [1] §6.5 p. 6-46. The Widget is the same protocol on an internal cable: the 10 MB
integral Winchester of the Lisa 2/10 and the rebadged Macintosh XL (§1.2). Apple's Boot ROM and OS driver
source call the drive-side controller **"Pippin"** ("did pippin return state requested?" [3] `FINDDD2`;
[4] `PROFASM/RESPOND`) — an Apple-internal name, used here only as the source comments use it.

The interface is **not SCSI** — the Lisa predates it and carries no SCSI hardware — and it is equally not
the Z8530: the Z8530 SCC serves the two *serial* ports only ([1] §6.4; [lisa.md](lisa.md) §15). The hard-disk
port is a dedicated parallel interface built from the second 6522 VIA plus parity and driver glue ([1] §6.5;
§2 below). The host never touches a drive signal: it exchanges a handful of state bytes, writes a 6-byte
command block, and whole 532-byte blocks stream one byte per handshake. Everything below the command
interface — seek, data separation, media-level CRC, defect sparing — belongs to the drive's controller.

### 1.2 Which machines carry them

| Machine | Drive present | Where | Boot ROM's machine-type byte (`$FCC031`) | Evidence |
|---|---|---|---|---|
| Lisa 1 (1983) | ProFile 5 MB / 10 MB | built-in parallel port | `0` (Lisa 1) | [3] module header; default boot is "a Profile attached to the builtin parallel port" |
| Lisa 2/5 | ProFile 5 MB / 10 MB | built-in parallel port | `1` (Lisa 2), `2` (Lisa 2 with external hard disk) | [3]; [5] p. 1-1 (a 2/5 "must have at least one external 5 megabyte ProFile") |
| Lisa 2/10 | Widget 10 MB | internal, on the same parallel interface | `3` (Lisa 2 with internal hard disk) | [3] module header and `ICONCHK` |
| Macintosh XL | Widget 10 MB | internal (rebadged 2/10) | `3` | [2]; [lisa.md](lisa.md) §17 |

The type byte comes from the ROM's power-on machine detection ([lisa.md](lisa.md) §16.2). On a machine with
the internal drive the ROM *skips* the external-ProFile probe — the boot-menu builder's comment is
"skip check if internal disk", and it offers only the integral-drive icon — because the built-in parallel
interface is occupied by the Widget [3] `ICONCHK` (*observed* in the released
ROM source). The Widget is therefore not a second port — it is the *same* port, internally cabled
(*inferred — unverified*: no schematic in the evidence set shows the internal cable, but the ROM drives both
drive types through the identical VIA2 protocol and buffer addresses).

Drive types the OS distinguishes, from the released OS driver [4] `PROFDRVR`:

| OS drive type | Value | Drive | Recognition (from the device-info block, §3.3) |
|---|---|---|---|
| `T_Profile` | 0 | 5 MB ProFile | type byte `0`, reported size ≤ 9,728 blocks (or > 30,000, a belt-and-braces branch) |
| `T_Seagate` | 1 | 10 MB external ProFile ("10mb seagate" per the driver source) | type byte `0`, 9,728 < reported size ≤ 30,000 |
| `T_Widget` | 2 | internal Widget | type byte ≠ `0`, 9,728 < reported size ≤ 30,000 |

### 1.3 Division of labor

| Function | Owner | Evidence |
|---|---|---|
| 8-bit data path DD0–DD7, strobe, direction, command phase, busy, cable detect | host: VIA2 + bus transceivers | [1] §6.5.2 pp. 6-47 – 6-49 |
| Odd parity generation/check over DD0–DD7 | host: two 9-bit LS280 parity generators, result latched in an LS109 JK flip-flop | [1] §6.5.2 p. 6-48 |
| Controller reset (`CRES/`), parity latch reset (`PRES/`) | host: VIA1 (keyboard VIA) PB7 and PB5 | [1] §6.6.5 p. 6-52a; [3] `DOCRES` |
| Command decoding, seek, MFM read/write, media CRC, defect sparing, spare table | drive controller ("Pippin" / Widget controller) | [3], [4] (the command/status protocol, below) |
| Page-label checksum | host: computed and inserted into the label before write; verified after read | [4] `PROFASM` `C_SUM`, `WRHDR`, [4] `LDRLDR` |
| Device query (type, size) | drive controller: synthesized block at block `$FFFFFF` | [4] `PROF_INIT`, `LDRLDR` (§3.3) |

The drive's controller is a computer in its own right — it powers up, runs a self-test measured in tens of
seconds (§3.11), maintains a spare table, and answers to a state-byte protocol. Its internal processor type
and firmware are not documented by any source in this page's evidence set ([1]'s preface lists a companion
*ProFile Owner's Manual* that covers the drive side); see §6.

### 1.4 What the interface is not

- **Not SCSI.** No SCSI bus, no initiator/target phases; the Lisa has no SCSI hardware of any kind.
- **Not the Z8530.** The SCC (Z8530) handles the two RS-232/RS-422 serial ports [1] §6.4 pp. 6-45 – 6-46; it
  has no connection to the hard disk.
- **Not a raw drive bus.** No step/direction or read-data lines cross the connector; the only drive-state
  lines are `BSY`, `OCD` (cable detect) and `CHK` (fault) [1] Figure 2-11.
- **Not exclusively the hard disk's.** Port A of the same 6522 also feeds the video contrast latch (a CMOS
  74C174 driving the display's contrast DAC) [1] §6.7.3 p. 6-55, and VIA2's self-test in the ROM doubles as
  the contrast-latch test [3] `VIA2TST` — see §5.

---

## 2. Register file

The host-side register surface of the drive subsystem is the second 6522 VIA (VIA2 in the Hardware Manual's
naming) plus the keyboard-VIA lines `PRES/` and `CRES/` and the keyboard-encoded `CHK`. Generic 6522
register semantics — timers, handshake modes, IFR/IER, DDRA/DDRB — are [via.md](../../hardware/via.md);
this section specifies the wiring, the
values Apple's drivers program, and the drive-visible meaning of each line.

### 2.1 The connector

25-pin D-type female, pin 7 blocked [1] §3.5.2 p. 3-21. Signal assignments from [1] Figure 2-11 p. 2-17 and
§3.5.2 pp. 3-21 – 3-22:

| Pin | Signal | Driven by | Function |
|---|---|---|---|
| — | `DD0`–`DD7` | bidirectional | eight data lines, bit DD7 is the MSB; pins not given in the manual's text (Figure 3-8 is a graphic) — see §6 |
| 3 | `DR/W` | Lisa (VIA2 PB3) | high = data expected *into* the Lisa; low = data being output |
| 7 | *(blocked)* | — | filled to prevent RS-232 cabling |
| 15 | `PSTRB/` | Lisa (VIA2 CA2) | processor strobe: valid data out, or data received |
| 16 | `BSY` | drive | asserted (low) = busy, unable to process commands; also VIA2 PB1/CA1 |
| 17 | `CMD/` | Lisa (VIA2 PB4) | asserted (low) = command byte(s) placed on the data lines / handshake phase |
| 18 | `PARITY/` | bidirectional | must carry odd parity of the data lines |
| 19 | `OCD` | drive (pulled low) | high = no device connected |
| 21 | `CRES/` | Lisa (VIA1 PB7) | asserted (low) = reset the peripheral to power-on state |
| 25 | `CHK` | drive | fault indication; see §2.5 |

### 2.2 The 6522 wiring

VIA2's port A is the 8-bit data path (and the contrast-latch source, §1.4); port B carries control and
status [1] §6.5.2 p. 6-47:

| Line | VIA2 bit | Direction (as the drivers set it) | Meaning |
|---|---|---|---|
| `OCD` | PB0 | in | 1 = open cable, no device [1] §6.5.2 p. 6-48 |
| `BSY` | PB1 | in | 1 = not busy, 0 = busy (§3.1); *also* wired to CA1 |
| `DEN` | PB2 | out | 0 = enable the LS245 data transceiver and LS244 control drivers [1] §6.5.2 p. 6-48 |
| `DRW` | PB3 | out | 1 = read direction, 0 = write |
| `CMD/` | PB4 | out | 0 = command/handshake phase asserted |
| `DIAGPAR` | PB5 | — | named "Diagnostic Parity" in [1] Figure 2-11; not exercised by the released ROM or OS driver (§6) |
| `DSKDIAG` | PB6 | in | from the floppy-disk controller: the 68000 is operating the interface in diagnostic mode [1] §6.5.2 p. 6-49 |

The 6522 control lines [1] §6.5.2 pp. 6-48 – 6-49:

| Control line | Wiring | Use |
|---|---|---|
| CA1 | `BSY` (driven by the drive) | the data interrupt input: an edge on the busy line is the protocol's event signal (§3.1) |
| CA2 | `PSTRB/` (to the drive, pin 15) | per-byte strobe; the drivers run it in pulse-output mode so every port-A access pulses it [1] §6.5.2 p. 6-49 |
| CB1 | (see §6) | the drivers clear and test IFR bit 3 around every parity-checked transfer as the parity-error flag (*observed*, [3] `STAT01`, [4] `PROFASM`) |
| CB2 | parity status of the interface | "CB2 is used to monitor the parity status of the interface" [1] §6.5.2 p. 6-49; see §6 on the CB1/CB2 tension |

Two drive control signals live on the **keyboard** VIA (VIA1, base `$00DD81`, [lisa.md](lisa.md) §10.1),
not on VIA2 [1] §6.6.5 p. 6-52a, Figure 2-11 note:

| Line | VIA1 bit | Function |
|---|---|---|
| `PRES/` | PB5 | resets the parallel-port parity flip-flop (LS109); the drivers pulse it before every parity-checked transfer |
| `CRES/` | PB7 | controller reset, connector pin 21; drives it low for ~100 µs to reset the drive controller (§3.11) |

### 2.3 Addressing and aliases

The full VIA2 register map — 16 registers at stride 8 off base `$00D901`, both `$D8xx` and `$D9xx` aliases,
and the VIA1 map — is [lisa.md](lisa.md) §10.2 and is not repeated here. What the drive protocol adds:

- Register selection is on address lines **A3–A6** (16 locations), device select on `DSKPT/` + VMA, clocked
  by the 6800-style `E` signal [1] §6.5.1 p. 6-47 — which is why the whole `$00D800`–`$00D9FF` window
  aliases the 16 registers and the two documented base images both work.
- The Boot ROM addresses VIA2 at `$00FCD901` [3] `RM248.E` (`VIA2BASE`), i.e. the manual's `$D901` alias;
  the released OS driver uses the `$D801` alias (`hwbase := iospacemmu*$20000 + $0D801`, with
  `iospacemmu = 126`, so logical `$FCD801`) [4] `PROFDRVR`.
- The OS driver computes its parity/reset handle as `hwbase + $400` — logical `$FCDC01`, i.e. I/O-space
  offset `$00DC01` — and drives bits 5 and 7 of *that* ORB as parity-reset and controller-reset [4]
  `PROF_INIT`, `PROFASM` (`HWSTATUS`, `RESETC = hwbase + $580`). `$00DC01`/`$00DC05` behave as aliases of
  VIA1's ORB/DDRB (register select of the keyboard VIA is on A1–A4 [1] §6.6.1 p. 6-49) — *inferred —
  unverified*: no schematic decode in the evidence set names `$DCxx`, but every field the driver writes or
  reads there (reset, parity reset, BSY mirror) is a keyboard-VIA port-B function per [1] §6.6.5.
- The keyboard VIA itself is at `$00DD81` (VIA1, [lisa.md](lisa.md) §10.1); the Boot ROM uses exactly that
  address for `CRES/` [3] `DOCRES`.

### 2.4 The control registers as the drivers program them

Values the released ROM and OS driver write, which together pin the required 6522 configuration (*observed*):

| Register | Value | Effect | Evidence |
|---|---|---|---|
| PCR | `$6B` (bit 4 preserved) | CA1 interrupts on the **rising** edge (busy released); CA2 = **pulse output** (one `PSTRB/` pulse per port-A access); CB1 = negative edge | [3] `PROINIT`, [4] `PROF_INIT`; mode table [via.md](../../hardware/via.md) §"Peripheral Control Register" |
| PCR bit 0 | 0 while waiting for the controller to go busy, 1 otherwise | the driver flips CA1's edge between "busy asserted" and "busy released" phases | [4] `PROFASM` `S1`/`RESPOND` |
| ACR | bits 0–1 cleared (`ANDI #$DC`) | no input latching on ports A/B; timer/shift modes untouched | [4] `PROF_INIT` |
| IER | `$3B` (disable) at init/down, then `$A2` (enable) | enabled sources: **CA1** (busy edge) and **T1**; the state machine's completion event is the CA1 flag | [4] `PROF_INIT`, `PROF_DOWN`, `DRIVER` entry |
| IFR | bit 1 read/cleared as completion; bit 3 cleared before and tested after parity-checked transfers; timer bits cleared as handled | | [4] `PROFASM` passim, `DRIVER` entry |
| DDRA | `$00` (input) / `$FF` (output) | flipped around every phase change: the data bus is driven only by the current talker | [3] `PROINIT` et al., [4] `PROFASM` passim |
| DDRB | `$1C` (bits 2,3,4 out; 0,1 in) | `DEN`, `DRW`, `CMD/` outputs; `OCD`, `BSY` inputs | [3] `PROINIT`, [4] `PROF_INIT` |
| ORB init | `$18`, then clear bit 2 | `DRW` = 1 (read direction), `CMD/` = 0 (no command), then `DEN` = 0 (bus drivers on) | [3] `PROINIT`, [4] `PROF_INIT` |
| T2 | loaded `$FF`, reloaded per poll tick | response-timeout poll tick (the OS driver re-arms it in the interrupt entry; [4] comment "poll again in .1 sec") | [4] `PROFASM` `S1`/`S200`, `DRIVER` `@30` |

The 6522s are clocked by the `E` signal at one quarter of the 68000 cycle ([1] §6.5.1; [lisa.md](lisa.md) §10).

### 2.5 Interrupts

- VIA2 interrupts the 68000 through the `IOIR/` line, **wired-OR with the floppy controller's `FDIR/`**
  [1] §6.5.1 p. 6-47. On the Lisa this aggregates at **IPL 1** and the level-1 handler must demux by polling
  ([lisa.md](lisa.md) §12): VIA2's IFR for the parallel transition, the floppy status byte, and the video
  VBL bit.
- The OS driver's interrupt entry reads `IFR & $2A` (CA1, CB1, T1), clears what it saw, and treats the CA1
  flag as the I/O-completion event [4] `DRIVER` entry (*observed*).
- `CHK` does **not** interrupt through VIA2: the line "is encoded by the keyboard interface and produces an
  independent interrupt and key code `D000`₂ `0101`₂, where D is a 1 on a rising edge and a 0 on a falling
  edge" [1] Figure 2-11 note p. 2-17 — a drive fault arrives as a keyboard event through the COPS path
  ([lisa.md](lisa.md) §11).
- The Boot ROM does not use VIA2 interrupts at all: its read path runs with interrupts disabled and polls
  the `BSY` level in IRB [3] `PROREAD`, `WFBSY`, `WFNBSY`.

### 2.6 The parallel expansion card's port images

A ProFile can also hang off an **expansion-slot parallel card**. Two facts are documented:

- The "Parallel Interface Card" carries **two** 6522s, one per port; their base addresses are the slot's
  low decode + `$2000` (lower port) and + `$2800` (upper port), and the card's ROM version sits at the slot
  low decode + `$FF8` [2].
- The released OS "four-port card" driver places each channel's 6522 at
  `slot_base + $2001 + $800 × channel` and dispatches interrupts by reading channel IFRs at that stride
  [4] `FOUR_PORT` (`SOURCE-2PORTCARD`); the Profile driver computes the same per-channel base
  (`$4000 × slot + $2001 + $800 × iochannel`) and uses **the channel's own ORB bits 5/7** as its
  parity-reset/controller-reset lines instead of the keyboard VIA [4] `PROFDRVR` `hdinit`,
  `PROF_INIT` (`hwstatus = hwbase` there, unlike the built-in port). Channels 0–2 can interrupt; channel 3
  cannot [4] `FOUR_PORT`.

The card's ROM and status-byte protocol are otherwise the same single-block ProFile protocol ([4] uses one
code path for both).

---

## 3. Behaviour

### 3.1 The handshake primitive

Every protocol phase is the same five-beat exchange, which Apple's source calls a *handshake*:

1. The host asserts `CMD/` (VIA2 ORB bit 4 ← 0), sets port A to input, arms CA1 for the **falling** edge,
   and clears the pending CA1 flag [4] `S1`.
2. The drive controller takes the bus, **asserts busy** (`BSY` line low, PB1 reads 0) and places its
   **state byte** — the phase code of §3.4 — on DD0–DD7 [4] `S1` `WFBSY`; the ROM polls the level
   (`WFBSY`) [3], the OS takes the CA1 falling edge or polls for ~1 ms first [4] `S2`.
3. The host reads the state byte through the **no-handshake** port-A register (register 15, "ORA/IRA
   without handshake" [1] Figure 2-12) — never through the handshaked register, which would pulse
   `PSTRB/` [4] `RESPOND`, [3] `GETRSP`.
4. The host turns the bus around (DDRA = `$FF`, `DRW` = 0), writes its **reply** byte to the same
   no-handshake register, and **deasserts `CMD/`** [4] `RESPOND` `SNDR1`.
5. The controller releases busy (`BSY` high, PB1 reads 1) — the ROM polls `WFNBSY`, the OS takes the CA1
   rising edge [3], [4] `S2`.

Busy polarity, from both released drivers (*observed*): **PB1 = 1 means not busy/ready; PB1 = 0 means busy**.
The ROM's ready-poll comment makes it explicit: "check if Profile ready (not busy)" on a *nonzero* PB1 [3]
`CHKBSY`. (The manual names the line `BSY/` and describes it asserted-when-busy [1] p. 3-22 — consistent with
an active-low busy.) The host reply bytes:

| Reply | Meaning | Used by |
|---|---|---|
| `$55` | affirmative — proceed | every successful handshake [3] `FINDD2`, [4] `RESPOND` |
| `$AA` | negative (probe/initialization path only) | [4] `DOSHAKE` `RS_BAD` |
| `$00` | negative — and *reset the controller's transaction state* | [4] `RESPOND` `BAD_RSP`; the ROM's probe sends `'0' response to reset Profile` [3] `CHKPROFILE` |
| `$69` | "free device" / negative on the Widget | [4] `S2A`, `RESPOND` |

Between handshakes, bulk bytes move through the **handshaked** port-A register (register 1): each read or
write of it pulses CA2 = `PSTRB/` (§2.4), which is the per-byte clock. The OS driver's tuned loops carry the
required spacing: "OPTIMAL READ RATE HAS 14-21 CPU CYCLES BETWEEN BYTES (INCLUDING THE READ OPERATION
ITSELF). ANY FEWER CYCLES, AND THE PULSE HANDSHAKE IS NOT GUARANTEED TO BEAT THE NEXT READ" [4] `RDDATA`;
the write side carries the same 14–21-cycle note [4] `WRDATA`.

### 3.2 Single-block I/O (ProFile and 10 MB ProFile)

The command frame is six bytes, sent to the handshaked port-A register with `DRW` = 0 [3] `STRTRD`,
[4] `S3`:

| Byte | Field | Values |
|---|---|---|
| 0 | command | `$00` read, `$01` write |
| 1–3 | block number | 24-bit, big-endian |
| 4 | retry count | the drivers use 10 |
| 5 | sparing threshold | the drivers use 3 (§3.9) |

A read transaction, in order (*observed* in [3] `PROREAD`/`STRTRD`/`STAT01` and [4] `NEW_CMD`/`S1`–`S7`):

1. Handshake: controller state `$01`, host replies `$55`.
2. Host sends the 6 command bytes (per-byte `PSTRB/`), sets `DRW` = 1, port A to input.
3. Handshake: controller state `$02` (read accepted), host replies `$55`.
4. Host reads **4 status bytes** (§3.5), then the block: **20-byte page label, then 512 data bytes** (§3.6)
   — 532 bytes total.
5. If status says sparing occurred (§3.5), one extra handshake runs: the host sends an "illegal" command
   byte `$FF` and handshakes accepting *any* response — the comment calls it the "extra handshake to update
   spare table" [4] `S30`/`HS`.
6. Multi-sector OS requests simply repeat from step 2 with the block number incremented [4] `S7`.

A write transaction mirrors it: after the `$03` handshake (write accepted) the host sends 20 label bytes
then 512 data bytes, then handshakes against state `$06` (data received), then reads the 4 status bytes
[4] `WRT` state table. The Boot ROM implements only the read path — that is all booting needs [3] `PROREAD`.

Command byte `$02` is described in [lisa.md](lisa.md) §14 as "write-verify"; the released ROM and OS driver
never issue it, and the Widget driver treats command bytes ≥ `$02` as system/non-I/O requests [4] `S50`.
The write-verify identification is therefore *unverified* (§6).

### 3.3 The device-info block: querying type and size

A single-block **read of block `$FFFFFF`** does not touch the platters: the controller synthesizes a
block describing itself [4] `PROF_INIT` (comment: "Get Device Characteristics"), re-read at every OS boot
by the loader ("read 'status' block from hard disk" [4] `LDRLDR`). After the usual 4 status bytes, the
block stream contains, at block-stream offsets (*observed* from the two independent readers):

| Offset | Size | Field |
|---|---|---|
| 14 | 1 | drive type: `0` = ProFile-class, nonzero = Widget [4] `PROF_INIT` `GTTYP2`, `LDRLDR` (`disk_type` at header byte 14) |
| 18–20 | 3 | device block count, 24-bit big-endian [4] `PROF_INIT` |

Nothing else in the stream is read by any released driver — the remaining field layout is unknown (§6).
The OS recognizes a 5 MB ProFile by `type = 0` and size ≤ 9,728 blocks, the 10 MB ProFile by `type = 0`
and a larger size, the Widget by `type ≠ 0` (§1.2). The Boot ROM does not use the device-info block at all:
it detects a drive by `OCD` plus one probe handshake [3] `CHKPROFILE`.

### 3.4 Controller state bytes and host replies

The state byte the controller presents at each handshake, collected from every expected-response constant
in the released drivers (*observed*; expected-response values `EXPECT_HS` in [4], `FINDD2` loads in [3]):

| State | Meaning (phase it announces) |
|---|---|
| `$01` | ready — on-line, waiting for a command |
| `$02` | read command accepted — data will follow |
| `$03` | write command accepted — send the block |
| `$06` | write data received — status will follow |
| `$0F` | Widget: spare-table read accepted — table will follow |
| `$10` | Widget: spare-table write accepted |
| `$22` | Widget multi-block: block ready to be read |
| `$23` | Widget multi-block: ready for the block's write data |
| `$27` | Widget multi-block: command complete |
| `$A3` | Widget multi-block write: error response |
| (any) | the OS also handshakes against a wildcard after the illegal-command sparing handshake [4] `S30` |

The OS driver tracks the *expected* state at each step and diverts to a bad-response path on mismatch [4]
`RESPOND` — the released sources' phrasing for the check is "did pippin return state requested?".

### 3.5 Status bytes

Every command ends with **four status bytes**, read back-to-back through the handshaked register [3]
`GETSTAT`, [4] `RD_STATUS`. Their known semantics:

| Fact | Evidence |
|---|---|
| Status byte 0 = `$09` is a **CRC error on read** — non-fatal: the OS continues the transfer and reports it through the checksum-error path; the ROM treats it as retryable | [4] `S6` ("JUST CONTINUE IF GOT CRC ERROR ON READ"), `S7`; [3] `STRTRD` retry chain |
| The fatal-error mask is **`$C140C000`** (big-endian over the four bytes): any of those bits set is a hard error | [3] `STATMSK`, [4] `S6` — identical constants in both |
| Status **byte 1 bit 2 = "sparing occurred"** on the just-completed access | [4] `S7`, `S13`, `S53` (`BTST #2,ERRSTAT+1`) |
| Non-fatal residue in the rest of the status longword is ignored (masked as "don't care") | [3] `STRTRD` `@2` |

The exact per-bit decoding of the four bytes beyond these three facts is not documented in the evidence
set (§6). The OS driver accumulates the four bytes of the Widget bad-response path into a separate
`accstat` accumulator for diagnostics [4] `S42`, and the most recent fatal status is kept per drive and
returned by the driver's `dcontrol 15` call [4] `PROFDRVR`.

### 3.6 Block and page-label format

Each block is **532 bytes on the interface: a 20-byte tag ("page label") header plus 512 data bytes**
(*observed*: Boot ROM equates `HDRSIZE = 20`, `BLKSIZE = 512` [3] `RM248.E`; loader equate
`disk_header = 20` "20 byte soft header with profile/widget" [4] `LDEQU`). The OS expands the wire label
into a 24-byte in-memory record — the "pagelabel" / distributed-directory record the file system chains
[4] `DRIVERDEFS`:

| Wire byte (ProFile order) | Wire byte (Widget order — see note below) | Field (24-byte pagelabel record) |
|---|---|---|
| 0–1 | 0–1 | `version` (16-bit; the Widget packs a 12-bit version plus a 4-bit flags nibble into bytes 0–1) |
| 2 | (in flags nibble) | `datastat` (`dataok`, `datamaybe`, `databad`) |
| 3 | (in flags nibble) | `filler` (signed) |
| 4 | 18 | `volume` |
| 5–6 | 2–3 | `fileid` (16-bit) — the boot block's file ID is `$AAAA` [3] `BOOTPAT` |
| 7 | 4–5 | `dataused` high byte; **bit 7 is the checksum-present flag** |
| 8–10 | 6–8 | `abspage`, low 24 bits |
| 11 | 19 | **checksum byte** (§3.7) — replaces `relpage` bits 16–23, which are never transmitted |
| 12–13 | 9–11 | `relpage`, low 16 bits |
| 14–16 | 12–14 | `fwdlink`, low 24 bits |
| 17–19 | 15–17 | `bkwdlink`, low 24 bits |

Wire order per drive type (*observed*): the ProFile streams and expects **label first, then data**, in both
directions [3] `RDDATA`, [4] `S7`/`S10`. The Widget's multi-block commands stream **data first, then the
label** [4] `S51`/`S10A` — the driver's comment for the Widget is "headers are at end of sector" [4]
`PROFDRVR` (`rvrs_hdr := 20`), against `0` = "headers at beginning of sector" for ProFile-class drives
[4] `PRIAM`, `SONY`. During Boot-ROM-driven reads (boot and loader, the only single-block Widget traffic
that exists) the loader still assumes the 20 label bytes arrive first, and simply discards them
("trash useless 'real' header"; "widget has less data in block 0" [4] `LDRLDR`) — *inferred — unverified*
that the Widget's single-block read is label-first like the ProFile's; see §6.

### 3.7 Checksum (page label) versus CRC (media)

Two independent integrity mechanisms; do not conflate them:

- **Page-label checksum** — host-computed, one byte, carried inside the label. Algorithm, from the
  released drivers (*observed*): XOR-fold the entire 536-byte unit — the 24-byte in-memory label expanded
  with its untransmitted filler bytes, plus the 512 data bytes — down to one byte (fold 32→16→8), and store
  that byte in the wire slot that `relpage` bits 16–23 would otherwise occupy; on write, the drivers cancel
  exactly the record bytes that do not cross the wire before writing the folded byte [4] `C_SUM`, `WRHDR`,
  `WR_WHDR`. A read verifies by folding all 532 wire bytes and requiring zero [4] `LDRLDR` (`xor_loop`,
  `cksum_on = $8000` on the `dataused` high byte). A nonzero fold is reported as checksum error `-663` [4]
  `PROFASM`.
- **Media-level CRC** — computed by the drive controller over the sector; surfaces as status byte 0 `$09`
  (§3.5) and is retried by the command's retry count [3] `STRTRD`, [4] `S7`.

### 3.8 Interleave

The 5 MB ProFile presents its blocks in a **5:1 physical interleave**, and the host adds a software
remap on top of it — the OS source's own description of its remap tables is "9:1 INTERLEAVE ON TOP OF 5:1
FOR PROFILE" [4] `PROFASM` `INT1TAB` (*observed* comment; the underlying 5:1 hardware interleave is
attested only by that comment). The remap rewrites the **low nibble of the block number** (command byte 3)
through the table:

| Low nibble in | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Low nibble out | 0 | 5 | 10 | 15 | 4 | 9 | 14 | 3 | 8 | 13 | 2 | 7 | 12 | 1 | 6 | 11 |

Three independent copies of this table exist in the released source and are identical: the OS driver's
Profile and Seagate tables [4] `INT1TAB`/`INT2TAB` and the boot loader's `intertabl` [4] `LDRLDR`. The
remap is applied on **every** ProFile/Seagate block access except the device-info block, and is **not**
applied to the Widget [4] `PROFDRVR` (`remap_interleave := false` for Widget), [4] `LDRLDR`
("dont use interleave on widget").

### 3.9 Sparing and the spare table

The command frame carries a **sparing threshold** (byte 5); the drivers use 3, which the Boot ROM equates
with "30% sparing" [3] `TCNT`. The driver exposes the semantics through its control call: setting the
threshold to 3 means "rewrite if >30% err rate", to 11 means "don't rewrite soft errs" — i.e. sparing
disabled [4] `PROFDRVR` `dcontrol 21`. A block whose access tripped sparing reports it in status byte 1
bit 2 (§3.5) and triggers the extra illegal-command handshake (§3.2 step 5).

The **Widget** additionally keeps a 512-byte **spare table** inside the drive, which the OS reads and
writes through dedicated system commands (§3.10's command list): read spare table `$12`, write spare table
`$16` with a 4-byte "fence" `$F0783C1E` [4] `S60`–`S63` (*observed*). Two locations in it are pinned by the
driver's format check [4] `S61`: byte offset 9 holds the **format interleave** (must be 1) and offset 454
holds the start of the **soft interleave map**, which must begin `$000C0511` — "FIRST PART OF INTERLEAVE
TABLE FOR 5:1" [4] `W_IMAP` — for the on-disk format to be accepted; anything else fails with driver error
618, "WIDGET DISK IS IN WRONG FORMAT" [4] `FMTERR`. The OS's format operation is exactly this
read-check-write of the spare table ("widget format doesn't do much" [4] `PROFDRVR` `dskformat`; a
non-Widget format returns "disk needs to have zero's written to it", error −684) — surface formatting is
not performed by any released driver.

### 3.10 Widget system commands, and the drive's diagnostic interface

The Widget's "system" traffic frames commands with a length nibble and a trailing **check byte** — the
one's complement of the 8-bit sum of the frame bytes [4] `SEND_CMD` (*observed*; e.g. the read-status
frame `$13 $01 $05 $E6`: `$13+$01+$05 = $19`, `~$19 = $E6`):

| Frame | Meaning |
|---|---|
| `$26`, block count, block[24-bit] (+ check) | multi-block I/O: up to **127 blocks** per command [4] `S50` |
| `$13 $01 $05 $E6` | diagnostic: read the controller's state registers (4 bytes) [4] `S41` |
| `$12 $0D …` (length 13) | read spare table — 512 bytes, next handshake state `$0F` [4] `S60` |
| `$16 $0E …` (length 14, fence `$F0783C1E`) | write spare table — next handshake state `$10` [4] `S62` |

Multi-block data phase (*observed* [4] `RD_NEXT`/`WRT_NEXT`): after the command handshake the controller
presents `$22` (read) or `$23` (write) per block; each block transfers 4 status bytes, then 512 data
bytes then the 20-byte label (§3.6); on the last block the controller completes with `$27` and the host
answers the final handshake with the **free-device reply `$69`** [4] `S2A`. A write that goes wrong
mid-stream answers `$A3` and diverts to a status read [4] `S53`.

Bad-response recovery on the Widget [4] `S40`–`S42` (*observed*): the driver sends the read-status
diagnostic, accumulates the controller's four state-register bytes, then **resets the controller**
(`CRES/` low ~100 µs, release, wait not-busy up to ~16 s), counts the reset, and re-issues the whole
request — up to 16 resets before giving up with the bad-response error.

### 3.11 Reset, self-test and timing

- **Controller reset** is the `CRES/` line (VIA1 PB7, connector pin 21): driven low ~100 µs and released
  [3] `DOCRES`, [4] `S42`. The ROM uses it to unstick a handshake before retrying a read [3] `TRYRD`.
- The drive runs a **power-on self-test of about 80 seconds**; the ROM's ready-wait "is presently set for
  about 100 seconds to allow enough time for normal Profile startup time of about 80 seconds" [3] `CHKBSY`
  comment. Equates: startup timeout **3 minutes** after power-up, **100 s** after reset, per-command read
  timeout **~16 s**, response timeout `$FFFF` loop iterations [3] `RM248.E` (`STRTIME`, `RSTRTIME`,
  `RDTIME`, `RSPTIME`).
- The OS driver's own timeout budget is 100 timer ticks — "5 secs before timeout on 2-port … 12 secs on
  parallel" [4] `PROFDRVR` — and its per-handshake poll is `$50` iterations, about 1 ms [4] `S1`, before
  deferring to an interrupt.
- Cable disconnect (`OCD` high) at any interrupt aborts with the disconnect error [4] `DRIVER` entry
  (`DISCERR`).

---

## 4. Programming model

### 4.1 Boot ROM: device probe

`CHKPROFILE` [3] is the ROM's presence test, run while building the boot menu and before a hard-disk boot:

1. `PROINIT` — program VIA1 ORB bits 5/7 as outputs, set VIA2 PCR/DDR/ORB as in §2.4, then read `OCD`;
   set = no drive [3] `PROINIT`.
2. Wait for not-busy (with the 10 ms/100 s/3 min budget ladder) [3] `WFNBSY`/`WFNBSY2`.
3. Assert `CMD/`, wait busy, read the state byte and check for the expected **`$01`** [3] `CHKPROFILE`.
4. Send a **`$00` reply** — the comment: "send '0' response to reset Profile" — and wait for command
   taken [3] `SENDRSP`, `WFNBSY`.

Machine type matters before any of this: on a Lisa 2 the ROM checks that a disk (internal or external) is
connected before defaulting to the hard-disk boot, "if no disk is detected, the system defaults to booting
from the floppy drive", while Lisa 1 defaults to the parallel-port ProFile outright [3] module header.
The keyboard sequence **Cmd-3** selects "boot from Profile attached to parallel port or integral hard
disk" [3] module header. The selected boot id is saved by the ROM at low-memory `$1B3` [3] module header
(the hardware manual notes the boot id is relayed to the loaded boot program in register D0 [1] p. 3-19);
the OS loader later reads the saved byte (`prom_bootdev = $1B3` [4] `LDEQU`).

### 4.2 Boot ROM: the bootstrap read

`PROBOOT`/`PROREAD` [3] (*observed*):

1. Command buffer at `$304`: **read block 0**, retry count 10, sparing threshold 3 [3] `CMDBUFR`,
   `PROBOOT`.
2. Handshake against `$01`, send the command, handshake against `$02` (`STAT01`→`FINDD2`), read 4 status
   bytes; on failure the retry ladder is re-handshake → `CRES/` reset → one more try [3] `TRYRD`.
3. Read 20 label bytes then 512 data bytes [3] `RDDATA`/`READIT` into buffers at `$1FFEC`/`$20000`
   (equates [3] `RM248.E`).
4. Validate the label: the **`fileid` at label offset 4 must be `$AAAA`**, the boot-block pattern
   [3] `BOOTPAT`, `FILEID`; else "bad header".
5. Jump to the loaded image at `$20000` [3] `PBOOT`, `STRTBOOT`. The ROM also publishes the read primitive
   for later use at jump-table entry **`$FE0090`** ("read profile utility" [4] `LDEQU` `prof_entry`).
6. Errors map to ROM codes: timeout `$10`, no disk `$11`, disk-not-ready `$12` [3] `RM248.E`; failure
   sounds three high tones and drops into the monitor [3] `BOOTFAIL`.

The ROM runs this with interrupts disabled, polling the BSY level only [3] `PROREAD`.

### 4.3 OS driver initialization

`PROF_INIT` [4], after the Pascal driver has placed its control blocks (*observed*):

1. Compute the VIA addresses: built-in port → base `$0D801`, parity/reset handle `$0DC01` (§2.3);
   expansion card → channel base, reset lines on the channel's own VIA (§2.6) [4] `PROFDRVR` `hdinit`.
2. Program VIA2 (PCR `$6B`, ACR, IER, DDRB, `DEN` low) exactly as §2.4; abort on `OCD` [4] `PROF_INIT`.
3. Do one handshake expecting `$01` — "DO 1 HANDSHAKE TO MAKE SURE IT IS A PROFILE!" [4] `DOSHAKE`.
4. Issue the device-info read — block `$FFFFFF`, retry 10, threshold 0 — and pull type and size from the
   returned stream (§3.3) [4] `PROF_INIT`.
5. Classify the drive per §1.2; seed retry count 10, sparing threshold 3, interleave remap on
   (Profile/Seagate) or off, and `rvrs_hdr` 0/20 [4] `PROFDRVR` `hdinit`.
6. Defaults before classification: the driver initially **assumes 9,720 blocks** [4] `PROFDRVR`
   ("assume the device is a profile … `num_bloks := 9720`"). The driver's classification bound treats
   ≤ 9,728 as a 5 MB ProFile, which is the canonical 5 MB size the boot ROM and OS were written for; the
   9,720 vs 9,728 discrepancy is unresolved (§6).

The driver stack on top: the generic hard-disk driver `HDISK` handles the page-label chaining
(`with_header`, `chained_hdrs` I/O modes: it verifies each label's `version`/`fileid`/`relpage` against
the expected soft header and walks `fwdlink`/`bkwdlink`) [4] `HDISK`; `PROFDRVR` is the device-dependent
layer; `PROFASM` is the interrupt-driven state machine.

### 4.4 OS driver: single-block state machine

`PROFASM` [4] is a table-driven state machine resumed at every VIA2 interrupt (`asm_state` indexes
`STATE_TABLE`). The single-block read path is `NEW_CMD → S1 (assert CMD, wait busy) → S2 (handshake $01,
reply $55) → S3 (send 6 command bytes; parity gate) → S1A/S200 (handshake $02) → S6 (read 4 status bytes,
apply the fatal mask) → S7 (read label + data, verify checksum) →` next command, or the sparing
handshake, or done. The write path adds `S8` (handshake + precompute the checksum), `S10` (write label +
data), a `$06` handshake, a status read, and — if the request asked for verification (`v_flag`) — a whole
read-back pass that recomputes the checksum on the fly (`S13`, `S20`) [4]. Writes are verified by reading
back; there is no controller-side verify command in the released drivers.

Error exits: parity error (IFR bit 3 set after a gated transfer), checksum/CRC (−663), hard error from
the status mask (654), disconnect (660), timeout, bad response [4] `PROFASM`. `HDISK` retries a failed
request by restarting the state machine and counts restarts (`dcontrol 15` returns the last controller
error and the restart count) [4] `PROFDRVR`.

### 4.5 OS driver: Widget multi-block state machine

For `drivetype = Widget` the machine takes the `MULTI_CMD` tables instead: frames via `SEND_CMD` (§3.10),
per-block `$22`/`$23` handshakes, per-block 4 status bytes, data-then-label order, `$27` completion with
the `$69` free-device reply, and the bad-response → read-status → `CRES/` → re-issue ladder with its
16-reset limit (§3.10) [4] `S50`–`S53`, `S40`–`S42`.

### 4.6 The OS boot loader

`LDRLDR` ([4] `LDPROF`, the loader-loader deposited in the boot block by the OS) (*observed*):

1. Read the device-info block (`$FFFFFF`) to learn profile-vs-widget (`dev_prof = 1`, `dev_widget = 3`)
   [4] `LDEQU`.
2. Relocate itself to the middle of RAM, then read the loader proper block by block starting at **block 8**
   of the device ("skip mount table, at block 7"), via the ROM's read primitive `prof_entry` (`$FE0090`)
   for the built-in port or the slot card's driver [4] `LDRLDR` `main_loop`, `read_it`.
3. Apply the ProFile interleave remap (§3.8) to every non-device-query block; never on the Widget.
4. Verify the page-label checksum of every block when the flag bit is set (§3.7); a bad fold aborts the
   boot into the ROM monitor with error 10726, "cant read boot device" [4] `LDRLDR` `bootbomb`, `LDEQU`.
5. Pass the MDDF block address and device type up to the loader [4] `LDEQU` (`ld_fs_block0`, `dev_type`).

### 4.7 A second drive on an expansion card

A Lisa 2/5 booting XENIX with the Text Processing or Development systems "must" carry a second ProFile,
"connected to the upper parallel port of a parallel expansion board installed in expansion slot 1" [5]
p. 1-2 — matching the card's upper VIA at slot-low + `$2800` (§2.6). The install procedure checks the
second disk the same way as the first: wait for its "ready" light, then let `firsttime` build the `/usr`
file system on it [5] §1.7.4.

### 4.8 Observed guest behaviour

From the XENIX installation guide (guest-visible behaviour of the subsystem, cited as the observation;
the protocol facts behind them are [3]/[4] above):

- The drive must be powered before the Lisa and its self-test awaited: "Wait for the 'ready' light on
  the front of the hard disk drive(s) to glow a steady red" [5] §1.4 — the user-visible face of the
  ~80-second self-test (§3.11).
- Installation asks "Enter size of hard disk (5 or 10)" — 5 for a Lisa 2/5 with a 5 MB ProFile on the
  parallel interface port, 10 for a Lisa 2/10's internal disk [5] §1.5.2.
- The kernel names the drives **`pf0`, `pf2`**; a boot error names the missing drive and instructs
  "check the hard disk connections and make sure power is on, then press the RESET button" [5] §1.5.2,
  §1.7.4 — the OCD/timeout paths of §3.11 as the guest reports them.
- Boot from hard disk happens with no key held: self-test icons, then auto-load of the XENIX `boot`
  program from the disk [5] §1.5.3 — the §4.2 sequence.

---

## 5. Quirks & errata

1. **The state byte is exchanged through the no-handshake register; the reply too.** Any use of the
   handshaked register for the state byte would pulse `PSTRB/` at the wrong moment. Both drivers are
   scrupulous about register 15 vs register 1 [3] `GETRSP`/`SENDRSP`, [4] `RESPOND`.
2. **The reply doubles as a command.** `$00` does not just decline a handshake — it resets the
   controller's transaction state ("send '0' response to reset Profile" [3]), and `$69` tells the Widget
   it is free. A host that replies carelessly can desynchronize the drive.
3. **The spare-table "extra handshake" is an illegal command byte.** The OS acknowledges a sparing event
   by sending command byte `$FF` and accepting any response [4] `S30` — a deliberately undefined command
   used as a poke.
4. **Port A feeds the video contrast latch** [1] §6.7.3: parallel-port data writes have a side effect on
   the display contrast DAC. The ROM exploits it (`CONOFF` after the VIA2 test [3]); a driver or debugger
   poking port A changes the screen.
5. **`BSY` is active-low but named `BSY/` and described inconsistently.** The manual's prose says the
   peripheral "asserts" busy [1] p. 3-22 while its own drivers poll PB1 = 1 as *not* busy (§3.1). [lisa.md](lisa.md)
   §14.1 calls `BSY` an input without a polarity; this page pins it from both drivers' poll directions.
6. **VIA1/VIA2 register strides differ** (2 vs 8) and VIA2's chip select ignores bit 8 of the address —
   the `$D8xx`/`$D9xx` alias pair (§2.3). The OS Profile driver is the only released consumer of the
   `$D801` alias [4]; the ROM and MacWorks XL use `$D901` [3], [lisa.md](lisa.md) §10.2.
7. **The parity-error flag is read from IFR bit 3 (CB1) while the manual says CB2 monitors parity**
   (§2.2). Every released driver clears IFR bit 3 before a parity-gated transfer and tests it after; the
   manual's §6.5.2 assigns parity monitoring to CB2. Which physical pin carries the LS109's latch output
   is not resolvable from the text sources (§6).
8. **The parity latch is cleared from two places**: the `PRES/` line on VIA1 PB5 (pulsed by the drivers
   before every gated transfer [4]) and — per a driver comment — an ORB write itself: "SET DIR=IN
   (ALSO CLEARS PARITY ERR FLAG)" [4] `FINI_WRITE`.
9. **The 10 MB external ProFile is not a Widget.** The OS types it `T_Seagate` and uses the ProFile
   single-block protocol and interleave remap [4] `PROFDRVR` — same protocol, bigger disk.
10. **A Widget displaces the external port at boot time.** With machine type 3 the ROM never offers or
    probes an external ProFile [3] `ICONCHK` — one parallel bus, one drive, at ROM level.
11. **[lisa.md](lisa.md) §14.2 describes `$55` as "the controller's ready reply"; that is backwards.**
    The controller presents the state byte (`$01` when ready); `$55` is the *host's* affirmative reply
    (§3.1). This page supersedes that sentence.
12. **[lisa.md](lisa.md) §14.3 gives the block size as "532–536 bytes".** The wire format is 532
    (20 + 512, [3] equates); the 24 is the OS's in-memory page-label record size, which never crosses the
    interface in that form. This page pins the wire size.

---

## 6. Open questions

1. **The ProFile controller's internals.** The drive-side processor type, its firmware, the medium-level
   format (sectors per track, CRC polynomial), and the mechanism (the 5 MB ProFile's drive) are not
   documented by any source here; [1]'s preface points at a *ProFile Owner's Manual* that covers the drive.
   Everything in §3 is protocol-level, established from the host-side drivers.
2. **The "Pippin" name.** Both released sources call the controller "pippin" in comments; nothing
   establishes whether that names the controller card, its firmware, or the whole drive.
3. **DD0–DD7 pin numbers** on the DB-25 — the manual's Figure 3-8 is a graphic; only the control-line
   pins are in its text (§2.1).
4. **CB1 vs CB2 for the parity flag** (§5 item 7) — needs schematic sheet 3 of 050-4008, which is not in
   the text evidence. Likewise the exact chip-select decode that makes `$DC01` a keyboard-VIA alias (§2.3).
5. **`DIAGPAR` (VIA2 PB5)** is named in [1] Figure 2-11 with no connector pin and no further description
   anywhere; no released driver touches it. [lisa.md](lisa.md) §14.1 instead labels VIA1 PB5 `DIAGPAR`;
   per [1] §6.6.5 the keyboard-VIA bit is `PRES/`, the parity reset — this page follows §6.6.5.
6. **Command byte `$02` (write-verify)** — never issued by the released ROM or OS; the Widget treats
   ≥ `$02` as system commands (§3.2). Needs a driver that uses it (MacWorks XL is a candidate) or
   controller documentation.
7. **The 5 MB ProFile's exact block count: 9,720 vs 9,728.** The OS driver seeds `num_bloks := 9720`
   before querying; its own classification bound and the canonical geometry are 9,728 (§4.3). Whether a
   real 5 MB unit reports 9,728 and the OS simply under-uses it by 8 blocks is not established here.
8. **The Widget's reported size.** The OS classification only brackets it (9,729–30,000 blocks,
   `num_bloks := discsize − strt_blok`); no source states the Widget's actual device-info values or its
   `strt_blok`.
9. **The Widget's single-block wire order** during ROM-path reads — inferred label-first from the loader's
   buffer layout and its "trash useless 'real' header" comment (§3.6); the Widget's own multi-block order
   is data-first. No source states whether a single-block Widget command even exists outside the ROM's
   read.
10. **The status bytes' full bit map.** Only byte 0 = `$09` (CRC), the `$C140C000` fatal mask and byte 1
    bit 2 (sparing) are pinned (§3.5); the remaining bits' meanings live in the controller documentation.
11. **The device-info block's remaining fields** beyond offsets 14 and 18–20 (§3.3) — drive name,
    firmware revision and the like are not read by any released driver and their layout is unknown.
12. **The VIA2 timer tick's exact period** — the OS reloads T2 with `$FF` and comments "poll again in
    .1 sec" [4] `DRIVER` `@30`; the tick period implied by the 100-count = 12 s budget (§3.11) is
    consistent but not independently pinned.
13. **Widget low-level formatting.** The OS "format" only reads/validates/rewrites the spare table
    (§3.9); what utility performs the surface format, and through which commands, is outside this
    evidence set.

---

## References

1. Apple Computer, *Apple Lisa Computer: Hardware Manual*, April 1983 — §2.5.3 "Parallel Port Control"
   (pp. 2-16 – 2-18, Figures 2-11 "Parallel Port Bit Correspondance", 2-12 "Parallel Port Addressing"),
   §3.5 "The External Ports", §3.5.2 "Parallel Interface Port" (pp. 3-20 – 3-22, Figure 3-8), §6.5
   "Parallel-Port Controller" (pp. 6-46 – 6-49, incl. §6.5.1 68000 bus interface, §6.5.2 parallel port
   interface), §6.6.1, §6.6.5 "Other Control Lines" (p. 6-52a), §6.7.3 "Video Contrast Latch" (p. 6-55),
   preface (companion manuals).
2. Apple Computer (M. Baumwell), *Apple Lisa Computer: Hardware Manual 1983 — Errata / "Macintosh XL
   Hardware Information"*, 16 May 1985 — Parallel Interface Card ROM version address and VIA offsets
   (slot low decode + `$2000` / `$2800`, ROM at +`$FF8`); internal VIA locations.
3. Apple Computer, *Apple Lisa Boot ROM source code, revision 2.48 ("H", RM248)* — officially released
   Lisa source archive; modules `RM248.B` (device probe `CHKPROFILE`, bootstrap `PROBOOT`/`PROREAD`,
   `PROINIT`, `STRTRD`, `STAT01`, `FINDD2`, `WFBSY`/`WFNBSY`, `SENDRSP`, `DOCRES`, boot-menu `ICONCHK`,
   `HDSKERR`), `RM248.E` (Profile equates: `CMDBUFR`, `STATMSK`, `HDRSIZE`, `BLKSIZE`, `BOOTPAT`,
   `RCNT`, `TCNT`, `STRTIME`, `RSTRTIME`, `RDTIME`, `RSPTIME`), `RM248.K` (`VIA2TST`).
4. Apple Computer, *Apple Lisa Office System source code* — officially released Lisa source archive;
   OS units `PROFASM` (SOURCE-PROFILEASM.TEXT, D. Offen / W. Henry 1983–84 — state machine `S0`–`S64`,
   `RESPOND`, `SEND_CMD`, `RD_STATUS`, `C_SUM`, `RDHDR`/`WRHDR`, `RD_WHDR`/`WR_WHDR`, `INT1TAB`/`INT2TAB`,
   driver interrupt entry), `PROFDRVR` (source-PROFILE.TEXT, W. Henry — `hdinit`, `NONIO_REQ`, `dcontrol`,
   `dskformat`), `HDISK` (SOURCE-HDISK.TEXT — page-label chaining), `LDRLDR` (source-LDPROF.TEXT with
   source-LDEQU.TEXT — boot loader, `prof_entry`, `disk_type`, `intertabl`, `bootbomb`),
   `FOUR_PORT` (SOURCE-2PORTCARD.TEXT — expansion-card channel dispatch), `DRIVERDEFS`
   (source-DRIVERDEFS.TEXT — `pagelabel` record, `iospacemmu`), `PRIAM`/`SONY` (for the `rvrs_hdr`
   convention).
5. The Santa Cruz Operation, *SCO XENIX System V for the Lisa 2: Installation Guide*, 1984 — Lisa 2/5
   and 2/10 configurations, second ProFile on the slot-1 card's upper parallel port, `pf0`/`pf2` drive
   names, "ready" light, 5/10 MB size prompt, boot-from-hard-disk sequence.
