# The Macintosh IIfx interrupt controller (OSS)

**Contents:**

1. [Overview](#1-overview) — what the part is, what it replaces, naming
2. [Register file](#2-register-file) — address decode, register summary, per-register detail, source map
3. [Behaviour](#3-behaviour) — request flow, autovectoring, arbitration, sensing, NMI, shutdown
4. [Programming model](#4-programming-model) — reset state, ROM POST self-tests, boot init, interrupt service
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

The Macintosh IIfx (1990) has no VIA2 and no GLUE-style fixed interrupt encoder. All interrupt
arbitration on the machine is performed by a single Apple custom IC, the **OSS** ("Operating
System Support"; Apple part 344S0076, *reported — unverified*). The OSS combines a **programmable
interrupt controller** with the machine's I/O address decoding, DSACK generation, bus time-out
logic, the NuBus slot-interrupt recording, the 60.15 Hz vertical-blanking monitor, a shutdown
control register, and — on parity-option machines — the parity-error interrupt path [1] p. 119.
This page documents the interrupt controller and its register window; the address-decode role is
summarised in §3.8, and the IOP chips that feed two of its sources are documented in `iop.md`.

Naming: Apple's own abbreviation **PIC**, in IIfx contexts, means *Peripheral Interface
Controller* — the IOP chip (`iop.md` §1), a different silicon part. The interrupt controller
function has no Apple marketing name; this page calls it the OSS interrupt controller. Apple
documentation calls the whole chip simply "the OSS custom IC" [1] p. 63.

The defining property is that **interrupt priorities are software-assigned**: any of the 16
possible interrupt sources can be assigned to any 68030 priority level, so different operating
systems can use different interrupt priorities on the same hardware [1] p. 63, [2] p. 14–15
(which directs the reader to Apple's internal OSS specification by Steven Ray for details).
Every other Macintosh of the generation has fixed interrupt levels; on the IIfx even the classic
"VIA1 = level 1" convention is merely what the boot ROM happens to program.

Sources fanned onto the 68030's three /IPL lines include: the six NuBus slots ($9–$E), the two
IOPs' host-interrupt outputs, the Apple Sound Chip, the SCSI subsystem, VIA1 (the machine's only
VIA), the 60.15 Hz vertical-blanking request, and the optional RAM Parity Unit's error
interrupt [1] pp. 101, 119, 155–157, 222, 393, 439. Level 7 is additionally driven directly by
the programmer's interrupt switch [1] p. 101.

---

## 2. Register file

### 2.1 Address decode

The interrupt controller's registers occupy an 8 KB window at **`$50F1A000`–`$50F1BFFF`** in the
IIfx I/O island. The I/O island itself spans `$50F00000`–`$50F3FFFF` and repeats every `$40000`
through the 256 MB I/O space at `$50000000`–`$5FFFFFFF`; the OSS also performs the address
decoding that maps the I/O devices into this space and generates the DSACK acknowledges for them
[1] p. 119. The window and its mirrors are established by the boot ROM's own address map and
model-detection logic [5]. The ROM reaches the window through a base pointer stored in its
per-machine descriptor block (descriptor slot `$48` = `$50F1A000`) [5].

The registers are **byte-wide**, decoded by their byte offset within the window. The boot ROM
accesses them exclusively with byte operations at the listed offsets [5]. Adjacent windows
`$50F1C000` and `$50F1E000` belong to the BIU30/"OSS-extension" hardware and the optional RPU,
not to the interrupt controller.

### 2.2 Register summary

| Offsets      | Register                    | Access | Reset     | Function                                              |
| ------------ | --------------------------- | ------ | --------- | ----------------------------------------------------- |
| `$000`–`00E` | level[0]…level[14]          | R/W    | 0 (*inf.*) | Per-source CPU level: 0 = disabled, 1–7 = IPL level  |
| `$200`       | status byte                 | R      | 0         | Bit 7 reads 1 while an interrupt is asserted (*inf.*) |
| `$201`       | (unknown)                   | ?      | ?         | Unestablished; see §6                                 |
| `$202`       | pending, sources 8–15       | R/W1C  | 0 (*inf.*) | Pending bitmap, high byte; word read with `$203`      |
| `$203`       | pending, sources 0–7        | R/W1C  | 0 (*inf.*) | Pending bitmap, low byte                              |
| `$204`       | ROM control (`ROM_CTRL`)    | R/W    | unknown   | 5 bits; boot ROM writes `$0D` here; see §2.3          |
| `$205`       | counter control             | R/W    | unknown   | 2 bits; stops/starts the free-running counter         |
| `$206`       | input status                | R      | ?         | Unestablished; candidate /SNDEXT bit; see §6          |
| `$207`       | 60.15 Hz acknowledge        | R/W    | —         | Writing (any value) clears source 10's pending bit   |
| `$208`–`20F` | free-running counter         | R      | unknown   | Reported in reconstructions; unverified — §6         |

*inf.* = *inferred — unverified* (see §4.1 for the basis of the reset-value inference).
All other rows are observed in the boot ROM's POST self-test and driver code [5], or stated by
Apple [1].

### 2.3 Per-source level registers (`$000`–`$00E`)

One byte per interrupt source, at offset = source number. Only the low **3 bits** are stored: a
read returns the value AND `$07`, and the ROM's POST round-trips every value 0…6 through each
register [5] (POST phase $91, §4.2). Apple's documentation calls these registers the
"Interrupt Mask registers" of the OSS [1] p. 222; ROM driver code names individual ones
`OSSMskPSwm` (offset `$006`, SWIM IOP) and `OSSMskPScc` (offset `$007`, SCC IOP).

| Value | Meaning                                                    |
| ----- | ---------------------------------------------------------- |
| 0     | Source disabled: it may become pending but never drives /IPL |
| 1–6   | Source drives 68030 autovector level 1–6                   |
| 7     | Source drives level 7 (non-maskable) — used for parity NMI  |

A value of 0 disables the source entirely; this is the only enable mechanism — there is no
separate mask register. Writing 7 to a source's level register makes its interrupt non-maskable;
system software does exactly this to enable the parity error as an NMI, and clears it by writing
0 back [1] p. 222. The Start Manager sets up all interrupt priority levels by writing these
registers and initialising the interrupt vectors to match [1] p. 119.

**Self-test quirk (source 10 only):** each *non-zero* write to `level[10]` (offset `$00A`) pulses
source 10's pending bit, and a write of 0 clears it. The ROM's POST interrupt-level self-test
depends on this behaviour (§4.2) [5]. No other level register has a pending side effect [5].

### 2.4 Pending / status registers (`$200`–`$203`)

The pending state is a 16-bit bitmap, one bit per source, readable as a big-endian word at
`$202` (Apple's internal software symbol for it: `OSSIntStat`; Apple's prose calls it the
"Interrupt Flags register" of the OSS [1] p. 393):

| Byte  | Bits          | Meaning                          |
| ----- | ------------- | -------------------------------- |
| `$202` | D7–D0 = sources 15–8 | Pending bitmap, high sources |
| `$203` | D7–D0 = sources 7–0  | Pending bitmap, low sources   |

Byte `$200` is reported by reverse engineering as a status byte whose bit 7 reads 1 while an
interrupt is being asserted to the 68030 (*inferred — unverified*); byte `$201`'s role is
unestablished (§6).

**Writing** to `$202` or `$203` is **write-1-to-clear**: each 1 bit clears the corresponding
pending bit, 0 bits leave the state unchanged (reconstructed from ROM handler code [5]; see
§4.4). Apple's SCSI discussion confirms the flags are stored in the OSS and used "under the
control of the system software" to generate the processor interrupts [1] p. 393.

### 2.5 ROM control and counter (`$204`–`$205`, `$208`–`20F`)

`$204` is a 5-bit control register (POST round-trips values 0…`$1F` with mask `$1F` [5]). The
boot ROM writes `$0D` to it very early in POST, gated on the descriptor's "OSS present" bit [5].
Bit 3 is exercised by the ROM's FMC self-test and interacts with the Fast Memory Controller's
ROM-path behaviour; its exact function is not established from available material (§6).

`$205` is a 2-bit control register (POST round-trips values 0–3 with mask `$03` [5]) that
enables/stops a free-running counter. Reconstructions derived from A/UX-era header files place a
wide free-running counter at `$208`–`$20F`, with bit 0 of `$205` as its stop/run control; its
width, clock rate and even presence are unverified — nothing in the available corpus reads it
(§6).

### 2.6 Source map

| Bit | Source wiring                                    | Apple symbol   | Sense   | Boot-ROM level [5] |
| --- | ----------------------------------------------- | -------------- | ------- | ------------------ |
| 0   | NuBus slot $9 (/IRQ1)                           | —              | level   | 2                  |
| 1   | NuBus slot $A (/IRQ2)                           | —              | level   | 2                  |
| 2   | NuBus slot $B (/IRQ3)                           | —              | level   | 2                  |
| 3   | NuBus slot $C (/IRQ4)                           | —              | level   | 2                  |
| 4   | NuBus slot $D (/IRQ5)                           | —              | level   | 2                  |
| 5   | NuBus slot $E (/IRQ6)                           | —              | level   | 2                  |
| 6   | SWIM IOP host interrupt (`/HINT`)               | `OSSIntIOPSWIM`| level   | 1                  |
| 7   | SCC IOP host interrupt (`/HINT`)                | `OSSIntIOPSCC` | level   | 4                  |
| 8   | Apple Sound Chip interrupt                     | —              | level   | 2                  |
| 9   | SCSI: 5380 /IRQ OR SCSI-DMA completion         | `OSSIntSCSI`   | level   | 2                  |
| 10  | 60.15 Hz vertical blanking request (/VBLK)      | —              | latched | 0, enabled later   |
| 11  | VIA1 /IRQ (timers T1/T2, one-second tick)      | —              | level   | 1                  |
| 12  | unknown                                         | —              | —       | 3                  |
| 13  | unknown                                         | —              | —       | 1                  |
| 14  | RAM Parity Unit error ("interrupt IRQ14")       | —              | latched | 7 (parity machines)|
| 15  | unknown; may not exist                          | —              | —       | —                  |

Provenance, row by row:

- **0–5, NuBus.** The OSS receives the six individual slot interrupt lines, ORs them to produce
  the slot interrupt, and records the individual lines [1] pp. 101, 157, 186. The per-slot pending
  bits are the record; the Guide describes it as "stores in a register the number of the slot
  generating the interrupt" [1] p. 157.
- **6–7, IOPs.** Each IOP's open-drain host-interrupt line `/HINT` [4] lands on its own OSS bit;
  the line stays asserted while either of the IOP's two host-interrupt latches is set, so the OSS
  bit is level-driven until the host acknowledges the IOP (`iop.md` §4, §13).
- **8, ASC.** The Apple Sound Chip's interrupt, which on two-VIA machines went to VIA2's IFR,
  goes to the OSS on the IIfx [1] pp. 157, 186; the ASC's external-jack sense signal `/SNDEXT`
  is *also* read through the OSS [1] pp. 157, 439 (a status bit, not the interrupt).
- **9, SCSI.** The 5380's /IRQ and the SCSI DMA wrapper's completion signal are OR'd into a
  single OSS source, bit 9 in the pending register [5]; Apple states the SCSI DMA's IRQ and DRQ
  signals "are stored in the Interrupt Flags register in the OSS" [1] p. 393. There is no
  VIA2-style IFR bit for SCSI on this machine.
- **10, 60.15 Hz.** The OSS itself monitors the 60.15 Hz interrupt request /VBLK — a VIA1
  function on other models — and interrupts the main processor as appropriate [1] p. 155. The
  event latches; software acknowledges at `$207` [5].
- **11, VIA1.** The machine's single VIA; its timers and the RTC one-second interrupt arrive
  here [1] pp. 101, 154.
- **14, parity.** On machines fitted with the optional RPU, the RPU sends "interrupt IRQ14" to
  the OSS [1] p. 222; system software enables it as an NMI by writing 7 to its level register
  [1] p. 222.

The "boot-ROM level" column is the assignment the boot ROM programs early in startup, as
established by ROM disassembly and POST traces [5]; sources 6, 7 and 9 are independently
confirmed by the ROM's own driver code (`OSSMskPSwm = 1`, `OSSMskPScc = 4`, source 9 = 2)
(`iop.md` §6, §13). Sources 12 and 13 are programmed by the ROM but their wiring is unknown
(§6). The whole mapping is software-configurable — A/UX was expected to use different
priorities [2] p. 14–15 — so these are conventions of Apple's System Software, not fixed
hardware properties.

A conflicting source numbering (sound = 6, SCSI = 7, ISM IOP = 8, SCC IOP = 9) circulates in
operating-system code derived from A/UX header files; it contradicts the ROM-anchored numbering
above and is unresolved — see §6.

---

## 3. Behaviour

### 3.1 Request flow

```
NuBus slots $9-$E ────┐
SWIM IOP /HINT ───────┤            level[0] ─┐
SCC IOP /HINT ────────┤            level[14] ├─→ max(level[s]) over pending
ASC interrupt ────────┤→ pending ─────────────┘   and enabled sources
SCSI IRQ/DMA-done ────┤   ($202/$203)                  │
60.15 Hz /VBLK ───────┤                               ▼
VIA1 /IRQ ────────────┤                       68030 /IPL2-/IPL0
RPU parity error ─────┘                              autovector $19+level-1
```

When a source asserts, its pending bit sets. The controller continuously computes the highest
programmed level among all pending-and-enabled sources and asserts that level on the 68030's
/IPL inputs; when the set of pending sources or any level register changes, the /IPL outputs are
re-evaluated (reconstructed from ROM handler and POST behaviour [5]; the IOP path is documented
in `iop.md` §13).

### 3.2 Autovectoring

All interrupts on the machine are autovectored. The 68030 acknowledges an interrupt by asserting
its function codes and placing an address in `$FFFFFFF0`–`$FFFFFFF` on the bus; the general
logic signals the automatic-vector condition, and the CPU generates a vector number from the
level — `$19` for level 1 through `$1F` for level 7 [1] pp. 63, 99, 101 (Table 3-5). Apple
states the family rule flatly: "Interrupts in Macintoshes are always auto-vectored; interrupting
devices do not respond to IACK cycles" [2] p. 14. The interrupt handler at the autovector then
identifies the source in software by reading the pending word (§4.4). The vector is fetched
through the CPU's VBR, i.e. at `VBR + 4 × (24 + level)`; the ROM's POST installs test handlers
this way, at `$78` for level 6 down to `$64` for level 1 [5].

### 3.3 Arbitration and priority

Because each source's level is a register, arbitration is a two-step rule:

1. Among sources whose pending bit is set **and** whose level register is non-zero, the highest
   level wins and is asserted on /IPL.
2. Sources at the same or lower levels remain pending; they assert only when the CPU's own
   interrupt priority mask (set in SR by the running handler) allows the level through.

Two or more sources may legitimately share one level (the boot assignment puts NuBus, ASC and
SCSI all at level 2 [5]); the single autovector handler for that level disambiguates by polling
the pending word. While an interrupt is being serviced, the CPU's priority mask — not the
controller — inhibits equal and lower levels.

### 3.4 Sensing: level vs latched

- **Level-driven sources.** The IOP sources (6, 7) follow their `/HINT` lines: `/HINT` is
  open-drain [4] and stays asserted until the host acknowledges both host-interrupt latches in
  the IOP (`iop.md` §4); the pending bit reflects the line. SCSI's source 9 follows the OR of
  the 5380 /IRQ and the DMA completion signal, which deassert when the SCSI device is serviced
  [1] p. 393. VIA1's and the NuBus slots' requests are level inputs, as on two-VIA machines
  [1] pp. 101, 157.
- **Latched sources.** The 60.15 Hz source (10) latches on each /VBLK event and stays pending
  until acknowledged — the ROM's own test handler must write `$207` before returning, "otherwise
  the trap would re-fire immediately" [5]. The parity source (14) is recorded by the RPU and
  cleared by writing 0 to its level register, not by the pending word [1] p. 222.

### 3.5 The 60.15 Hz source

The OSS takes over the vertical-blanking interrupt request directly: it monitors /VBLK at
60.15 Hz and interrupts the main processor as appropriate — on other Macintosh II-family
machines this request arrives at VIA1 [1] p. 155. Each event sets source 10's pending bit;
the interrupt service routine acknowledges by writing to `$207` [5]. The source is disabled
(level 0) until software assigns it a level [5]. Firmware and a variety of software use this
tick [1] p. 155.

### 3.6 Level-7 paths

Two requestors reach level 7, which the 68030 cannot mask:

- **The programmer's interrupt switch**, wired to the OSS, which "generates level-7 interrupts,
  which cannot be inhibited by the MC68030's interrupt priority mask" [1] p. 101. Whether this
  switch passes through a pending bit or drives /IPL7 directly is not established (§6).
- **Parity errors**, on machines fitted with the optional RPU: the RPU sends IRQ14 to the OSS
  [1] p. 222; software makes the source non-maskable by writing 7 to its level register [1]
  p. 222. The parity interrupt handler clears the interrupt by writing 0 back to that register
  [1] p. 222.

### 3.7 Shutdown

When the user chooses Shut Down, the software causes the OSS to assert the /POWEROFF signal;
the power supply switches off within 2 ms of the assertion [1] pp. 119, 256. Apple states only
that this is "a control register in the OSS" [1] p. 119; its offset is not established from
the available material (§6).

### 3.8 Non-interrupt functions of the OSS

For completeness, the same chip also: decodes all I/O addresses and generates DSACK0/DSACK1 for
the I/O spaces [1] p. 119; provides bus time-out logic [1] p. 119; supplies the interface to
the 65C22 VIA for compatibility [1] p. 119; records NuBus slot interrupts (§2.6) [1] p. 157;
exposes a readable bit for the /SNDEXT external-sound-jack sense [1] pp. 157, 439; and controls
the parity-enable signal and parity-error interrupt on parity machines [1] p. 157. These
functions share the register window's neighbourhood but are not part of the interrupt logic
documented above; the /SNDEXT bit's register location is unknown (§6).

---

## 4. Programming model

### 4.1 Hardware reset state

System reset brings the chip to a known state [1] p. 256. No Apple document enumerates the
register reset values. The boot ROM behaves as if **all level registers reset to 0** (all
sources disabled): POST's interrupt-level test saves and restores `level[10]` expecting 0 on
entry, and the boot sequence re-programs every level register from scratch [5] — *inferred —
unverified*. The pending bitmap is likewise treated as 0 at entry (*inferred — unverified*).
Reset values of `$204` and `$205` are unknown; the first boot-ROM write to `$204` is the value
`$0D` [5].

### 4.2 ROM POST self-tests

The IIfx ROM's power-on self-test exercises the controller twice [5]. Both phases are gated on
the per-machine descriptor's "OSS present" flag (bit 18) [5].

**Phase $91 — register round-trip** (`$40842FC4`). With the CPU priority mask raised to 7, the
test walks a table of (offset, mask, max) entries; for each it saves the current byte, writes
every value 0…max, reads each back AND the mask and requires an exact match, then restores the
original value:

| Offsets tested  | Mask | Max | Identifies                     |
| -------------- | ---- | --- | ------------------------------ |
| `$000`–`$00D`  | `$07`| 6   | 14 level registers             |
| `$204`         | `$1F`|`$1F`| ROM control, 5 bits            |
| `$205`         | `$03`| 3   | counter control, 2 bits        |

**Phase $92 — interrupt-level autovector test** (`$4084306E`). For each level N = 6 down to 1:
a three-instruction handler (clear `$207`; set a magic value in D0; RTE) is installed at the
level-N autovector; the CPU priority mask is lowered; level N is written to `level[10]`
(offset `$0A`); and the test busy-waits for the handler's magic. For the trap to fire on every
iteration, each non-zero write to `level[10]` must itself pulse source 10's pending bit — the
self-test behaviour of §2.3. The handler's write to `$207` clears the pending bit so the next
iteration starts clean; on exit the test restores the saved `level[10]` value [5].

### 4.3 Boot and Start Manager initialisation

1. Early in POST, gated on the descriptor's OSS-present bit, the ROM writes `$0D` to `$204`
   [5].
2. The ROM writes the level registers — zeroing them first, then programming the assignment of
   §2.6 [5].
3. The system software's Start Manager "sets up the interrupt priority levels by writing to
   control registers in the OSS and initializing the interrupt vectors to match the priorities"
   [1] p. 119 — i.e. the autovectors `$19`–`$1F` are each pointed at a dispatcher that polls
   the pending word for sources at that level.
4. The SCSI Manager arms the SCSI DMA wrapper's interrupt enable once, leaving it set for the
   lifetime of the boot (`iop.md` §6).

The resulting assignment observed in the ROM's own boot-time state [5]:

| Level | Sources (observed boot assignment)              |
| ----- | ----------------------------------------------- |
| 1     | SWIM IOP (6), VIA1 (11), source 13              |
| 2     | NuBus slots (0–5), ASC (8), SCSI (9)            |
| 3     | source 12                                       |
| 4     | SCC IOP (7)                                     |
| 6     | (unused at boot; VIA1 sits at level 1)          |
| 7     | parity (14), programmer's switch (direct)       |

A secondary reconstruction reports a different assignment for some sources (NuBus = 3, VIA1 = 6,
ASC sometimes 5) and attributes it to the same boot; the discrepancy is unresolved (§6). The
60.15 Hz source is left at 0 and enabled later by software that wants the tick [5].

### 4.4 Interrupt service

The autovector handler for level N, as reconstructed from ROM handler code [5]:

1. Read the pending word at `$202`.
2. AND it with the mask of sources whose level register equals N.
3. Service each set bit (acknowledging the device, which deasserts level sources, or writing
   `$207` for the 60.15 Hz tick).
4. Write 1s to the serviced bits at `$202`/`$203` to clear them.

Concrete ROM example (SCSI bulk read, `iop.md` §6): the wrapper raises source 9 on transfer
completion; the driver's wait loop spins on bit 9 of the pending byte at `$202`:

```
@waitForIntr
        btst.b  #1, (a1)        ; a1 = OSS + $202; bit 1 = source 9
        beq.s   @waitForIntr
```

IOP interrupts are the same shape: the IOP firmware raises `iopInt0Active`/`iopInt1Active` in its
status/control register, asserting `/HINT`; the OSS bit (6 or 7) follows the line; the host
handler services the mailbox and acknowledges the IOP, which deasserts `/HINT` and clears the
OSS pending bit (`iop.md` §4, §11, §13).

### 4.5 Parity NMI setup (parity-option machines)

System software first determines that the RPU is installed, reads the RPU, and enables parity
checking (VIA1 Data register B bit 6 enables parity generation and checking by the RPU) [1]
p. 222. To make parity errors non-maskable it writes 7 into the parity source's Interrupt Mask
(level) register in the OSS; the handler clears the interrupt by writing 0 to that register
[1] p. 222.

### 4.6 A/UX

Apple expected A/UX (and Pink) to program different priorities on this hardware [2] p. 14–15.
The priorities being pure registers, an alternative OS rewrites the level registers and
re-points the autovectors; no other controller change is required. A/UX's specific IIfx
priority assignment is not documented in the available material (§6).

---

## 5. Quirks & errata

- **Level registers are the only enable.** There is no separate interrupt-enable register; a
  source is enabled exactly when its level register is non-zero, and "disabled" sources can
  still latch pending bits that only surface if the level is later raised.
- **The source-10 pulse quirk.** Every non-zero write to `level[10]` pulses the 60.15 Hz
  pending bit, and a zero write clears it; the ROM's POST relies on this (§4.2) [5]. Software
  that reprograms the tick's level in a running system generates one immediate tick per write.
- **Writing 0 to a level register has side effects on source 10 only** (clears pending);
  for every other source it only disables the source and leaves the pending bit alone [5].
- **No VIA2 exists.** "All of the functionality of VIA2 has been moved to other chips in
  Macintosh IIfx, so if an application depends on VIA2 registers, it must find a different way
  to get the information" [3]. SCSI and ASC interrupts never appear in any VIA interrupt flag
  register; code that polls VIA2 for SCSI or sound interrupts sees nothing [1] p. 393.
- **Autovector positions are a software convention.** Because levels are assigned, the
  autovector a driver must hook depends on how the running OS programmed the OSS; Mac II
  drivers that hardcode "VIA1 = level 1" or "SCSI = VIA2 = level 2" break on the IIfx [1]
  p. 63. The boot ROM patches many traps for exactly this reason.
- **Shared levels are normal.** The boot assignment puts NuBus, ASC and SCSI on one level [5];
  a handler at that level must poll the pending word rather than assume a single source, and
  must clear only the bits it serviced.
- **Parity clears through the level register.** The parity interrupt is cleared by writing 0 to
  its Interrupt Mask register — not through the pending word [1] p. 222.
- **The pending word is write-1-to-clear at the byte lanes**; a longword store at `$200`
  composes four byte-lane writes, so its high lanes are no-ops (reconstructed; the ROM uses
  byte and word access [5]).

---

## 6. Open questions

1. **Source-numbering conflict.** An A/UX-header-derived numbering used by third-party
   operating-system code assigns pending bits 6–9 as sound, SCSI, ISM IOP, SCC IOP,
   contradicting the ROM-anchored numbering of §2.6 (SWIM IOP = 6, SCC IOP = 7, ASC = 8,
   SCSI = 9). The ROM disassembly, Apple's own internal software symbols (`OSSIntIOPSWIM = 6`,
   `OSSIntIOPSCC = 7`, `OSSIntSCSI = 9`) and the RPU's "IRQ14" all support the numbering used
   here; the discrepancy is unresolved.
2. **Canonical Mac OS level assignment.** The observed boot state (§4.3) and a secondary
   reconstruction disagree on NuBus (2 vs 3), VIA1 (1 vs 6) and ASC (2 vs 5). Which set the
   shipping System 6/7 Start Manager writes on real silicon — and whether A/UX differs — is
   unverified.
3. **Sources 12, 13 and 15.** The ROM programs levels for sources 12 and 13, but nothing in
   the available material names their wiring. Whether a 16th source (level register at `$00F`)
   exists — implied by "any one of 16 possible interrupts" [2] p. 14–15 — is unknown.
4. **The shutdown register.** /POWEROFF is attested [1] pp. 119, 256, but its offset and trigger
   value are not established; one reconstruction places a power-off register at `$201`, another
   does not model one at all.
5. **`$204` semantics.** Five bits, boot-written `$0D`, with bit 3 involved in the ROM's FMC
   self-test — the exact function of every bit is unknown.
6. **The free-running counter.** The register block at `$208`–`$20F` and the run/stop bit of
   `$205` are reported from A/UX-derived reconstructions; width, clock rate and read semantics
   are unverified, and nothing observed so far reads it.
7. **`$200`/`$201`/`$206`.** The status byte at `$200` (reported bit 7 = "interrupt currently
   asserted"), the byte at `$201`, and the read-only byte at `$206` (candidate location for the
   /SNDEXT sound-jack sense bit [1] pp. 157, 439) are all unestablished.
8. **Programmer's-switch path.** Level 7 from the switch is attested [1] p. 101; whether it
   sets a pending bit (and at which bit position) or drives /IPL7 combinationally is unknown.
9. **Reset values.** The all-zero level-register reset state is inferred from ROM behaviour,
   not from a datasheet; the reset values of `$204`, `$205` and the pending word are unknown.
10. **DRQ disposition.** Apple states the SCSI DMA's DRQ as well as IRQ is "stored in the
    Interrupt Flags register in the OSS" [1] p. 393; the reconstructed register file provides
    one SCSI source bit, and where DRQ is recorded is unestablished.
11. **Bus timing.** No available source specifies the OSS's interrupt latency, the /IPL
    re-evaluation timing after a pending-bit clear, or the DSACK timing of register accesses.

---

## References

1. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, 2nd edition, Addison-Wesley,
   1990. (OSS: p. 119; interrupt architecture: pp. 63, 99–101; VIA1/VIA2 function split:
   pp. 154–157; interrupt registers note: pp. 186–187; parity: p. 222; power down: p. 256;
   SCSI interrupts: p. 393; sound: p. 439.)
2. Apple Computer, Inc., *Macintosh Hardware Overview*, revision 2, 11 February 1991 —
   "Interrupts", pp. 14–15, and bibliography, p. 77.
3. Apple Computer, Inc., Apple Technical Note HW #09, *Macintosh IIfx: The Inside Story*,
   April 1990.
4. Apple Computer, Inc., *Peripheral Interface Controller Specification*, revision 1
   (Macintosh IIfx "F19" theory of operation, PIC/IOP chapter).
5. Macintosh IIfx ROM (512 KB, 5 February 1990), annotated disassembly — early OSS init at
   `$40802E50`; POST phase $91 at `$40842FC4`; POST phase $92 at `$4084306E`; 60.15 Hz
   acknowledge at `$207`.
