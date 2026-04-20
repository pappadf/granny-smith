# SCSI in the Macintosh Family

This document describes the SCSI subsystem at the Macintosh-system level:
how the bus is wired, how pseudo-DMA is implemented on each machine,
and how classic Mac OS and A/UX drive the NCR 5380 so differently that
a single emulator must model both flows correctly.  Chip-internal
register-level details live in [ncr_5380.md](ncr_5380.md); SCSI
protocol / command-set details are at the end of this file.

Status of our emulator:

* **Macintosh Plus** — polled SCSI, `/DTACK`-stalled pseudo-DMA.
* **Macintosh SE/30** — fully interrupt-driven SCSI via VIA2.
  Both Mac OS ROM driver and A/UX 3.0.1 kernel boot from SCSI in the
  emulator.
* **Macintosh IIcx** — same wiring as SE/30 (VIA2 CB2/CA2), identical
  SCSI driver on both machines.

---

## 1. The SCSI-1 bus

The Macintosh uses a single-ended, asynchronous SCSI-1 bus:

* 8 data lines DB[0..7] + 1 parity line DBP, open-collector / wired-OR.
* 9 control lines: BSY, SEL, C/D, I/O, MSG, REQ, ACK, ATN, RST.
* Apple's external connector is a DB-25; the internal ribbon on
  machines with internal drives is the standard 50-pin IDC header.
* No terminator power is provided by the Plus; later machines supply
  it through a terminator-power diode.
* Bus speed is asynchronous — in practice 1.5–3 MB/s depending on
  cable length and target firmware.

### 1.1 Bus phases

In strict order for a normal transaction:

`BUS FREE → ARBITRATION → SELECTION → {COMMAND → [DATA IN | DATA OUT] →
STATUS → MESSAGE IN} → BUS FREE`

plus two exceptional phases:

* **MESSAGE OUT** — initiator asserts ATN to send a message
  (IDENTIFY, ABORT, BUS DEVICE RESET, queue tags).
* **RESELECTION** — a disconnected target reconnects the initiator.

Each information-transfer phase is a stream of single-byte REQ/ACK
handshakes.  The target drives MSG/C/D/I/O to encode which phase is
active; the initiator's TCR encodes which phase it *expects*.  The
5380 computes **phase match** (BSR.PM) combinationally from these two.

### 1.2 SCSI IDs

IDs 0–7; 7 is highest priority and is reserved for the host on every
Apple Macintosh.  Peripherals default to ID 0 (internal HD) and 3
(CD-ROM); the external DB-25 chain covers 1, 2, 4, 5, 6.

### 1.3 Arbitration vs. non-arbitrated selection

The real bus spec requires every initiator to arbitrate.  Because
Apple machines are single-initiator, every Mac SCSI driver we've seen
skips the arbitrate step and performs **non-arbitrated selection**:
write the target ID to ODR, assert `ICR.SEL`, `ICR.DATA`, then drop
BSY.  The 5380 is perfectly willing to do this — see
[ncr_5380.md §5.2](ncr_5380.md).

---

## 2. Host integration — shared mechanism

All 5380-based Macs use a common trick for data transfer: the chip's
**DMA MODE** (`MR.DMA=1`) offloads the REQ/ACK handshake, but the CPU
itself is still responsible for moving each byte between memory and
the chip.  This is commonly called **pseudo-DMA**.

Two different strategies appear across the family:

* **Plus**: a single memory-mapped alias for CDR; bus cycles stall via
  `/DTACK` until the chip is ready.  No IRQ wiring.
* **SE/30 / IIcx**: two aliases (DRQ-synchronised and blind) plus
  SCSI IRQ and DRQ routed into VIA2 as edge-triggered interrupt
  sources.

The chip doesn't know or care which it is — it just sees register
accesses arrive and raises DRQ.

### 2.1 Pseudo-DMA byte moves

During a pseudo-DMA read (target → initiator) the host:

1. Issues a read at the pseudo-DMA alias address.
2. Bus cycle hits the glue logic.  Glue routes the read to the 5380's
   CDR register *and* drives an internal `DACK` so the chip treats it
   as a DMA cycle — the chip returns the latched byte and pulses ACK
   to the target.
3. If the chip isn't ready (`BSR.DR=0`), glue holds `/DTACK` high
   (Plus) or simply returns whatever CDR happens to contain (SE/30
   blind path).  In the DRQ path on SE/30, `/DTACK` assertion is also
   gated on DRQ.

Writes work symmetrically: the host writes to an alias; glue routes
the byte to ODR and asserts DACK.

### 2.2 Where the CPU waits

The CPU has to know *when* the next byte is ready:

* **Plus**: doesn't know.  It relies on `/DTACK` to freeze the CPU
  mid-instruction until DRQ asserts.
* **SE/30 DRQ path (`$06000`)**: same as Plus — `/DTACK` stalling.
* **SE/30 blind path (`$12000`)**: glue always completes the cycle.
  Either the target is known to be ready (because the host has already
  polled BSR) or the byte is irrelevant because the chip is in DRQ-less
  status phase.  This path is what A/UX's `SPH_STAT` uses to fetch the
  status byte via pseudo-DMA.
* **SE/30 register path (`$10000`)**: direct register read, no DACK.
  Used for control registers (ICR/MR/TCR/CSR/BSR).

---

## 3. Host integration — Macintosh Plus

### 3.1 Memory map

| Address range         | Function                                     |
| --------------------- | -------------------------------------------- |
| `$580000–$5FFFFF`     | NCR 5380 registers, mirrored across 512 KB   |
| Mirror reads          | Must be on **even** CPU addresses (UDS)      |
| Mirror writes         | Must be on **odd** CPU addresses (LDS)       |
| `$5800drn`            | Address formation: R/W, DMA, register-number |

Our emulator registers the SCSI memory interface at `0x00500000`
spanning 1 MB (see [scsi.c `scsi_init`](../src/core/peripherals/scsi.c)),
covering both the `$500000` reserved block and the `$580000` SCSI
window.

Bit semantics of the address LSBs:

* A4–A6 select which of the eight chip registers is accessed.
* A9 (the "DMA" alias bit) distinguishes a regular register access
  from a DACK-ing pseudo-DMA access.  `$580200` is ODR with DACK
  asserted; `$580000` is ODR with DACK off.
* Word or longword accesses fault with a bus error on real hardware.

### 3.2 No IRQ, no DRQ

On the Plus, **neither IRQ nor DRQ is wired into the 68000's IPL
lines** or into the VIA.  The chip's IRQ latch is observable only by
explicit polling of `BSR.INT`.  Mac OS copes by running the entire
transfer synchronously — the CPU stays inside the SCSI Manager from
command issue until message-in.

### 3.3 `/DTACK` pseudo-DMA

The Macintosh Plus glue logic implements the classic trick: it
**with­holds `/DTACK`** on pseudo-DMA addresses until the chip raises
DRQ.  The 68000 enters wait states mid-instruction; when DRQ finally
asserts, `/DTACK` goes low, the MOVE completes, and the next
iteration of the host's blind-transfer loop fires immediately.

The loop therefore contains **no polling** — every MOVE is a
byte-transfer that stalls until the byte is ready.  The whole
transfer is clocked at chip speed, not at CPU speed.

Drawbacks:

* The CPU is unavailable during the transfer.  A slow disk can starve
  VBL interrupts for hundreds of microseconds.
* `/DTACK` timing is very tight; later machines added dedicated
  handshake hardware partly to relax this.

### 3.4 What the emulator implements

* Full register model at `$500000–$5FFFFF`.
* `phase_data_in` / `phase_data_out` that stuff the buffer with
  exactly the expected number of bytes, so blind reads always
  succeed.
* No `/DTACK` modelling — pseudo-DMA aliases return bytes
  synchronously.  For a single-initiator Plus this is indistinguishable
  from real hardware unless a timing-sensitive diagnostic is run.

---

## 4. Host integration — Macintosh SE/30 (and IIcx)

### 4.1 Memory map

Within the SE/30 I/O decode, the SCSI chip occupies four sub-ranges
(see [src/machines/se30.c](../src/machines/se30.c)):

| Offset from I/O base     | Function                                        |
| ------------------------ | ----------------------------------------------- |
| `$06000–$07FFF`          | Pseudo-DMA **with DRQ handshaking**             |
| `$08000–$0FFFF`          | (reserved — decoded but unused)                 |
| `$10000–$11FFF`          | Direct register access (register-select in A4–A6)|
| `$12000–$13FFF`          | Pseudo-DMA **blind** (no DRQ check)             |

All four map through the chip's `read_uint8` / `write_uint8`.  The
emulator does *not* need to distinguish blind-vs-DRQ: our chip model
asserts `BSR.DR` only when the bus phase matches TCR (see
[ncr_5380.md §3.3](ncr_5380.md)), which is the real signal behind the
glue logic's DRQ wait.

The absolute SCSI base on the SE/30 I/O page is `$50010000`.

### 4.2 IRQ and DRQ wiring to VIA2

The SE/30 routes both 5380 interrupt outputs into VIA2:

* **IRQ** → VIA2 **CB2** (active-low).  Fires on phase mismatch
  during DMA, end of DMA, bus reset, parity error.  VIA2 raises the
  SCSI bit of its IFR; the SE/30's IPL glue promotes that to IPL 2
  (plus IPL 1 from the VIA itself → level 2 effective).
* **DRQ** → VIA2 **CA2** (active-low).  Fires whenever the chip
  asserts DRQ.  The SCSI DRQ interrupt is masked in most drivers; its
  main use is debugging / polling.

See `scsi_update_irq()` and `scsi_update_drq()` in
[scsi.c](../src/core/peripherals/scsi.c) for the emulator plumbing.
Both signals are driven "active-low" through `via_input_c()`.

### 4.3 Three distinct access paths

Classic Mac OS and A/UX each prefer a different SE/30 pseudo-DMA
alias:

* **Mac OS (ROM SCSI Manager)** — uses `$06000` with DRQ handshaking
  for bulk data.  Status and message-in are read via register access
  at `$10000` with ICR/ACK handshaking (`MR.DMA=0`).
* **A/UX retail 3.0.1 kernel** — uses `$12000` blind for data phases
  *and* status phase.  The kernel programs TCR to the expected phase
  before setting `MR.DMA=1`; the chip's phase-match gate enforces
  correctness.

---

## 5. How classic Mac OS uses SCSI

The Apple ROM SCSI Manager was written for the Plus but extended to
drive later chips with roughly the same state machine.  Key properties:

* **Synchronous.**  The caller (File Manager, a device driver, a
  utility like Apple HD SC Setup) blocks until the command completes.
* **Arbitration is skipped.**  See §1.3.
* **Pseudo-DMA for data phases only.**  Command, status, and message-in
  are all driven by programmed I/O through ICR (`ACK` toggling).
* **Byte-exact transfers.**  The Manager knows the expected byte count
  from the CDB and writes exactly that many MOVE instructions in the
  pseudo-DMA loop.  No implicit "drain until phase changes" logic.

### 5.1 Typical INQUIRY transaction

1. **Selection.**  Write target ID to ODR; `ICR = DATA|BSY|SEL`; clear
   BSY; chip enters SELECTION, target drives BSY; chip enters COMMAND.
2. **Command bytes.**  For each of 6 bytes: `TCR=COMMAND(0x02)`; write
   ODR; toggle `ICR.ACK` (assert, then clear).  `run_cmd` fires after
   the 6th ACK-clear.
3. **Data in.**  `TCR=DATA_IN(0x01)`; set `MR.DMA=1`; read 36 bytes
   from `$06000` — each read transfers one byte via pseudo-DMA.  After
   the 36th byte the buffer is empty; phase stays in DATA_IN until the
   host clears `MR.DMA`.
4. **Status.**  Clear `MR.DMA`.  The emulator transitions the bus to
   STATUS automatically (see `write_mr` in
   [scsi.c](../src/core/peripherals/scsi.c)).  `TCR=STATUS(0x03)`;
   host toggles `ICR.ACK` to consume the status byte via CDR.
5. **Message in.**  Falling edge of ACK in STATUS phase transitions
   to MESSAGE_IN with `COMMAND COMPLETE`.  Host toggles ACK again.
6. **Bus free.**  Falling edge of ACK in MESSAGE_IN returns to
   BUS_FREE.

### 5.2 Surprise CHECK CONDITION

A command with no data phase (TEST UNIT READY, SEEK, PREVENT/ALLOW)
can return **CHECK CONDITION** (`0x02`) from the target.  On real
hardware the bus goes COMMAND → STATUS directly.

Mac OS's Manager, however, has already programmed `TCR=DATA_IN` in
anticipation of data and enabled DMA for some of these flows.  The
result on real hardware: `MR.DMA=1` with bus in STATUS but TCR in
DATA_IN ⇒ **phase mismatch IRQ** fires; DRQ never asserts.  The
Manager services the IRQ, reads BSR, sees PM=0, and recovers by
re-programming TCR to STATUS and reading the byte.

The emulator **must** reproduce this: the phase-match gate on
`BSR.DR` and the CDR-read auto-transition is what makes the
`se30-format-hd` integration test work.

### 5.3 Apple HD SC Setup bus scan

On launch, the utility iterates SCSI IDs 0..6 issuing a minimal probe
(TEST UNIT READY / INQUIRY combination) on each.  The scan is
entirely synchronous and in the failing case hangs forever with a
watch cursor — exactly the failure mode we hit in early SCSI commits
before the phase-match gate was in place.

---

## 6. How A/UX uses SCSI

A/UX 3.0.1's retail kernel bundles a completely different SCSI
driver.  It is **interrupt-driven** and built around two abstractions:

* `scsitask` — the kernel task that dispatches SCSI requests.
* A state machine keyed on `stp->state` (`ST_WAIT`, `ST_CMD`,
  `ST_READ`, `ST_WRAP`, `ST_STAT`, `ST_MIN`, …) that runs one
  transition per scsitask iteration.

Each state has a handler; the names follow the `SPH_*` convention
(`SPH_SEL`, `SPH_CMD`, `SPH_STAT`, `SPH_MIN`, etc.).  The handlers
use **pseudo-DMA for every phase**, including status and message-in.

### 6.1 Why it matters for the emulator

A/UX enables `MR.DMA=1` in phases where Mac OS never does — in
particular, STATUS and MESSAGE IN.  Two specific flows:

**`SPH_STAT` (status-byte read):**

1. Bus is in STATUS phase (set by the target when it's done with the
   data phase, or — in our emulator — by the `write_mr(MR_DMA=0)`
   hook when the data buffer has drained).
2. Handler writes `TCR=0x03`.
3. Handler sets `MR.DMA=1` via `BSET #1, MODE`.
4. Handler polls `BSR & 0x48`:
   * `0x08` (PM=1, DR=0) — still waiting.
   * `0x48` (PM=1, DR=1) — status byte available.
5. Handler reads CDR through the blind pseudo-DMA port at `$12000`.
   The emulator's `read_uint8` CDR branch, when phase match holds,
   returns the byte *and* transitions the bus to MESSAGE IN.
6. Handler reads the reset-parity register (`$70(A2)`) to
   acknowledge the DMA cycle.

A watchdog counter (`unjam`) guards the poll loop and trips an
`SST_TIMEOUT` if BSR never settles.  This was the origin of most of
the A/UX-investigation notes: our emulator's earlier failure to
assert `BSR_DR` in the status phase caused the watchdog to fire and
the kernel to stamp `SST_TIMEOUT` into `req->ret`, which then
cascaded into "no root file system".

**`SPH_MIN` (message-in read):**

Essentially the same flow: `TCR=0x07`, `MR.DMA=1`, poll BSR, read
CDR via blind pseudo-DMA.  The host treats receiving
`COMMAND COMPLETE` (`0x00`) as the end of the transaction and lets
the bus go free by clearing BSY.

### 6.2 Autoconfig probe

At kernel init, A/UX's `autoconfig_disks()` (`$100080B0`) issues an
INQUIRY followed by a TEST UNIT READY on every SCSI ID.  The
per-probe callback `probe_complete()` writes the detected device
type into `disk_slots[id]` only when **both** `$26(req)` and
`$27(req)` are zero (clean success).  A timeout, phase mismatch, or
short read leaves `$27(req)` non-zero and the slot stays
unpopulated — which is why the chip-model quirks above matter for
A/UX boot.

Once `disk_slots[]` is populated, `vfs_mountroot()` (`$10032BC2`)
walks the fs-types table and calls each type's `probe_root()`
against the rootdev at `$11003544` (hardcoded `$1B06` = SCSI major,
ID 3, slice 6 — the CD-ROM).  A successful match returns via the
`RTS` at `$10032CFC`, which is the assertion target of the
`se30-aux-3` integration test.


### 6.3 Why A/UX never uses `/DTACK` stalling

A/UX runs preemptively scheduled user tasks on top of a real kernel;
freezing the CPU mid-instruction for hundreds of microseconds would
destroy interactive latency.  The entire SCSI driver is IRQ-driven
*and* short-lived: each ISR completes one state transition and then
exits.  The main loop polls BSR only after `MR.DMA=1` has been set
and only until the first DR-asserted sample — the pseudo-DMA
transfer itself happens at bus speed because the blind-read path
succeeds immediately once DR is observed.

---

## 7. Plus vs. SE/30 vs. A/UX — side-by-side

| Aspect                      | Mac Plus + Mac OS           | SE/30 + Mac OS                      | SE/30 + A/UX                              |
| --------------------------- | --------------------------- | ----------------------------------- | ----------------------------------------- |
| SCSI base                   | `$580000` (24-bit)          | `$50010000` within I/O page         | same as SE/30 + Mac OS                    |
| Pseudo-DMA aliases          | one, `/DTACK`-stalled       | DRQ path `$06000` + blind `$12000`  | blind `$12000`                            |
| IRQ wiring                  | not wired                   | VIA2 CB2                            | VIA2 CB2                                  |
| DRQ wiring                  | drives `/DTACK` glue only   | VIA2 CA2                            | VIA2 CA2                                  |
| Driver posture              | polled, synchronous         | polled, synchronous                 | interrupt-driven, task-based              |
| Data phase                  | pseudo-DMA                  | pseudo-DMA (DRQ path)               | pseudo-DMA (blind)                        |
| Status phase                | ICR/ACK (no DMA)            | ICR/ACK (no DMA)                    | `MR.DMA=1`, pseudo-DMA blind read         |
| Message-in phase            | ICR/ACK                     | ICR/ACK                             | `MR.DMA=1`, pseudo-DMA blind read         |
| Arbitration                 | skipped (non-arbitrated)    | skipped                             | skipped                                   |
| Assumed host ID             | 7                           | 7                                   | 7 (but kernel says `host_id=7` in table)  |
| Surprise CHECK_CONDITION    | phase-mismatch IRQ recovery | same                                | watchdog (`unjam`) path                   |

---

## 8. SCSI command set reference

The rest of this document covers the SCSI-1 / SCSI-2 command subset
that Apple's 5380-era machines actually exercise.  The emulator's
direct-access device (hard disk) and CD-ROM models implement the
**Minimal Compliance Matrix** at the end of §8.10.

### 8.1 Historical disk models

Apple's original SCSI hard drives in the Plus era were typically
rebranded drives from various manufacturers:

| Apple Model | Vendor     | Model     | Cylinders | Sectors/Track | Heads | Total Blocks |
| ----------- | ---------- | --------- | --------- | ------------- | ----- | ------------ |
| HD20SC      | Seagate    | ST225N    | 615       | 17            | 4     | 41 820       |
| HD20SC      | Miniscribe | M8425S    | 612       | 17            | 4     | —            |
| HD40SC      | Conner     | CP2040A   | 1026      | 40            | 2     | —            |
| HD80SC      | Quantum    | Q280      | 823       | 32            | 6     | —            |
| HD160SC     | CDC        | Vren V HH | —         | —             | —     | —            |

Modern emulation abstracts away CHS addressing in favour of LBA.

### 8.2 Command Descriptor Blocks (CDBs)

SCSI groups opcodes by CDB length:

* **Group 0 (6-byte)** — opcodes `0x00–0x1F`; most core SCSI-1
  commands.
* **Group 1 (10-byte)** — opcodes `0x20–0x5F`; READ(10), WRITE(10),
  READ CAPACITY(10), VERIFY(10), etc.
* **Group 5 (12-byte)** — opcodes `0xA0–0xBF`; READ(12), WRITE(12),
  some optical/other.

#### Common 6-byte (Group 0) layout

| Byte | Bits | Meaning                                                                     |
| ---- | ---- | --------------------------------------------------------------------------- |
| 0    | 7..0 | Operation Code                                                              |
| 1    | 7..5 | LUN (legacy addressing)                                                     |
| 1    | 4..0 | High 5 bits of LBA (READ/WRITE/VERIFY) or command-specific                  |
| 2    | 7..0 | Middle 8 bits of LBA                                                        |
| 3    | 7..0 | Low 8 bits of LBA                                                           |
| 4    | 7..0 | Transfer Length (0 = 256 blocks for READ/WRITE) or parameter length         |
| 5    | 7..0 | Control (usually 0)                                                         |

> Some Group 0 commands (TEST UNIT READY, INQUIRY) don't carry LBA;
> bytes 1–4 are command-specific.

#### Common 10-byte (Group 1) layout

| Byte | Meaning                                                                |
| ---- | ---------------------------------------------------------------------- |
| 0    | Operation Code                                                         |
| 1    | Flags (RelAddr, FUA, DPO, etc.; device-type specific) + LUN (obsolete) |
| 2–5  | Logical Block Address (LBA)                                            |
| 6    | Group Number (upper LBA bits in some older devices)                    |
| 7–8  | Transfer Length (number of blocks)                                     |
| 9    | Control                                                                |

### 8.3 Status byte (target → initiator)

|   Code | Name                     | Meaning                                                      |
| -----: | ------------------------ | ------------------------------------------------------------ |
| `0x00` | **GOOD**                 | Command completed successfully                               |
| `0x02` | **CHECK CONDITION**      | Sense data available; initiator must issue REQUEST SENSE     |
| `0x08` | **BUSY**                 | Target/LUN busy; retry later                                 |
| `0x18` | **RESERVATION CONFLICT** | Access denied due to another initiator's reservation         |
| `0x28` | **QUEUE FULL** (SCSI-2)  | Target queue depth exceeded (if tagged queuing)              |

> The emulator should terminate each command with STATUS then
> MESSAGE IN: COMMAND COMPLETE (`0x00`) unless an exception occurs.

### 8.4 Message system (common subset)

**Single-byte messages (Message In unless noted):**

* `0x00` COMMAND COMPLETE
* `0x02` SAVE DATA POINTER
* `0x03` RESTORE POINTERS
* `0x04` DISCONNECT
* `0x06` MESSAGE REJECT
* `0x07` NO OPERATION
* `0x08` MESSAGE PARITY ERROR
* `0x0C` BUS DEVICE RESET
* `0x0D` ABORT
* `0x5A` INITIATOR DETECTED ERROR (Message Out)

**IDENTIFY (Message Out):** `0x80 | lun | (disconnect<<6)`.

**Extended Messages:**

* `0x01, 0x03, 0x01, <period>, <offset>` — SYNCHRONOUS DATA TRANSFER
  REQUEST (SDTR)
* `0x01, 0x02, 0x03, <width>` — WIDE DATA TRANSFER REQUEST (WDTR)

> A 5380-based Mac implementation stays asynchronous 8-bit.  Reject
> SDTR/WDTR unless explicitly emulating negotiation.

### 8.5 Mandatory commands (direct-access disks)

Below, **CDB** shows byte-accurate layouts and **Data** shows exact
payloads.  Multi-byte numeric fields are big-endian.

#### TEST UNIT READY — `0x00` (6)

* CDB: `00 | (lun<<5) | 00 | 00 | 00 | 00`
* Phases: COMMAND → STATUS → MESSAGE IN.
* Status: GOOD if ready; CHECK CONDITION with sense NOT READY when
  spun down / media absent.

#### REQUEST SENSE — `0x03` (6)

* CDB: `03 | (lun<<5) | 00 | 00 | alloc_len | control`
* Fixed-format response (response code `0x70` current or `0x71`
  deferred):

|  Byte | Field                                                             |
| ----: | ----------------------------------------------------------------- |
|     0 | Response Code                                                     |
|     1 | Obsolete                                                          |
|     2 | Sense Key (4-bit)                                                 |
|   3–6 | Information (vendor/command-specific or LBA for certain errors)   |
|     7 | Additional Sense Length (n)                                       |
|  8–11 | Command-specific information                                      |
|    12 | ASC                                                               |
|    13 | ASCQ                                                              |
|    14 | Field Replaceable Unit (FRU) (opt)                                |
| 15–17 | Sense-key-specific (opt)                                          |
|  18.. | Additional bytes as reported by byte 7                            |

Common sense keys: NO SENSE (`0x0`), RECOVERED (`0x1`), NOT READY
(`0x2`), MEDIUM ERROR (`0x3`), HARDWARE ERROR (`0x4`), ILLEGAL
REQUEST (`0x5`), UNIT ATTENTION (`0x6`), DATA PROTECT (`0x7`),
ABORTED COMMAND (`0xB`).

#### INQUIRY — `0x12` (6)

* CDB: `12 | (lun<<5)|evpd | page_code | 00 | alloc_len | control`
* Standard data (EVPD=0):

|  Byte | Field                                              |
| ----: | -------------------------------------------------- |
|     0 | Peripheral Qualifier (3) / Device Type (5)         |
|     1 | RMB (bit 7) removable                              |
|     2 | Version (`0x02` = SCSI-2)                          |
|     3 | Response Data Format                               |
|     4 | Additional Length (n)                              |
|   5–7 | Flags (CmdQue etc.)                                |
|  8–15 | Vendor Identification (8 ASCII, space-padded)      |
| 16–31 | Product Identification (16 ASCII, space-padded)    |
| 32–35 | Product Revision Level (4 ASCII)                   |
|  36.. | Optional SCSI-2+ fields                            |

#### MODE SENSE(6) — `0x1A` / MODE SELECT(6) — `0x15`

Both carry a 6-byte header: mode data length, medium type,
device-specific parameter, block-descriptor length.  Block
descriptors are 8 bytes each for direct-access devices: density,
number-of-blocks(3), reserved, block-length(3).

Pages of interest:

* `0x01` Read-Write Error Recovery
* `0x02` Disconnect/Reconnect
* `0x03` Format Device
* `0x04` Rigid Disk Geometry
* `0x08` Caching
* `0x30` Apple vendor-specific (Apple HD SC Setup's "APPLE COMPUTER, INC."
  drive identification — the emulator returns this to pass Setup's
  identity check; see `CMD_MODE_SENSE` in
  [scsi.c](../src/core/peripherals/scsi.c)).

#### READ(6)/WRITE(6) — `0x08` / `0x0A`

* Group 0 layout.  21-bit LBA, 8-bit Transfer Length (`0` → 256
  blocks).
* Phases: COMMAND → DATA IN/OUT → STATUS → MESSAGE IN.

#### READ(10)/WRITE(10) — `0x28` / `0x2A`

* Group 1 layout.  32-bit LBA, 16-bit Transfer Length.

#### READ CAPACITY(10) — `0x25`

* CDB: `25 00 00 00 00 00 00 00 00 00`
* Data (8 bytes): last_lba (4, BE), block_length (4, BE).

#### START STOP UNIT — `0x1B`

* CDB: `1B | immed | 00 | 00 | LoEj|Start | control`

#### PREVENT/ALLOW MEDIUM REMOVAL — `0x1E`

* CDB: `1E 00 00 00 prevent 00`

#### VERIFY(10) — `0x2F`

Like READ(10) but no data transfer.  Emulator returns GOOD if the
LBA range is in-bounds.

#### FORMAT UNIT — `0x04`

Low-level format / defect map.  Emulator accepts as a no-op on
writable devices; rejects with DATA PROTECT on read-only media.

### 8.6 CD-ROM extensions

The emulator's CD-ROM handler (`scsi_cdrom.c`) layers the following on
top of the direct-access set:

* `CMD_READ_TOC` (`0x43`)
* `CMD_READ_SUB_CHANNEL` (`0x42`)
* `CMD_READ_HEADER` (`0x44`)
* Audio commands (`0x45`, `0x47`, `0x4B`) — rejected with ILLEGAL
  REQUEST for data-only discs
* Sony vendor commands (`0xC1`..`0xC9`) — CDU-8002 proprietary; `0xC1`
  maps to `READ TOC`, rest are rejected

CD-ROM INQUIRY returns type `0x05` with the RMB bit set.  The MODE
SENSE for CD-ROM uses the CD-specific page set.

### 8.7 Sense, Unit Attention, and error policy

1. Target ends command with CHECK CONDITION.
2. Initiator issues REQUEST SENSE on the same I_T_L nexus.
3. Emulator returns fixed-format sense.

Common conditions modelled:

* **UNIT ATTENTION** on power-on, media change, or parameters changed.
  * Power on/Reset: ASC/ASCQ `0x29/0x00`
  * Media change: `0x28/0x00` (ready change) or `0x3A/0x00` (not
    present)
  * Parameters changed: `0x2A/0x01` or `0x2A/0x09`
* **ILLEGAL REQUEST** `0x20/0x00` (invalid opcode), `0x24/0x00` (bad
  field), `0x25/0x00` (LUN not supported).
* **DATA PROTECT** `0x27/0x00` on write to read-only media.

The emulator keeps a per-device sense structure that is cleared by a
successful REQUEST SENSE.  Auto-sense is not a SCSI-1 feature; hosts
must issue REQUEST SENSE explicitly.

### 8.8 Data transfer semantics

* **Block length**: 512 for HD, 2048 for CD-ROM (switchable via MODE
  SELECT).
* **Residuals**: on host-supplied length mismatch, prefer CHECK
  CONDITION/ABORTED COMMAND (`0x0B/0x00`).
* **REQ/ACK**: exactly one handshake per byte in the payload.

### 8.9 Tagged command queuing (optional, SCSI-2)

Message Out queue tags:

* `0x20, tag` — SIMPLE QUEUE TAG
* `0x21, tag` — ORDERED QUEUE TAG
* `0x22, tag` — HEAD OF QUEUE TAG

A Mac-authentic emulator **disables** tagged queuing and responds
with MESSAGE REJECT to these — classic Mac drivers don't emit them.

### 8.10 Minimal compliance matrix

* Implement **INQUIRY, TEST UNIT READY, REQUEST SENSE,
  READ/WRITE(6|10), READ CAPACITY(10), MODE SENSE(6), MODE SELECT(6)**.
* Implement **START STOP UNIT** (no-op acceptable) and
  **PREVENT/ALLOW MEDIUM REMOVAL** (image lock).
* Implement **UNIT ATTENTION**, **LUN not supported**, **Illegal
  Request** paths.
* CD-ROM: add **READ TOC**, **READ SUB-CHANNEL**, **READ HEADER**,
  and reject audio commands with ILLEGAL REQUEST + INCOMPATIBLE
  MEDIUM (`0x30/0x00`).
* Optional: **REASSIGN BLOCKS**, **FORMAT UNIT**, **VERIFY(10)**,
  **Tagged Queuing**.

### 8.11 Worked transaction examples

**READ(6) of 2 blocks @ LBA 0x012345 (block size 512):**

1. COMMAND: `08 | 0x01 | 0x23 | 0x45 | 0x02 | 0x00`
2. DATA IN: 1024 bytes over 1024 REQ/ACK handshakes
3. STATUS: `0x00` (GOOD)
4. MESSAGE IN: `0x00` (COMMAND COMPLETE)

**READ CAPACITY(10):**

1. COMMAND: `25 00 00 00 00 00 00 00 00 00`
2. DATA IN (8 bytes): last_lba (MSB…LSB) | block_len (MSB…LSB)
3. STATUS GOOD → COMMAND COMPLETE.

**Failing WRITE(10) to write-protected medium:**

* Emulator detects WP → CHECK CONDITION, stages sense DATA PROTECT,
  ASC/ASCQ `0x27/0x00`.
* Initiator issues REQUEST SENSE; emulator returns the fixed sense
  payload.

### 8.12 Direct-access opcodes used by classic Mac OS

| Opcode | CDB Len | Name                                      |
| -----: | :-----: | ----------------------------------------- |
| `0x00` |    6    | TEST UNIT READY                           |
| `0x03` |    6    | REQUEST SENSE                             |
| `0x04` |    6    | FORMAT UNIT                               |
| `0x07` |    6    | REASSIGN BLOCKS                           |
| `0x08` |    6    | READ(6)                                   |
| `0x0A` |    6    | WRITE(6)                                  |
| `0x0B` |    6    | SEEK(6) (often ignored)                   |
| `0x12` |    6    | INQUIRY                                   |
| `0x15` |    6    | MODE SELECT(6)                            |
| `0x16` |    6    | RESERVE(6)                                |
| `0x17` |    6    | RELEASE(6)                                |
| `0x1A` |    6    | MODE SENSE(6)                             |
| `0x1B` |    6    | START STOP UNIT                           |
| `0x1E` |    6    | PREVENT/ALLOW MEDIUM REMOVAL              |
| `0x25` |   10    | READ CAPACITY(10)                         |
| `0x28` |   10    | READ(10)                                  |
| `0x2A` |   10    | WRITE(10)                                 |
| `0x2F` |   10    | VERIFY(10)                                |
| `0xA8` |   12    | READ(12) (optional)                       |
| `0xAA` |   12    | WRITE(12) (optional)                      |
