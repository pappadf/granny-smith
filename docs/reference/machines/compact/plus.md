# Macintosh Plus

The Macintosh Plus (January 1986) is the fourth and last of Apple's classic Macintosh
computers: a 7.8336 MHz MC68000 on the same PAL-based main logic board as the Macintosh
128K, 512K and 512K enhanced, but with 1 MB of RAM on SIMMs expandable to 4 MB, a 128 KB
ROM, the built-in 9-inch display of the family, and — uniquely in the family — an NCR 5380
SCSI bus, mini-DIN 8 serial connectors and a keyboard with a built-in numeric keypad and
arrow keys [1] p. 4. Everything the four boards share is documented once in the family doc
([compact.md](compact.md) §2–§5); this page carries only what is unique to the Plus, and
cites the family doc rather than restating it.

**Contents:**

1. [Identity](#1-identity) — what the machine is; identity values; the spec table; the case
   and the connector set
2. [Deltas vs the family doc](#2-deltas-vs-the-family-doc) — the complete delta list; clocks
3. [Per-subsystem wiring](#3-per-subsystem-wiring) — the SIMM array; the VIA, SCC, IWM, RTC,
   5380, sound/video, keyboard and mouse as actually fitted
4. [Expansion](#4-expansion) — RAM expansion rules; the SCSI chain; storage on the floppy
   port; what this machine does not expand by
5. [Boot sequence summary](#5-boot-sequence-summary) — the Plus ROM's startup path, as
   observed in the disassembly
6. [Open questions](#6-open-questions)

---

## 1. Identity

### 1.1 What the machine is

Apple's definition is one sentence long and is the authority for this page's subject:
"The Macintosh Plus computer, with an MC68000 microprocessor running at 8 MHz, 1 MB of RAM
(minimum), 128 KB of ROM, and an 800 KB internal disk drive. The RAM in the Macintosh Plus
can be expanded to 4 MB. Other enhancements in the Macintosh Plus include a Small Computer
System Interface (SCSI) port for high-speed communications with disk drives and other
peripheral devices, new connectors for the serial ports, and a keyboard with built-in
cursor keys and numeric keypad" [1] p. 4. The 512K enhanced shares the Plus's ROM and
internal disk drive, so those two are not Plus-unique ([compact.md](compact.md) §6.3); what
is left — SIMM memory, the SCSI port, the mini-DIN 8 serial connectors and the keypad
keyboard — plus the enclosure differences, is exactly what this page documents.

The Plus is the family's fully specified member and the one this reference emulates
([compact.md](compact.md) §1.3, §6.4). Its evidence base is also the family's: Apple's
*Guide to the Macintosh Family Hardware* is the only published hardware document for any
classic Macintosh, and its Plus-specific sections — the SIMM configuration pages of
Chapter 5, the power-supply specifications, the whole of Chapter 7 on the Plus mouse and
keyboard, and the Plus rows of the SCSI, serial and floppy chapters — supply this page's
citable backbone [1]. The boot behaviours are *observed* in the annotated disassembly of
the Plus's own 128 KB boot ROM [2], cited here by ROM version and checksum, never by note
or session.

### 1.2 Identity values

The classic Macintosh boards carry no custom identification silicon: no box-flag register,
no ID ROM, no gestalt hook — the general logic is PALs ([compact.md](compact.md) §2.3), and
a machine identifies itself to software only through its ROM image and the global
`HWCfgFlags`. The values for this machine:

| Identity value | Value | Witness |
|---|---|---|
| ROM size | 128 KB, in two byte-wide ICs soldered to the main logic board [1] pp. 68, 196 (compact.md §2.5) | [1] |
| ROM version | `$75` (byte at image offset $9) | [2] (*observed*) |
| ROM header checksum | `$4D1F8172` (longword at image offset $0); the boot code re-derives it over the whole image and fails the boot if the folded halves disagree (§5, step 9) | [2] (*observed*) |
| ROM header machine-type byte | `$00` (byte at image offset $8) | [2] (*observed*) |
| Boot vector | `$0040002A` — the longword at image offset $4 that the CPU loads as its initial PC out of reset, landing on the entry at file offset $2A | [2] (*observed*); compact.md §2.8 |
| ROM resources block | image offset `$176F8` | [2] (*observed*) |
| Gestalt machine type | 4 (`gestaltMacPlus`), per the published Gestalt constants enumeration [4] | [4]; §6.2 |
| `HWCfgFlags` at boot | `$C0` — bits 6 and 7 set, meaning "SCSI present" — written by the Reset handler after the probe of §5 step 12 | [2] (*observed*); §6.1 |
| Parameter RAM | 256 bytes in the RTC, plus the 4-byte seconds counter (compact.md §4.2) | [1] pp. 77, 144, 468 |
| Box flag | none — the classic boards have no identification register; software identification is the ROM image, `HWCfgFlags`, and the keyboard's Model Number response (§3.8) | compact.md §1.1, §2.3 |

Two of these rows need their honest qualifiers. The **Gestalt value** is a system-software
fact, not a ROM fact: the `$75` ROM predates the Gestalt Manager, and no `_Gestalt` dispatch
exists anywhere in the disassembly [2] (*observed*) — the value 4 is what system software
equipped with the Gestalt Manager reports for this machine, from the published constants
list [4]. The **`HWCfgFlags`** bits are defined by Apple in the MPW include file
`HardwareEqu.a` [1] p. 6; that file is not in the evidence set, so the meaning of bits 6–7
rests on the ROM's own use of them as the SCSI-present test [2] (*observed*), and the
remaining bits' meanings are open (§6.1).

### 1.3 Spec table

| Parameter | Value |
|---|---|
| CPU | Motorola MC68000, 32-bit internal, 16-bit external data bus, 24-bit external address bus [1] p. 468; compact.md §2.2 |
| Processor clock | 7.8336 MHz [1] pp. 18, 468 — the rounded "8 MHz" of Apple's feature list [1] p. 4; compact.md §5.5 |
| RAM | 1 MB minimum on SIMMs, expandable to 4 MB; configurations of 1, 2, 2.5 and 4 MB (§4.1) [1] pp. 4, 197–202, 468 |
| ROM | 128 KB [1] pp. 4, 468 — "expandable to 256 KB" per the specification appendix [1] p. 468; no 256 KB Plus ROM is documented anywhere (§6.3) |
| Parameter RAM | 256 bytes in the RTC, battery-backed by a rechargeable 4.5 V battery (Eveready No. 523 or equivalent), user-replaceable [1] p. 468 |
| Internal floppy | one 800 KB double-sided 3.5-inch drive [1] pp. 4, 468 |
| External floppy | one DB-19 connector for an external 800 KB drive or an Apple Hard Disk 20 [1] pp. 337–338, 468 |
| Hard disk | external SCSI connector accepts a SCSI hard disk; the DB-19 connector accepts an Apple Hard Disk 20 [1] p. 468 |
| SCSI | one external DB-25 port, NCR 5380, polled — no IRQ or DRQ connection (§3.6) [1] pp. 376–380, 394 |
| Video | built-in 9-inch (diagonal) 512×342 1-bit display, the family's shared-RAM frame buffer (compact.md §2.6) [1] pp. 399–401, 468 |
| Sound | PWM sound system: four-voice mono with digital-to-analog conversion at a 22.255 kHz sample rate (compact.md §2.7) [1] pp. 427–431, 468 |
| Serial | two RS-422 ports on mini-DIN 8 connectors (modem port A, printer port B) (§3.3) [1] pp. 357–366 |
| Input devices | Macintosh Plus keyboard with built-in keypad and arrow keys, RJ-11 connector (§3.8); mouse, DB-9 connector, mechanical/optical mechanism generating 90 pulses per inch on each axis [1] pp. 273–286, 468 |
| Expansion | no expansion slots (compact.md §1.2) [1] Table 1-1 pp. 2–3; RAM SIMMs, the SCSI bus and the floppy port are the only growth paths (§4) |
| Power | 85–135 V or 170–270 V rms input at 47–63 Hz, configured by switch SW1; 80 W peak input; 46.8 W maximum continuous DC output (§4.4) [1] pp. 252–258 |
| Enclosure | the classic compact case: main unit 344 × 246 × 276 mm, 7.5 kg; keyboard 1.2 kg; mouse 0.2 kg [1] Table A-1 p. 469 |

### 1.4 The case and the connector set

"The Macintosh Plus computer has the same exterior appearance as the earlier Macintosh
computers, but has different external connectors. The front view of the Macintosh Plus is
identical to that of the Macintosh computers that preceded it … Notice that a connector for
the SCSI (parallel) port has been added and that the connectors for the two serial ports
have been changed from DB-9 connectors to mini 8-pin connectors" [1] p. 8. The back panel
therefore carries: the two mini-DIN 8 serial connectors, the DB-25 SCSI connector, the DB-19
external floppy connector, the DB-9 mouse connector, the sound jack, and the rear-mounted
mechanical on/off switch shared by the whole family [1] pp. 8, 241. The keyboard connects at
the front, on a four-wire RJ-11 telephone-style connector [1] p. 281. The keyboard connector
on the Plus sits behind the same front-panel opening as on the earlier machines; the keypad
is in the keyboard case, not the main unit [1] pp. 280–281.

---

## 2. Deltas vs the family doc

### 2.1 The delta list

Everything not in this table is the classic Macintosh platform by construction
([compact.md](compact.md) §2–§5): one MC68000, PAL general logic, the shared-RAM video,
sound and disk-speed channels, the single VIA, the SCC, the IWM, the RTC, the phase read,
the ROM overlay, the three-level interrupt scheme.

| Area | Family baseline (compact.md) | Macintosh Plus |
|---|---|---|
| RAM carrier | DRAM soldered to the board, CAS1/CAS2 per row of eight (§2.4) | two or four SIMMs, CAS1–CAS4 one per SIMM, ten RAM address lines to 4 MB (§3.1) [1] pp. 196–202 |
| RAM size | 128 KB–512 KB, fixed at build | 1 MB minimum, 4 MB maximum, user-configurable; size discovered at startup (§5, step 9) [1] pp. 4, 201 |
| ROM | 64 KB (128K/512K) or 128 KB (512Ke/Plus) (§2.5) | the 128 KB `$75` image, also used by the 512K enhanced [1] p. 4; [2] |
| Floppy drive | 400 KB single-sided or 800 KB double-sided (§4.3) | 800 KB double-sided, internal speed control — the PWM motor-speed line is still wired for external 400 KB drives (§3.4) [1] pp. 18, 335–338 |
| Serial connectors | two DB-9 [1] p. 8 | two mini-DIN 8 — HSKo added, +5 V/+12 V removed, GPi not connected (§3.3) [1] pp. 359–361 |
| SCSI | none | NCR 5380, external DB-25 only, polled (§3.6) [1] pp. 377–380, 394 |
| Keyboard | no keypad; optional separate keypad on 128K/512K/512Ke | keypad and arrow keys built in, with keypad-protocol compatibility sequences (§3.8) [1] pp. 280, 283–284 |
| Parameter RAM | 20 bytes (128K/512K) or 256 bytes (512Ke/Plus) (§4.2) | 256 bytes [1] pp. 77, 144, 468 |
| RTC battery | battery-backed parameter RAM (§4.2) | rechargeable 4.5 V battery, user-replaceable [1] p. 468 |
| Power supply | classic supply on the analog board (§2.1) | switch-configurable 120 V/240 V input (SW1), 80 W peak, 46.8 W continuous, −5 V derived on the logic board (§4.4) [1] pp. 252–258 |
| Mouse | family DB-9 mouse on SCC + VIA (§5.3) | same wiring; the Plus mechanism generates 90 pulses per inch [1] p. 468 |
| Gestalt machine type | not applicable to the family | 4 (`gestaltMacPlus`) [4]; §1.2 |

### 2.2 Clocks on this machine

The Plus carries the family clock set unchanged: one 15.6672 MHz crystal, divided by the
PALs into the 7.8336 MHz CPU clock, the 3.672 MHz SCC clock, the 783.36 kHz VIA E clock,
and the 22.25 kHz/60.15 Hz scan cadences ([compact.md](compact.md) §5.5). No
Plus-specific clock fact exists in the evidence set beyond the family table — the "8 MHz"
of Apple's feature list [1] p. 4 is the rounded 7.8336 MHz [1] p. 18. The one Plus-only
clock remark worth making is the SCC clock's consumer: the 3.672 MHz reference times both
serial ports' baud generators and the AppleTalk rate, and the Plus's SCC register windows
and the word-access phase-shift trap are the family's ([compact.md](compact.md) §3.2,
§3.3); which SCC input pin the 3.672 MHz PAL output actually reaches is not established
([compact.md](compact.md) §7, item 14).

---

## 3. Per-subsystem wiring

### 3.1 Memory: the SIMM array

The Plus is the family's only SIMM machine. "The RAM in the Macintosh Plus and Macintosh SE
is provided in either two or four packages known as Single In-line Memory Modules
(SIMMs). A SIMM consists of a small printed circuit board that contains several
surface-mounted DRAM ICs" [1] p. 197. Each Plus SIMM carries eight DRAM ICs and presents
ten address pins and eight data pins; each data pin serves one of the eight DRAMs. The
SIMMs work in pairs, and a pair is a *row*: row 1 is SIMMs 1 and 2, row 2 is SIMMs 3 and 4;
two SIMMs in a row supply the 16-bit word the MC68000 needs [1] p. 197. Nine-bit SIMMs work
— the ninth bit is simply not connected [1] p. 197.

Addressing is the family's two-strobe scheme ([compact.md](compact.md) §2.4) widened from
two CAS lines to four: the PALs decode a RAM address, the RAM address multiplexers split it
into two 10-bit halves, `/RAS` strobes the row half, and one of four column strobes —
CAS1 through CAS4, one per SIMM — strobes the column half into the addressed SIMM. "In
this way, 4 MB of RAM can be addressed through the ten RAM address lines" [1] p. 198. The
installed population is told to the general logic not by software but by two resistors in
the board area labeled RAM SIZE: R8 ("256K BIT") and R9 ("ONE ROW"), installed or removed
per the rules of §4.1; "the general logic circuits use the SIMM resistors or jumper to
determine which row to access for each address range" [1] pp. 198, 201.

The SIMM socket pinout (Table 5-4 [1] pp. 200–201), the wiring a card or fixture must
reproduce:

| Pin | Signal | Pin | Signal | Pin | Signal |
|---|---|---|---|---|---|
| 1 | +5 V | 11 | RA4 | 21 | READ (RAM read) |
| 2 | /CAS1, /CAS2, /CAS3 or /CAS4 (per SIMM) | 12 | RA5 | 22 | GND |
| 3 | RDQ0 (SIMM 1/3) or RDQ8 (SIMM 2/4) | 13 | RDQ3 / RDQ11 | 23 | RDQ6 / RDQ14 |
| 4 | RA0 | 14 | RA6 | 24 | n.c. |
| 5 | RA1 | 15 | RA7 | 25 | RDQ7 / RDQ15 |
| 6 | RDQ1 / RDQ9 | 16 | RDQ4 / RDQ12 | 26 | n.c. |
| 7 | RA2 | 17 | RA8 | 27 | /RAS |
| 8 | RA3 | 18 | RA9 | 28 | n.c. |
| 9 | GND | 19 | n.c. | 29 | n.c. |
| 10 | RDQ2 / RDQ10 | 20 | RDQ5 / RDQ13 | 30 | +5 V |

The data-pin pairing is the row structure made physical: SIMMs 1 and 3 drive the low byte
(RDQ0–RDQ7) and SIMMs 2 and 4 the high byte (RDQ8–RDQ15), so a row is one 16-bit-wide
bank [1] pp. 200–201.

The installed size is a *software* fact, discovered at every startup: the boot code tests
RAM by exploiting the family's address mirroring ([compact.md](compact.md) §3.4) — it
writes at successively doubled offsets from address 0 until the write disturbs location
zero, which is the moment the offset first exceeds the physical RAM size, then verifies
the boundary with pattern writes and stores the result in `MemTop` [2] (*observed*; §5,
step 9). Apple describes the outcome: "Each time you switch on the Macintosh Plus …, system
software does a memory test and determines how much RAM is installed in the machine.
Software stores this information in the global variable MemTop" [1] p. 201.

### 3.2 The 6522 VIA as fitted

The Plus carries the family's single 6522 VIA with the family's full duty list
([compact.md](compact.md) §5.2) — keyboard shift register, RTC serial line, mouse
quadrature half, VBL concentration, screen and sound buffer selects, overlay control,
volume, floppy SEL and the SCC /W/REQ monitor — and adds no VIA function of its own. What
is Plus-specific is the concrete base and register arithmetic the ROM uses, and the boot
values it writes. The base is `$EFE1FE`, stored in the global `VIA` at boot, with register
*k* at offset `$200`×k from that base [2] (*observed*) — register 0 (DRB) at `$EFE1FE`,
register 15 (DRA, no-handshake) at `$F003FE` — an arithmetic that reaches beyond the
`$EFE000–$EFFFFF` window named in the family map ([compact.md](compact.md) §3.1) and is
one of the reasons the window's true extent is an open question
([compact.md](compact.md) §7, item 2). Register semantics are [via.md](../../hardware/via.md)
§"Accessing the VIA" and §"Peripheral Ports A and B".

The Reset handler's VIA initialization, in order, as the disassembly shows it [2]
(*observed*):

1. DDRB ← `$87` and DRB ← `$87` — the RTC/keyboard port directions and idle value.
2. One byte exchange with the RTC over port B bits 0–2 to fetch the speaker-volume byte
   (§3.5); the returned volume is masked to three bits and merged into
3. DRA ← `$6B` plus the volume bits — one write that clears vOverlay (bit 4, dropping the
   ROM overlay of [compact.md](compact.md) §2.8), selects the main screen buffer (bit 6)
   and the main sound buffer (bit 3);
4. DDRA ← `$7F`; a read of DRB shifted left four becomes the initial `MBState` global
   (the mouse-button bit lands in `MBState` bit 7);
5. IER ← `$82` — enable CA1 only, the vertical-blanking interrupt;
6. PCR ← `$00`, against Apple's standing warning not to change the PCR [1] p. 176.

Both timers are spoken for by the family ([compact.md](compact.md) §5.2): T1 by the Sound
Driver, T2 by the Disk Driver [1] p. 182. The disassembly adds the Plus ROM's own second
use of T2: the Time Manager. At boot the ROM allocates a 16-byte time-variable block and
installs a VIA timer-2 interrupt handler; a `PrimeTime`-style call writes the delay to
T2C-L/T2C-H, sets IER bit 5, and the handler reloads T2 for the remaining delay, clamping
any single countdown at 84 ms — the 65536-tick ceiling of the 1.2766 µs E-clocked counter
[2] (*observed*: the timer code at `$1762E`–`$176B0` and its boot-time installation; the
"84 ms" clamp is the handler's own limit constant, and 65536 × 1.2766 µs ≈ 83.7 ms is the
derivation, *inferred — unverified* as the annotator's arithmetic, not Apple's).

### 3.3 The SCC and the two serial ports as fitted

The Plus keeps the family's Z8530 and its register windows ([compact.md](compact.md)
§3.2) and the family's mouse-on-SCC wiring ([compact.md](compact.md) §5.3); the machine's
delta is entirely at the connectors and line-interface level. The ROM's serial primitives
are built on the read base `$9FFFF8` and the write base `$BFFFF9`, stored into the
`SCCRd`/`SCCWr` globals at boot [2] (*observed*), and the boot-time SCC initialization
writes a 16-entry register sequence through those bases [2] (*observed*); the chip itself
is [scc.md](../../hardware/scc.md).

The serial connectors are the Plus's new "miniature 8-pin connectors", and the change cuts
both ways: "Those mini 8-pin connectors provide an output handshake signal not available
on the DB-9 connectors for the serial ports of the earlier Macintosh computers; on the
other hand, the mini 8-pin connectors do not provide the +5 volts and +12 volts provided
by the serial ports of the earlier Macintosh computers" [1] p. 359. Signal assignments
(Table 10-1 [1] p. 360):

| Pin | Signal | Description |
|---|---|---|
| 1 | HSKo | Handshake output, driven inverted from the SCC's /DTR; Voh = 3.6 V, Vol = −3.6 V, Rl = 450 Ω |
| 2 | HSKi | Handshake input or external clock, received uninverted at the SCC's /CTS and /TRxC |
| 3 | TxD− | Transmit data, inverted from the SCC's TxD; tri-stated when /RTS is not asserted |
| 4 | GND | Signal ground, connected to logic and chassis ground |
| 5 | RxD− | Receive data, inverted, received at the SCC's RxD |
| 6 | TxD+ | Transmit data, uninverted; tri-stated when /RTS is not asserted |
| 7 | GPi | General-purpose input — **not connected on the Macintosh Plus** [1] p. 360 |
| 8 | RxD+ | Receive data, uninverted, received at the SCC's RxD |

The unconnected pin 7 is a machine fact with a driver-level consequence: "On the Macintosh
Plus and earlier Macintosh computers, the GPi signal is not connected. On those computers,
the DCD inputs to the SCC are used to generate mouse interrupts" [1] p. 362 — the
connector pin that later Macintoshes use for an external DCD input does not exist here,
because the SCC's DCD pins are the mouse's interrupt lines ([compact.md](compact.md)
§5.3). Two further Plus-only facts: "The Macintosh Plus and earlier models do not support
synchronous transmission on either port" [1] p. 361, and the line interface parts are
named: on the Plus the RS-422 drivers are 26LS30 and 9636A ICs and the receivers are
26LS32s, with the HSKo driver a 3488A or 9636A [1] p. 361. Port A (modem) has the higher
SCC-internal interrupt priority, and the Plus's ceiling rates are the classic ones:
57,600 baud maximum for asynchronous interrupt-driven Toolbox-driven transmission and
230.4 kbaud for AppleTalk, both timed by the 3.672 MHz clock [1] p. 363
([compact.md](compact.md) §2.2, §5.5).

### 3.4 The IWM and the floppy port as fitted

The Plus uses the family's IWM ([compact.md](compact.md) §4.2) with the family's softswitch
window; the ROM talks to it at base `$DFE1FF` [2] (*observed*), stored in the `IWM`
global at boot [2] (*observed*), with the register interface described in
[iwm-floppy.md](../../hardware/iwm-floppy.md) §"Macintosh Plus CPU-visible interface".
The drive complement is one internal 800 KB double-sided drive plus one external drive on
a DB-19 connector [1] pp. 337–338, 468 — the supply "provides sufficient current to run
the main logic board, the video display, the internal disk drive, and an external disk
drive" [1] p. 240. On the Plus, the internal and external drives have separate enable
signals, both generated by the IWM: "On the Macintosh Plus, the enable signals for the
internal and external drives are /ENBL.INT and /ENBL.EXT" [1] p. 336.

Internal drive connector (20-pin, Table 9-3 [1] p. 335):

| Pin | Signal | Description | Pin | Signal | Description |
|---|---|---|---|---|---|
| 1 | GND | Ground | 11 | +5 V | +5 volts |
| 2 | PH0 | State-control line | 12 | SEL | State-control line SEL (from the VIA) |
| 3 | GND | Ground | 13 | +12 V | +12 volts |
| 4 | PH1 | State-control line | 14 | /ENBL.INT | Internal drive enable |
| 5 | GND | Ground | 15 | +12 V | +12 volts |
| 6 | PH2 | State-control line | 16 | RD | Read data |
| 7 | GND | Ground | 17 | +12 V | +12 volts |
| 8 | PH3 | Register write strobe | 18 | WR | Write data |
| 9 | −12 V | −12 volts | 19 | +12 V | +12 volts |
| 10 | /WRREQ | Write data request | 20 | PWM | Motor-speed control |

External drive connector (DB-19, Table 9-5 [1] p. 338):

| Pin | Signal | Description | Pin | Signal | Description |
|---|---|---|---|---|---|
| 1 | GND | Ground | 11 | PH0 | State-control line |
| 2 | GND | Ground | 12 | PH1 | State-control line |
| 3 | GND | Ground | 13 | PH2 | State-control line |
| 4 | GND | Ground | 14 | PH3 | Register write strobe |
| 5 | −12 V | −12 volts | 15 | /WRREQ | Write data request |
| 6 | +5 V | +5 volts | 16 | SEL | State-control line SEL |
| 7 | +12 V | +12 volts | 17 | /ENBL.EXT | External drive enable |
| 8 | +12 V | +12 volts | 18 | RD | Read data |
| 9 | n.c. | Not connected | 19 | WR | Write data |
| 10 | PWM | Motor-speed control | | | |

PH2–PH0 come from the IWM and SEL from the VIA; PH3 writes drive registers and /WRREQ is a
programmable IWM handshake output [1] p. 336 ([compact.md](compact.md) §4.2). The PWM pin
carries the family's disk-speed channel ([compact.md](compact.md) §2.7) — useless to the
Plus's own 800 KB drive, whose speed control is internal, but live for an external 400 KB
single-sided drive: "The PWM line carries a speed-control signal used only by 400 KB
single-sided drives" [1] p. 337. The external-drive power budget is Apple's warning, worth
reproducing exactly because it bounds the whole port: "The combined load of all devices
connected to a Macintosh Plus computer must not exceed 600 milliamps at +12 volts, 700
milliamps at +5 volts, and 10 milliamps at −12 volts. A Macintosh single-sided floppy disk
drive requires 500 milliamps at +12 volts and 500 milliamps at +5 volts. An Apple 800 KB
drive requires 600 milliamps at +12 volts and 360 milliamps at +5 volts" [1] p. 335.

The Reset handler initializes the IWM before testing memory: it polls the IWM status
register (via the Q6/Q7 softswitch sequence) until the status no longer reports a drive
motor on, writes `$1F` to the mode register, and leaves Q6 off [2] (*observed*; §5,
step 8). The register meanings are [iwm-floppy.md](../../hardware/iwm-floppy.md)
§"IWM Internal Registers".

### 3.5 The RTC and parameter RAM as fitted

The Plus carries the family's custom RTC on the VIA's three-wire serial interface
([compact.md](compact.md) §4.2), with the 256-byte parameter RAM of the 512K enhanced —
the machine's own spec sheet: "CMOS custom chip containing 256 bytes of parameter RAM;
rechargeable 4.5-volt (Eveready No. 523 or equivalent) user-replaceable battery backup"
[1] p. 468. The chip, command encoding and register file are
[rtc.md](../../hardware/rtc.md) §"Communication Protocol" and §"RTC Registers"; nothing in
them is Plus-specific. What is Plus-specific in the evidence is the boot code's first use
of the interface: the Reset handler's volume fetch (§3.2, step 2) is a byte exchange over
port B bits 0–2 executed before the RAM test, with interrupts masked to level 3 for the
serial bit-banging [2] (*observed*). The one-second interrupt the chip generates is the
family's ([compact.md](compact.md) §5.2).

### 3.6 The NCR 5380 SCSI controller as fitted

The SCSI port is the Plus's headline addition — the first SCSI bus on any compact
Macintosh — and its wiring is the family's one deliberate omission
([compact.md](compact.md) §5.4): "In the Macintosh Plus computer, neither of the NCR 5380
interrupt signals (IRQ and DRQ) is connected to the CPU: there is no hardware handshaking
on the SCSI port. Instead, software must poll the Bus and Status register in the NCR 5380
to detect interrupt requests. Approximate maximum SCSI transfer rates within a block are
170 KB per second for polled transfers and 263 KB per second for blind transfers" [1]
p. 394. Two further facts bound the machine: the general logic raises no timeout — "This
timeout does not occur in the Macintosh Plus, whose SCSI operations never wait for DRQ"
[1] p. 393 — and the 68020/68030 longword trick that speeds later machines does not apply
to a 68000 [1] p. 394. Every later Macintosh corrects this wiring ([compact.md](compact.md)
§5.4); on the Plus, SCSI traffic never interrupts the CPU at all.

The register decode is the family's SCSI window ([compact.md](compact.md) §3.2): the eight
5380 registers selected by A6–A4 at A9=0, the pseudo-DMA data aperture at A9=1, even
addresses reading and odd writing on the lower byte. The ROM's SCSI primitives fix the
concrete addresses [2] (*observed*): read base `$580000`, write base `$580001` — loaded
into registers as the working pair at the top of every primitive — and the data aperture
at base + `$200`, i.e. `$580200`/`$580201`. The chip's register file and bus behavior are
[ncr-5380.md](../../hardware/scsi/ncr-5380.md) §2–§4; the pseudo-DMA mode and the
polled/blind transfer styles are the Guide's SCSI chapter's [1] p. 392, and the same
register and pseudo-DMA offsets are confirmed as the Plus's by Apple's card-design book,
whose test card's SCSI chip "is identical to that used in the Macintosh Plus" with
"address offsets of the registers and pseudo-DMA … the same as on a Macintosh SE or
Macintosh Plus" [3] pp. 231, 235.

The external connector is a DB-25 on the back panel [1] p. 377 — Apple's non-standard
substitution for the ANSI 50-pin connector, with all defined signals present but the
spare grounds removed [1] p. 378. Signal assignments (Table 11-1 [1] p. 380):

| Pin | Signal | Description | Pin | Signal | Description |
|---|---|---|---|---|---|
| 1 | /REQ | REQ/ACK handshake request | 14 | GND | Ground |
| 2 | /MSG | Message phase | 15 | /C/D | Control/data |
| 3 | /I/O | Data direction | 16 | GND | Ground |
| 4 | /RST | SCSI bus reset | 17 | /ATN | Attention condition |
| 5 | /ACK | REQ/ACK acknowledge | 18 | GND | Ground |
| 6 | /BSY | Bus busy | 19 | /SEL | Select target/initiator |
| 7 | GND | Ground | 20 | /DBP | Data parity |
| 8 | /DB0 | Data bit 0 | 21 | /DB1 | Data bit 1 |
| 9 | GND | Ground | 22 | /DB2 | Data bit 2 |
| 10 | /DB3 | Data bit 3 | 23 | /DB4 | Data bit 4 |
| 11 | /DB5 | Data bit 5 | 24 | GND | Ground |
| 12 | /DB6 | Data bit 6 | 25 | TPWR | Terminator power — **not connected on the Macintosh Plus** |
| 13 | /DB7 | Data bit 7 | | | |

Two Plus-only negations are load-bearing for anyone building or attaching devices: "the
Macintosh Plus and earlier models" are the exception to the internal-connector rule —
the Plus has an external port only, no internal SCSI connector [1] p. 377 — and "Because
of power supply limitations, the Macintosh Plus and the Macintosh Portable do not provide
power for termination resistors at the SCSI connector. If you want your SCSI device to be
usable with those machines, the device must provide power for the termination resistors"
[1] p. 378. The bus discipline is standard SCSI: the Macintosh itself is always device ID
7 [1] p. 377, and Apple warns never to connect more than two sets of termination
resistors [1] p. 378 and never to attach an RS-232 device to the DB-25, whose TTL levels
the ±25 V of RS-232 would destroy [1] p. 379.

### 3.7 Sound and video as fitted

Sound and video on the Plus are the family systems with no machine-specific deltas: the
PWM sound channel ([compact.md](compact.md) §2.7) and the shared-RAM frame buffer
([compact.md](compact.md) §2.6), at the family's rates — 22.2545 kHz sample scan
([compact.md](compact.md) §2.7), 512×342 at 60.15 Hz ([compact.md](compact.md) §2.6). The
spec sheet's phrasing — "Four-voice mono sound with digital-to-analog conversion using
22.255 KHz sample rate" [1] p. 468 — is the software mixer over the one hardware channel;
the circuit diagram for the Plus's own PWM path is the Guide's Figure 13-3 [1] p. 434.

What is machine-specific is where the buffers land, because the Plus is the family's
machine with variable RAM. The Reset handler's early screen and beep operations use the
hard-coded address `$3FA700` — the screen-buffer address of a fully loaded 4 MB machine —
and the boot beep fills the sound buffer at the hard-coded `$3FFD00` [2] (*observed*);
both addresses reach the physical top-of-RAM image through the family's address mirroring
([compact.md](compact.md) §3.4) on any smaller machine. After the RAM test the handler
computes the real placement from the measured `MemTop` [2] (*observed*): `SoundBase` =
MemTop − `$300`, `PWMBuf1`/`PWMBuf2` = MemTop − `$2FF` (the odd, motor-speed byte of the
sound-buffer words), and `BufPtr` = MemTop − `$5900`, which becomes `ScrnBase`. On the
shipping 1 MB configuration those are `$FFD00`, `$FFD01` and `$FA700`; the erase routine
that runs before the beep writes 21,888 bytes of `$FF` — exactly one screen [2]
(*observed*). The beep itself is the family's worked example ([compact.md](compact.md)
§2.7) with the Plus's numbers: the handler waits for the vertical-blanking flag, fills the
even bytes of the sound buffer with a waveform built from the repeating longword
`$C006FA40`, gates the sound-enable bit around the burst, and runs for 41 ticks
(`$28`+1) — about 0.68 s at the family's 60.15 Hz tick [2] (*observed*).

### 3.8 The keyboard and mouse as fitted

The Plus keyboard is the family's non-ADB keyboard protocol
([compact.md](compact.md) §4.3, [keyboard.md](../../hardware/keyboard.md) §2–§6) with a
larger case: "The keyboard for the Macintosh Plus computer has a built-in numeric keypad
and arrow keys not included on the keyboards of earlier Macintosh models … In all other
respects, the operation of the Macintosh Plus keyboard is identical to that of the earlier
Macintosh keyboards" [1] p. 280. The keyboard contains its own scanning microprocessor
with private ROM and RAM [1] p. 280. Connector (four-wire RJ-11, Table 7-3 [1] p. 281):

| Pin | Signal | Description |
|---|---|---|
| 1 | GND | Ground |
| 2 | CLOCK | Keyboard clock (input to the VIA) |
| 3 | DATA | Keyboard data (serial, bidirectional) |
| 4 | +5 V | +5 volts |

The wire protocol is [keyboard.md](../../hardware/keyboard.md) §3–§5: keyboard-driven
clock, 330 µs cycles toward the computer and 400 µs cycles toward the keyboard, the
computer the only initiator, Model Number (`$16`) as the first command, Inquiry (`$10`)
every 0.25 s, Instant (`$14`), Test (`$36`), Null (`$7B`) [1] pp. 282–283. The keypad
compatibility sequences are the machine's own piece of protocol: arrow keys answer with
the keypad response `$79` followed by the code, and the keypad's `+`, `*` and `/` answer
with a Shift key-down (`$71`), then `$79`, then the code — reproducing the separate-keypad
protocol of the 128K/512K/512Ke [1] pp. 283–284; the driver-side translation, including
the `+$40` keypad offset, is [keyboard.md](../../hardware/keyboard.md) §7. At boot the
Reset handler clears the keyboard-type globals, installs the VIA shift-register interrupt
handler, and sends the Model Number command [2] (*observed*); which model value the Plus
keyboard answers with is not established (§6.7). The connector warning is Apple's:
"All devices connected to the Macintosh Plus must not draw a combined current of more
than 200 milliamps at +5 volts from all connectors" [1] p. 281.

The mouse is the family's ([compact.md](compact.md) §5.3, §4.3) — quadrature X1/Y1 to the
SCC's DCD inputs, X2/Y2 and the button to VIA port B, the button read as bit 3 of DRB
during each vertical-blank interrupt [1] pp. 275–277 — and the machine's own numbers are
the connector pinout (Table 7-2 [1] p. 278):

| Pin | Signal | Description |
|---|---|---|
| 1 | GND | Ground |
| 2 | +5 V | +5 volts |
| 3 | GND | Ground |
| 4 | MSE.X2 | Mouse X2 (quadrature) |
| 5 | MSE.X1 | Mouse X1 (interrupt) |
| 6 | n.c. | Not connected |
| 7 | MSE.SW | Mouse switch (grounds pin 7 when pressed) |
| 8 | MSE.Y2 | Mouse Y2 (quadrature) |
| 9 | MSE.Y1 | Mouse Y1 (interrupt) |

and the mechanism rate: the Plus mouse generates 90 pulses per inch on each axis [1]
p. 468. The motion-decoding software state machine and low-memory globals are
[mouse.md](../../hardware/mouse.md) §"Interrupt Handler Operation" and §"Low-Memory
Global Variables".

---

## 4. Expansion

### 4.1 Memory expansion rules

The Plus's four legal SIMM populations and their RAM SIZE resistor settings (Figure 5-2
[1] p. 202):

| System memory | Row 1 (SIMMs 1 & 2) | Row 2 (SIMMs 3 & 4) | R8 "256K BIT" | R9 "ONE ROW" |
|---|---|---|---|---|
| 1 MB | 256 KB | 256 KB | 150 Ω, installed | not installed |
| 2 MB | 1 MB | empty | not installed | 150 Ω, installed |
| 2.5 MB | 1 MB | 256 KB | not installed | not installed |
| 4 MB | 1 MB | 1 MB | not installed | not installed |

The rules Apple attaches to those rows [1] pp. 198–199:

- The RAM ICs in each SIMM must have 150 ns RAS access time or faster, and all ICs in a
  row must have the same access time.
- Each SIMM must be filled with eight RAM ICs; a nine-IC SIMM works but the ninth IC is
  not connected.
- All the RAM ICs in a row must be the same size.
- A row cannot contain a single SIMM: a row is either empty or holds two.
- If the rows differ, the row with the larger ICs must be **row 1** on the Plus (the
  opposite of the jumpered late SE).
- The resistors: two SIMMs ⇒ install R9; four ⇒ remove it; all-256 Kbit SIMMs ⇒
  install R8; any 1 Mbit SIMMs ⇒ remove R8 [1] p. 199.

Because the row-to-address-range mapping depends on the resistors and the IC sizes
([compact.md](compact.md) §3.4 gives the mirroring), the 2.5 MB mixed population is the
one configuration whose decode split is not spelled out anywhere (§6.10). The DRAM
interface electrical details — /RAS, CAS1–CAS4, the ten RA lines, RDQ0–15 — are §3.1's.

### 4.2 The SCSI chain

The SCSI port is the machine's expansion bus for everything faster than a floppy: up to
seven peripheral devices on the external chain [1] p. 377, the computer itself fixed at
device ID 7 [1] p. 377, one external DB-25 connector and no internal one [1] p. 377.
Termination is the installer's duty on this machine: at least one set of termination
resistors must be present, chains longer than about three feet may need two, never more
than two, and the Plus supplies **no terminator power** on the connector, so every device
intended for Plus use must power its own terminators [1] pp. 377–378. Transfer rates are
the polled ones of §3.6 — approximately 170 KB/s polled, 263 KB/s blind within a block
[1] p. 394. A device needing the standard 50-pin connector attaches through an Apple
adapter cable [1] p. 378.

### 4.3 Storage on the floppy port

Besides the SCSI chain, the machine's spec names one other storage path: "External DB-19
connector accepts an Apple Hard Disk 20; external SCSI connector accepts a SCSI hard
disk" [1] p. 468 — the Hard Disk 20 speaks a direct-connect protocol over the floppy
port, and the external 800 KB floppy drive is the same port's other occupant [1] pp. 337,
468. The port's combined-load ceiling is the §3.4 warning: 600 mA at +12 V, 700 mA at
+5 V, 10 mA at −12 V across all connected devices [1] p. 335.

### 4.4 What this machine does not expand by

The Plus has no expansion slot of any kind — the compact row of Apple's feature matrix
reads "(none)" for expansion method ([compact.md](compact.md) §1.2, [1] Table 1-1 p. 3) —
no processor-direct slot, no internal SCSI connector [1] p. 377, no second internal floppy
bay, and no ADB ([compact.md](compact.md) §4.4). The power supply is sized for the shipped
configuration: 85–135 V rms or 170–270 V rms at 47–63 Hz single-phase, configured by
"closing switch SW1 on the analog board for 120 V operation" [1] p. 252; input surge
150/300 V rms for 100 ms, 16 ms line-dropout immunity, 70/140 V rms starting voltage,
80 W maximum input under peak load, 65% minimum efficiency [1] Table 6-7 p. 253. The DC
side delivers four rails — +5 V at 4.850–5.150 V, +12 V at 11.90–12.80 V, −5 V at
−5.250 to −4.750 V, −12 V at −13.00 to −11.30 V [1] Table 6-12 p. 258 — with the −5 V
rail not from the supply at all but "a three-terminal regulator IC on the main logic
board [converting] from −12 V to −5 V for use by the serial port drivers" [1] p. 252.
Continuous load limit 46.8 W (4.5 A at +5 V, 1.5 A at +12 V), 55.2 W peak for 15 s at 10%
duty [1] Table 6-13 p. 258; ripple and switching noise peaks are Table 6-14's [1] p. 258.
The keyboard and mouse share the same supply and the 200 mA ceiling of §3.8.

---

## 5. Boot sequence summary

The power-on contract up to and including the overlay is the family's
([compact.md](compact.md) §2.8): the Sony sound IC holds `/RESET` for 0.25 s after the
voltages stabilize, the CPU takes the Reset vector from the overlay map with ROM at
`$000000`, and one of the first Reset-handler instructions drops the overlay through VIA
Data register A. From there the Plus's own ROM (version `$75`, checksum `$4D1F8172`) runs
the following path, every step *observed* in the disassembly [2] unless marked
otherwise:

1. **Vector and entry.** The header's boot-vector longword (`$0040002A`, image offset $4)
   lands the CPU at the entry stub (offset $2A), which branches around a vestigial
   memory-check loop that the annotation marks "never called by the Mac Plus" [2].
2. **Test-software hook 1** (offset $42): read a magic longword and a jump target from
   `$F80000`; if the longword is `$55AAAA55`, jump. No device is documented at that
   address ([compact.md](compact.md) §7, item 3).
3. **Phase check** (offsets $58–$85): with SR at `$2700`, read three longwords from the
   phase-read window (`$F00000`), test their low bits, and if the sum is wrong perform the
   word-wide SCC access that shifts the CPU/SCC clock phase by 128 ns
   ([compact.md](compact.md) §3.3).
4. **VIA initialization and overlay drop** (offsets $88–$E6): the §3.2 sequence — DDRB
   and DRB to `$87`, the RTC volume exchange, the Data-register-A write that merges `$6B`
   with the volume bits and clears the overlay bit, DDRA to `$7F`, the `MBState` latch,
   IER `$82`, PCR `$00`.
5. **Screen erase** ($E6): 21,888 bytes of `$FF` at the hard-coded `$3FA700` — one full
   screen, white.
6. **Boot beep** ($F0): the §3.7 routine — VBL wait, the `$C006FA40`-derived waveform into
   the even bytes of `$3FFD00`, sound-enable around the burst, 41 ticks.
7. **IWM initialization** ($FC–$126): poll the status register until no drive motor is on,
   write `$1F` to the mode register, Q6 off.
8. **ROM checksum** ($D76): sum all 128 KB from `$400000`, XOR the first longword (the
   header checksum) into the accumulated high and low halves, and require the halves to
   agree; failure runs the fatal-error path (below).
9. **Memory test and sizing** ($D9E–$E36): the §3.1 mirroring probe — write at doubled
   offsets from address 0 until location zero is disturbed, then pattern tests
   (`$FFFF`/`$0000` alternations and incrementing longwords) across the boundary —
   returning the RAM top as `MemTop` [2] (*observed*); Apple's one-line description is
   the memory test at startup [1] p. 201.
10. **Boot part 2** ($352): fill all of RAM from address 0 to `MemTop` with `$FFFFFFFF`;
    set `MemTop`, `PWMBuf1`/`PWMBuf2` = MemTop − `$2FF`, `SoundBase` = MemTop − `$300`,
    `BufPtr` = MemTop − `$5900`; `PWMValue` = −1.
11. **Test-software hook 2** ($368): the `$F80080` variant of step 2; on the non-match path
    the code proceeds to the soft-reset entry.
12. **System bring-up** ($3A8–$616): SR `$2700`; `ROM85` ← `$7F`; `ROMBase` ← `$400000`;
    the boot stack at half of RAM (`MemTop`/2 − `$400`); `OneOne` ← `$00010001`,
    `MinusOne`, `DSAlertTab` ← 0; `HWCfgFlags` ← 0, then the probe — read `$420000`,
    compare with `$440000`, and on the result write `$C0` (bits 6–7, SCSI present) —
    see §6.1; `VIA` ← `$EFE1FE`, `IWM` ← `$DFE1FF`; the level-1, level-2 and spurious
    auto-vector handlers (VIA, SCC, spurious); the mouse interrupt dispatch table; the
    A-line/Tool dispatcher tables; the CPU probe (a `MOVEC` attempt distinguishing a
    68020, a frame-format check for a 68010 — a stock Plus stores CPUFlag = 0);
    `_InitUtil`; key-repeat and caret timing from parameter RAM; `_InitResources`;
    `TimeVars` allocation (§3.2); **SCC initialization** (base equates `$9FFFF8`/
    `$BFFFF9` and the register sequence); **keyboard initialization** (clear the keyboard
    globals, install the VIA keyboard interrupt handler, send the Model Number command);
    the drive queue; the I/O manager, which opens the `.Sony` and `.Sound` drivers and
    installs the serial driver from its ROM resource.
13. **Test-software hook 3** ($628): the `$F80088` variant.
14. **Interrupts on** ($63E): SR ← `$2000` — the first unmasked moment of the boot.
15. **Graphics bring-up** ($6A4): `_InitGraf`, `_OpenPort`, `_SetCursor`, `_PaintRect`,
    the disk icon; wait for the drive queue.
16. **Boot-device scan** ($6E2–$78A): the question-mark-disk loop — for each drive in the
    boot mask, `_Read` two sectors from position 0 and test the boot-block signature
    `$4C4B`; on a match, `_HideCursor`, the Happy Mac icon, and branch to boot from the
    disk. Errors walk the drive out of the boot mask (`offLinErr`, `noDriveErr`, eject by
    `_Control` function 7); holding the mouse button at this point forces the eject path
    ($72E: `MBState` button bit → eject). **SCSI boot probe** ($77C → `$7D40`): if
    `HWCfgFlags` bits 6–7 are set, for each SCSI ID from 6 down to 0 not already driven,
    `_SCSIDispatch` a read of block 0, test the Apple Partition Map signature `$4552`
    ("ER"), walk the partition entries for a valid driver partition, allocate it, install
    the SCSI driver and add the drive to the drive queue. The scan repeats until a volume
    boots.
17. **Fatal-error path** ($136): on checksum or memory-test failure the handler saves all
    registers and the PC to `$3FFC80` — the top-of-RAM mirror location, physical `$FFC80`
    on a 1 MB machine — erases the screen at `$3FA700`, draws the error glyph set, and
    loops displaying the failure codes [2] (*observed*).

The two test-software hooks and the phase-read values remain open questions at the family
level ([compact.md](compact.md) §7, items 2–4); the Plus-specific unknowns they leave are in
§6.

---

## 6. Open questions

1. **The SCSI-present probe.** The Reset handler sets `HWCfgFlags` to `$C0` (bits 6–7,
   SCSI present) based on comparing the longwords at `$420000` and `$440000` [2]
   (*observed*). Both addresses are undecoded on this board — `$420000` is inside the
   ROM select window but disabled at A17=1 with a 128 KB ROM, and `$440000` is outside
   it ([compact.md](compact.md) §2.5, §3.4) — and a two-byte gap in the listing at $3F0
   (the conditional branch) hides the polarity. What a real Plus returns at those two
   addresses, why they differ (or match), and how the probe discriminates a Plus from a
   512K enhanced — which Apple says carries the same ROM but has no SCSI [1] p. 4 — is
   not established by anything in the evidence set.
2. **Gestalt and SysEnvirons reporting.** The machine-type value 4 comes from the
   published constants enumeration [4]; the `$75` ROM implements no `_Gestalt` dispatch
   [2] (*observed*). What a Plus answers, under which System version, for
   `gestaltMachineType` and for `SysEnvirons` has not been recorded here, and
   `HardwareEqu.a` — Apple's own authority for `HWCfgFlags`'s bits [1] p. 6 — is not in
   the evidence set.
3. **ROM revisions.** The evidence set holds exactly one Plus ROM image (`$75`,
   `$4D1F8172`). Whether Apple shipped other Plus ROM revisions, what the "expandable to
   256 KB" ROM statement of the specification appendix [1] p. 468 refers to, and whether
   any Plus build populated the upper ROM half are all open; the earlier machines' images
   are equally absent ([compact.md](compact.md) §7, item 13).
4. **The test-software hooks.** All three `$F800xx` probes are *observed* in this ROM
   (steps 2, 11, 13); no device at those addresses is documented on any classic
   Macintosh, and whether the hook can ever fire on a stock machine is
   ([compact.md](compact.md) §7, item 3)'s open question, inherited here unchanged.
5. **The boot-beep busy loop.** Between the VBL wait and the waveform fill the beep
   routine executes a `DBF` loop of `$D00`+1 iterations whose purpose the disassembly's
   annotator cannot identify [2]. Whether it is a settle delay for the sound IC, a
   volume-ramp artifact, or dead timing is unknown.
6. **The volume byte's source.** The Reset handler's RTC exchange compares the returned
   byte against `$A8` before extracting the volume bits [2] (*observed*). Which
   parameter-RAM address the exchange reads, and what the `$A8` sentinel discriminates,
   is not established here; the command encoding is [rtc.md](../../hardware/rtc.md)
   §"Command Format".
7. **The keyboard's model number.** The Model Number response's bits 1–3 carry the
   keyboard model [1] Table 7-4 p. 283; which value the Plus's own keyboard reports is
   not established — [compact.md](compact.md) §7, item 10's open question, unchanged here,
   with [keyboard.md](../../hardware/keyboard.md) §5 carrying the protocol.
8. **The SCC clock tree.** The 3.672 MHz SCC clock is a PAL output
   ([compact.md](compact.md) §5.5), but which SCC input it feeds and how each port's
   baud generator is clocked on the Plus is not spelled out in the compact-Macintosh
   sections — [compact.md](compact.md) §7, item 14's question, inherited.
9. **The alternate screen buffer.** The boot code sets only `BufPtr`/`ScrnBase`; the rule
   placing the alternate screen buffer is in no source in the evidence set
   ([compact.md](compact.md) §7, item 6, inherited).
10. **The 2.5 MB decode split.** With 1 MB SIMMs in row 1 and 256 KB SIMMs in row 2, the
    general logic must split the address range between rows using the resistor
    information [1] p. 201, but no Apple document states where the split falls or how
    the 2.5 MB map aliases. The resistors encode the configuration; the decode equation
    does not exist in the evidence set ([compact.md](compact.md) §7, item 1).
11. **The 5380's IRQ and DRQ pins.** Confirmed not connected to the CPU [1] p. 394 — but
    whether they go to test points, unconnected pads, or nowhere at all on the Plus board
    is not documented anywhere in the evidence set.
12. **The PRAM battery circuit.** The battery is rechargeable and user-replaceable
    [1] p. 468, but the charging path, current and expected life are not specified in any
    document in the evidence set; the family doc's RTC coverage
    ([compact.md](compact.md) §4.2) carries no charging details either.
13. **The Hard Disk 20 protocol.** The DB-19 connector "accepts an Apple Hard Disk 20"
    [1] p. 468, but the direct-connect protocol the HD 20 speaks over the floppy port is
    not documented in the compact-Macintosh sections of the evidence set; only the
    driver-side structure is known, which does not meet the re-implementation bar for
    that device.
14. **Undecoded-bus determinism.** The no-bus-error contract guarantees "meaningless data
    from an undriven bus" ([compact.md](compact.md) §3.4) [1] p. 124; what a real Plus
    actually returns at the probe addresses of open question 1 — stable, random, or
    last-driven — is a property of the specific board that no document records.

## References

1. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition,
   Addison-Wesley Publishing Company, 1990. Macintosh Plus material used on this page:
   the machine definition p. 4 and the feature matrix Table 1-1 pp. 2–3; exterior views
   pp. 6–8 (back view p. 8); interior and component lists pp. 18–19; the identification
   developer tip (Environ/SysEnvirons, HWCfgFlags, HardwareEqu.a) p. 6; RAM configuration
   in the Macintosh Plus pp. 196–202 (SIMM structure p. 197, CAS1–CAS4 and the ten
   address lines p. 198, installation guidelines and RAM SIZE resistors pp. 198–199, the
   SIMM socket pinout Table 5-4 pp. 200–201, the configuration figure p. 202); power
   chapter pp. 239–258 (classic power-up sequence p. 242, supply loads p. 240, AC input
   p. 252 and Table 6-7 p. 253, output limits and loads Tables 6-12/6-13/6-14 p. 258);
   Macintosh Plus mouse and keyboard chapter pp. 273–286 (mouse mechanism and Table 7-1
   pp. 275–276, mouse connector Table 7-2 p. 278, keyboard description p. 280, keyboard
   connector Table 7-3 p. 281, communication pp. 282–283, commands Table 7-4 p. 283,
   keypad protocol and key codes pp. 283–284); floppy connectors Tables 9-3 and 9-5 and
   the enable-signal text pp. 335–338; serial ports pp. 357–366 (mini-DIN 8 connectors
   and Table 10-1 pp. 359–360, line-interface ICs and rates pp. 361–363, SCC addresses
   p. 364); SCSI chapter pp. 376–395 (topology and device IDs p. 377, termination and
   terminator power p. 378, external connector and Table 11-1 pp. 379–380, transfer
   modes p. 392, handshaking and the Plus developer tip pp. 393–394); PWM sound
   pp. 427–431 and the Plus sound circuit Figure 13-3 p. 434; specification appendix
   pp. 468–469 (spec table p. 468, size and weight Table A-1 p. 469).
2. Macintosh Plus boot ROM, annotated 68000 disassembly (ROM version `$75`, header
   checksum `$4D1F8172`, 128 KB image). Evidence used on this page: the header (checksum,
   boot vector, machine type, version, resources offset) at file offsets $0–$1A; the
   Reset handler entry and test-software hooks $2A–$56, $368, $628; the phase check
   $58–$85; the VIA initialization and overlay drop $88–$E6; the screen erase $1EA; the
   boot beep $28A–$350; the IWM initialization $FC–$126; the ROM checksum and
   memory-test/sizing routine $D76–$E36; the boot part 2 and system bring-up
   $352–$616 (buffer placement $37E–$3A4, the HWCfgFlags probe $3E0–$3F8, vector and
   dispatch setup $4BC–$5AE, SCC base equates $82C–$84E, keyboard initialization
   $2568–$26D6); the boot-device scan and SCSI probe $6E2–$7DA (P_CheckSCSI at $7D40);
   the fatal-error path $136–$1B8; the Time Manager's VIA Timer 2 usage $1762E–$176B0;
   the SCSI primitives and their bases $17294–$17620 (pseudo-DMA aperture at base + $200,
   $173E0–$173E6).
3. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third
   edition, Addison-Wesley Publishing Company, 1992 — the SCSI-NuBus test card: "The
   software model of this card is essentially the same as that of the SCSI chip on the
   main logic board … The address offsets of the registers and pseudo-DMA are the same
   as on a Macintosh SE or Macintosh Plus" p. 231; "The SCSI chip is identical to that
   used in the Macintosh Plus" p. 235.
4. Apple Computer, Inc., MkLinux source release, header `POWERMAC/powermac_gestalt.h`
   (the published Gestalt machine-type constants): `gestaltClassic = 1`,
   `gestaltMac512KE = 3`, `gestaltMacPlus = 4`, `gestaltMacSE = 5`, and the rest of the
   enumeration — the authority for the value 4 used in §1.2.
