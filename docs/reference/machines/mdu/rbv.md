# RBV — RAM-Based Video

**Contents:**

1. [Overview](#1-overview) — what the part is, the three function sets, the machine roster, division of
   labor, the V8 variant, clocking
2. [Register file](#2-register-file) — the $50F26000 window, the A0/A1/A4 decode, the eight byte
   registers, and the VIA-spaced aliases the shared OS code uses
3. [Behaviour](#3-behaviour) — interrupt aggregation and IPL levels, the slot-interrupt path, NuBus
   status and RAM blocking, the video fetch engine, monitor sensing, video timing, memory bandwidth,
   cache and power control
4. [Programming model](#4-programming-model) — the virtual VIA2 contract, register access patterns,
   a level-2 handler worked example, interrupt enables, video initialization, cache traps, shutdown
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)
- [References](#references)

---

## 1. Overview

### 1.1 What the part is

The **RBV** ("RAM-Based Video") is an Apple custom integrated circuit that combines three jobs in one
part: a slice of the general-logic (GLUE) function, the entire second Versatile Interface Adapter
(VIA2), and the control logic for built-in video that scans its frame buffer out of main RAM. Apple's
own summary: "The RBV is an Apple custom integrated circuit that performs three different sets of
functions… It performs some functions of the GLUE in the Macintosh II and Macintosh IIx, it contains
the registers and other circuitry implemented in the second VIA in other models in the Macintosh II
family, and it controls the built-in video circuitry" [3] pp. 116–117. The chip is documented under
Apple specification number **343S1019** ("RBV Spec, Apple No. 343S1019" [4] p. 59; "RBV chip spec,
Part number: 343S1019-A" [4] p. 72).

The part is the distinguishing component of the Macintosh IIci's architecture: "a new chip set
provides memory decoding and low-cost video by utilizing existing on-board DRAM for the frame
buffer" [1] p. 3, the other half of that chip set being the MDU (Memory Decode Unit; see
[mdu.md](mdu.md)). "RAM-based" is the point of the name: unlike every earlier Apple video design,
the frame buffer is not separate video RAM but the bottom of ordinary system DRAM, bank A, starting
at physical address $0000 0000 [1] p. 23, [3] p. 412. The IIci's on-board video "supports the
Macintosh II 12" B&W, 13" RGB, and 15" B&W Portrait monitors" [1] p. 45; the IIsi adds the 12-inch
RGB (512 × 384) [2] ch. 4, Table 4-2.

This page documents the RBV of the IIci, the variant the IIsi carries, and the related **V8** part of
the Macintosh LC family, whose VIA-cell register interface follows the same layout (§1.5). Machine
wiring — address maps, RAM banks, expansion slots — is the machine pages' subject, not repeated here:
[iisi.md](iisi.md) §3–§5 for the IIsi, and the IIci page for its maps.

### 1.2 The three function sets

**GLUE-subset functions** [3] p. 116:

- Generates the **15.6672 MHz** clock used by the bus-error and RAM-refresh circuitry.
- Generates the **3.672 MHz** clock used by the microprocessor in the ADB transceiver and by the SCC
  to set communication rates.
- Performs an OR of the slot interrupt signals and delivers the result to a register in the VIA
  portion.
- Monitors its internal interrupt registers plus the interrupt sources of VIA1, the SCC and the
  nonmaskable interrupt switch, "assigns a priority to each interrupt; and asserts the appropriate
  interrupt lines to the main processor. If the RBV IC receives more than one interrupt at the same
  time, it passes only the highest priority interrupt on to the main processor."

**VIA2 functions** [3] p. 116, [2] ch. 5 ("Versatile Interface Adapter (VIA) interface"): there is
no physical second VIA; the RBV's VIA2-emulation registers perform everything VIA2 does elsewhere in
the II family:

- Decodes the interrupts from the expansion slots (three NuBus slots on the IIci at geographic
  addresses $C–$E [5] ch. 6; one PDS/NuBus slot on the IIsi at $9 [2] ch. 6).
- "Decodes NuBus errors and enables or disables NuBus access to main RAM" [3] p. 117 — the same two
  duties the IIsi note lists as "blocking NuBus accesses to RAM; and decoding NuBus transaction
  errors" [2] ch. 5.
- Provides the two SCSI interrupt inputs (IRQ and DRQ) and the ASC interrupt.
- Detects the external-speaker plug (/SNDEXT) — "the Sound Manager can read a bit … to determine
  whether the mono internal speaker or the stereo external sound jack is being used" [3] pp. 156–157.
- Controls the parity-circuit interrupt, and — IIci-specific — "provides a signal used for testing
  the parity circuits" [3] p. 157.
- Controls flushing and disabling of the optional external cache RAM card, and provides its
  enable/flush output signals [3] pp. 116, 157.
- Contains the soft power-off signal [3] p. 117.

**Video functions** [3] pp. 116–117, in Apple's own list: reads the monitor ID lines; generates
horizontal and vertical synchronization with monitor-appropriate timing; requests video data from the
MDU, which generates the screen-buffer addresses; reads the data "in bursts of 8 longwords at a time"
into an internal FIFO; and contains a bit-order arranger plus shift register that "take data from
the FIFO buffer and arrange it into single pixels of 1, 2, 4, or 8 bits each, which it transmits to
the CLUT DAC for conversion into video signals". What the RBV does *not* do is video addressing: "The
RBV does not control the addressing of the video buffer; that function belongs to the MDU" [3]
p. 117. Section 3.4 gives the full fetch protocol.

### 1.3 Machines that carry it

| Machine | Chip | Role of the RBV in that machine | Sources |
|---|---|---|---|
| Macintosh IIci (1989) | RBV (343S1019-A) | VIA2 replacement + interrupt concentrator + on-board video; frame buffer at the bottom of bank A, mapped logically at $FB00 0000 (slot $B) | [1] p. 3, [3] pp. 116–117, 412, [4] pp. 59, 63–64 |
| Macintosh IIsi (1990) | RBV variant (IIsi Developer Note calls the part "RBV" throughout) | same triple role; frame buffer mapped into NuBus super-slot logical space $E; VIA2 duties per [2] ch. 5 | [2] chs. 3–5, [4] pp. 63–64 |
| Macintosh LC (1990) | V8 (343S0116) | related part: a VIA cell inside the V8 plus a VRAM-based video controller and a sound subsystem; V8 "also has several other registers in addition to the VIA registers" | [4] pp. 38, 59, 72–73 |
| Later LC-family and low-end machines | V8-derived parts (Sonora, Everest…) | "an improved sound subsystem … in the 2nd generation V8 chips, such as Sonora and Everest" | [4] p. 47 |

Two further data points bound the roster. The Macintosh Hardware Overview's high-end machine table
lists one more 68030 project ("Buccaneer", then unreleased) as RBV-based with VIA2 = RBV [4] Table
1.4 — which product that became is not established from the printed record (§6.13). And the RBV
register layout outlived the chip: the Power Macintosh PDM platform's AMIC implements its pseudo-VIA2
slot bank "as an RBV-layout register bank at base $50F26000" [9] Figure 2-2 pp. 22–23 — see
[amic.md](../../machines/pdm/amic.md) §2.3.

### 1.4 Division of labor

Most of what surrounds the RBV belongs to other parts. The split, from the IIci block diagram [1]
Figure 1-2 pp. 6–7 and the Guide's component descriptions [3] pp. 116–117, 417–419:

| Function | Owner |
|---|---|
| Screen-buffer addresses, RAM row/column multiplexing, burst reads, refresh, device selects | the MDU — [mdu.md](mdu.md); "the MDU provides the addressing for the video buffer, located in bank A" [3] p. 116 |
| Frame buffer storage | bank A of system DRAM, from physical $0000 0000 [1] p. 23 |
| Disconnecting bank A's data bus from the CPU during a video fetch | F245 bus buffers between the banks [1] pp. 23–24, Figure 3-2 |
| Color lookup table and video DACs | the Brooktree Bt478 CLUT DAC ("two parts of a single integrated circuit" [3] p. 417), mapped at $5002 4000–$5002 5FFF on the IIsi [5] Table 15-10 p. 344 |
| Sync and blanking waveforms, dot clock, pixel serialization | the RBV [3] pp. 116–117, 414–415 |
| VIA1 (ADB, RTC, one-second, 60.15 Hz VBL, machine ID bits) | a real 6523 VIA, unchanged [2] ch. 5, [4] Table 1.4 |
| Sound generation | the ASC; its interrupt reaches the RBV's flag register (bit 4) — [asc.md](../../hardware/asc.md) §6 |
| NuBus arbitration and slot space | NuChip30 [1] Figure 1-2 pp. 6–7 |
| DRAM parity generation/checking | the optional PGC chip [1] p. 3 |

The practical consequence for the register file (§2) is that the RBV's software surface is small:
eight byte-wide registers plus the interrupt steering around them.

### 1.5 The V8 variant and the wider family

The stub of this page called the IIsi/LC parts the "V8 variant"; the printed record draws the line
slightly differently, and this section is the precise version. Apple's published documents name the
**IIsi's** part simply RBV — "the architecture of the Macintosh IIsi is based on the Memory Decode
Unit (MDU) and RAM-Based Video (RBV) chips" [2] ch. 1 — and give it the same VIA2 duties [2] ch. 5;
it is a main-RAM-framebuffer design like the IIci's [2] ch. 3. The video-lineage survey attributes
the codename "Erickson" to the IIsi variant and notes the Apple IIe Card's video hooks are present in
the RBV driver as well ([video-overview.md](../../hardware/video-overview.md) §3.1, §3.3).

The **V8** is the LC's part and is a genuinely different, though register-compatible, design: "The
Macintosh LC and subsequent low-end machines … use a more sophisticated built-in video scheme"; the
VISA chip was planned for the LC "but was shelved in favor of the V8 chip" [4] p. 37. Where RBV scans
main DRAM, "The V8 chip uses VRAM to isolate video buffer refreshes from the CPU" [4] p. 38; where
RBV carries a real VIA2 emulation, V8 contains a *VIA cell* ("VIA Cell inside V8" [4] pp. 63–64,
Table 1.3) that implements both VIA1 and VIA2 functions for the LC, "and the V8 chip in the Mac LC
also has several other registers in addition to the VIA registers" [4] p. 59. V8 also integrates the
LC's sound subsystem — an ASC-subset emulation with a 1 KB FIFO implemented in main RAM and filled
by DMA [4] pp. 43–44 — which on the IIci/IIsi is the separate ASC's job.

The two designs share the monitor-sense scheme and the register-offset contract (§2.2, §3.5): the
m68k operating-system register definitions that define the RBV offsets apply the same set to the
IIci, IIsi and LC classes [7], [8]. V8-specific behavior differences that are in print: it "ignores
monitor ID bit 1" and "interprets monitor ID = 011 as a VGA monitor" [4] Table 6.1 footnotes,
pp. 39–41.

### 1.6 Clocking

The RBV is a clock source as well as a clock consumer. It generates the 15.6672 MHz clock for the
bus-error and RAM-refresh circuits, and the 3.672 MHz clock "used by the microprocessor in the ADB
transceiver and used by the SCC to set communication rates" [3] p. 116. The 783.36 kHz E clock that
paces VIA communication is *not* the RBV's — the MDU generates it [3] p. 116. The video side derives
its dot clocks internally from the selected monitor: 30.2400 MHz for the 12" B&W / 13" RGB class
and 57.2832 MHz for the 15" Portrait [1] Table 5-2 p. 45, 15.6672 MHz for the IIsi's 12" RGB [2]
Table 4-2; "all timings are derived from the dot clock and have the same tolerance" [1] p. 50. All
video output signals change on the rising edge of the dot clock [1] Figure 5-2 note 1, p. 49.

## 2. Register file

### 2.1 Address decode: the window and the A0/A1/A4 wiring

On the IIsi the RBV occupies physical **$5002 6000–$5002 7FFF** [2] ch. 5, Figure 5-1; [5] Table 15-10
p. 344 — the 8 KB I/O sub-window the IIsi shares with the VDAC at $5002 4000–$5002 5FFF and VIA1 at
$5000 0000–$5000 1FFF. No Apple document in print publishes the IIci's I/O sub-decode at this
granularity; the IIci window is taken to be the same $50F26000 base (*inferred — unverified*,
§6.1), corroborated by two independent facts: the PDM platform's AMIC was later documented at the
identical base with the identical layout [9] Figure 2-2 pp. 22–23, and the m68k OS register
definitions drive IIci and IIsi with one offset set against one base [7], [8].

Which address lines reach the register file is established by the IIci hardware block diagram: of
the processor's address bus, only **A0, A1 and A4** are drawn into the "Custom RAM-Based Video IC
(Registers and Interrupts)" block [1] Figure 1-2 pp. 6–7 — where VIA1, by contrast, receives A9–A12
[1] Figure 1-2 pp. 6–7. Three lines give eight register positions; the two groups are the low group
(A4 = 0) and the high group (A4 = 1), making the register file 20 bytes wide, $000–$013. The Designing
Cards and Drivers cache-card equations confirm the decode independently: the IIsi PDS adapter derives
its cache-control strobes from `/RBV • /RW • /A0 • /A1 • /A4` — an RBV cycle with A0 = A1 = A4 = 0 —
and keys them off data bits D0 and D3, i.e. "writing a 0 to bit 0 of the RBV register 0" and "bit 3
of the RBV register 0" [5] pp. 360–361. The adapter card even exposes the chip select directly: pin
A1 of the IIsi expansion connector is "/RBV … an active-low chip-select signal for the RAM-based
video IC … can be used, along with further decoding, to enable signals for a cache circuit" [5]
Table 15-15 p. 359.

The register file sits behind an **8-bit bidirectional data bus that is separate from the 32-bit RAM
data bus used by the video portion** — "Having separate data buses for the two parts of the RBV
makes it possible for the main processor to read and write to the VIA registers while video activity
is taking place on the other data bus" [3] p. 116. Like every other I/O device on these machines the
registers are byte-wide on the processor's upper data byte, D24–D31, like the other 8-bit
peripherals the diagram marks with that pair — VIA1, SCC, SWIM and SCSI are each drawn with
(A9–12)(D24–31) [1] Figure 1-2 pp. 6–7 (*inferred — unverified*: the RBV block's own data-bus
annotation is not legible in the printed diagram, and no text states the lane).

### 2.2 Register map

Eight byte registers, named below by the m68k kernel header symbols [7], [8] — Apple never printed
the register names:

| Offset | Header symbol | Direction | Function |
|---|---|---|---|
| +$000 | `rBufB` | R/W | Control byte — the VIA2 buffer-B position: cache enable, bus lock, power off, cache flush, NuBus transfer-mode status, sound source, parity test (§2.3) |
| +$001 | `rExp` | R/W | Expansion register; purpose undocumented in print (§2.9) |
| +$002 | `rSIFR` | R/W | Slot-interrupt status (§2.4) |
| +$003 | `rIFR` | R/W | Interrupt Flag register, also decoded at the VIA alias +$1A03 (§2.5) |
| +$010 | `rMonP` | R/W | Monitor parameters: depth, monitor-sense latch, video off (§2.8) |
| +$011 | `rChpT` | R/W | Chip-test register (§2.9) |
| +$012 | `rSIER` | R/W | Slot-interrupt enable (§2.7) |
| +$013 | `rIER` | R/W | Interrupt Enable register, also decoded at the VIA alias +$1C13 (§2.6) |

The three VIA-spaced aliases that shared II-family OS code uses (§4.2) reach +$002, +$003 and +$013:

| Alias offset | Reaches | Used by |
|---|---|---|
| +$1E02 | slot-interrupt status (+$002) | the VIA2 port-A position, which carried the slot lines on real-6522 machines; A/UX reads it there [6] |
| +$1A03 | IFR (+$003) | the VIA2 vIFR position, $1A00, plus the RBV's own A0/A1 bits selecting sub-register 3 [7], [8] |
| +$1C13 | IER (+$013) | the VIA2 vIER position, $1C00, plus A0/A1/A4 — both the VIA register-select field and the RBV's own sub-decode must name the register (§2.6) [8] |

Whether the register file repeats throughout the 8 KB window, and what (if anything) decodes above
+$013, is not established by any printed source (§6.2, §6.3).

### 2.3 Control byte (+$000, the buffer-B position)

The IIci bit assignment, from the Hardware Overview's IIci RBV table [4] pp. 63–64:

| Bit | Signal | Meaning |
|---|---|---|
| 0 | CENABLE~ | external cache enable, active low: 0 = cache enabled, 1 = disabled [5] pp. 360–361 |
| 1 | BUS.LOCK~ | bus lockout — blocks NuBus masters from main RAM while low [4] pp. 63–64 |
| 2 | PWR.OFF~ | soft power off, active low: writing 0 shuts the machine down [4] pp. 63–64, [3] p. 117 |
| 3 | FCFLUSH~ | external cache flush, active low: "should be set low when writing a 0 to bit 3 of the RBV register 0" [5] pp. 360–361 |
| 4 | TM1A~ | NuBus transfer-mode status line 1, recorded by the RBV [4] pp. 63–64 |
| 5 | TM0A~ | NuBus transfer-mode status line 0, recorded by the RBV [4] pp. 63–64 |
| 6 | SND.EXT~ | external-speaker sense: "the Sound Manager can read a bit … to determine whether the mono internal speaker or the stereo external sound jack is being used" [3] pp. 156–157 |
| 7 | PAR.TEST~ | parity test control — pairs with the IIci-only "signal used for testing the parity circuits" [3] p. 157, [4] pp. 63–64 |

The cache bits are the ones with externally documented strobe semantics: the IIci's cache connector
carries /CENABLE and /CFLUSH as driven outputs [5] Table 23-3 p. 521, and the adapter-card equations
show both strobes asserted by a 0 in the corresponding data bit during an RBV register-0 write, with
RESET returning them high [5] pp. 360–361 — i.e. the external cache is disabled after any reset.
Bits 4 and 5 exist because "It records two status lines from the NuBus (/TM0A and /TM1A). If an error
occurs during a NuBus access … the error type is sent to VIA2 over these two lines. The main
processor can read a register in VIA2 to find out the values of these two signals" [3] pp. 156–157.

### 2.4 Slot-interrupt status register (+$002)

The IIci assignment, from the Hardware Overview [4] pp. 63–64 ("Slot Interrupts Register"):

| Bit | Signal | Meaning |
|---|---|---|
| 0–5 | SLOT.IRQ1~ … SLOT.IRQ6~ | the six expansion-slot interrupt lines, active low |
| 6 | RAM.SIZ0 | RAM-size strapping bit 0 (see below) |
| 7 | RAM.SIZ1 | RAM-size strapping bit 1 |

The six slot positions are the geographic NuBus slots $9–$E of the six-slot II family; on the IIci
only slots $C, $D and $E have connectors [5] ch. 6, and "The Macintosh IIci can also generate a slot
interrupt ($0) for its built-in video circuits" [3] ch. 4, Table 4-20 — the built-in video's VBL is
delivered through this register family and "handled like a NuBus interrupt" [3] ch. 3, p. 99. Exactly
which bit position the slot-$0 video interrupt occupies is not printed anywhere, and the printed
evidence conflicts: the Hardware Overview labels bits 6–7 of this register as RAM-size strapping
(§6.4). The RAM-size labels themselves are
plausible but unexplained in print — on these machines the ROM sizes RAM by bank-wrap probing [1]
p. 22, so what the two bits strap is unknown (§6.4).

The register is read-polled: an interrupting slot reads 0 in its bit (*observed*: A/UX's level-2
drainer reads this register and NOTs it before masking [6]). It is also reachable at the VIA2
port-A alias +$1E02, because on real-6522 machines the slot lines were read out of VIA2 Data
register A [3] ch. 4; A/UX's slot-interrupt service uses exactly that address [6].

### 2.5 Interrupt Flag register (+$003, alias +$1A03)

The flag bits, per the Guide's VIA2 table with its IIci annotations [3] Table 4-27 pp. 186–187 and
the Hardware Overview [4] pp. 63–64:

| Bit | Meaning |
|---|---|
| 0 | SCSI DRQ — the SCSI controller's DMA request interrupt |
| 1 | ANY.SLOT — the OR of the (enabled) slot interrupt lines |
| 2 | /EXP.IRQ — expansion interrupt, "Macintosh IIci only" [3] Table 4-27; what it is wired to is not printed (§6.10) |
| 3 | SCSI IRQ — the SCSI controller's interrupt request |
| 4 | SND.IRQ — the ASC's interrupt (CB1 position in VIA terms; see [asc.md](../../hardware/asc.md) §6) |
| 5 | Timer T2 — "not used on the Macintosh IIci" [3] Table 4-27; the timers are not implemented in the ASIC generation at all ("The timers in the VIA2 chip are not used, and are not even implemented in the newer ASIC implementations" [4] pp. 59–60) |
| 6 | Timer T1 — likewise not used / not implemented |
| 7 | read: IRQ — set while any enabled VIA2 interrupt flag is set; write: the set/clear select for bits 0–6 |

Write semantics follow the classic VIA contract: "if bit 7 is a 1, each 1 in bits 0 through 6
[sets] the corresponding [flag]; if bit 7 is a 0, each 1 in bits 0 through 6 [clears] it" — the
Guide describes the mechanism for the IER [3] p. 187, and the flag register behaves the same way
(*observed*: A/UX acknowledges the level-2 path by writing $82 to the IFR alias [6]). Bit 7 on
read is the aggregate: "Bit 7 of the Interrupt Flag register remains set high (and the /IRQ line to
the general logic circuits is held low) as long as any VIA interrupt flag is set" [3] p. 185.

The alias is the compatibility path: the IFR answers at +$1A03 as well as at +$003 [7], [8]. The
$1A00 stride is the standard Macintosh VIA2 register select (RS3–RS0 on A12–A9, 512-byte sub-windows
— the same layout VIA1 uses [1] Figure 1-2 pp. 6–7), and $1A03 is that VIA position plus A0 and A1 — the RBV's
own low-group decode of the IFR's native offset +$003 (A4 is 0 in $1A03, and 1 in the IER's alias
$1C13, matching its native +$013). The VIA field and the RBV field must agree; this is also why
$1C03 does not select the IER (§2.6).

### 2.6 Interrupt Enable register (+$013, alias +$1C13)

Same bit order as the IFR, bits 0–6; bit 7 is the direction select: "When a program writes an 8-bit
value to the Interrupt Enable register, if bit 7 is a 1, each 1 in bits 0 through 6 enables the
corresponding interrupt; if bit 7 is a 0, each 1 in bits 0 through 6 disables the corresponding
interrupt. In either case, 0's in bits 0 through 6 … have no effect" [3] p. 187. When read, "bit 7
is always read as a 1" [3] p. 187. An interrupt disabled here still sets its IFR flag; it just does
not assert the IRQ aggregate [3] p. 187.

The IER alias carries the strangest decode rule in the file: it answers at **+$1C13** — the VIA2
vIER position $1C00 plus A0, A1 and **A4 = 1** — and *not* at $1C03, which carries the
VIA field for the IER but the RBV sub-decode for the IFR (its native offset +$013 needs A4 = 1). The mac68k register header records the anomaly verbatim: "CSA: in fact,
only bits 0, 1, and 4 seem to be decoded. BUT note the values for `rIER` and `rIFR`, where the top
8 bits do seem to matter. In fact all of the top 8 bits seem to matter; setting `rIER` = 0x1813 and
`rIFR` = 0x1803 doesn't work, either. Perhaps some sort of 'compatibility mode' is built-in?" [8].
No Apple document explains the asymmetry (§6.3).

### 2.7 Slot-interrupt enable register (+$012)

A per-slot enable register exists at +$012 (`rSIER` [7]) — VIA2 on real-6522 machines has no
equivalent, since their slot interrupt is a single OR gate. Its bit layout is not printed by Apple;
by the file's internal symmetry (bits 0–5 for the six slot lines, bit 7 as a set/clear select on
write) it should mirror the slot-status register (*inferred — unverified*, §6.8). The VIA2-level
enable for the slot aggregate as a whole lives in the IER, bit 1 [3] Table 4-27 pp. 186–187 — the
Guide's general statement that the slot interrupt "can be enabled or disabled by setting a bit in
the Interrupt Enable register" [3] ch. 3, p. 102 refers to that bit.

### 2.8 Monitor parameters register (+$010)

`rMonP` [7]. This register is the software handle on the video half: the monitor-sense value the RBV
latched at power-up, the selected pixel depth, and the video-off control. Apple's print corpus never
tabulates its bits. What is printed, and matches the register's effects, is:

- the sense inputs are the three MON.ID lines from the DB-15 video connector, grounded by the
  monitor for 0s, left unconnected for 1s [1] Table 5-1 p. 45, Table 5-3 p. 53 — pins 4 (MON.ID1),
  7 (MON.ID2) and 10 (MON.ID3) [1] Table 5-3 p. 53;
- the RBV "reads the ID lines from the video monitor to determine the type of monitor" at startup,
  and thereafter drives its timing from them [3] pp. 116–117 — "logic in the RBV reads the sense
  lines and sets the display parameters to the appropriate values" [3] p. 409;
- the pixel depth is software-selected among 1, 2, 4 and 8 bits per pixel [3] p. 408, [1] p. 45;
- when the monitor is unsupported or absent, "the system software switches the built-in video
  circuits off" [3] p. 420.

The bit fielding used by the ROM video driver — depth in bits 0–2 with the encoding 000 = 1 bpp,
001 = 2, 010 = 4, 011 = 8; the latched sense value in bits 3–5; video-off in bit 6; and an
all-outputs-tri-state bit 7 — is *inferred — unverified*: it is consistent with every printed fact
above but no Apple document states it (§6.5). The V8's version of this register carries additional
mode fields (an Apple-II 560 × 384 mode and a VRAM-vs-DRAM refresh select exist on the LC [4] p. 37,
[video-overview.md](../../hardware/video-overview.md) §3.3) whose bit positions are likewise
unprinted (§6.12).

### 2.9 Chip-test (+$011) and expansion (+$001) registers

`rChpT` and `rExp` [7]. Neither is described in any printed Apple document. The chip-test register
pairs with the IIci's parity-test signal (§2.3 bit 7) and the "signal used for testing the parity
circuits" [3] p. 157; the expansion register's name suggests the IIci's then-unused expansion paths.
Both are accept-and-ignore in every observed driver access — no shipped code reads either back with
a checked value (§6.6).

### 2.10 Reset state

Only one register bit has a documented reset value: the cache-enable strobe, which the adapter-card
equations return high (cache disabled) on RESET [5] pp. 360–361 — matching the cache-card guideline
that "the cache should also flush on a RESET~, but the system doesn't rely on this" [1] p. 32. The
power-on values of every other latch in the file — IFR/IER flags and enables, the slot enables, the
monitor register — are not printed anywhere (§6.9). A soft restart does not reinitialize the file:
nothing in the documented boot path writes it wholesale.

## 3. Behaviour

### 3.1 Interrupt aggregation and processor priority levels

The RBV is the IIci's and IIsi's interrupt concentrator: "In the Macintosh IIci, interrupt requests
are handled by the RBV, which assigns a priority to each and asserts one or more interrupt lines to
the CPU" [3] ch. 3, p. 99. The level assignment is fixed, not programmable ("In all machines except
the high-end machines (Mac IIfx and Eclipse), the interrupt priorities are fixed" [4] p. 15), and
follows the II-family pattern [3] Table 3-5 pp. 100–101:

| Level | Source | Autovector |
|---|---|---|
| 1 | VIA1 | $19 |
| 2 | VIA2 — **the RBV's own emulation on the IIci/IIsi** | $1A |
| 3 | none | $1B |
| 4 | SCC (connected "directly to the general-logic IC — … RBV in the Macintosh IIci" [3] ch. 3, p. 102) | $1C |
| 5 | none | $1D |
| 6 | none | $1E |
| 7 | interrupt switch — on the IIci "the parity checking circuits can also initiate a level-7 interrupt" [3] Table 3-5 footnote | $1F |

The Hardware Overview's IIci/IIsi RBV table gives the same picture from the chip's side: the RBV's
interrupt inputs are "7- NMI, 6- PURFAIL (NOT USED), 4- SCC, 3- LVL3 IRQ (NOT USED), 2- VIA2
(INTERNAL), 1- VIA1" [4] pp. 63–64 — that is, the RBV receives VIA1's request, generates level 2
internally from its own flag/enable pair, receives the SCC's request for level 4, and drives NMI at
level 7. Interrupts are always auto-vectored: "Interrupts in Macintoshes are always auto-vectored;
interrupting devices do not respond to IACK cycles" [4] p. 14. When several sources coincide, "If
the RBV IC receives more than one interrupt at the same time, it passes only the highest priority
interrupt on to the main processor" [3] p. 116 — the software-level re-polling loop (§4.3) is what
drains the remainder.

The IIsi's level-7 source differs in kind: "Mac IIsi … 68HC05 NMI" [4] Figure 2-1 p. 15 — the ADB
microcontroller raises NMI (Command-power on the keyboard) rather than a programmer's switch, and
the microcontroller also owns soft power [2] ch. 5 ("Power control", "Keyboard reset and NMI").

### 3.2 The slot-interrupt path

Each expansion slot has one interrupt line. "In the Macintosh IIci, individual interrupt lines go to
the RBV custom IC. The RBV performs the OR operation and stores the generic slot interrupt along
with the states of the individual interrupt lines in its VIA2-emulation registers" [3] ch. 3,
p. 100. The flow:

1. A card (or the built-in video, in its slot-$0 role [3] ch. 4, Table 4-20) pulls its line low.
2. The RBV ORs the six lines and stores the aggregate with the individual line states [3] ch. 3,
   p. 100, setting ANY.SLOT (IFR bit 1).
3. With IER bit 1 enabled, the RBV asserts the level-2 interrupt to the processor [3] Table 4-27
   pp. 186–187.
4. The processor's level-2 handler reads the IFR, sees the slot bit, and polls the
   slot-interrupt status register to find which line is low — the exact role VIA2 Data register A
   plays on real-6522 machines [3] ch. 3, p. 102, ch. 4.
5. The dispatch runs the per-slot service queue — on the II family each slot (and the built-in
   video's slot $0) carries its own Vertical Retrace Manager queue, executed "when the corresponding
   slot interrupt occurs" [3] ch. 12.

The status bits are level images, not latches: the card must drop its line when serviced, and the
handler re-polls until the register reads all-inactive (*observed* in the A/UX handler's loop shape,
§4.3). This level-sensitivity is the documented general VIA2 contract — "VIA2 also records in an
internal register the states of the … slot interrupt lines" [3] ch. 3, p. 99 — and it is why the
handler structure is a poll loop rather than a single dispatch.

### 3.3 NuBus status, RAM blocking and bus lock

The RBV is also the IIci's NuBus-status observer and gate. It "decodes NuBus errors and enables or
disables NuBus access to main RAM" [3] p. 117 ([2] ch. 5 words it "blocking NuBus accesses to RAM;
and decoding NuBus transaction errors"). The mechanism has three visible pieces:

- The control byte's TM1A~/TM0A~ bits record the NuBus transfer-mode status lines: on a NuBus error
  "the error type is sent to VIA2 over these two lines" and software reads the pair back [3]
  pp. 156–157 (§2.3).
- The control byte's BUS.LOCK~ bit is the gate: asserted, it blocks NuBus masters from main RAM [4]
  pp. 63–64 — the II family's mechanism "used to protect time-critical operations from interruption
  by NuBus transactions".
- The bus-error machinery the first duty refers to is timed by the RBV itself: the 15.6672 MHz
  clock it generates feeds "bus-error and RAM-refresh circuitry" [3] p. 116 (§1.6).

### 3.4 The video fetch engine

The video half of the RBV is a FIFO plus a serializer, fed by the MDU over the bank-A side of the
memory bus. The Guide gives the complete protocol [3] pp. 414–415; the Developer Notes add the
board-level view [1] pp. 23–24, [2] ch. 3:

1. **FIFO structure.** "The RBV's data-formating circuits comprise a 16 × 32-bit FIFO buffer, logic
   to keep the FIFO filled with data from the RAM, and logic to arrange that data and shift it out
   in the appropriate format. The FIFO operates as two halves, each containing eight 32-bit
   longwords. While video data from one half of the FIFO is being arranged and shifted out, the
   other half of the FIFO is being loaded with data from the screen buffer" [3] p. 414.
2. **Burst request.** "Whenever data in one half of the FIFO is used up, the RBV asserts its
   data-request line to the MDU. That signal tells the MDU to disconnect the bank A data bus from
   the main processor and to begin a page-mode burst read of RAM data as soon as possible" [3]
   p. 414. On the board, "the MDU responds by disconnecting the RAM data bus from the CPU data bus
   and performing an eight-longword DMA burst read from RAM while clocking the read data into the
   RBV FIFO" [1] p. 46, [2] ch. 4.
3. **Handshake.** "As each longword of video data is read onto the bank A portion of the data bus,
   the MDU sends a data-load signal to the RBV. The data-load signal causes the RBV to read one
   32-bit longword of data from the bus into the FIFO and to advance the input pointer … When the
   RBV has read seven longwords, it deasserts its data-request line, which causes the MDU to end the
   data burst after it has read the eighth longword and sent it to the RBV. That sequence of events
   fills the empty half of the FIFO" [3] p. 414.
4. **Serialization.** "In the other half of the FIFO, eight longwords of data are being loaded,
   sixteen bits at a time, into a shift register. A dot clock causes the data to advance through the
   shift register one bit at a time. The shift register has output taps every two bits along its
   length. By using one, two, four, or all eight of these taps, the logic circuits in the RBV can
   make the data appear at the outputs in the required pixel size: one, two, four, or eight bits at
   a time. When all sixteen bits have been shifted out, the logic circuits load the next sixteen
   bits from the FIFO" [3] p. 414. The 8-bit output feeds the Bt478 CLUT DAC, which indexes its
   256-entry lookup table and drives the three video DACs [4] pp. 38–39, [3] p. 417.
5. **Frame wrap.** "The RBV knows nothing about screen mapping or video addresses. Likewise, the MDU
   knows nothing about video. Each simply follows a protocol for passing data" [1] p. 47, [2] ch. 4.
   The RBV "tells the MDU to reset this pointer at the end of a screen, and the MDU sets the frame
   buffer pointer back to physical address $0000 0000" [1] p. 23, [2] ch. 3 — all MDU addresses are
   physical, since logical mapping is the 68030 MMU's job [1] p. 23.
6. **Timing.** "The RBV's video timing circuits generate the dot clock that advances the video data
   through the shift register and count out the appropriate number of dots per line. The video timing
   circuits also control the number of lines in the display and the timing of the synchronization
   pulses … based on the type of monitor connected to the computer, which the RBV determines by
   reading the sense lines" [3] p. 414.

The whole engine runs while the processor still has full access to the VIA registers: the register
file's 8-bit bus is separate from the 32-bit RAM bus the video half uses [3] p. 116.

### 3.5 Monitor sensing

The RBV latches a 3-bit monitor identity from the DB-15 connector's MON.ID1–3 lines (pins 4, 7, 10
[1] Table 5-3 p. 53), "asserted by the monitor by grounding lines for 0's and leaving no connects
for 1's" [1] p. 47, [2] ch. 4. The IIci decode [1] Table 5-1 p. 45:

| MON.ID3 ID2 ID1 | Monitor selected | Result |
|---|---|---|
| 0 0 0 | unsupported | video halted |
| 0 0 1 | 15" B&W Portrait | 640 × 870, 57.2832 MHz dot clock |
| 0 1 0 | reserved by Apple | — (12" RGB on the IIsi, below) |
| 0 1 1 | unsupported | video halted |
| 1 0 0 | unsupported | video halted |
| 1 0 1 | reserved by Apple | — |
| 1 1 0 | Macintosh II 12" B&W, 13" RGB | 640 × 480, 30.2400 MHz dot clock |
| 1 1 1 | no external monitor | video halted |

The IIsi variant adds one live code: `0 1 0` selects the 12" RGB (Rubik-style) monitor, 512 × 384 at
a 15.6672 MHz dot clock, 24.48 kHz line rate, 60.15 Hz frame rate [2] ch. 4, Table 4-2. When the code is
unsupported or absent, "on-board video is halted" and the halted state is externally well-defined:
the RBV drives no signals and stops them all — "VID.OUT(0-7) = 1's, CBLANK~ = 0, CSYNC~ = 1,
HSYNC~ = 1, VSYNC~ = 1" [1] Table 5-2 p. 45; the OS completes the picture by switching the circuits
off in software [3] p. 420.

The three-line scheme is the whole connector's allowance, and Apple extended it without adding pins:
"By selectively connecting diodes or wires between the pins, 28 additional ID codes are available" —
the reader pulls each sense line low in turn and reads the other two ("If any bit is zero, this is a
conventional ID code. Stop. If all bits are 1, then, for each ID line, pull down the ID line and
record the value on the other ID lines") [4] pp. 40–41. The IIci/IIsi RBV uses the conventional
3-bit codes only; the extended scheme matters for later monitors ([4] Table 6.1, and
[video-overview.md](../../hardware/video-overview.md) §5). The V8's sense handling has two printed
quirks: it "ignores monitor ID bit 1" (so monitors that differ only in that bit cannot be used with
it) and it "interprets monitor ID = 011 as a VGA monitor" [4] Table 6.1 footnotes, pp. 39–41.

### 3.6 Video timing

The RBV generates the timing; the tables below are the IIci's two supported formats. The 640 × 480
format is identical on the IIsi [2] ch. 4, Table 4-2.

Horizontal timing [3] Table 12-5 p. 416, [1] Figure 5-3 p. 50:

| Parameter | 640 × 480 | 640 × 870 |
|---|---|---|
| Dot clock | 30.24 MHz | 57.28 MHz |
| Dot time | 33.07 ns | 17.46 ns |
| Line rate | 35.0 kHz | 68.85 kHz |
| Line time | 28.57 µs | 14.52 µs |
| Full line | 864 dots | 832 dots |
| Visible line | 640 dots | 640 dots |
| Horizontal blanking | 224 dots | 192 dots |
| Front porch | 64 dots | 32 dots |
| Sync pulse | 64 dots | 80 dots |
| Back porch | 96 dots | 80 dots |

Vertical timing [3] Table 12-6 p. 416, [1] Figures 5-3/5-4 pp. 50–51:

| Parameter | 640 × 480 | 640 × 870 |
|---|---|---|
| Frame rate | 66.67 Hz | 75.0 Hz |
| Frame time | 15.00 ms | 13.33 ms |
| Full frame | 525 lines | 918 lines |
| Visible frame | 480 lines | 870 lines |
| Vertical blanking | 45 lines | 48 lines |
| Front porch | 3 lines | 3 lines |
| Sync pulse | 3 lines | 3 lines |
| Back porch | 39 lines | 42 lines |

Waveform contracts [1] Figure 5-2 notes, p. 49: all signals change on the rising edge of the dot
clock; "the width of the pulse on CSYNC~ during VSYNC~ low is the same width as the HSYNC~ pulse";
and for all supported monitors "both edges of VSYNC~ coincide with HSYNC~ falling". The RBV drives
pixel data, /CSYNC, /CBLANK, /HSYNC and /VSYNC to the CLUT DAC and connector; the sync outputs are
the RBV's own, not the DAC's — "the horizontal and vertical synchronization signals come directly
from the RBV" [3] p. 417.

### 3.7 Memory bandwidth and CPU contention

Video fetches tax only bank A. "The optional Bank B of DRAM connects directly to the CPU data bus,
and the CPU has full access to this bank at all times, as it does to ROM and the I/O devices" [1]
p. 46; "only accesses to RAM bank A are affected by video" [3] p. 413. Within bank A the RBV wins:
"there is contention for the bank between the processor and the RBV with the RBV taking precedence"
[4] p. 39. The measured cost on the 25 MHz IIci [3] Table 12-4 p. 413, [4] p. 39:

| Pixel depth | 640 × 480 | 640 × 870 |
|---|---|---|
| 1 bpp | 6 % | 13 % |
| 2 bpp | 13 % | 26 % |
| 4 bpp | 26 % | 65 % |
| 8 bpp | 64 % | not supported [3] Table 12-3 p. 411 |

Two further sizing rules are printed: the screen buffer occupies 300 KB at the bottom of bank A at
8 bpp/640 × 480 [3] p. 412, and "the MDU in the Macintosh IIci allocates memory for the screen
buffer in increments of 32 KB" — 64 KB for 1 bpp, 320 KB for 8 bpp [3] p. 411. The contended
probability also depends on how much of the machine's RAM lives in bank A, which is why the IIci
guideline is to "put the smaller SIMMs in bank A" [1] p. 22.

### 3.8 External-cache control

The RBV's control byte carries the external cache card's enable and flush strobes (§2.3), and the
IIci's cache connector receives them as the driven signals /CENABLE and /CFLUSH [5] Table 23-3
p. 521. The cache itself is entirely the card's business — "The organization of a particular cache
card's data and tag memory is determined by the card. System software does not make any assumptions
about the card's organization" [1] p. 33 — and the card asserts the connector's CACHE signal to
steal a cycle from the MDU: "This active high signal disables the memory controller (MDU), so that it
will not start a memory cycle and will allow the cache to supply the data instead … CACHE has no
effect on memory controller cycles for I/O devices" [1] p. 33. RESET disables the cache (§2.10).

### 3.9 Power-off

The control byte's PWR.OFF~ bit is the soft-power path on the IIci — "It contains a signal used to
turn off the computer" [3] p. 117, with the bit assignment PWR.OFF~ active low [4] pp. 63–64. On
the IIsi the ADB microcontroller owns the supply instead ("Shutdown is software controlled: the OS
sends a command that lets the microcontroller pull PFW low" [2] ch. 5, "Power control"), so what the
IIsi RBV's power bit actually gates is not printed (§6.11).

## 4. Programming model

### 4.1 The virtual VIA2 contract

Software is not supposed to notice that VIA2 is gone. "Although VIA2 is not a physical device on
the main logic board, its functions are provided by the RBV circuitry. These VIA2 functions include
decoding of the expansion slot interrupts, two SCSI interrupts, and the sound subsystem interrupt;
blocking NuBus accesses to RAM; and decoding NuBus transaction errors" [2] ch. 5; the Guide's
version: "In the Macintosh IIci, there is no VIA2; the VIA2-emulation portion of the RBV performs
all the functions listed above for the VIA2 in other machines; in addition, VIA2 emulation in the
RBV also performs the following functions: It provides two signals used to enable and to flush the
optional RAM cache. It provides a signal used for testing the parity circuits" [3] p. 157.

VIA1 stays a real 6523 VIA in both machines [4] Table 1.4, and carries the machine-identification
bits the ROM uses to tell machines apart: "Several bits in VIA1 have been redefined to allow the
ROM to distinguish between different computers" [2] ch. 5 — that is how a single universal II-family
ROM finds the RBV machines and selects their drivers. A driver that follows the VIA2 register
contract — flags and enables at the VIA2 offsets, slot status where the 6522 machines put port A —
works unmodified, provided it uses the alias offsets of §4.2.

### 4.2 Accessing the registers: native offsets and VIA aliases

Code reaches the RBV two ways, and both must work:

- **Native offsets** +$000–+$013 (§2.2), decoded from A0/A1/A4.
- **VIA2-style alias offsets** against the same base: the flag register at +$1A03, the enable
  register at +$1C13, the slot-interrupt status at +$1E02 [7], [8]. This is the path the shared
  II-family code takes, because it is the path a real VIA2 answers on.

The aliases exist because the RBV only decodes three address lines of its own; a real 6522 decodes
RS3–RS0 on A9–A12 and *ignores* A0/A1 entirely, which is why 512-byte-strided offsets like $1A00
work at all on the 6522 machines. On the RBV the same offsets must carry A0 = A1 = A4 = 1 to land on
a register, and — the asymmetry — the IER additionally requires the VIA register-select bits to name
it: $1C13 works, $1C03 does not, and even $1813 "doesn't work" either [8]. The m68k register header
sums up the decode as "only bits 0, 1, and 4 seem to be decoded. BUT … the top 8 bits do seem to
matter" [8].

The practical rule for register-level code is therefore fixed by what shipped operating systems
actually do (*observed*): use the byte-lane-agnostic VIA1 layout for VIA1, and the RBV alias values
— $1A03, $1C13, $1E02 — for the RBV's flag, enable and slot-status registers; the native offsets
+$000–+$013 for the RBV-only registers (control byte, monitor parameters, slot enable).

### 4.3 A level-2 interrupt handler, worked example

A/UX 3.0.1's retail kernel is the clearest published-shape example of the whole contract, because
its level-2 service routine drives the RBV exactly as this page describes. The kernel loads its
VIA2/RBV base from a global (at $0005AFD2, machine-dependent), then polls:

```asm
; via2intr — A/UX 3.0.1 retail kernel, $10011DE0 (level-2 autovector $1A path)
$10011DF6  MOVEA.L $0005AFD2,A2   ; A2 = VIA2/RBV base
$10011E0E  MOVE.B  $1A03(A2),D0   ; D0 = IFR  (RBV alias)
$10011E12  OR.B    via2_soft,D0   ; D0 |= soft-IFR (software-forced interrupts)
$10011E18  AND.B   $1C13(A2),D0   ; D0 &= IER (RBV alias)
$10011E1C  ANDI.L  #$7F,D0        ; mask off the set/clear bit
$10011E22  BEQ.S   done           ; nothing pending
$10011E24  MOVE.B  prio_lut(D0),D0; priority-encode the pending byte
$10011E38  MOVEA.L lvl2funcs(D0.W*4),A0
$10011E40  JSR     (A0)           ; dispatch one handler
$10011E42  BRA.S   poll           ; re-poll — the sources are level, not latched
```

Three contracts fall out of this loop and are general:

1. **The pending test is `IFR & IER & $7F`** — bit 7 of both registers is control, not status.
2. **The dispatch drains by priority and re-polls** — because the slot and device lines are level
   inputs (§3.2); a single-shot handler leaves sources unserviced.
3. **The slot-status read is separate**: A/UX's slot-interrupt sub-handler reads the slot register at
   the port-A alias $1E02 and NOTs it — the bits are active-low — before masking [6]
   ($10011E62, $10011E96). An acknowledge is a write of $82 to the IFR alias: set bit 7
   (write-select) plus bit 1 (the slot aggregate) [6] ($10011E90).

### 4.4 Enabling and disabling interrupts

The IER is the single enable point, with the set/clear-by-bit-7 contract of §2.6. To enable an
interrupt without touching the others, write bit 7 and the target bit; to disable, write the target
bit with bit 7 clear; zeros in bits 0–6 never change anything [3] p. 187. The ASC path is worked in
detail on the sound page ([asc.md](../../hardware/asc.md) §6.2): the sound interrupt is IER/IFR bit
4, enabled by writing $90 and disabled by writing $10. Disabling does not stop the flag from being
set — it only stops the aggregate from asserting [3] p. 187, so a device that interrupts while
masked is seen on the next unmask.

### 4.5 Video initialization and depth selection

The boot-time sequence the ROM and OS perform, assembled from the printed descriptions:

1. The RBV latches the monitor sense code and the ROM's driver selects the timing from it — "At
   startup time, logic in the RBV reads the sense lines and sets the display parameters to the
   appropriate values for the monitor that is connected" [3] p. 409.
2. "Software will determine the maximum (default, or previous selection by the user) video bit
   depth to be made available at startup, and set aside that memory for video. If a smaller bit
   depth than this maximum is selected by the user, operating system software may make use of this
   additional space" [1] p. 23, [2] ch. 3 — the MDU allocates in 32 KB increments [3] p. 411.
3. The OS maps the physical frame buffer into the logical NuBus slot window so the video stack can
   treat it as a card: "Using the memory management unit in the MC68030, the Macintosh IIci maps the
   screen buffer to logical address space starting at $FB00 0000 … the same as the address space
   used by expansion slot $B in the six-slot models" [3] p. 412 (the Guide's feature list: "memory
   addresses in slot $B of NuBus space; accessibility as pseudo-slot video in slot $0" [3] p. 408);
   the IIsi maps its buffer into super-slot logical space $E instead [2] ch. 3, [iisi.md](iisi.md)
   §4.2. In the 24-bit compatibility map the video window rides the same slot alias as any NuBus
   card [1] Table 2-1 p. 15.
4. Depth changes go through the monitor parameters register (§2.8): the driver writes the depth
   field and the RBV re-shapes its shift-register tap selection (§3.4 note 4) to 1, 2, 4 or 8 bits
   per pixel [3] p. 408.
5. If the monitor is unsupported or absent, video is halted — the RBV's halted waveform set (§3.5)
   plus "the system software switches the built-in video circuits off" [3] p. 420.

### 4.6 External cache control

The cache card is driven by ROM traps, not by direct register pokes from user software: "Cache card
enable, disable, and flush are controlled by ROM traps. They are called using a selector off the
HWPriv (A098) trap" [1] Table 4-2 p. 33:

| Function | Selector |
|---|---|
| EnableExtCache | 4 |
| DisableExtCache | 5 |
| FlushExtCache | 6 |

Each trap bottoms out in the control byte's cache bits (§2.3), producing the /CENABLE and /CFLUSH
strobes on the cache connector [5] Table 23-3 p. 521. Diagnostic and test software can also reach
the card's memories directly: cache data at $5200 0000–$527F FFFF, tags at $5280 0000–$52FF FFFF
(the card decodes the ranges itself; "no select signal is provided on the connector", and the space
is invisible to 24-bit mode — test software must switch with SwapMMUMode first) [1] Table 4-1 p. 32.

### 4.7 Shutdown

Turning the IIci off in software is the control byte's bit 2: drive it low and the supply drops
[4] pp. 63–64, [3] p. 117. Nothing else in the register file is involved; the IIsi's path runs
through its 68HC05 instead (§3.9). No printed source documents any arm or debounce step around the
IIci's bit, so the ROM's shutdown code is the only contract; whether anything beyond the single bit
write is required is an open question (§6.11).

## 5. Quirks & errata

- **The IER alias requires A4 = 1; the IFR alias does not.** The IER answers at $1C13 and not at
  $1C03 — the VIA register-select field and the RBV's own A0/A1/A4 sub-decode must both name the
  register — and the upper address bits matter too: $1813/$1803 "doesn't work" [8]. Any register
  map that assumes a uniform decode rule gets the IER wrong.
- **Only three address lines are decoded.** The register file sees A0, A1 and A4 [1] Figure 1-2
  pp. 6–7 — where a real 6522 ignores A0/A1 entirely and register-selects RS3–RS0 from A9–A12. The
  two decodes coexist on the RBV (§2.5–§2.6), which is why a real 6522 answers a $1A03 read as
  happily as a $1A00 one, but the RBV only answers where both fields agree.
- **The slot-status bits are active low.** Handlers read the slot register and NOT it before
  masking [6]; treating a set bit as "interrupting" inverts every dispatch decision.
- **IER bit 7 always reads as 1** [3] p. 187 — do not loop on it.
- **The timers do not exist.** "The timers in the VIA2 chip are not used, and are not even
  implemented in the newer ASIC implementations, so they should never be assumed to be available"
  [4] pp. 59–60; the Guide's flag table marks T1/T2 "not used on the Macintosh IIci" [3] Table 4-27
  pp. 186–187.
- **The halted-video waveform set is specified.** With an unsupported or absent monitor the RBV does
  not float its outputs — it drives VID.OUT = all ones with CBLANK asserted and all syncs negated
  [1] Table 5-2 p. 45.
- **Sense code 010 is reserved on the IIci and live on the IIsi.** The IIci's table marks MON.ID = 010
  "RESERVED for use by Apple" [1] Table 5-1 p. 45; the IIsi's variant runs the 12" RGB on it [2]
  Table 4-2. Code keyed to one machine's table mis-drives the other's monitor set.
- **8 bpp does not exist on the portrait format.** "The built-in video circuits on the Macintosh IIci
  do not support 8 bpp on the portrait display" [3] Table 12-3 p. 411 — the bandwidth table's "Can't"
  [4] p. 39.
- **The built-in video takes slot $B, below the physical slots.** The IIci's NuBus connectors are
  geographic slots $C–$E [5] ch. 6, and the frame buffer maps logically at $FB00 0000, slot $B of
  the six-slot models [3] p. 412 — while the IIsi's video rides super-slot $E above its single $9
  slot [2] ch. 3. Slot-number-dependent code must not assume the video slot's position.
- **RBV has precedence over the CPU for bank A** [4] p. 39 — the processor is the one that waits.
- **The monitor-sense scheme is only three bits on these machines.** The extended pull-down
  handshake exists [4] pp. 40–41 but the IIci/IIsi RBV implements the conventional codes; and the
  V8 differs in sense handling outright — it "ignores monitor ID bit 1" and reads 011 as VGA [4]
  Table 6.1 footnotes.
- **VIA bits PA0–PA2 and PB7 are reserved for sound across the family** — "Although these may be
  used for burn-in and CPU ID, they can be driven by software, so should have soft pull-ups or
  pull-downs" [4] p. 59. A machine-identification scheme that hard-straps these bits fights the
  sound path.
- **NMI arrives through the RBV on the IIci, and parity can ride the same level.** The programmer's
  interrupt switch is wired "directly to the general-logic IC — the GLUE … RBV in the Macintosh
  IIci" and produces a level-7 interrupt [3] ch. 3, p. 102; "In the Macintosh IIci … the parity
  checking circuits can also initiate a level-7 interrupt" [3] Table 3-5 footnote. On the IIsi the
  68HC05 microcontroller raises NMI instead [4] Figure 2-1 p. 15.
- **The register layout outlived the chip.** The same base address and layout reappear as the AMIC's
  pseudo-VIA2 slot bank on the Power Macintosh PDM platform [9] Figure 2-2 pp. 22–23; see
  [amic.md](../../machines/pdm/amic.md) §2.3. Compatibility code written against the RBV survived
  into the PowerPC era.
- **Marginal interrupt latency.** The RBV's interrupt path is known to be slow enough to have
  required a driver workaround on the 16 MHz IIsi — see
  [video-overview.md](../../hardware/video-overview.md) §3.1 for the lineage note.

## 6. Open questions

1. **The IIci's register base.** No Apple document in print publishes the IIci's I/O sub-decode; the
   $50F26000 base is inferred from the IIsi map [2] Figure 5-1, [5] Table 15-10 p. 344, the PDM
   AMIC's documented reuse of the same window [9] Figure 2-2, and the shared OS register
   definitions [7], [8]. A printed IIci decode would close this.
2. **Mirroring within the 8 KB window.** Whether the 20-byte register file repeats at a stride
   (512-byte VIA sub-windows? 8 KB?) and what a read of an offset between $014 and $1FFF returns is
   not printed anywhere.
3. **The exact decode equation.** The mac68k annotation [8] records that "all of the top 8 bits seem
   to matter" for $1A03/$1C13 while bits 0, 1 and 4 select the register — but no source explains the
   rule that makes $1C13 work and $1C03 and $1813 fail. Whether A9–A12 are gated into the decode at
   all, and why the IFR and IER differ, is unresolved.
4. **The slot-$0 video interrupt's bit position, and the RAM.SIZ bits.** The Hardware Overview labels
   slot-status bits 6–7 as RAM.SIZ0/1 [4] pp. 63–64, yet the built-in video's slot-$0 interrupt is
   documented to arrive "like a NuBus interrupt" [3] p. 99, Table 4-20. Which bit carries it, and
   what the RAM-size bits actually strap on machines that size RAM by wrap probing [1] p. 22, is
   unknown.
5. **The monitor parameters register's bit layout.** The depth/sense/off/tri-state fielding of §2.8
   is inferred, consistent with the printed behavior but never printed itself; likewise the depth
   encoding 000/001/010/011 = 1/2/4/8 bpp.
6. **The chip-test register (+$011) and expansion register (+$001).** Neither function nor bit
   layout is printed; only the offsets are recorded by the OS headers [7].
7. **The slot-interrupt enable register (+$012).** Bit layout and write semantics (set/clear select?
   per-slot mask?) are inferred from file symmetry only (§2.7).
8. **Power-on/reset values of every latch except the cache-enable strobe** (§2.10) — flags,
   enables, the monitor register.
9. **What /EXP.IRQ (IFR bit 2) is wired to on the IIci.** The Guide marks it "Macintosh IIci only"
   [3] Table 4-27; no source names the device behind it.
10. **The IIsi RBV's power-off bit.** The IIsi's 68HC05 owns the supply [2] ch. 5, so whether the
    RBV's PWR.OFF~ bit gates anything on that board is undetermined (§3.9).
11. **The shutdown write's ordering requirements** on the IIci — whether the ROM does anything beyond
    the single bit-2 write before the supply drops is not printed; no annotated IIci ROM shutdown
    sequence is in the evidence corpus.
12. **The V8's extra registers.** "The V8 chip in the Mac LC also has several other registers in
    addition to the VIA registers" [4] p. 59, with the layout only in the unprinted V8 spec
    (343S0116 [4] pp. 72–73); likewise the bit positions of the LC's Apple-II 560 × 384 mode and
    VRAM/DRAM refresh select.
13. **The "Buccaneer" project.** The Hardware Overview lists an unreleased 40 MHz 68030 machine as
    RBV-based [4] Table 1.4; which shipped product (if any) that became is not established from the
    printed record.
14. **The 3.672 MHz and 15.6672 MHz clock outputs' board distribution** — the two clocks are the
    RBV's to generate [3] p. 116, but no printed schematic-level source shows their loads.

## References

1. Apple Computer, Inc., *Macintosh IIci Developer Notes*, Developer Technical Publications, 1989.
   RBV/MDU architecture and features p. 3; hardware block diagram Figure 1-2 pp. 6–7 (RBV address
   wiring A0/A1/A4, VIA1 A9–A12); physical memory maps Figure 2-1 p. 14; 24-bit mapping Table 2-1
   p. 15; use of RAM by the video p. 23; RAM and video block diagram Figure 3-2 p. 24; cache
   address space Table 4-1 p. 32 and cache control trap Table 4-2 p. 33; video chapter: MON.ID
   values Table 5-1 p. 45, RBV signal descriptions Table 5-2 p. 45, sync-timing notes Figure 5-2
   p. 49, timing Figures 5-3/5-4 pp. 50–51, connector pinout Table 5-3 p. 53.
2. Apple Computer, Inc., *Macintosh IIsi Developer Note*, 1990. Chapter 1 (MDU/RBV architecture);
   chapter 3 "Use of RAM by the video"; chapter 4 The Video Interface (Table 4-2 RBV signal
   descriptions and monitor codes, sync-timing notes, video cables); chapter 5 Input/Output
   Interfaces (Figure 5-1 I/O address map, "Versatile Interface Adapter (VIA) interface", power
   control, keyboard reset and NMI); chapter 6 Expansion Interface.
3. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, 2nd ed., Addison-Wesley, 1990.
   RBV chapter 3 pp. 116–117; IIci interrupt handling ch. 3 p. 99 and interrupt levels Table 3-5
   pp. 100–101; NuBus slot interrupts p. 100; VIA2 general functions ch. 4 pp. 156–157 and "VIA2
   functions in the Macintosh IIci computer" p. 157; Interrupt Flag register Table 4-27 and
   Interrupt Enable register pp. 186–187; IIci video circuits ch. 12 pp. 408–420 (Table 12-3 p. 411,
   Table 12-4 p. 413, video portion of the RBV pp. 414–415, timing Tables 12-5/12-6 p. 416, CLUT DAC
   p. 417, sense values Table 12-8 p. 420).
4. Apple Computer, Inc., *Macintosh Hardware Overview*, revision 2, 11 February 1991. Interrupt
   priorities Figure 2-1 p. 15 and p. 14 (auto-vectoring); LC/V8 video pp. 37–38; RBV-style video
   pp. 38–39; monitor ID Table 6.1 pp. 39–41 with extended-sense algorithm pp. 40–41; V8 sound
   pp. 43–44 and second-generation V8 p. 47; VIA functions and spec list p. 59; timer and sound-bit
   notes pp. 59–60; machine comparison Table 1.4 and IIci/IIsi RBV register bit tables pp. 63–64;
   chip-spec part list pp. 72–73.
5. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, 3rd ed.,
   Addison-Wesley, 1992. NuBus slot ID ranges ch. 6; IIsi 32-bit physical address spaces Table 15-10
   p. 344; IIsi adapter-card signals Table 15-15 p. 359 and cache-control decode equations
   pp. 360–361; Macintosh IIci cache connector ch. 23, signal descriptions Table 23-3 p. 521.
6. Apple Computer, Inc., A/UX 3.0.1 retail kernel (installation boot disk), annotated disassembly.
   Level-2 service routine `via2intr` at `$10011DE0` (IFR read `$1A03(A2)`, IER read `$1C13(A2)`,
   dispatch loop, `$10011E0E–$10011E42`); slot-interrupt read at `$1E02(A2)` and `$82` IFR
   acknowledge at `$10011E62–$10011E96`; VIA2 base global at `$0005AFD2`.
7. Linux kernel, m68k Macintosh VIA/RBV register definitions,
   `arch/m68k/include/asm/mac_via.h` — the `rBufB`/`rExp`/`rSIFR`/`rIFR`/`rMonP`/`rChpT`/`rSIER`/
   `rIER` offset set ($0000/$0001/$0002/$1A03/$0010/$0011/$0012/$1C13) and the machine-class
   dispatch that selects it.
8. NetBSD/mac68k VIA/RBV register definitions, `sys/arch/mac68k/include/viareg.h` — parallel
   offset set, carrying the MacBSD-era annotation (21 May 1999) on the RBV address decode: "only
   bits 0, 1, and 4 seem to be decoded … the top 8 bits do seem to matter … setting rIER = 0x1813
   and rIFR = 0x1803 doesn't work, either. Perhaps some sort of 'compatibility mode' is built-in?"
9. Apple Computer, Inc., *Power Macintosh Computers* (Developer Note, March 1994), Developer
   Press — interrupt emulation and register diagram Figure 2-2 pp. 22–23, showing the AMIC
   pseudo-VIA2 slot bank in the RBV layout at base $50F26000.
