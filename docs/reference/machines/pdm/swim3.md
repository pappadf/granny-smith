# SWIM III — the Sony floppy controller

**Contents:**

1. [Overview](#1-overview) — what the chip is, which machines carry it, what generation 3 changed
2. [Register file](#2-register-file) — bus interface, summary map, per-register detail, reset state
3. [Behaviour](#3-behaviour) — DMA handshake, read/write/format engines, stepping, interrupts,
   errors, GCR conversion, raw mode, drive interface, timing
4. [Programming model](#4-programming-model) — the sequences the ROM `.Sony` driver actually performs
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

> **Authority.** The primary source for this chip is Apple's SWIM3 engineering
> reference specification [1]; where this page and [1] disagree, [1] wins. Facts
> marked *observed* are established from the boot ROM's `.Sony` floppy driver [8]
> (a disassembly of the shipping Power Macintosh 6100/7100/8100 ROMs), from the
> officially published drive interface specification [3], or from the developer
> notes [4]–[7]. Facts marked *inferred — unverified* are deductions, not
> observations. The available scan of [1] is an OCR of a confidential Apple
> document; a few of its fields are illegible and are flagged below rather than
> guessed.

## 1. Overview

### 1.1 What the part is

SWIM3 (also written "SWIM III") is Apple's third-generation Sony floppy disk
controller. Apple positions it as "the logical extension to SWIM2", whose design
goal is "to relieve the system processor of most of the babysitting tasks
required to talk to the floppy disk" and, above all, "to allow interrupts to
remain enabled during disk accesses"; it is a small design (Apple quotes "five
to six thousand gates, Verilog based, vendor independent") [1]. The Power
Macintosh developer note lists the customer-visible features: DMA data
transfers, no requirement to disable interrupts during floppy access, GCR and
MFM support on 1.44 MB disks, and compatibility with the manual-inject floppy
drive [4].

Unlike its predecessors, SWIM3 has no CPU-side data path of consequence: it is
assumed to be "connected to the host system through a DMA channel", an
interface "patterned after the industry standard 765" so that machines designed
around NEC µPD765-style floppy DMA could adopt the chip directly [1]. Data
moves only over the DMA channel; the byte-level floppy protocol (address-field
parsing, sector matching, mark bytes, CRCs, GCR 6-bit encoding) moved into the
chip, so the driver programs a transfer and waits for an interrupt.

What did *not* change: the drive-side interface is still the multiplexed
20-pin Sony drive protocol — CA0–CA2, LSTRB, SEL and a single RD line — that
the original 3.5-inch drive specification defines [3], driven here through the
Phase register rather than through IWM softswitches.

### 1.2 Which machines carry it

| Machines | Hosted as | DMA channel | Source |
|---|---|---|---|
| Power Macintosh 6100/7100/8100 (incl. AV models) | standalone chip; 16 registers decoded on $200 centres at $50F16000 (*observed*, the ROM's device decoder table [8]) | AMIC floppy channel | [4] |
| Power Macintosh 7500/8500/9500 | integrated in the Grand Central I/O chip; $10 register stride (*observed*, the driver's alternate offset set [8]) | DBDMA | [5], [6] |
| Apple Network Server 500/700 | integrated in Grand Central, device select $05 | DBDMA | [7] |

Every machine above carries exactly **one internal manual-inject SuperDrive**
and no external floppy connector: "All Power Macintosh models contain one
internal Apple SuperDrive floppy disk drive… Unlike previous Apple floppy disk
drives, the one used in Power Macintosh computers does not automatically pull in
the floppy disk" [4]; the TNT developer notes repeat this for the 7500/8500/9500
[5], [6]. The drive "supports GCR and MFM formats for 1.44 MB disks" [4] and
accepts disks up to 1.44 MB; 400K/800K GCR media read and write through the
same mechanism.

Board wiring — address decode, the DMA channel's own registers, and interrupt
routing into the host (pseudo-VIA2 bit 5 on the PDM [8]; Grand Central on the
PCI machines [5]–[7]) — belongs to the family pages for each machine. This page
covers the chip.

### 1.3 Lineage: what generation 3 changed

The two earlier generations are covered on the SWIM page
([swim.md](../../hardware/swim.md)); the table below states only the deltas.

| Area | SWIM (1988) | SWIM2 | SWIM3 |
|---|---|---|---|
| Cores | IWM + ISM behind a crossbar | one logic set, ISM bus interface kept [2] | one logic set, flat 16-register file [1] |
| IWM compatibility | yes, via the 4-write handshake | gone — mode bit 6 tied to 1 [2] | gone — bit 6 is Format mode [1] |
| CPU data path | PIO through a 2-byte FIFO | PIO through a 2-byte FIFO | DMA per byte, µPD765-style [1] |
| Interrupts | none | none | 5 sources, maskable [1] |
| Head stepping | software pulses every step | software | Step register + GoStep, 80 µs pacing [1] |
| Sector identification | software scans bytes | software | hardware ID parse + FirstSector match [1] |
| GCR 6↔8 conversion | software | software | hardware, both directions [1] |
| Mark/CRC writing | wMark / wCRC FIFO tokens | same [2] | in-band $99 escape commands [1] |
| Parameter RAM | 16 bytes, auto-increment | 4 entries, 2-bit counter [2] | one byte: {late, early} [1] |
| Error bits | 7 | 6 [2] | 4 [1] |
| Data separation | Error Correction Machine | none — drive PLL assumed [2] | none — drive PLL assumed [2] |

Two register-level details matter for compatibility code. First, the ISM's
A3-read/A2-write addressing convention is gone: SWIM3 presents 16 real register
addresses and distinguishes read from write with the bus strobes [1]. Second,
the ISM/SWIM2 Mode register bit 0 (Clear FIFO) and bit 7 (MotorOn) are
repurposed — to EnableInts and GoStep respectively — so ISM-era mode-manipulation
code is not portable [1], [2].

## 2. Register file

### 2.1 Bus interface

CPU side: an 8-bit data bus D0–D7, four address lines A0–A3 selecting one of 16
byte-wide registers, read and write strobes RD/ and WR/, a device select Dev/
(Q3 is OR'ed with Dev/ on-chip), an active-low Reset/, a clock input (C32M; 16
or 32 MHz class, see Setup bit 3 below), the DMA pair DMAreq/ (output) and
DMAack/ (input), and an interrupt request output IntReq/ [1].

Three registers have paired read/write personalities: $6 reads the Mode
register but writes clear mode bits, $7 reads the Handshake register but writes
set mode bits, and $C reads the captured format byte but writes the Gap count
[1]. Everything else is a plain register, with the direction fixed by the
strobe.

The chip decodes only A0–A3; the address stride is board decode. On the PDM the
registers sit on $200 centres (16 × $200 = 8 KB from $50F16000); behind Grand
Central the stride is $10 (*observed* [8]). Access timing: data is valid within
95 ns of Dev/ or RD/ low, Dev/ must stay high at least 70 ns between accesses,
address setup to Dev/ low is 15 ns (hold 0 ns), write data is valid 15 ns after
WR/ asserts and held 200 ns, and DMAreq/ drops within 10 ns of DMAack/ low [1].
There is no SWIM/ISM-style minimum spacing between register accesses — only the
70 ns deselect — and the ROM driver issues back-to-back accesses separated by
single instructions (*observed* [8]).

### 2.2 Register summary

| # | Read | Write | Reset | Function |
|---|---|---|---|---|
| $0 | Data (FIFO) | Data (FIFO) | — | byte port; selected by DMAack regardless of the address bus [1] |
| $1 | Timer | Timer (load) | — | 1 µs countdown, interrupt at zero [1] |
| $2 | Error (read-to-clear) | — | $00 | error flags [1] |
| $3 | Parameter | Parameter | — | write pre-compensation: {late, early} nibbles [1] |
| $4 | Phase | Phase | $00 (*inferred*) | drive lines CA0–CA2 + LSTRB [1] |
| $5 | Setup | Setup | $00 | mode configuration [1] |
| $6 | Mode | Zeroes (clear mode bits) | $00 | mode register, bit-wise clear port [1] |
| $7 | Handshake | Ones (set mode bits) | — | status; mode register, bit-wise set port [1] |
| $8 | Interrupt (read-to-clear) | — | — | pending interrupt flags [1] |
| $9 | Step | Step | — | step-pulse count [1] |
| $A | Current track | (see §6) | $FF | cylinder + head from last address field [1] |
| $B | Current sector | (see §6) | $7F | sector + Last_ID_valid from last address field [1] |
| $C | Format byte | Gap | — | header byte 4 (read); pad-byte count (write) [1] |
| $D | First sector | First sector | $FF | sector number that starts a transfer [1] |
| $E | Sectors to xfer | Sectors to xfer | $00 | transfer length, decremented by hardware [1] |
| $F | Interrupt mask | Interrupt mask | — | per-bit interrupt enables [1] |

$3, $4, $5, $6 (read), $F read back their written values — $4 is the chip's
presence probe, $5 is written-then-verified by the driver, and $F is maintained
with read-modify-write instructions (*observed* [8]). [1] documents the readback
of Mode ($6) explicitly; for the others the readback is established by driver
behaviour, and the reset column follows [1] where it gives one.

### 2.3 Setup register ($5)

Reset value $00 [1].

| Bit | 1 means |
|---|---|
| 0 | invert WRDATA (positive write pulses; 0 = negative pulses) [1] |
| 1 | copy-protection mode — raw capture, two DMA bytes per disk byte (§3.9) [1] |
| 2 | GCR mode (GCR cell times and framing); 0 = MFM [1] |
| 3 | divide the internal clock by two [1] |
| 4 | disable the GCR conversion tables (copy protection) [1] |
| 5 | IBM drive data mode (RZ); 0 = Apple (NRZ) [1] |
| 6 | GCR data writes; 0 = MFM writes [1] |
| 7 | *not documented in ERS v1.2* — the ROM driver uses it as a self-clearing software reset, equivalent to the Reset/ pin (*observed* [8]; see §6) |

Bit 3 pairs with the clock: a 16 MHz-class clock with bit 3 clear, or a 32
MHz-class clock with bit 3 set, produce the standard 2/3/4 µs MFM cells and
2/4/6 µs GCR cells; a 32 MHz clock with bit 3 clear gives 1/1.5/2 µs MFM cells
(the 2.88 MB data rate) [1]. The PDM driver always sets bit 3 and the ERS
tabulates write cells against a 31.334 MHz clock, so the PDM feeds SWIM3 a
≈31.334 MHz clock (*inferred — unverified*) [1], [8].

### 2.4 Mode register ($6 write = Zeroes, $7 write = Ones, $6 read)

The Mode register is written bit-wise: a byte written to $7 sets every mode bit
whose data bit is 1, a byte written to $6 clears every mode bit whose data bit
is 1, and 0 bits are don't-care; $6 reads the accumulated mode back [1]. Reset
clears all mode bits [1].

| Bit | 1 means |
|---|---|
| 0 | enable interrupts (master enable) [1] |
| 1 | Enable1 — assert /ENABLE1 to drive 1 [1] |
| 2 | Enable2 — assert /ENABLE2 to drive 2 [1] |
| 3 | GO — start the read/write/format engine ("was action") [1] |
| 4 | Write mode; 0 = read [1] |
| 5 | Side select: side 1; drives the SEL drive line and drive-address bit 3 [1], [8] |
| 6 | Format mode — whole-track write from index (§3.4) [1] |
| 7 | GoStep — run the Step-register seek (§3.5) [1] |

Bits 1–5 are positional holdovers from the ISM mode register [2]; bits 0, 6 and 7
are new uses (§1.3).

### 2.5 Handshake register ($7 read)

Read-only status [1].

| Bit | Meaning |
|---|---|
| 0 | Mark — the next byte in the FIFO is a mark byte [1] |
| 1 | *illegible in the available scan of [1]* ("bit 1 =0", definition lost); SWIM2 carried "CRC zero" here [2]; the ROM driver treats the bit as *interrupt pending* (*observed* [8]; see §6) |
| 2 | RDDATA — live state of the drive's RD data line [1] |
| 3 | Sense — live state of the Sense input [1] |
| 4 | reads 0; "was motor still on" (the ISM motor-timeout status, dropped since SWIM2) [1]; the driver instead names the bit "a drive is explicitly enabled" and never relies on it (*observed* [8]) |
| 5 | Error — some bit in the Error register is set [1] |
| 6 | Dat2Bytes — FIFO has 2 bytes (read) or 2 free slots (write) [1] |
| 7 | Dat1Byte — FIFO has at least one byte (read) or one free slot (write); **gated with Error in write mode so that after a write error the FIFO appears empty, "so to not cause the software to hang"** [1] |

Drive status is sampled from bit 2 by the Mac OS driver, while Open Firmware's
`swim3` package and independent third-party drivers (the Linux kernel's swim3
driver) sample bit 3; both bits present the drive's single RD line on these
machines, since the drive connector has only one RD pin [4] and both pins
answer sense addressing (*observed* in the driver set; whether the two chip
pins are tied at the board or internally is *unverified*) [8].

### 2.6 Error register ($2, read-to-clear)

Cleared by reset or by a read; **only one error can be set at a time**; it must
be cleared before starting a read or write [1].

| Bit | Meaning |
|---|---|
| 0 | underrun — FIFO empty while writing, or full and not read during a read [1] |
| 2 | overrun — FIFO written while full in write mode, or read while empty in read mode [1] |
| 6 | CRC (MFM) or checksum (GCR) error on an address mark [1] |
| 7 | CRC error on a data field [1] |
| 1, 3, 4, 5 | not used — bit 1 "was mark byte read from data register"; the ISM's correction and transition-width errors were shed by SWIM2 and dropped here [1], [2] |

Any set bit raises Handshake bit 5 [1]. The ERS note "errors on reads only
function in MFM mode and after the mark byte is found" [1] predates the error
handling text, which extends bits 6 and 7 to GCR checksum failures ("or
checksum error in gcr") [1]; the driver treats any nonzero read of the register
as a checksum failure and also re-verifies GCR checksums in software (*observed*
[8]).

### 2.7 Interrupt register ($8, read-to-clear) and interrupt mask ($F)

Both registers share one bit layout; $F sets a one to enable each bit, a zero
to disable it [1]. A source sets its flag in $8 regardless of the mask; IntReq/
asserts when an enabled flag is pending and mode bit 0 is set, and a read of $8
returns and clears all pending flags (*observed* in the driver's "get and clear"
idiom [8]; the equation is the ERS's two statements combined — see §6).

| Bit | Flag | Set when |
|---|---|---|
| 0 | timer_done | the Timer count reaches zero [1] |
| 1 | step_done | stepping completes (§3.5) [1] |
| 2 | ID_read | an address field was just read (registers $A/$B/$C valid) [1] |
| 3 | sectors_done | the last byte of the last sector was read, or the end-data command ended a write, **or the process was terminated by an error** [1] |
| 4 | sense_change | the Sense line just changed — "useful for generating an interrupt when a disk is inserted or when waiting for ready" [1] |

The ROM driver enables exactly one source at a time and never enables bit 4
(*observed* [8]).

### 2.8 Timer ($1)

A 1 µs countdown: a loaded value decrements at a 1 µs rate, "the first count
after the load can occur at any time but all subsequent counts will be 1us
apart" (a load of 2 times out in >1 µs but <2 µs), the rate is independent of
the Setup clock-divide bit, and timer_done fires when the count reaches zero
[1]. The Mac OS driver never touches the register; a later Apple floppy driver
loads N+1 and polls the register until it reads zero, so the running count
reads back live on real silicon, and a write of 0 stops the timer (*observed*
in driver behaviour [8]; neither is stated in [1] — see §6).

### 2.9 Parameter register ($3)

The ISM's parameter RAM is reduced to "two nibbles of data, early time and late
time. The data is stored as {late time, early time}. The nominal, no precomp
value is $7" [1] — i.e. write pre-compensation only; the read-cell window
parameters are gone (data separation is the drive PLL's job [2]). Each count
shifts a written transition by one clock period [1]. The ROM driver writes $95
(late = 9, early = 5) and never reads the register back (*observed* [8]).

### 2.10 Phase register ($4)

Bits 0–3 are the phase lines 0–3, driven straight to the drive as CA0, CA1, CA2
and LSTRB; bits 4–7 are not used [1]. There is no direction nibble (unlike the
ISM Phase register [2]) and no reset value is given; the register reads back
what was written, which the ROM driver exploits as the chip-presence probe
(*observed* [8]).

### 2.11 Step ($9), current track ($A), current sector ($B)

**Step** holds the number of step pulses to issue; the hardware decrements it
after each pulse (§3.5) [1].

**Current track**: bits 6:0 hold the cylinder and bit 7 the head from the last
address field read; resets to $FF [1].

**Current sector**: bits 6:0 hold the sector number from the last address field
— raw as written on the media, so GCR sectors are 0-based and MFM sectors 1-based
(*observed*: the driver subtracts 1 for MFM [8]) — and bit 7 is Last_ID_valid,
set when the address field's CRC (MFM) or checksum (GCR) verified [1]. The bit
is false at startup, cleared when the next address mark starts being read,
cleared during the data-field header, and cleared when GO goes off; resets to
$7F [1].

Both position registers update continuously on every address field read while
GO is set in read mode [1].

### 2.12 Gap/format byte ($C), first sector ($D), sectors to xfer ($E)

**$C** written is the Gap count: the number of "pad"-byte DMA requests issued
*after* the data bytes of each sector in a multiple-sector read, so the DMA
pointer advances past the inter-sector gap and leaves a rewritable track image
in memory [1]. $C read returns the fourth address-field byte — the GCR format
byte or the MFM sector-size byte — captured with the last header [1]. The ERS
gives no reset value.

**$D** written is FirstSector: bits 5:0 are the sector number the transfer
starts at, and "if bit 6 is set to a one (x1xxxxxx) any sector will match and be
transferred" [1]. The register resets to $FF, which the ERS says means "don't
match any sector" — yet $FF also has bit 6 set, so bit 7 evidently overrides as
a no-match flag (*inferred — unverified*; see §6). For best performance software
loads FirstSector only after the current position is known [1].

**$E** written is SectorsToXfer: the number of sectors "desired to be accessed
continuously"; the hardware decrements it after each completed sector until it
reaches zero, then raises sectors_done, and after an error the residual count of
untransferred sectors remains readable in the register [1]. Resets to $00 [1].

### 2.13 Data register ($0)

The FIFO byte port, "selected by DMAack input regardless of values on address
bus" [1]: during DMA cycles the controller acknowledges with DMAack and the
address bus plays no part. On the machines in scope the CPU never performs
programmed I/O to this register — the driver has no data-register vector at all
and moves data exclusively over the DMA channel (*observed* [8]).

### 2.14 Reset state

Reset/ (or the driver's Setup bit 7, §2.3) restores: Mode $00 [1], Setup $00
[1], Error cleared [1], Current track $FF [1], Current sector $7F
(Last_ID_valid clear) [1], FirstSector $FF (match none) [1], SectorsToXfer $00
[1]. No reset values are documented for Timer, Parameter, Gap/Format byte,
Phase, the Interrupt flags or the mask; the ROM driver clears the mask and
reads Interrupt during initialisation (*observed* [8]).

## 3. Behaviour

### 3.1 DMA handshake

Data moves one byte at a time over a request/acknowledge pair: SWIM3 asserts
DMAreq/ when the FIFO can take or deliver a byte, the DMA controller answers
with DMAack/ and strobes the byte onto or off the data bus, and DMAreq/
invalidates within 10 ns of DMAack/ low [1]. The acknowledge cycle accesses the
Data register directly, ignoring the address bus [1]. Because the DMA channel
replaced the CPU bus interface, FIFO under/overrun "will only occur if the DMA
controller fails to service SWIM3 DMA requests" — the pending transaction then
terminates with the appropriate FIFO error [1].

### 3.2 Read engine

With GO set in read mode, the chip searches for mark bytes "following the bytes
of zeros" [1]. When a mark is found, new circuitry decides whether it begins an
ID field; if so, the current head, track and sector positions are latched into
the two position registers and Last_ID_valid is set if the address field's CRC
checks (MFM) — in GCR mode the checksum is checked on the address mark — and
each ID read raises ID_read (if enabled) [1]. The registers keep updating on
every ID for as long as GO is true, and "this process will continue forever as
long as the go bit is set" — a search that finds no ID field simply restarts
[1]. The ERS notes software may need to request sector+2 relative to the
observed position "to be sure a complete revolution is not taken" [1].

When a **CRC-valid** ID whose sector matches FirstSector is read, the chip looks
for the data-field mark bytes and starts a DMA read transfer [1]:

- MFM: 512 bytes, starting after the data-field mark bytes [1].
- GCR: 704 bytes, starting at the sector byte — one sector-number byte followed
  by 703 six-bit values (12 tag bytes + 512 data bytes as 699 nibbles, plus the
  4 checksum nibbles; §3.8) (*observed* in the DMA stream [8]; the 704-byte
  length and start point are [1]).

After the data bytes, the chip issues Gap-register-many further DMA requests,
advancing the memory pointer across the inter-sector gap [1]. At the end of the
sector, if SectorsToXfer is nonzero it is decremented and the engine hunts the
next sector; when it reaches zero, sectors_done fires [1].

### 3.3 Write engine and the $99 escape protocol

"A sector write proceeds just like a read except the DMA transfer starts in
the gap between the ID field and the data field" [1]. The outgoing DMA byte
stream is a command protocol: "when SWIM3 sees a $99 in the data flow the next
byte is a command" [1]:

| Code | Command |
|---|---|
| $99 | transfer data $99 (no command) — a literal $99 byte [1] |
| $A1 | write an $A1 mark byte (missing-clock MFM sync mark) [1] |
| $C2 | write a $C2 mark byte (MFM index mark) [1] |
| $04 | write both CRC bytes [1] |
| $0F | turn off escaping for 512 bytes [1] |
| $08 | end data — terminate the DMA transfer [1] |

Escape commanding is active in write and format modes alike; once writing
starts, data is written until a $99 $08 pair is seen [1].

### 3.4 Format mode

Formatting and whole-track write "as required by GCR with the 2.8M drive are
handled by a special case": with the mode bit Format set, "the phase register is
overridden by SWIM3 so that the index mark is being read from the drive", and
the chip begins DMA write requests once the index pulse is seen; writing
continues exactly as a normal write until the $99 $08 terminator, since escape
commanding stays active [1]. The driver nonetheless programs a sense address
that idles high before starting (§4.7), so start-of-write timing is effectively
immediate whenever the overridden line reads high; which physical line the
override addresses is not stated in [1] (*observed* behaviour of the driver,
*inferred* wiring — see §6).

### 3.5 Head positioning

The drive is enabled and its step direction set by software as before; the step
command address is placed on the phase lines and the step count written to the
Step register. When GoStep is set, SWIM3 "will monitor the /step status from
the drive and pulse phase3 to cause a step, waiting 80us between steps as
required, at the same time decrementing the Step Register", repeating until
the count reaches zero, then raising step_done [1]. Post-step settle — head
settling and motor speed-zone changes — "is left to the software" [1]. While
GoStep runs, the Phase register remains loaded with the STEP sense address and
reads back $01; the LSTRB pulses are generated internally (*observed* [8]).

### 3.6 Interrupt behaviour

A source sets its flag in the Interrupt register unconditionally; IntReq/
asserts when a flag is both unmasked and pending, mode bit 0 (enable
interrupts) being the master gate; reading $8 clears every flag and releases the
line; Handshake bit 1 mirrors the pending condition [1], [8]. The one uninterruptible
requirement the chip lifts — the whole point of the generation — is that the
CPU no longer masks interrupts during sector transfers [1], [4].

### 3.7 Error handling

FIFO errors occur only when the DMA controller fails to service requests; the
pending transaction then terminates with the FIFO error set [1]. A CRC error on
a requested data field sets the data-field error bit; **any error terminates a
multiple-sector request** (sectors_done still fires, and the residual
SectorsToXfer remains readable — §2.12), so software checks the error state
when sectors_done arrives [1]. A CRC (or GCR checksum) error during an
address-mark read leaves the position registers un-updated, forces
Last_ID_valid false, and suppresses the start of a transfer even when the
sector number matched FirstSector [1]. Because only one error latches at a
time, simultaneous conditions are lost until the register is read [1].

### 3.8 GCR conversion

"The gcr conversion is built into the hardware both on read or write" [1]. On
write, a DMA byte below $40 is a 6-bit value looked up in the standard 6→8 GCR
codeword table (the same table the drive specification publishes [3]); "to
write any data pattern that is not in the standard conversion table just write
what the pattern should be with the high bit set. Example, D5, AA, 3F." — $D5
and $AA are how the address/data marks D5 AA reach the disk, and $BF writes a
literal $3F [1]. "Otherwise write the non-encoded value. For example the bit
slip bytes, FF,3F,CF,F3,FC,FF, should be written as 3F,BF,1E,34,3C,3F" [1] —
i.e. $3F→$FF, $1E→$CF, $34→$F3, $3C→$FC all through the standard table, and the
interior $3F via $BF, reproducing the canonical sync train of [3] byte for
byte. On read, the chip decodes GCR codewords to their 6-bit values and streams
those on DMA (§3.2); a nibble that was not a valid GCR code is delivered with
bit 7 set, which the driver's checksum loop treats as a bad nibble (*observed*
[8]). Setup bit 4 disables the conversion tables entirely for copy protection
[1].

### 3.9 Copy-protect (raw) mode

With Setup bit 1 set, "all data from the next mark byte will be transfered, not
stopping until the go bit is set false. The transfered data will consist of two
bytes per byte of read data. The first byte is $00 unless it was a mark byte in
which case a $80 is transfered. The second byte is the data" [1]. Capture starts
at the next mark byte and runs until GO is cleared; software doubles its DMA
byte count for the flag bytes, and skips pairs whose flag is neither $00 nor
$80 — flag values outside those two occur on real silicon (a chip defect the
driver works around) (*observed* [8]). In GCR raw mode the conversion tables
are disabled (§3.8) and the undecoded 8-bit groups stream as the data bytes
(*observed* [8]).

### 3.10 Drive interface

The chip talks to the Sony drive over the multiplexed protocol of [3]: six input
signals — CA0, CA1, CA2, LSTRB, /ENBL and SEL — and one output, RD, which
carries either a status bit or read data depending on the address set by
SEL/CA2/CA1/CA0; /ENBL must be low for any communication [3]. On SWIM3, Phase
bits 0–2 drive CA0–CA2, Phase bit 3 drives LSTRB, Mode bit 5 drives SEL (the
Hedsel output), and Mode bits 1/2 drive the /ENABLE lines through Enabl1/ and
Enabl2/ [1], [8]. The drive's RD line returns to the chip's Rddata and Sense
inputs, so Handshake bits 2 and 3 both present it (§2.5).

**Address convention used throughout this page** (matching the drive
specification's table order [3] and the ROM driver's equates [8]):
`addr = SEL<<3 | CA2<<2 | CA1<<1 | CA0`. Other orderings appear in the
literature; state the convention when implementing.

**Sense (read) registers** — set the address, allow a short settle (the ROM
driver waits two host-bus access times, *observed* [8]), then read Handshake
bit 2:

| Addr | Drive signal | RdData = 1 means |
|---|---|---|
| $0 | /DIRTN [3] | current step direction outward |
| $1 | /STEP [3] | step complete — drive sets it ~12 ms after the pulse [3], [8] |
| $2 | /MOTORON [3] | motor off |
| $3 | EJECT [3] | eject latch set (eject button pressed) |
| $4 | RDDATA head 0 [3] | routes head 0's read data to RD |
| $5 | mfmDrv (*SuperDrive extension* [8]) | SuperDrive (MFM-capable drive) |
| $6 | SIDES [3] | double-sided drive |
| $7 | /DRVIN [3] | **no** drive present at this select |
| $8 | /CSTIN [3] | **no** disk in place |
| $9 | /WRTPRT [3] | disk **not** write-protected (reads 0 also when no disk is in [3]) |
| $A | /TKO [3] | head **not** at track 0 |
| $B | /TACH [3] | GCR mode: tachometer, 60 pulses/revolution [3]; MFM mode: index, one pulse per revolution (*observed* [8]) |
| $C | RDDATA head 1 [3] | routes head 1's read data to RD |
| $D | — (*SuperDrive extension* [8]) | drive is in MFM mode |
| $E | — (*SuperDrive extension* [8]) | drive not ready (motor not settled) |
| $F | — (*SuperDrive extension* [8]) | drive-kind bit; with a disk in a SuperDrive: 1 = 1 MB (DD) media, 0 = 2 MB (HD) media |

The act of addressing $4 or $C also **selects the head** for data transfer —
whatever head the lines address at transfer time is the head whose data the chip
decodes (*observed*: the driver reads the sense before every transfer, and
firmware that only sets the lines without reading back still gets the selected
head [8]). During GCR formats the driver instead addresses $1 (head 0) / $F
(head 1), addresses that read back 1 so RD idles high while writing from the
index (*observed* [8]).

**Control (strobe) registers** — CA2 carries the on/off data bit, so each
function has a pair of addresses four apart [3]; to write, set the address then
pulse LSTRB high then low (the pulse is generated by writing Phase bit 3
high-then-low [8]):

| Addr | Latch [3] / driver name [8] | Effect |
|---|---|---|
| $0 | DIRTN = 0 | step inward (toward track 79) |
| $4 | DIRTN = 1 | step outward (toward track 0) |
| $1 | STEP = 0 | step one track; also the address SWIM3 pulses under GoStep |
| $5 | STEP = 1 | step line off |
| $2 | MOTORON = 0 | spindle motor on |
| $6 | MOTORON = 1 | spindle motor off |
| $3 | EJECT = 0 | eject latch reset |
| $7 | EJECT = 1 | begin eject — strobe held 750 ms ± 25 ms; the mechanism finishes in under 1.5 s [3] |
| $8 | disk-in latch | the driver doubts these strobe addresses are real (*observed* comment [8]; see §6) |
| $C | disk-in latch reset | resets the disk-in-place/eject latch — strobed after insert and eject events (*observed* [8]) |
| $9 | drive mode | SuperDrive into MFM mode — 500 kbit/s, fixed 300 rpm (*observed* [8]) |
| $D | drive mode | SuperDrive into GCR mode — variable speed (*observed* [8]) |

The drive loses its GCR/MFM mode latch when the motor is turned off or the
drive deselected, and the driver re-strobes the mode at every power-up
(*observed* [8]). /ENBL high tri-states RD and presets the control latches
inactive [3].

**Drive kind** is identified by sensing $F (revised), $7 (drive present), $6
(double-sided) and $5 (SuperDrive) in that order and combining the four bits;
the internal SuperDrive must answer with drive-present, double-sided and
SuperDrive set, and a nonexistent second drive with $7 reading 1 (*observed*
[8]).

### 3.11 Electrical and timing specification

DC (Vdd 4.75–5.25 V, 0–70 °C): supply current ≤ 50 mA; input low ≤ 0.8 V,
input high ≥ 2.0 V; output high ≥ 2.4 V and low ≤ 0.4 V at 3.2 mA, except
Wrreq/ and Phase1 which sink 10 mA and Enabl1//Enabl2/ which sink 5 mA; the
Rddata and Sense inputs carry internal pull-ups [1].

**Write cell times** (nominal): MFM 2/3/4 µs, GCR 2/4/6 µs; with a 32 MHz-class
clock and the divide bit clear, MFM 1/1.5/2 µs [1]. The ERS tabulates measured
cells for both clock options, within about 1% of nominal (e.g. 2 µs = 1.979 µs
at 15.667 MHz / 2.010 µs at 31.334 MHz; 3 µs = 2.999 µs; 4 µs = 3.989/4.021 µs;
6 µs = 5.999 µs; 1 µs = 0.989 µs; 1.5 µs = 1.499 µs), and notes that
pre-compensation (§2.9) adds to or subtracts from these "in one clock
resolution" [1]. The MFM write pulse is five clock periods wide [1].

**Read cell windows** [1]:

| Nominal cell | Window |
|---|---|
| 1 µs | 0.734–1.245 µs |
| 1.5 µs | 1.277–1.723 µs |
| 2 µs | 1.468–2.489 µs |
| 3 µs | 2.553–3.447 µs |
| 4 µs | 3.510–4.532 µs |
| GCR 2 µs | 0.957–2.999 µs |
| GCR 4 µs | 3.064–4.979 µs |
| GCR 6 µs | 5.042–7.021 µs |

"Gaps between adjacent read cell boundaries represent areas of uncertainty
which may decode as either possible cell" [1].

AC: clock rise to output ≤ 25 ns (Wrdata, Wrreq/, Dat1byte — the FIFO-ready
condition is also brought out as a pin); bus-cycle numbers as in §2.1 [1].

**Mechanical timing** (drive side, from [3]): track-to-track ≤ 12 ms, step
settling ≤ 30 ms, speed-group-to-speed-group ≤ 150 ms, motor start ≤ 400 ms,
eject total < 1.5 s, head auto-homes to track 0 at power-on. GCR spindle speed
is variable by zone (z rpm): tracks 0–15 run 394 rpm with 12 sectors, 16–31 run
429 rpm with 11, 32–47 run 472 rpm with 10, 48–63 run 525 rpm with 9, 64–79 run
590 rpm with 8 [3]; MFM runs fixed 300 rpm with 9 (720K) or 18 (1440K)
sectors/track (*observed* geometry [8]).

## 4. Programming model

The sequences in this section are *observed*: they are what the ROM's `.Sony`
floppy driver performs on the Power Macintosh 6100/7100/8100 [8], and hardware
must satisfy them for Mac OS to boot, mount and eject disks. The driver's own
timeouts (sector header 50 ms with a 300 ms track budget, format track 600 ms,
eject 1.5 s, disk-chuck 500 ms, motor settle 300 ms, ready poll 1 s) pace these
sequences [8]. The chip must be present and probeable for the system to boot at
all, even with no floppy media: an open that fails its presence probe leaves
the driver's globals unset and later system software crashes dereferencing them
(*observed* [8]).

### 4.1 Initialisation and presence detection

At driver open and at every recalibrate [8]:

1. Write Zeroes = GO|Write (force the engine off).
2. Loopback-test Phase: write $05, $06, $07 and read each back exactly — any
   mismatch fails the open.
3. Write Parameter = $95 (nominal pre-compensation); write Setup and verify the
   readback.

At open, additionally: clear the interrupt mask to zero, then write Ones =
EnableInts. So Phase, Setup and the mask must read back written values, and no
interrupt may assert before EnableInts [8]. Setup is reloaded from a
per-format table after seeks and at format-detection time: GCR formats get
GCRWrites|ClockDiv2|GCRMode ($4C), MFM formats get IBMDrive|ClockDiv2 ($28)
(*observed* [8]).

### 4.2 Drive enumeration and media sensing

The driver probes the internal drive by the drive-kind sense sequence (§3.10)
and installs it only if the answer is a double-sided MFM/GCR-capable drive; a
probe of drive 2 must sense "no drive" [8]. With no disk inserted, the correct
steady state is sense $8 reading 1 forever [8]. On insertion ($8 → 0), the
driver strobes the disk-in latch reset ($C), senses media density ($F: 0 = HD,
1 = DD) and write-protect ($9), and posts the disk-inserted event [8].

### 4.3 Address-header read

1. Sense $4 or $C to route the head (§3.10).
2. Read Error (clears it); write Zeroes = GO|Write.
3. Read Interrupt (clears stale flags); set the mask to ID_read; write Ones = GO.
4. Hardware hunts the next address mark — GCR `D5 AA 96`, MFM `A1 A1 A1 FE`
   (*observed* mark sets [8]) — checks the header CRC/checksum, latches Current
   track/sector/Format byte, and raises ID_read.
5. Driver: clear GO, then read Error, Format byte, Current track, Current
   sector.

If no header arrives the driver times out after 300 ms — an absent or
wrong-format disk simply never raises the interrupt [8]. The Format byte read
must match the expected per-format value: $02 for MFM 512-byte sectors, $22 for
800K GCR, and so on [8].

### 4.4 Sector read

Per sector (the driver loops in interleave order, one sector per GO) [8]:

1. Write Gap = 0; write FirstSector = the target sector (raw, +1 for MFM); set
   up the DMA channel: memory address = the sector's buffer slot, count = a
   large value, direction = read, run.
2. Write SectorsToXfer = 1; read Error; write Zeroes = GO|Write.
3. Set the mask to sectors_done; write Ones = GO.
4. Hardware finds the matching CRC-valid header, streams the data field to DMA
   (§3.2), decrements SectorsToXfer and raises sectors_done.
5. Driver: stop the DMA channel; clear GO; check Error (nonzero ⇒ checksum
   error); verify Current track bits 6:0 equals the expected cylinder.

The DMA stream per sector is exactly §3.2: 512 data bytes for MFM; for GCR one
sector byte plus 703 six-bit values, each in a byte with the top two bits clear
(bit 7 set marks an invalid code, §3.8) [8]. The leftover DMA count at
sectors_done is normal and ignored — the hardware stops requesting at the end
of the sector [8].

### 4.5 Sector write

Like a read, but: set up DMA with direction = write; write SectorsToXfer = 1;
read Error; **clear GO, then set Write while GO is clear**; set the mask to
sectors_done; set GO [8]. Hardware finds the matching header and streams from
DMA starting in the ID-to-data gap, honouring the escape protocol, until the
$99 $08 terminator; the driver then stops the DMA channel and clears GO and
Write [8].

The MFM per-sector stream the driver builds [8] exercises every escape command:

```
4E x10            gap
00 x12            sync zeros
99 A1 99 A1 99 A1 99 FB    data mark (FB with missing clock)
99 0F             literal mode for the next 512 bytes
<data x512>
99 04             write both CRC bytes
4E x4
99 08             end data
```

The GCR per-sector stream is: the sync train and marks `3F BF 1E 34 3C 3F D5
AA` (which the converter writes as `FF 3F CF F3 FC FF D5 AA`), the table-encoded
sector number, 699 data nibbles and the 4 precomputed checksum nibbles, then
`27 AA 3F 3F 3F 3F 99 08` — $27 encodes the $DE of the `DE AA` bit-slip [8], [3].

### 4.6 Seek and recalibrate

1. Strobe the direction register ($0 or $4).
2. Set Phase to the STEP address ($1) **without** strobing.
3. Write the step count to Step; read Error; set the mask to step_done; write
   Ones = GoStep.
4. Hardware pulses LSTRB Step times, 80 µs apart (§3.5), and raises step_done.
5. Driver: clear GoStep, then poll sense $E until the drive reports ready, and
   reload Setup.

Recalibrate = a seek of −80 tracks followed by a track-0 check on sense $A [8].

### 4.7 Format

The driver builds a whole track image in the DMA buffer, then [8]:

1. Sense the head-route address that idles high ($1 for head 0, $F for head 1 —
   §3.10).
2. Set up DMA with direction = write; read Error; clear GO; set Format mode.
3. Set the mask to sectors_done; set GO.
4. Hardware waits for the index pulse and writes the whole stream (escape
   protocol active) until its $99 $08 terminator; sectors_done fires.
5. Driver: stop DMA; clear GO **first, then** clear Format mode — the ordering
   is a hardware requirement.

The MFM 1.44 MB track image is: 80 ×4E, 12 ×00, index mark `99 C2 99 C2 99 C2
99 FC`, 50 ×4E, then per sector — 12 ×00, address mark `99 A1 ×3, 99 FE`, the
C/H/S/N bytes, `99 04` (CRC), 22 ×4E, 12 ×00, the data mark, `99 0F`, 512 data
bytes, `99 04`, a 101 ×4E inter-sector gap — and finally a closing gap and `99
08` [8]. The GCR image interleaves per-sector sync groups, address marks
(`D5 AA` + $00, which encodes $96), the bit-slip `27 AA`, data marks (`D5 AA` +
$0B, which encodes $AD) and the nibble fields [8]. GCR format self-tunes its
inter-sector sync count by measuring rotational wrap against the host clock, so
format timing must be roughly rotational-rate accurate or the driver's tuning
loop fails (*observed* [8]).

### 4.8 Format detection

On first access to newly inserted media, the driver walks the formats in the
order MFM 1440K, MFM 720K, GCR 800K, GCR 400K, GCR-on-HD: recalibrate, then
perform an address-header read (§4.3) and check the Format byte against the
per-format expected value [8]. Wrong-mode attempts must fail cleanly — either
no ID_read within the timeout or a CRC error. One pitfall is real hardware
behaviour the driver depends on: reading MFM media in GCR mode produced *no
error at all* on real silicon, so the driver tries MFM first and treats "no
ID_read" as the discriminator; a GCR-mode read of MFM media must not produce a
valid-looking GCR header [8].

### 4.9 Insertion, ejection and idle behaviour

- **Idle, no disk**: a polling task selects the drive and senses $8 every
  500 ms; returning 1 forever is correct. No interrupts occur at idle, and
  the poll runs with host interrupts fully masked — sense reads must work without
  interrupt help [8].
- **Insertion**: $8 → 0; latch reset strobe, density and write-protect senses,
  inserted event; format detection on first access (§4.8) [8].
- **Eject** (menu or drag-out): seek to track 40 (mechanical stress), motor-off
  strobe, 200 ms, eject strobe ($7) held, 1.5 s wait, then $8 must read 1 [8],
  [3].
- **Motor timeout**: the driver turns the drive off 2.0–2.5 s after last use;
  because the drive forgets its GCR/MFM mode at power-off, the mode strobe is
  repeated at every motor-on [8].

## 5. Quirks & errata

- **Dat1Byte lies after a write error on purpose**: Handshake bit 7 is gated
  with the error flag in write mode so the FIFO "will appear empty so to not
  cause the software to hang" [1] — a deliberate hang-prevention inherited from
  SWIM2 [2].
- **One error at a time**: only the first error latches until the register is
  read; read-to-clear before every operation, and check it when sectors_done
  arrives even though the interrupt fired [1], [8].
- **FirstSector $FF vs the wildcard bit**: bit 6 alone means match-any, yet the
  $FF reset value means match-none — bit 7 evidently overrides (§2.12; the
  interaction is *inferred*, not documented) [1].
- **SectorsToXfer keeps its residue**: after an error the count of untransferred
  sectors stays readable in $E [1].
- **Wrong-mode reads can be silent**: a GCR-mode read of MFM media gives no
  error; drivers must discriminate by absence of ID_read, not by an error flag
  (§4.8) [8].
- **Raw-mode flag bytes outside {00, 80}**: real silicon emits other values
  (a chip defect); software skips such pairs, and an implementation emitting
  only $00/$80 is compatible [8].
- **Phase readback is the chip detect**: Phase, Setup and the interrupt mask
  must read back written values or the ROM driver's open fails (§4.1) [8].
- **No inter-access wait states**: unlike the SWIM/ISM's 4–8-clock /DEV
  spacing rule, only the 70 ns deselect is required; drivers issue back-to-back
  accesses [1], [8].
- **GO-before-Format ordering**: when ending a format, clear GO first, then
  Format mode; and set Write only while GO is clear (§4.7) [8].
- **Drive status is on Handshake bit 2** for the Mac OS driver, while firmware
  and third-party drivers read bit 3 — both must show the drive's RD line
  (§2.5) [8].
- **Hardware steps pace themselves at 80 µs**; a Phase read during a seek
  returns the held STEP address, $01 (§3.5) [1], [8].
- **Handshake bit 4 disagreement**: ERS v1.2 says it reads 0 ("was motor still
  on"); the driver names it "drive enabled" and never uses it (§2.5) [1], [8].
- **Interrupt bit 5 ($20)**: not defined by ERS v1.2, which documents bits 0–4
  only — yet independent drivers (the Linux kernel's swim3 driver, and a later
  Apple driver's ISR) test it as an error-pending flag (*observed*; possibly
  second-silicon behaviour) [8].
- **Two silicon revisions exist**: the ERS v1.2 change history is explicitly
  "Rev1.2 — Second silicon changes", and drivers carry revision-specific
  behaviour; what the second silicon changed is not documented (§6) [1].

## 6. Open questions

1. **Setup bit 7**: blank in ERS v1.2. The ROM driver uses it as a self-clearing
   soft reset equivalent to Reset/ (*observed* [8]); whether that is its only
   function, and whether reading Setup after writing bit 7 returns the bit
   clear, is unverified.
2. **Handshake bit 1**: the field is illegible in the available scan of the ERS
   ("bit 1 =0"). SWIM2 documented "CRC zero" there [2]; the SWIM3 driver uses
   the bit as interrupt-pending [8]. What real silicon returns, and whether the
   CRC-zero status survives anywhere on SWIM3, is unknown.
3. **Handshake bit 4**: ERS says it reads 0; the driver's equate names it
   drive-enabled. Neither relies on it. Actual silicon behaviour unknown.
4. **Interrupt bit 5**: absent from ERS v1.2 but observed in two independent
   drivers as error-pending. Is it second silicon? Does the ERS revision that
   documented it exist?
5. **The second silicon**: the ERS v1.2 change note is one line. Which
   behaviours differ between revisions (the drivers distinguish them), and
   which revision do the surviving machines carry, is unknown.
6. **Timer details**: read-back of the running count and write-0-to-stop are not
   in the ERS; both are modelled from driver behaviour [8]. Also unstated: the
   register's reset value, and what the "Timer **A**" name implies about a
   Timer B.
7. **Reset values** for Parameter, Gap/Format byte, Phase, Interrupt flags and
   the interrupt mask are not documented in [1]; §2.14 lists only what the driver
   establishes. Whether $A/$B accept writes (they are described as captures) is
   unknown.
8. **Multi-sector continuation**: the ERS says SectorsToXfer counts sectors
   "accessed continuously" but never says whether FirstSector auto-increments
   or how the next sector is matched after the first. Every observed driver
   transfers one sector per GO [8], so multi-sector continuation is unverified.
9. **GCR converter corners**: the encoding of inputs $40–$7F (high bits `01`) is
   undefined; whether the "high bit set" literal flag is physically written to
   the disk or masked (both read back identically) is unresolved.
10. **Format-mode phase override**: the ERS says the phase register is overridden
    "so that the index mark is being read from the drive" [1] without naming the
    line; the driver's own sense-address choice suggests the override is
    cosmetic when the addressed line already idles high (§3.4). The exact
    override behaviour is unverified.
11. **Gap on writes**: the ERS describes Gap only for read pad requests; the
    Linux driver writes a gap3 value for writes [8]. Write-side use of the
    register is unverified.
12. **The 2.88 MB drive**: the ERS carries the 1/1.5/2 µs cell machinery and
    mentions the "2.8M drive" for GCR-format track writes [1], but no machine in
    scope ships one; that path is untested.
13. **Motoen/ and 3.5Sel/** appear in the ERS's output-pin AC specification [1],
    but no v1.2 register bit is documented as driving them (SWIM2's 3.5 output
    bit became copy-protect mode). Their controllers are unknown.
14. **FIFO depth** is not stated in [1]; the Dat1Byte/Dat2Bytes handshake and
    the Dat1byte pin imply a small FIFO, but its depth (SWIM2's was 2 bytes [2])
    is unverified.
15. **Sense-address strobes $8/$C writability**: the driver's own comment doubts
    the disk-in latch can be written at $8 [8]; only $C (latch reset) is
    observed in use.

## References

1. Apple Computer, *SWIM3 ERS* (SWIM 3 Engineering Reference Specification),
   version 1.2, 24 March 1993 (confidential; the revision documents "second
   silicon changes").
2. Apple Computer, *SWIM2 ASIC ERS* (Engineering Requirements Specification),
   revision 3.1.
3. Apple Computer, *Specification for 3.5-Inch Single-Sided Disk Drive*, Apple
   part 699-0285, including Appendix B, "Sector Format" (the base Sony drive
   interface protocol and the GCR sector format; the SuperDrive extends the
   same protocol).
4. Apple Computer, *Power Macintosh Developer Note* (Power Macintosh
   6100/7100/8100), 1994 — SWIM III controller features, the single internal
   manual-inject SuperDrive, and Table 3-7, the 20-pin floppy drive connector.
5. Apple Computer, *Power Macintosh 7500/8500 Developer Note*, 1995 — SWIM III
   in the Grand Central IC.
6. Apple Computer, *Power Macintosh 9500 Developer Note*, 1995 — SWIM III in
   the Grand Central IC.
7. Apple Computer, *Apple Network Server 500/700 Hardware Note*, 1996 — Grand
   Central device select $05 is SWIM3.
8. Power Macintosh 6100/7100/8100 ROM disassembly: the `.Sony` floppy driver
   (ROM versions $9FEB69B3, March 1994, and $9B7A3AAD, January 1995), plus the
   same driver family's Grand Central variant for the register-stride
   observation.
